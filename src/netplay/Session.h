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

// A netplay session over the internet: every PC runs every player's console (Lockstep) and only
// controller input travels. Star topology: guests connect to the host (one forwarded UDP port),
// the host relays. ENet carries it (reliable, ordered).
//
//   lobby:   guest -> host  HELLO (protocol, ROM hash, name), then SAVE (its own save file)
//            host -> guest  WELCOME (its slot) or REJECT; LOBBY (the player list) to everyone
//   start:   host -> all    START: player count, names, the consoles' clock, input delay, JIT,
//                           every player's save. Every PC then boots every console from these.
//   frames:  each PC sends its own input for frame f + delay (INPUT); the host forwards guests'
//            inputs to the other guests. A PC runs frame f once it has every player's input
//            for f, and waits (polling the network) until then.
//   check:   every PC sends the RAM hashes of all consoles every kHashPeriod frames (HASH); the
//            host compares them with its own.
//
// Players leaving and coming back, and repairs:
//   drop:    when a guest's connection ends, the host tells everyone (DROP) from which frame on
//            that player's console runs with no buttons pressed: the newest frame it had from
//            them + 1. Every PC got the player's earlier inputs first (one ordered channel), so
//            every PC switches at the same frame. The game goes on; the console stays.
//   resync:  every PC loads a snapshot of every console at one frame R (RESYNC from the host).
//            R is one past the newest frame the host has sent its own input for, so no PC can
//            have run R yet when it hears of it. At R the host snapshots its consoles and sends
//            the snapshot (STATE) to the PCs that need it: a player coming back (who boots from
//            it) and any PC whose hashes differed from the host's. Every other PC reloads its
//            own snapshot of R: a console that has loaded a snapshot runs on with an empty JIT
//            cache, and the JIT's results depend on its cache's history, so every PC loads.
//   rejoin:  a player whose connection ended joins again (HELLO while running): the host gives
//            back the slot (WELCOME + START) and schedules a resync with them; their input
//            counts again from R + input delay.

#ifndef MELONDS_NETPLAY_SESSION_H
#define MELONDS_NETPLAY_SESSION_H

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "types.h"
#include "Lockstep.h"

typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;

namespace melonDS::Netplay
{

constexpr u32 kProtocolVersion = 2;
constexpr int kDefaultPort = 7065;
constexpr int kNameLength = 10;         // the DS firmware's nickname length
constexpr u32 kHashPeriod = 60;

using RomHash = std::array<u8, 16>;

struct StartInfo
{
    int NumPlayers = 0;
    int InputDelay = 4;
    bool JIT = true;
    int Year = 2026, Month = 1, Day = 1, Hour = 12, Minute = 0, Second = 0;
    std::array<std::string, kMaxConsoles> Names;
    std::array<std::vector<u8>, kMaxConsoles> Saves;
};

// A resync every PC carries out before it runs frame `Frame`.
struct Resync
{
    u32 Frame = 0;
    u8 Targets = 0;                     // slots that load the host's snapshot
    u8 Joins = 0;                       // ... of which players coming back
};

class Session
{
public:
    enum class Phase { Idle, Connecting, Lobby, Running, Ended };

    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Open a session as its host (slot 0) or join one. `save` is this player's own save file.
    // `bindAddress` empty: every interface (what players on other networks need).
    bool Host(int port, const std::string& name, const RomHash& rom, std::vector<u8> save,
              const std::string& bindAddress = "");
    bool Join(const std::string& address, int port, const std::string& name, const RomHash& rom,
              std::vector<u8> save);
    void Leave();

    // Network work; call it often (every frame, and in a loop while waiting).
    void Poll(int timeoutMs = 0);

    // Host, in the lobby: everyone who joined plays.
    bool StartGame(int inputDelay, bool jit, int year, int month, int day, int hour, int minute,
                   int second);

    Phase GetPhase() const { return CurPhase; }
    int LocalSlot() const { return MySlot; }
    int LobbyCount() const;             // players in the lobby (host included)
    bool LobbyUsed(int slot) const { return slot >= 0 && slot < kMaxConsoles && SlotUsed[slot]; }
    std::string LobbyName(int slot) const
    {
        return slot >= 0 && slot < kMaxConsoles ? LobbyNames[slot] : std::string();
    }
    bool IsHosting() const { return IsHost; }
    // Running: whether a player is connected (false once they dropped, until they are back).
    bool Connected(int slot) const { return slot >= 0 && slot < kMaxConsoles && Present[slot]; }
    const StartInfo& Start() const { return Info; }
    const std::string& Error() const { return ErrorText; }

    // A player coming back: Running starts at FirstFrame() from a snapshot (RejoinState()).
    bool Rejoined() const { return IsRejoin; }
    u32 FirstFrame() const { return StateFrame; }
    const std::vector<u8>& RejoinState() const { return StateData; }

    // The local player's input for `frame` (already `InputDelay` ahead of the frame being run).
    void SubmitInput(u32 frame, const ConsoleInput& input);
    bool HaveInputs(u32 frame) const;
    u8 MissingInputs(u32 frame) const;  // slots whose input for `frame` has not arrived
    // Every player's input for `frame`; frames before it are forgotten.
    void TakeInputs(u32 frame, ConsoleInput* out);

    // Resyncs: the one to carry out before running `frame`, if any.
    const Resync* ResyncAt(u32 frame) const;
    // Host, at the resync's frame: send the snapshot to the PCs that load it.
    void SendState(const Resync& rs, const std::vector<u8>& snapshot);
    // Target of a resync: the host's snapshot for `frame`, once it has arrived (else nullptr).
    const std::vector<u8>* StateFor(u32 frame) const;
    // Every PC, once it has loaded the snapshot of the resync at `frame`.
    void ResyncDone(u32 frame);

    // RAM hashes of every console after `frame` (every kHashPeriod frames).
    void SubmitHashes(u32 frame, const u64* hashes, int count);
    bool Desynced() const { return DesyncFrame >= 0; }
    s64 DesyncAt() const { return DesyncFrame; }

    // What happened (players leaving, coming back, resyncs), for the screen; false when none.
    bool PopNotice(std::string& out);

    // Testing: hold every outgoing packet this long before it is sent.
    void SetSimulatedLag(int ms) { LagMs = ms; }

    // Diagnostics.
    u64 WaitMs = 0;                     // time spent waiting for remote input

private:
    struct Outgoing
    {
        u64 DueMs;
        ENetPeer* Peer;                 // nullptr: every connected peer
        std::vector<u8> Data;
    };

    // Frames [first, second) of a slot run with no input (the player had left).
    using Range = std::pair<u32, u32>;

    void Fail(const std::string& why);
    void Notice(const std::string& text);
    void Send(ENetPeer* peer, std::vector<u8> data);       // peer nullptr: everyone
    void Flush();
    void Handle(ENetPeer* peer, const u8* data, size_t len);
    void HandleHost(ENetPeer* peer, const u8* data, size_t len);
    void HandleGuest(const u8* data, size_t len);
    void HostHello(ENetPeer* peer, const u8* data, size_t len);
    void HostDrop(int slot);
    void HostScheduleResync();
    void SendLobby();
    std::vector<u8> StartMessage(bool withSaves) const;
    void BeginRunning(StartInfo info);
    void StoreInput(int slot, u32 frame, const ConsoleInput& input);
    bool Neutral(int slot, u32 frame) const;
    void CompareHashes(u32 frame);
    int SlotOf(ENetPeer* peer) const;

    Phase CurPhase = Phase::Idle;
    bool IsHost = false;
    ENetHost* Net = nullptr;
    ENetPeer* HostPeer = nullptr;
    std::array<ENetPeer*, kMaxConsoles> Peers {};           // host: guest peers by slot
    std::array<bool, kMaxConsoles> SlotUsed {};
    std::array<std::string, kMaxConsoles> LobbyNames;
    std::array<std::vector<u8>, kMaxConsoles> LobbySaves;
    int MySlot = -1;
    std::string MyName;                                     // guest, until START
    std::vector<u8> MySave;
    RomHash Rom {};
    std::string ErrorText;
    StartInfo Info;

    std::array<std::map<u32, ConsoleInput>, kMaxConsoles> Inputs;
    std::array<bool, kMaxConsoles> Present {};
    std::array<std::vector<Range>, kMaxConsoles> NeutralRanges;
    std::array<s64, kMaxConsoles> NewestInput {};           // host: newest frame received per slot
    s64 NewestSubmitted = -1;                               // our own

    // resyncs
    std::deque<Resync> Resyncs;                             // announced, not carried out yet
    u8 NextTargets = 0;                                     // host: for the next one
    u8 NextJoins = 0;
    bool IsRejoin = false;                                  // guest: came back into a running session
    u32 StateFrame = 0;                                     // the snapshot we have (target / rejoin)
    std::vector<u8> StateData;
    bool HaveState = false;
    u32 LastResync = 0;                                     // hashes before this frame are not compared

    std::map<u32, std::vector<u64>> HostHashes;             // host: its own, by frame
    std::map<u32, std::array<std::vector<u64>, kMaxConsoles>> GuestHashes;
    s64 DesyncFrame = -1;

    std::deque<std::string> Notices;

    int LagMs = 0;
    std::deque<Outgoing> Delayed;
};

}

#endif
