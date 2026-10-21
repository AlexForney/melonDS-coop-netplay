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

// Lockstep: every console of a netplay session runs on one OS thread, each in its own fiber, and
// their local wireless link (the DS "MP" exchange) is delivered in an order that depends only on
// what the consoles do, never on wall-clock time. Same inputs in, same consoles out, on any PC.
//
// The delivery rules are LocalMP's (src/net/LocalMP.cpp): one packet FIFO shared by all consoles
// (regular frames, MP commands and acks), one reply FIFO read by the MP host, each console reading
// from where it was when it started its wireless link. Where LocalMP blocks on a semaphore with a
// real-time timeout (RecvTimeout), a console here suspends its fiber and the scheduler runs the
// others; a wait ends when its packet or replies arrive, or, when every unfinished console is
// waiting, the next waiting console in a fixed order gets "nothing" (the timeout) and goes on.

#ifndef MELONDS_NETPLAY_LOCKSTEP_H
#define MELONDS_NETPLAY_LOCKSTEP_H

#include <cstdint>
#include <deque>
#include <vector>
#include "types.h"

namespace melonDS
{
class NDS;
}

namespace melonDS::Netplay
{

struct Fiber;

constexpr int kMaxConsoles = 4;

struct ConsoleInput
{
    u32 KeyMask = 0xFFF;                // active low, as NDS::SetKeyMask takes it
    bool Touching = false;
    u16 TouchX = 0;
    u16 TouchY = 0;
};

class Lockstep
{
public:
    Lockstep();
    ~Lockstep();
    Lockstep(const Lockstep&) = delete;
    Lockstep& operator=(const Lockstep&) = delete;

    // A console of the session; `id` is the instance id its Platform::MP_* calls carry.
    // All consoles are added before the first frame.
    void AddConsole(int id, NDS* nds);

    // One video frame of every console with these inputs (indexed by id).
    void RunFrame(const ConsoleInput* inputs);

    // The link's own state between two frames (packets in flight, where each console reads), to
    // go with the consoles' savestates in a snapshot. Load into a link with the same consoles.
    std::vector<u8> SaveState() const;
    bool LoadState(const u8* data, std::size_t len);

    // The wireless link, for Platform::MP_* (called on the console's own fiber).
    void Begin(int id);
    void End(int id);
    int SendPacket(int id, u32 type, const u8* data, int len, u64 timestamp);
    int RecvPacket(int id, u8* data, u64* timestamp, bool block);
    u16 RecvReplies(int id, u8* data, u64 timestamp, u16 aidmask);

    // Diagnostics.
    u64 Switches = 0;                   // fiber switches into consoles
    u64 Timeouts = 0;                   // waits ended because everyone was waiting

private:
    enum class State { Idle, Ready, Running, Waiting, FrameDone };
    enum class Wait { None, Packet, Replies };

    struct Packet
    {
        int Sender;
        u32 Type;                       // 0 regular, 1 MP command, 2 reply (aid << 16), 3 ack
        u64 Timestamp;
        std::vector<u8> Data;
    };

    struct Console
    {
        Lockstep* Owner = nullptr;
        int ID = -1;
        NDS* Nds = nullptr;
        Fiber* Context = nullptr;           // its fiber
        State St = State::Idle;
        Wait W = Wait::None;
        bool TimedOut = false;
        ConsoleInput Input;
        u64 PacketRead = 0;             // next packet sequence number to read
        u64 ReplyRead = 0;
    };

    static void ConsoleMain(void* arg);
    void Suspend(Console& c, Wait w);
    bool Satisfied(const Console& c) const;
    bool HasPacketFor(const Console& c) const;
    void Trim();
    Console* Find(int id);

    std::vector<Console*> Consoles;     // in id order: the schedule's order
    Fiber* MainFiber = nullptr;
    Console* Current = nullptr;

    std::deque<Packet> Packets;
    u64 PacketBase = 0;                 // sequence number of Packets.front()
    std::deque<Packet> Replies;
    u64 ReplyBase = 0;

    u16 Connected = 0;                  // consoles whose wireless link is on
    int HostID = -1;                    // the console that sent the last MP command
    int LastHostID = -1;                // ... as the receiving side saw it (LocalMP::LastHostID)
};

}

#endif
