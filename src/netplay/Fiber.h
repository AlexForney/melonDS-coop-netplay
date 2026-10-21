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

// Cooperative fibers: several emulated consoles share one OS thread and switch only at points the
// lockstep scheduler chooses, so the order of everything they do is the same on every machine.

#ifndef MELONDS_NETPLAY_FIBER_H
#define MELONDS_NETPLAY_FIBER_H

#include <cstddef>

namespace melonDS::Netplay
{

struct Fiber;

// The calling thread as a fiber (once per thread, before any switch).
Fiber* Fiber_FromThread();

// A new fiber that runs func(arg) on its own stack when first switched to. func must never return.
Fiber* Fiber_Create(std::size_t stackSize, void (*func)(void*), void* arg);

// Suspend the current fiber and run `to`.
void Fiber_Switch(Fiber* to);

void Fiber_Free(Fiber* fiber);

}

#endif
