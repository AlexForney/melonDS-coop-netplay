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

// Netplay in the frontend (src/netplay: Session, Lockstep, Consoles). One session per process, on
// one emulator window: that window's console is this player's, and every other player's console
// runs here too as a hidden, silent mirror. All of them run on the window's emulation thread, one
// lockstep frame at a time, once every player's input for that frame has arrived.
//
//   UI thread:   Create, Host/Join, PollLobby, StartGame (the netplay dialog)
//   emu thread:  Boot (msg_NetplayStart), Step every frame, Stop
//   any thread:  the Platform layer routes a console's wireless link and callbacks through
//                LinkFor / IsMirror

#ifndef NETPLAYCONTROLLER_H
#define NETPLAYCONTROLLER_H

#include <array>
#include <chrono>
#include <cstdio>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "types.h"
#include "netplay/Lockstep.h"
#include "netplay/Session.h"

class EmuInstance;

namespace melonDS
{
class NDS;
}

class NetplayController
{
public:
    struct MirrorContext
    {
        int Slot = -1;
    };

    // The session of this process, nullptr when none.
    static NetplayController* Current();
    // A mirror console's userdata (its Platform callbacks do nothing).
    static bool IsMirror(void* userdata);
    // The lockstep link a console's wireless traffic goes through, nullptr when not in a running
    // session; `slot` gets the console's slot.
    static melonDS::Netplay::Lockstep* LinkFor(void* userdata, int* slot);

    // A session on `inst`, which must have a DS ROM loaded and be the only emulator window.
    static NetplayController* Create(EmuInstance* inst, std::string* error);
    static void Destroy();

    bool Host(int port, const std::string& name, const std::string& bindAddress = "");
    bool Join(const std::string& address, int port, const std::string& name);

    EmuInstance* Instance() const { return Local; }
    melonDS::Netplay::Session& Net() { return *NetSession; }

    // UI thread, before the game.
    void PollLobby();
    bool StartGame(int inputDelay);

    // Emulation thread.
    bool Boot(std::string* error);
    bool Running() const { return IsRunning.load(); }
    bool Step();                        // one frame of every console; false: still waiting
    void Stop(const std::string& why);

    melonDS::u32 FrameNumber() const { return Frame; }

private:
    explicit NetplayController(EmuInstance* inst);
    ~NetplayController();

    EmuInstance* Local;
    std::unique_ptr<melonDS::Netplay::Session> NetSession;
    std::unique_ptr<melonDS::Netplay::Lockstep> Lock;
    std::array<std::unique_ptr<melonDS::NDS>, melonDS::Netplay::kMaxConsoles> Mirrors;
    std::array<melonDS::NDS*, melonDS::Netplay::kMaxConsoles> Consoles {};
    std::vector<melonDS::u8> Rom;
    std::string RomName;
    std::atomic<bool> IsRunning {false};
    melonDS::u32 Frame = 0;
    bool Submitted = false;
    std::chrono::steady_clock::time_point BootTime;
    std::chrono::steady_clock::time_point FrameAsked;    // when this frame's input went out
    std::chrono::steady_clock::time_point LastWaitNote;  // the last "waiting for" message

    // The resync due before this frame; false while its snapshot has not arrived.
    bool DoResync(const melonDS::Netplay::Resync& rs);
    void ShowNotices();
    void NoteWaiting();

    // Tests (environment): MELONDS_NETPLAY_SCRIPT replaces this player's input with a frame script
    // ("<frame> <slot> <keys> [<held>]", as melonDS-netprobe's); MELONDS_NETPLAY_HASHES writes
    // every console's RAM hash every frame, to compare the PCs afterwards.
    struct ScriptEvent
    {
        melonDS::u32 Frame;
        int Slot;
        melonDS::u32 Keys;
        melonDS::u32 Hold;
    };
    std::vector<ScriptEvent> Script;
    bool HaveScript = false;
    std::FILE* HashFile = nullptr;
    // MELONDS_NETPLAY_CORRUPT=<frame>: after that frame, change a byte of this player's console
    // (a desync on purpose, to test the repair)
    melonDS::s64 CorruptAt = -1;
    void LoadTestHooks();
};

#endif
