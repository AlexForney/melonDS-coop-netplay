/*
    Copyright 2016-2025 melonDS team, 2026 KH Days co-op mod

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "Session.h"

#include <chrono>
#include <cstring>
#include <enet/enet.h>

namespace melonDS::Netplay
{

enum Msg : u8
{
    Msg_Hello = 1,      // guest: u32 protocol, rom[16], name
    Msg_Welcome,        // host: u8 slot
    Msg_Reject,         // host: u8 reason
    Msg_Save,           // guest: u32 len, data
    Msg_Lobby,          // host: u8 count, then per slot: u8 used, name
    Msg_Start,          // host: see StartGame
    Msg_Input,          // u8 slot, u32 frame, u16 keys, u8 touching, u16 x, u16 y
    Msg_Hash,           // u8 slot, u32 frame, u8 count, u64 hash[count]
    Msg_Desync,         // host: u32 frame
};

enum Reject : u8
{
    Reject_Version = 1,
    Reject_Rom,
    Reject_Full,
    Reject_Started,
};

static int ENetUsers = 0;

static u64 NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

namespace
{

struct Writer
{
    std::vector<u8> D;
    void U8(u8 v) { D.push_back(v); }
    void U16(u16 v) { U8(v & 0xFF); U8(v >> 8); }
    void U32(u32 v) { U16(v & 0xFFFF); U16(v >> 16); }
    void U64(u64 v) { U32(static_cast<u32>(v)); U32(static_cast<u32>(v >> 32)); }
    void Bytes(const void* p, size_t n)
    {
        const u8* b = static_cast<const u8*>(p);
        D.insert(D.end(), b, b + n);
    }
    void Str(const std::string& s)
    {
        u8 n = static_cast<u8>(s.size() > 255 ? 255 : s.size());
        U8(n);
        Bytes(s.data(), n);
    }
};

struct Reader
{
    const u8* P;
    size_t N;
    size_t Pos = 0;
    bool Ok = true;

    Reader(const u8* p, size_t n) : P(p), N(n) {}
    bool Need(size_t k)
    {
        if (Pos + k > N) Ok = false;
        return Ok;
    }
    u8 U8() { return Need(1) ? P[Pos++] : 0; }
    u16 U16() { u16 lo = U8(); return static_cast<u16>(lo | (U8() << 8)); }
    u32 U32() { u32 lo = U16(); return lo | (static_cast<u32>(U16()) << 16); }
    u64 U64() { u64 lo = U32(); return lo | (static_cast<u64>(U32()) << 32); }
    void Bytes(void* out, size_t n)
    {
        if (!Need(n)) return;
        std::memcpy(out, P + Pos, n);
        Pos += n;
    }
    std::string Str()
    {
        u8 n = U8();
        if (!Need(n)) return {};
        std::string s(reinterpret_cast<const char*>(P + Pos), n);
        Pos += n;
        return s;
    }
};

}

static std::string SanitizedName(const std::string& name)
{
    std::string out;
    for (char ch : name)
    {
        if (out.size() >= kNameLength) break;
        if (ch >= 0x20 && ch < 0x7F) out.push_back(ch);
    }
    if (out.empty()) out = "PLAYER";
    return out;
}

Session::Session()
{
    if (ENetUsers++ == 0) enet_initialize();
}

Session::~Session()
{
    Leave();
    if (--ENetUsers == 0) enet_deinitialize();
}

void Session::Fail(const std::string& why)
{
    if (ErrorText.empty()) ErrorText = why;
    CurPhase = Phase::Ended;
}

int Session::SlotOf(ENetPeer* peer) const
{
    for (int s = 1; s < kMaxConsoles; s++)
        if (Peers[s] == peer) return s;
    return -1;
}

int Session::LobbyCount() const
{
    int n = 0;
    for (bool used : SlotUsed)
        if (used) n++;
    return n;
}

bool Session::Host(int port, const std::string& name, const RomHash& rom, std::vector<u8> save,
                   const std::string& bindAddress)
{
    Leave();
    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    if (!bindAddress.empty() && enet_address_set_host(&addr, bindAddress.c_str()) != 0)
    {
        Fail("unknown address " + bindAddress);
        return false;
    }
    addr.port = static_cast<enet_uint16>(port);
    Net = enet_host_create(&addr, kMaxConsoles, 1, 0, 0);
    if (!Net)
    {
        Fail("could not open UDP port " + std::to_string(port));
        return false;
    }
    IsHost = true;
    MySlot = 0;
    Rom = rom;
    SlotUsed = {};
    SlotUsed[0] = true;
    LobbyNames[0] = SanitizedName(name);
    LobbySaves[0] = std::move(save);
    CurPhase = Phase::Lobby;
    return true;
}

bool Session::Join(const std::string& address, int port, const std::string& name, const RomHash& rom,
                   std::vector<u8> save)
{
    Leave();
    Net = enet_host_create(nullptr, 1, 1, 0, 0);
    if (!Net)
    {
        Fail("could not open a UDP socket");
        return false;
    }
    ENetAddress addr;
    if (enet_address_set_host(&addr, address.c_str()) != 0)
    {
        Fail("unknown host " + address);
        return false;
    }
    addr.port = static_cast<enet_uint16>(port);
    HostPeer = enet_host_connect(Net, &addr, 1, 0);
    if (!HostPeer)
    {
        Fail("could not connect");
        return false;
    }
    IsHost = false;
    Rom = rom;
    MyName = SanitizedName(name);
    MySave = std::move(save);
    CurPhase = Phase::Connecting;
    return true;
}

void Session::Leave()
{
    if (!Net) return;
    Flush();
    for (std::size_t i = 0; i < Net->peerCount; i++)
    {
        ENetPeer* peer = &Net->peers[i];
        if (peer->state == ENET_PEER_STATE_CONNECTED)
            enet_peer_disconnect_now(peer, 0);
    }
    enet_host_flush(Net);
    enet_host_destroy(Net);
    Net = nullptr;
    HostPeer = nullptr;
    Peers = {};
    Delayed.clear();
    if (CurPhase != Phase::Ended) CurPhase = Phase::Idle;
}

void Session::Send(ENetPeer* peer, std::vector<u8> data)
{
    if (!Net) return;
    if (LagMs > 0)
    {
        Delayed.push_back({NowMs() + static_cast<u64>(LagMs), peer, std::move(data)});
        return;
    }
    ENetPacket* pkt = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
    if (peer)
        enet_peer_send(peer, 0, pkt);
    else
        enet_host_broadcast(Net, 0, pkt);
}

void Session::Flush()
{
    if (!Net) return;
    u64 now = NowMs();
    while (!Delayed.empty() && Delayed.front().DueMs <= now)
    {
        Outgoing& out = Delayed.front();
        ENetPacket* pkt = enet_packet_create(out.Data.data(), out.Data.size(), ENET_PACKET_FLAG_RELIABLE);
        if (out.Peer)
            enet_peer_send(out.Peer, 0, pkt);
        else
            enet_host_broadcast(Net, 0, pkt);
        Delayed.pop_front();
    }
    enet_host_flush(Net);
}

void Session::Poll(int timeoutMs)
{
    if (!Net) return;
    Flush();

    ENetEvent ev;
    int timeout = timeoutMs;
    while (Net && enet_host_service(Net, &ev, static_cast<enet_uint32>(timeout)) > 0)
    {
        timeout = 0;
        switch (ev.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            if (!IsHost && ev.peer == HostPeer)
            {
                Writer w;
                w.U8(Msg_Hello);
                w.U32(kProtocolVersion);
                w.Bytes(Rom.data(), Rom.size());
                w.Str(MyName);
                Send(HostPeer, std::move(w.D));
            }
            break;

        case ENET_EVENT_TYPE_RECEIVE:
            Handle(ev.peer, ev.packet->data, ev.packet->dataLength);
            enet_packet_destroy(ev.packet);
            break;

        case ENET_EVENT_TYPE_DISCONNECT:
            if (IsHost)
            {
                int slot = SlotOf(ev.peer);
                if (slot < 0) break;
                Peers[slot] = nullptr;
                if (CurPhase == Phase::Lobby)
                {
                    SlotUsed[slot] = false;
                    LobbyNames[slot].clear();
                    LobbySaves[slot].clear();
                    SendLobby();
                }
                else if (CurPhase == Phase::Running)
                {
                    Fail("P" + std::to_string(slot + 1) + " left the session");
                }
            }
            else
            {
                Fail(CurPhase == Phase::Connecting ? "could not reach the host" : "the host left the session");
            }
            break;

        default:
            break;
        }
    }
    Flush();
}

void Session::Handle(ENetPeer* peer, const u8* data, size_t len)
{
    if (len == 0) return;
    if (IsHost)
        HandleHost(peer, data, len);
    else
        HandleGuest(data, len);
}

void Session::SendLobby()
{
    Writer w;
    w.U8(Msg_Lobby);
    w.U8(kMaxConsoles);
    for (int s = 0; s < kMaxConsoles; s++)
    {
        w.U8(SlotUsed[s] ? 1 : 0);
        w.Str(LobbyNames[s]);
    }
    Send(nullptr, std::move(w.D));
}

void Session::HandleHost(ENetPeer* peer, const u8* data, size_t len)
{
    Reader r(data, len);
    u8 type = r.U8();

    if (type == Msg_Hello)
    {
        u32 version = r.U32();
        RomHash rom {};
        r.Bytes(rom.data(), rom.size());
        std::string name = SanitizedName(r.Str());
        if (!r.Ok) return;

        u8 reason = 0;
        int slot = -1;
        if (version != kProtocolVersion) reason = Reject_Version;
        else if (rom != Rom) reason = Reject_Rom;
        else if (CurPhase != Phase::Lobby) reason = Reject_Started;
        else
        {
            for (int s = 1; s < kMaxConsoles; s++)
            {
                if (!SlotUsed[s])
                {
                    slot = s;
                    break;
                }
            }
            if (slot < 0) reason = Reject_Full;
        }

        Writer w;
        if (reason)
        {
            w.U8(Msg_Reject);
            w.U8(reason);
            Send(peer, std::move(w.D));
            enet_peer_disconnect_later(peer, 0);
            return;
        }
        Peers[slot] = peer;
        SlotUsed[slot] = true;
        LobbyNames[slot] = name;
        LobbySaves[slot].clear();
        w.U8(Msg_Welcome);
        w.U8(static_cast<u8>(slot));
        Send(peer, std::move(w.D));
        SendLobby();
        return;
    }

    int slot = SlotOf(peer);
    if (slot < 0) return;

    if (type == Msg_Save)
    {
        u32 n = r.U32();
        if (!r.Need(n)) return;
        LobbySaves[slot].assign(data + r.Pos, data + r.Pos + n);
        return;
    }

    if (type == Msg_Input && CurPhase == Phase::Running)
    {
        u8 claimed = r.U8();
        u32 frame = r.U32();
        ConsoleInput in;
        in.KeyMask = r.U16();
        in.Touching = r.U8() != 0;
        in.TouchX = r.U16();
        in.TouchY = r.U16();
        if (!r.Ok || claimed != slot) return;
        StoreInput(slot, frame, in);
        // relay to the other guests, as received
        for (int s = 1; s < kMaxConsoles; s++)
        {
            if (s != slot && Peers[s])
                Send(Peers[s], std::vector<u8>(data, data + len));
        }
        return;
    }

    if (type == Msg_Hash && CurPhase == Phase::Running)
    {
        u8 claimed = r.U8();
        u32 frame = r.U32();
        u8 count = r.U8();
        std::vector<u64> hashes(count);
        for (u8 i = 0; i < count; i++) hashes[i] = r.U64();
        if (!r.Ok || claimed != slot) return;
        GuestHashes[frame][slot] = std::move(hashes);
        CompareHashes(frame);
        return;
    }
}

void Session::HandleGuest(const u8* data, size_t len)
{
    Reader r(data, len);
    u8 type = r.U8();

    switch (type)
    {
    case Msg_Welcome:
    {
        u8 slot = r.U8();
        if (!r.Ok || slot >= kMaxConsoles) return;
        MySlot = slot;
        Writer w;
        w.U8(Msg_Save);
        w.U32(static_cast<u32>(MySave.size()));
        w.Bytes(MySave.data(), MySave.size());
        Send(HostPeer, std::move(w.D));
        CurPhase = Phase::Lobby;
        return;
    }
    case Msg_Reject:
    {
        u8 reason = r.U8();
        const char* why = reason == Reject_Version ? "the host runs another version" :
                          reason == Reject_Rom ? "the host runs another ROM (patch it from the same EU ROM)" :
                          reason == Reject_Full ? "the session is full" :
                          reason == Reject_Started ? "the session has already started" : "rejected";
        Fail(why);
        return;
    }
    case Msg_Lobby:
    {
        u8 n = r.U8();
        for (int s = 0; s < n && s < kMaxConsoles; s++)
        {
            SlotUsed[s] = r.U8() != 0;
            LobbyNames[s] = r.Str();
        }
        return;
    }
    case Msg_Start:
    {
        StartInfo info;
        info.NumPlayers = r.U8();
        info.InputDelay = r.U8();
        info.JIT = r.U8() != 0;
        info.Year = r.U16();
        info.Month = r.U8();
        info.Day = r.U8();
        info.Hour = r.U8();
        info.Minute = r.U8();
        info.Second = r.U8();
        if (info.NumPlayers < 1 || info.NumPlayers > kMaxConsoles) return;
        for (int s = 0; s < info.NumPlayers; s++)
        {
            info.Names[s] = r.Str();
            u32 n = r.U32();
            if (!r.Need(n)) return;
            info.Saves[s].assign(data + r.Pos, data + r.Pos + n);
            r.Pos += n;
        }
        if (!r.Ok) return;
        Info = std::move(info);
        for (int s = 0; s < kMaxConsoles; s++) Inputs[s].clear();
        for (int f = 0; f < Info.InputDelay; f++)
            for (int s = 0; s < Info.NumPlayers; s++)
                Inputs[s][static_cast<u32>(f)] = ConsoleInput{};
        CurPhase = Phase::Running;
        return;
    }
    case Msg_Input:
    {
        u8 slot = r.U8();
        u32 frame = r.U32();
        ConsoleInput in;
        in.KeyMask = r.U16();
        in.Touching = r.U8() != 0;
        in.TouchX = r.U16();
        in.TouchY = r.U16();
        if (!r.Ok || slot >= kMaxConsoles || static_cast<int>(slot) == MySlot) return;
        StoreInput(slot, frame, in);
        return;
    }
    case Msg_Desync:
    {
        u32 frame = r.U32();
        if (r.Ok && DesyncFrame < 0) DesyncFrame = frame;
        return;
    }
    default:
        return;
    }
}

bool Session::StartGame(int inputDelay, bool jit, int year, int month, int day, int hour,
                        int minute, int second)
{
    if (!IsHost || CurPhase != Phase::Lobby) return false;

    int n = 0;
    while (n < kMaxConsoles && SlotUsed[n]) n++;
    for (int s = n; s < kMaxConsoles; s++)
    {
        if (SlotUsed[s])
        {
            ErrorText = "a player left the lobby: wait for the slot to fill again";
            return false;
        }
    }

    StartInfo info;
    info.NumPlayers = n;
    info.InputDelay = inputDelay < 1 ? 1 : (inputDelay > 30 ? 30 : inputDelay);
    info.JIT = jit;
    info.Year = year; info.Month = month; info.Day = day;
    info.Hour = hour; info.Minute = minute; info.Second = second;

    Writer w;
    w.U8(Msg_Start);
    w.U8(static_cast<u8>(n));
    w.U8(static_cast<u8>(info.InputDelay));
    w.U8(jit ? 1 : 0);
    w.U16(static_cast<u16>(year));
    w.U8(static_cast<u8>(month));
    w.U8(static_cast<u8>(day));
    w.U8(static_cast<u8>(hour));
    w.U8(static_cast<u8>(minute));
    w.U8(static_cast<u8>(second));
    for (int s = 0; s < n; s++)
    {
        info.Names[s] = LobbyNames[s];
        info.Saves[s] = LobbySaves[s];
        w.Str(info.Names[s]);
        w.U32(static_cast<u32>(info.Saves[s].size()));
        w.Bytes(info.Saves[s].data(), info.Saves[s].size());
    }
    Send(nullptr, std::move(w.D));

    Info = std::move(info);
    for (int s = 0; s < kMaxConsoles; s++) Inputs[s].clear();
    for (int f = 0; f < Info.InputDelay; f++)
        for (int s = 0; s < Info.NumPlayers; s++)
            Inputs[s][static_cast<u32>(f)] = ConsoleInput{};
    CurPhase = Phase::Running;
    Flush();
    return true;
}

void Session::StoreInput(int slot, u32 frame, const ConsoleInput& input)
{
    if (slot < 0 || slot >= kMaxConsoles) return;
    Inputs[slot][frame] = input;
}

void Session::SubmitInput(u32 frame, const ConsoleInput& input)
{
    if (CurPhase != Phase::Running) return;
    StoreInput(MySlot, frame, input);

    Writer w;
    w.U8(Msg_Input);
    w.U8(static_cast<u8>(MySlot));
    w.U32(frame);
    w.U16(static_cast<u16>(input.KeyMask));
    w.U8(input.Touching ? 1 : 0);
    w.U16(input.TouchX);
    w.U16(input.TouchY);
    Send(IsHost ? nullptr : HostPeer, std::move(w.D));
}

bool Session::HaveInputs(u32 frame) const
{
    for (int s = 0; s < Info.NumPlayers; s++)
        if (Inputs[s].find(frame) == Inputs[s].end()) return false;
    return true;
}

void Session::TakeInputs(u32 frame, ConsoleInput* out)
{
    for (int s = 0; s < kMaxConsoles; s++)
    {
        out[s] = ConsoleInput{};
        auto it = Inputs[s].find(frame);
        if (it != Inputs[s].end()) out[s] = it->second;
        Inputs[s].erase(Inputs[s].begin(), Inputs[s].upper_bound(frame));
    }
}

void Session::SubmitHashes(u32 frame, const u64* hashes, int count)
{
    if (CurPhase != Phase::Running) return;
    if (IsHost)
    {
        HostHashes[frame].assign(hashes, hashes + count);
        CompareHashes(frame);
        return;
    }
    Writer w;
    w.U8(Msg_Hash);
    w.U8(static_cast<u8>(MySlot));
    w.U32(frame);
    w.U8(static_cast<u8>(count));
    for (int i = 0; i < count; i++) w.U64(hashes[i]);
    Send(HostPeer, std::move(w.D));
}

void Session::CompareHashes(u32 frame)
{
    auto mine = HostHashes.find(frame);
    auto theirs = GuestHashes.find(frame);
    if (mine == HostHashes.end() || theirs == GuestHashes.end()) return;

    for (int s = 1; s < Info.NumPlayers; s++)
    {
        const std::vector<u64>& h = theirs->second[s];
        if (!h.empty() && h != mine->second && DesyncFrame < 0)
        {
            DesyncFrame = frame;
            Writer w;
            w.U8(Msg_Desync);
            w.U32(frame);
            Send(nullptr, std::move(w.D));
        }
    }

    // forget what every guest has reported
    bool all = true;
    for (int s = 1; s < Info.NumPlayers; s++)
        if (theirs->second[s].empty()) all = false;
    if (all)
    {
        GuestHashes.erase(theirs);
        HostHashes.erase(HostHashes.begin(), HostHashes.upper_bound(frame > 600 ? frame - 600 : 0));
    }
}

}
