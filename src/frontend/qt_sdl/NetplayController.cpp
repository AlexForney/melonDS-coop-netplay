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

#include "NetplayController.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#include "EmuInstance.h"
#include "main.h"
#include "NDS.h"
#include "NDSCart.h"
#include "netplay/Consoles.h"

namespace NP = melonDS::Netplay;

// Every console of a session that is not this player's: their userdata, at fixed addresses so the
// Platform layer can tell them apart without a lock.
static NetplayController::MirrorContext MirrorContexts[NP::kMaxConsoles];
static std::atomic<NetplayController*> Active {nullptr};

NetplayController* NetplayController::Current()
{
    return Active.load();
}

bool NetplayController::IsMirror(void* userdata)
{
    auto* p = static_cast<MirrorContext*>(userdata);
    return p >= &MirrorContexts[0] && p < &MirrorContexts[NP::kMaxConsoles];
}

NP::Lockstep* NetplayController::LinkFor(void* userdata, int* slot)
{
    NetplayController* np = Active.load();
    if (!np || !np->IsRunning.load() || !np->Lock) return nullptr;
    if (IsMirror(userdata))
    {
        *slot = static_cast<MirrorContext*>(userdata)->Slot;
        return np->Lock.get();
    }
    if (userdata == np->Local)
    {
        *slot = np->NetSession->LocalSlot();
        return np->Lock.get();
    }
    return nullptr;
}

NetplayController* NetplayController::Create(EmuInstance* inst, std::string* error)
{
    if (Active.load())
    {
        *error = "A netplay session is already open.";
        return nullptr;
    }
    if (numEmuInstances() > 1)
    {
        *error = "Close the other emulator windows first: netplay runs every player's console "
                 "in this window.";
        return nullptr;
    }
    if (!inst->nds || !inst->nds->CartInserted() || inst->nds->ConsoleType != 0)
    {
        *error = "Load the DS ROM first (DS mode, not DSi).";
        return nullptr;
    }

    auto* np = new NetplayController(inst);
    const melonDS::NDSCart::CartCommon* cart = inst->nds->GetNDSCart();
    np->Rom.assign(cart->GetROM(), cart->GetROM() + cart->GetROMLength());
    np->RomName = inst->baseROMName;
    Active.store(np);
    return np;
}

void NetplayController::Destroy()
{
    NetplayController* np = Active.exchange(nullptr);
    delete np;
}

NetplayController::NetplayController(EmuInstance* inst) :
    Local(inst),
    NetSession(std::make_unique<NP::Session>())
{
}

NetplayController::~NetplayController()
{
    Lock.reset();
    for (auto& m : Mirrors) m.reset();
    NetSession.reset();
}

static std::vector<melonDS::u8> CurrentSave(melonDS::NDS* nds)
{
    const melonDS::u8* data = nds->GetNDSSave();
    melonDS::u32 len = nds->GetNDSSaveLength();
    if (!data || !len) return {};
    return std::vector<melonDS::u8>(data, data + len);
}

bool NetplayController::Host(int port, const std::string& name, const std::string& bindAddress)
{
    return NetSession->Host(port, name, NP::HashRom(Rom.data(), static_cast<melonDS::u32>(Rom.size())),
                            CurrentSave(Local->nds), bindAddress);
}

bool NetplayController::Join(const std::string& address, int port, const std::string& name)
{
    return NetSession->Join(address, port, name, NP::HashRom(Rom.data(), static_cast<melonDS::u32>(Rom.size())),
                            CurrentSave(Local->nds));
}

void NetplayController::PollLobby()
{
    if (!IsRunning.load()) NetSession->Poll(0);
}

bool NetplayController::StartGame(int inputDelay)
{
    // every console's clock starts at the host's local time
    std::time_t t = std::time(nullptr);
    std::tm now = *std::localtime(&t);
    return NetSession->StartGame(inputDelay, true, now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                                 now.tm_hour, now.tm_min, now.tm_sec);
}

void NetplayController::LoadTestHooks()
{
    Script.clear();
    HaveScript = false;
    if (const char* path = std::getenv("MELONDS_NETPLAY_SCRIPT"))
    {
        static const char* names[] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN",
                                      "R", "L", "X", "Y"};
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            auto hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            std::istringstream ls(line);
            ScriptEvent ev {};
            std::string keys;
            if (!(ls >> ev.Frame >> ev.Slot >> keys)) continue;
            if (!(ls >> ev.Hold)) ev.Hold = 4;
            std::istringstream ks(keys);
            std::string key;
            while (std::getline(ks, key, '+'))
                for (int i = 0; i < 12; i++)
                    if (key == names[i]) ev.Keys |= 1u << i;
            Script.push_back(ev);
        }
        HaveScript = true;
    }
    if (const char* path = std::getenv("MELONDS_NETPLAY_HASHES"))
        HashFile = std::fopen(path, "w");
}

bool NetplayController::Boot(std::string* error)
{
    const NP::StartInfo& info = NetSession->Start();
    int me = NetSession->LocalSlot();
    if (info.NumPlayers < 1 || me < 0 || me >= info.NumPlayers)
    {
        *error = "The session did not start properly.";
        return false;
    }

    Lock = std::make_unique<NP::Lockstep>();
    Consoles = {};
    for (int s = 0; s < info.NumPlayers; s++)
    {
        if (s == me)
        {
            NP::SetUpLocal(*Local->nds, info, s, RomName);
            Consoles[s] = Local->nds;
        }
        else
        {
            MirrorContexts[s].Slot = s;
            Mirrors[s] = NP::MakeMirror(info, s, Rom.data(), static_cast<melonDS::u32>(Rom.size()), RomName,
                                        &MirrorContexts[s]);
            if (!Mirrors[s])
            {
                *error = "Could not start the other players' consoles.";
                Lock.reset();
                return false;
            }
            Consoles[s] = Mirrors[s].get();
        }
        Lock->AddConsole(s, Consoles[s]);
    }

    Frame = 0;
    Submitted = false;
    DesyncShown = false;
    LoadTestHooks();
    BootTime = std::chrono::steady_clock::now();
    IsRunning.store(true);
    Local->osdAddMessage(0, "Netplay: %d players, you are P%d", info.NumPlayers, me + 1);
    return true;
}

bool NetplayController::Step()
{
    if (!IsRunning.load()) return false;
    const NP::StartInfo& info = NetSession->Start();
    int me = NetSession->LocalSlot();

    if (!Submitted)
    {
        NP::ConsoleInput in;
        melonDS::u32 target = Frame + info.InputDelay;
        if (HaveScript)
        {
            for (const ScriptEvent& ev : Script)
                if (ev.Slot == me && target >= ev.Frame && target < ev.Frame + ev.Hold)
                    in.KeyMask &= ~ev.Keys;
        }
        else
        {
            in.KeyMask = Local->inputMask;
            in.Touching = Local->isTouching;
            in.TouchX = Local->touchX;
            in.TouchY = Local->touchY;
        }
        NetSession->SubmitInput(target, in);
        Submitted = true;
        FrameAsked = std::chrono::steady_clock::now();
    }

    // wait a little for the others' input; the caller comes back if it is not there yet
    auto give_up = std::chrono::steady_clock::now() + std::chrono::milliseconds(15);
    NetSession->Poll(0);
    while (!NetSession->HaveInputs(Frame))
    {
        if (NetSession->GetPhase() == NP::Session::Phase::Ended)
        {
            Stop(NetSession->Error());
            return false;
        }
        if (std::chrono::steady_clock::now() >= give_up) return false;
        NetSession->Poll(1);
    }

    NP::ConsoleInput inputs[NP::kMaxConsoles];
    NetSession->TakeInputs(Frame, inputs);
    Lock->RunFrame(inputs);
    Submitted = false;

    if ((Frame + 1) % NP::kHashPeriod == 0 || HashFile)
    {
        melonDS::u64 hashes[NP::kMaxConsoles];
        for (int s = 0; s < info.NumPlayers; s++)
            hashes[s] = NP::HashConsole(*Consoles[s]);
        if ((Frame + 1) % NP::kHashPeriod == 0)
            NetSession->SubmitHashes(Frame, hashes, info.NumPlayers);
        if (HashFile)
        {
            auto now = std::chrono::steady_clock::now();
            long long sinceBoot = std::chrono::duration_cast<std::chrono::milliseconds>(now - BootTime).count();
            long long waited = std::chrono::duration_cast<std::chrono::milliseconds>(now - FrameAsked).count();
            std::fprintf(HashFile, "%u", Frame);
            for (int s = 0; s < info.NumPlayers; s++)
                std::fprintf(HashFile, " %016llx", static_cast<unsigned long long>(hashes[s]));
            std::fprintf(HashFile, " t=%lld w=%lld", sinceBoot, waited);
            std::fprintf(HashFile, "\n");
            if ((Frame + 1) % 60 == 0) std::fflush(HashFile);
        }
    }
    if (NetSession->Desynced() && !DesyncShown)
    {
        DesyncShown = true;
        Local->osdAddMessage(0xFFA000, "Netplay: the consoles went out of sync at frame %lld",
                             static_cast<long long>(NetSession->DesyncAt()));
    }

    Frame++;
    (void)me;
    return true;
}

void NetplayController::Stop(const std::string& why)
{
    if (!IsRunning.exchange(false)) return;
    if (HashFile)
    {
        std::fclose(HashFile);
        HashFile = nullptr;
    }
    Lock.reset();
    for (auto& m : Mirrors) m.reset();
    Consoles = {};
    NetSession->Leave();
    Local->osdAddMessage(0xFF4040, "Netplay ended: %s", why.empty() ? "left" : why.c_str());
}
