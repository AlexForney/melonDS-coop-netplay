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

#ifndef MELONDS_NETPLAY_PROBE_H
#define MELONDS_NETPLAY_PROBE_H

#include "Platform.h"

namespace melonDS::Netplay
{
class Lockstep;
}

namespace Probe
{

// What each console's userdata points to.
struct ConsoleContext
{
    int ID = 0;
};

// The session the Platform layer routes the wireless link to.
melonDS::Netplay::Lockstep& Session();

extern melonDS::Platform::LogLevel LogThreshold;

}

#endif
