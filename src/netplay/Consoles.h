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

// How every PC of a session sets up each console, so that console k is the same machine on all
// of them: free BIOS, generated firmware with slot k's MAC address (as the frontend makes it for
// instance k) and player name, the session's JIT setting with default options, no audio
// interpolation, the software 3D renderer, the session's clock, direct boot, player k's save.

#ifndef MELONDS_NETPLAY_CONSOLES_H
#define MELONDS_NETPLAY_CONSOLES_H

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "types.h"
#include "Args.h"
#include "SPI_Firmware.h"
#include "Session.h"

namespace melonDS
{
class NDS;
}

namespace melonDS::Netplay
{

// The software 3D renderer on its own thread (default): no effect on the game, only on speed.
extern bool Threaded3D;

Firmware MakeFirmware(int slot, const std::string& name);
std::optional<JITArgs> SessionJIT(bool enabled);

// A mirror console of `slot`: built, cart inserted with the slot's save, booted.
std::unique_ptr<NDS> MakeMirror(const StartInfo& info, int slot, const u8* rom, u32 romLength,
                                const std::string& romName, void* userdata);

// An existing console (the local player's) set up as slot `slot` and booted. Its cart stays.
void SetUpLocal(NDS& nds, const StartInfo& info, int slot, const std::string& romName);

u64 HashConsole(NDS& nds);
RomHash HashRom(const u8* rom, u32 length);

// Every console of a session and their link, between two frames, zstd-compressed: what a player
// who rejoins, or a PC that went out of sync, loads to carry on from the host's frame.
// `consoles` is indexed by slot.
bool SaveSnapshot(NDS* const* consoles, int count, const Lockstep& link, std::vector<u8>& out);
bool LoadSnapshot(NDS* const* consoles, int count, Lockstep& link, const u8* data, std::size_t len);

}

#endif
