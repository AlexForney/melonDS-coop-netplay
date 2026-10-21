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

#include "Lockstep.h"
#include "Fiber.h"
#include "NDS.h"
#include "Platform.h"

#include <cstring>

namespace melonDS::Netplay
{

using Platform::Log;
using Platform::LogLevel;

static constexpr std::size_t kFiberStack = 4 * 1024 * 1024;
static constexpr int kMaxFrameSize = 0x948;     // LocalMP's limit

Lockstep::Lockstep() = default;

Lockstep::~Lockstep()
{
    for (Console* c : Consoles)
    {
        Fiber_Free(c->Context);
        delete c;
    }
    // MainFiber is the thread itself: nothing to free but the wrapper
    if (MainFiber) Fiber_Free(MainFiber);
}

void Lockstep::AddConsole(int id, NDS* nds)
{
    Console* c = new Console;
    c->Owner = this;
    c->ID = id;
    c->Nds = nds;
    c->Context = Fiber_Create(kFiberStack, ConsoleMain, c);

    auto it = Consoles.begin();
    while (it != Consoles.end() && (*it)->ID < id) ++it;
    Consoles.insert(it, c);
}

Lockstep::Console* Lockstep::Find(int id)
{
    for (Console* c : Consoles)
        if (c->ID == id) return c;
    return nullptr;
}

void Lockstep::ConsoleMain(void* arg)
{
    Console* c = static_cast<Console*>(arg);
    for (;;)
    {
        c->Nds->SetKeyMask(c->Input.KeyMask);
        if (c->Input.Touching)
            c->Nds->TouchScreen(c->Input.TouchX, c->Input.TouchY);
        else
            c->Nds->ReleaseScreen();

        c->Nds->RunFrame();

        c->St = State::FrameDone;
        Fiber_Switch(c->Owner->MainFiber);
    }
}

void Lockstep::RunFrame(const ConsoleInput* inputs)
{
    if (!MainFiber) MainFiber = Fiber_FromThread();

    for (Console* c : Consoles)
    {
        c->Input = inputs[c->ID];
        c->St = State::Ready;
        c->W = Wait::None;
        c->TimedOut = false;
    }

    std::size_t n = Consoles.size();
    std::size_t last = n - 1;           // so the first pick is console 0
    for (;;)
    {
        Console* pick = nullptr;
        for (std::size_t k = 1; k <= n; k++)
        {
            Console* c = Consoles[(last + k) % n];
            if (c->St == State::Ready || (c->St == State::Waiting && Satisfied(*c)))
            {
                pick = c;
                break;
            }
        }
        if (!pick)
        {
            // nobody can go on: everyone has finished the frame, or every unfinished console waits
            // for something no one will send before it moves on (the timeout, deterministically:
            // the next waiting console in schedule order)
            for (std::size_t k = 1; k <= n; k++)
            {
                Console* c = Consoles[(last + k) % n];
                if (c->St == State::Waiting)
                {
                    pick = c;
                    pick->TimedOut = true;
                    Timeouts++;
                    break;
                }
            }
            if (!pick) break;           // all done
        }

        for (std::size_t i = 0; i < n; i++)
            if (Consoles[i] == pick) last = i;
        pick->St = State::Running;
        pick->W = Wait::None;
        Current = pick;
        Switches++;
        // the core keeps "the console running on this thread" in a thread_local (NDS::Current,
        // set when a frame starts; the JIT's slow memory paths and fastmem fault handler use it);
        // consoles share this thread, so it follows each switch
        NDS::Current = pick->Nds;
        Fiber_Switch(pick->Context);
        Current = nullptr;
    }

    Trim();
}

namespace
{

void Put32(std::vector<u8>& out, u32 v)
{
    for (int i = 0; i < 4; i++) out.push_back(static_cast<u8>(v >> (8 * i)));
}

void Put64(std::vector<u8>& out, u64 v)
{
    Put32(out, static_cast<u32>(v));
    Put32(out, static_cast<u32>(v >> 32));
}

struct In
{
    const u8* P;
    std::size_t N;
    std::size_t Pos = 0;
    bool Ok = true;

    u32 Get32()
    {
        if (Pos + 4 > N) { Ok = false; return 0; }
        u32 v = 0;
        for (int i = 0; i < 4; i++) v |= static_cast<u32>(P[Pos++]) << (8 * i);
        return v;
    }
    u64 Get64()
    {
        u64 lo = Get32();
        return lo | (static_cast<u64>(Get32()) << 32);
    }
};

}

std::vector<u8> Lockstep::SaveState() const
{
    std::vector<u8> out;
    auto putQueue = [&out](const std::deque<Packet>& q, u64 base) {
        Put64(out, base);
        Put32(out, static_cast<u32>(q.size()));
        for (const Packet& p : q)
        {
            Put32(out, static_cast<u32>(p.Sender));
            Put32(out, p.Type);
            Put64(out, p.Timestamp);
            Put32(out, static_cast<u32>(p.Data.size()));
            out.insert(out.end(), p.Data.begin(), p.Data.end());
        }
    };
    putQueue(Packets, PacketBase);
    putQueue(Replies, ReplyBase);
    Put32(out, Connected);
    Put32(out, static_cast<u32>(HostID));
    Put32(out, static_cast<u32>(LastHostID));
    Put32(out, static_cast<u32>(Consoles.size()));
    for (const Console* c : Consoles)
    {
        Put32(out, static_cast<u32>(c->ID));
        Put64(out, c->PacketRead);
        Put64(out, c->ReplyRead);
    }
    return out;
}

bool Lockstep::LoadState(const u8* data, std::size_t len)
{
    In in {data, len};
    auto getQueue = [&in](std::deque<Packet>& q, u64& base) {
        base = in.Get64();
        u32 count = in.Get32();
        q.clear();
        for (u32 i = 0; i < count && in.Ok; i++)
        {
            Packet p;
            p.Sender = static_cast<int>(in.Get32());
            p.Type = in.Get32();
            p.Timestamp = in.Get64();
            u32 n = in.Get32();
            if (n > kMaxFrameSize || in.Pos + n > in.N) { in.Ok = false; break; }
            p.Data.assign(in.P + in.Pos, in.P + in.Pos + n);
            in.Pos += n;
            q.push_back(std::move(p));
        }
    };
    std::deque<Packet> packets, replies;
    u64 packetBase = 0, replyBase = 0;
    getQueue(packets, packetBase);
    getQueue(replies, replyBase);
    u16 connected = static_cast<u16>(in.Get32());
    int hostID = static_cast<int>(in.Get32());
    int lastHostID = static_cast<int>(in.Get32());
    u32 count = in.Get32();
    if (!in.Ok || count != Consoles.size()) return false;
    std::vector<std::pair<u64, u64>> reads(count);
    for (u32 i = 0; i < count; i++)
    {
        u32 id = in.Get32();
        reads[i] = {in.Get64(), in.Get64()};
        if (!in.Ok || static_cast<int>(id) != Consoles[i]->ID) return false;
    }

    Packets = std::move(packets);
    PacketBase = packetBase;
    Replies = std::move(replies);
    ReplyBase = replyBase;
    Connected = connected;
    HostID = hostID;
    LastHostID = lastHostID;
    for (u32 i = 0; i < count; i++)
    {
        Consoles[i]->PacketRead = reads[i].first;
        Consoles[i]->ReplyRead = reads[i].second;
    }
    return true;
}

void Lockstep::Suspend(Console& c, Wait w)
{
    c.St = State::Waiting;
    c.W = w;
    Fiber_Switch(MainFiber);
}

bool Lockstep::HasPacketFor(const Console& c) const
{
    for (u64 seq = c.PacketRead; seq < PacketBase + Packets.size(); seq++)
    {
        if (Packets[seq - PacketBase].Sender != c.ID)
            return true;
    }
    return false;
}

bool Lockstep::Satisfied(const Console& c) const
{
    switch (c.W)
    {
    case Wait::Packet:
        if (LastHostID != -1 && !(Connected & (1 << LastHostID))) return true;   // host gone
        return HasPacketFor(c);
    case Wait::Replies:
        return c.ReplyRead < ReplyBase + Replies.size();
    default:
        return true;
    }
}

void Lockstep::Trim()
{
    // packets every connected console has read
    u64 minRead = PacketBase + Packets.size();
    u64 minReply = ReplyBase + Replies.size();
    for (Console* c : Consoles)
    {
        if (!(Connected & (1 << c->ID))) continue;
        if (c->PacketRead < minRead) minRead = c->PacketRead;
        if (c->ReplyRead < minReply) minReply = c->ReplyRead;
    }
    while (PacketBase < minRead && !Packets.empty())
    {
        Packets.pop_front();
        PacketBase++;
    }
    while (ReplyBase < minReply && !Replies.empty())
    {
        Replies.pop_front();
        ReplyBase++;
    }
}

void Lockstep::Begin(int id)
{
    Console* c = Find(id);
    if (!c) return;
    c->PacketRead = PacketBase + Packets.size();
    c->ReplyRead = ReplyBase + Replies.size();
    Connected |= (1 << id);
}

void Lockstep::End(int id)
{
    Connected &= ~(1 << id);
}

int Lockstep::SendPacket(int id, u32 type, const u8* data, int len, u64 timestamp)
{
    if (len > kMaxFrameSize)
    {
        Log(LogLevel::Warn, "lockstep: frame too big (len=%d max=%d)\n", len, kMaxFrameSize);
        return 0;
    }

    Packet p;
    p.Sender = id;
    p.Type = type;
    p.Timestamp = timestamp;
    if (len > 0) p.Data.assign(data, data + len);

    u32 kind = type & 0xFFFF;
    if (kind == 2)
    {
        Replies.push_back(std::move(p));
    }
    else
    {
        Packets.push_back(std::move(p));
        if (kind == 1)
        {
            // a new MP exchange: the host reads only the replies to this command
            HostID = id;
            if (Console* host = Find(id))
                host->ReplyRead = ReplyBase + Replies.size();
        }
    }
    return len;
}

int Lockstep::RecvPacket(int id, u8* data, u64* timestamp, bool block)
{
    Console* c = Find(id);
    if (!c) return 0;

    if (block && LastHostID != -1 && !(Connected & (1 << LastHostID)))
        return -1;                      // the host is gone (LocalMP::RecvHostPacket)

    for (;;)
    {
        while (c->PacketRead < PacketBase + Packets.size())
        {
            const Packet& p = Packets[c->PacketRead - PacketBase];
            c->PacketRead++;
            if (p.Sender == id) continue;

            int len = static_cast<int>(p.Data.size());
            if (len) std::memcpy(data, p.Data.data(), len);
            if (len && (p.Type & 0xFFFF) == 1) LastHostID = p.Sender;
            if (timestamp) *timestamp = p.Timestamp;
            return len;
        }

        if (!block) return 0;
        if (c->TimedOut)
        {
            c->TimedOut = false;
            return 0;
        }
        Suspend(*c, Wait::Packet);
        if (LastHostID != -1 && !(Connected & (1 << LastHostID)))
            return -1;
    }
}

u16 Lockstep::RecvReplies(int id, u8* data, u64 timestamp, u16 aidmask)
{
    Console* c = Find(id);
    if (!c) return 0;

    u16 ret = 0;
    u16 seen = (1 << id);
    if ((seen & Connected) == Connected)
        return 0;                       // every client has left

    for (;;)
    {
        while (c->ReplyRead < ReplyBase + Replies.size())
        {
            const Packet& p = Replies[c->ReplyRead - ReplyBase];
            c->ReplyRead++;
            if (p.Sender == id || p.Timestamp < (timestamp - 32))
                continue;               // our own, or a stale reply

            if (!p.Data.empty())
            {
                u32 aid = p.Type >> 16;
                std::memcpy(&data[(aid - 1) * 1024], p.Data.data(), p.Data.size());
                ret |= (1 << aid);
            }
            seen |= (1 << p.Sender);
            if (((seen & Connected) == Connected) || ((ret & aidmask) == aidmask))
                return ret;
        }

        if (c->TimedOut)
        {
            c->TimedOut = false;
            return ret;
        }
        Suspend(*c, Wait::Replies);
    }
}

}
