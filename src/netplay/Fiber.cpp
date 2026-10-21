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

#include "Fiber.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <ucontext.h>
#include <cstdlib>
#endif

namespace melonDS::Netplay
{

#ifdef _WIN32

struct Fiber
{
    LPVOID Handle;
    bool OwnsHandle;
    void (*Func)(void*);
    void* Arg;
};

static void CALLBACK FiberEntry(LPVOID param)
{
    Fiber* fiber = static_cast<Fiber*>(param);
    fiber->Func(fiber->Arg);
}

Fiber* Fiber_FromThread()
{
    Fiber* fiber = new Fiber{};
    fiber->Handle = ConvertThreadToFiber(nullptr);
    if (!fiber->Handle)
        fiber->Handle = GetCurrentFiber();    // already a fiber
    fiber->OwnsHandle = false;
    return fiber;
}

Fiber* Fiber_Create(std::size_t stackSize, void (*func)(void*), void* arg)
{
    Fiber* fiber = new Fiber{};
    fiber->Func = func;
    fiber->Arg = arg;
    fiber->Handle = CreateFiber(stackSize, FiberEntry, fiber);
    fiber->OwnsHandle = true;
    if (!fiber->Handle)
    {
        delete fiber;
        return nullptr;
    }
    return fiber;
}

void Fiber_Switch(Fiber* to)
{
    SwitchToFiber(to->Handle);
}

void Fiber_Free(Fiber* fiber)
{
    if (!fiber) return;
    if (fiber->OwnsHandle && fiber->Handle)
        DeleteFiber(fiber->Handle);
    delete fiber;
}

#else

struct Fiber
{
    ucontext_t Context;
    void* Stack;
    void (*Func)(void*);
    void* Arg;
};

static thread_local Fiber* CurrentFiber = nullptr;

static void FiberEntry(unsigned int hi, unsigned int lo)
{
    Fiber* fiber = reinterpret_cast<Fiber*>((static_cast<unsigned long long>(hi) << 32) | lo);
    fiber->Func(fiber->Arg);
}

Fiber* Fiber_FromThread()
{
    Fiber* fiber = new Fiber{};
    getcontext(&fiber->Context);
    CurrentFiber = fiber;
    return fiber;
}

Fiber* Fiber_Create(std::size_t stackSize, void (*func)(void*), void* arg)
{
    Fiber* fiber = new Fiber{};
    fiber->Func = func;
    fiber->Arg = arg;
    fiber->Stack = std::malloc(stackSize);
    if (!fiber->Stack)
    {
        delete fiber;
        return nullptr;
    }
    getcontext(&fiber->Context);
    fiber->Context.uc_stack.ss_sp = fiber->Stack;
    fiber->Context.uc_stack.ss_size = stackSize;
    fiber->Context.uc_link = nullptr;
    unsigned long long p = reinterpret_cast<unsigned long long>(fiber);
    makecontext(&fiber->Context, reinterpret_cast<void (*)()>(FiberEntry), 2,
                static_cast<unsigned int>(p >> 32), static_cast<unsigned int>(p & 0xFFFFFFFF));
    return fiber;
}

void Fiber_Switch(Fiber* to)
{
    Fiber* from = CurrentFiber;
    CurrentFiber = to;
    swapcontext(&from->Context, &to->Context);
}

void Fiber_Free(Fiber* fiber)
{
    if (!fiber) return;
    std::free(fiber->Stack);
    delete fiber;
}

#endif

}
