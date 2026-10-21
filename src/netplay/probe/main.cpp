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

// melonDS-netprobe: lockstep netplay without a window.
//
// Local mode: boots N consoles of a ROM (free BIOS, generated firmware with per-console MAC
// addresses as the frontend makes them, direct boot, a fixed clock), runs them through the
// Lockstep scheduler with a scripted input, hashes every console's main RAM every frame,
// repeats the whole run, and reports the first frame where two runs differ.
//
// Net mode (--host / --join): one process per player, over UDP. Each process scripts only its own
// player's console; the others' inputs come through the Session. Compare the processes' --hashes
// files afterwards: identical means every PC computed the same game.
//
// Script lines: "<frame> <console> <keys> [<frames held>]", keys joined with '+'
// (A B SELECT START RIGHT LEFT UP DOWN R L X Y), e.g. "900 0 START 4". '#' starts a comment.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "NDS.h"
#include "NDSCart.h"
#include "Args.h"
#include "SPI_Firmware.h"
#include "../Consoles.h"
#include "../Lockstep.h"
#include "../Session.h"
#include "Probe.h"


using namespace melonDS;
using melonDS::Netplay::ConsoleInput;
using melonDS::Netplay::Lockstep;
using melonDS::Netplay::Session;

namespace Probe
{
static std::unique_ptr<Lockstep> CurrentSession;
Lockstep& Session() { return *CurrentSession; }
melonDS::Platform::LogLevel LogThreshold = melonDS::Platform::Warn;
}

struct ScriptEvent
{
    u32 Frame;
    int Console;
    u32 Keys;                           // bits pressed (active high here)
    u32 Hold;
};

struct Options
{
    std::string Rom;
    std::string Script;
    std::string HashOut;
    std::string ShotPrefix;
    std::vector<u32> ShotFrames;
    std::vector<std::string> Saves;     // per console (local mode); [0] is our own (net mode)
    int Consoles = 2;
    u32 Frames = 600;
    u32 From = 0;                       // also time the frames from here on
    u32 SnapshotAt = 0;                 // run 0 saves a snapshot here; later runs load it and start here
    int Runs = 2;
    bool Jit = true;
    bool Realtime = false;              // pace frames at the DS's 59.83 fps

    // net mode
    bool NetHost = false;
    std::string JoinAddress;
    std::string Bind;
    int Port = Netplay::kDefaultPort;
    std::string Name = "PLAYER";
    int Delay = 4;
    int LagMs = 0;
};

static u32 KeyBit(const std::string& name)
{
    static const char* names[] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN",
                                  "R", "L", "X", "Y"};
    for (u32 i = 0; i < 12; i++)
        if (name == names[i]) return 1u << i;
    std::fprintf(stderr, "unknown key %s\n", name.c_str());
    std::exit(2);
}

static std::vector<ScriptEvent> LoadScript(const std::string& path)
{
    std::vector<ScriptEvent> events;
    if (path.empty()) return events;
    std::ifstream in(path);
    if (!in)
    {
        std::fprintf(stderr, "cannot read script %s\n", path.c_str());
        std::exit(2);
    }
    std::string line;
    while (std::getline(in, line))
    {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        ScriptEvent ev{};
        std::string keys;
        if (!(ls >> ev.Frame >> ev.Console >> keys)) continue;
        if (!(ls >> ev.Hold)) ev.Hold = 4;
        std::istringstream ks(keys);
        std::string key;
        while (std::getline(ks, key, '+'))
            ev.Keys |= KeyBit(key);
        events.push_back(ev);
    }
    return events;
}

static ConsoleInput ScriptInput(const std::vector<ScriptEvent>& script, int console, u32 frame)
{
    ConsoleInput in;
    for (const ScriptEvent& ev : script)
    {
        if (ev.Console == console && frame >= ev.Frame && frame < ev.Frame + ev.Hold)
            in.KeyMask &= ~ev.Keys;
    }
    return in;
}

static std::vector<u8> ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string BaseName(const std::string& path)
{
    auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static void WriteShot(const std::string& path, NDS& nds)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n256 384\n255\n");
    int front = nds.GPU.FrontBuffer;
    for (int screen = 0; screen < 2; screen++)
    {
        const u32* fb = nds.GPU.Framebuffer[front][screen].get();
        for (int i = 0; i < 256 * 192; i++)
        {
            u32 px = fb ? fb[i] : 0;
            u8 rgb[3] = {static_cast<u8>((px >> 16) & 0xFF), static_cast<u8>((px >> 8) & 0xFF),
                         static_cast<u8>(px & 0xFF)};
            std::fwrite(rgb, 1, 3, f);
        }
    }
    std::fclose(f);
}

static std::vector<u8> ProbeSnapshot;

using InputSource = std::function<bool(u32 frame, ConsoleInput* inputs)>;
using FrameHook = std::function<void(u32 frame, const u64* hashes, int count)>;

// One full run: hashes[frame * consoles + console]; empty if the input source gave up.
static std::vector<u64> Run(const Options& opt, const Netplay::StartInfo& setup,
                            const std::vector<u8>& rom, const InputSource& inputsFor,
                            const FrameHook& afterFrame, int runIndex)
{
    Probe::CurrentSession = std::make_unique<Lockstep>();
    std::vector<Probe::ConsoleContext> ctx(setup.NumPlayers);
    std::vector<std::unique_ptr<NDS>> consoles;
    std::string romName = BaseName(opt.Rom);

    for (int i = 0; i < setup.NumPlayers; i++)
    {
        ctx[i].ID = i;
        auto nds = Netplay::MakeMirror(setup, i, rom.data(), static_cast<u32>(rom.size()), romName,
                                       &ctx[i]);
        if (!nds)
        {
            std::fprintf(stderr, "the ROM did not parse\n");
            std::exit(1);
        }
        Probe::Session().AddConsole(i, nds.get());
        consoles.push_back(std::move(nds));
    }

    std::vector<u64> hashes(static_cast<size_t>(opt.Frames) * setup.NumPlayers);
    ConsoleInput inputs[Netplay::kMaxConsoles];
    std::vector<NDS*> ptrs;
    for (auto& c : consoles) ptrs.push_back(c.get());

    u32 first = 0;
    if (opt.SnapshotAt && runIndex > 0)
    {
        // fresh consoles, as a rejoining PC boots them, then the snapshot
        auto t0 = std::chrono::steady_clock::now();
        if (!Netplay::LoadSnapshot(ptrs.data(), setup.NumPlayers, Probe::Session(), ProbeSnapshot.data(),
                                   ProbeSnapshot.size()))
        {
            std::fprintf(stderr, "run %d: the snapshot did not load\n", runIndex);
            std::exit(1);
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("run %d: loaded the snapshot of frame %u in %.0f ms\n", runIndex, opt.SnapshotAt, ms);
        if (const char* dump = std::getenv("NETPROBE_SNAPDUMP"))
        {
            // the snapshot as loaded, and saved again at once: any difference is state the load
            // did not restore
            std::vector<u8> again;
            Netplay::SaveSnapshot(ptrs.data(), setup.NumPlayers, Probe::Session(), again);
            for (int k = 0; k < 2; k++)
            {
                std::string path = std::string(dump) + (k ? "_again.zst" : "_saved.zst");
                const std::vector<u8>& blob = k ? again : ProbeSnapshot;
                if (std::FILE* f = std::fopen(path.c_str(), "wb"))
                {
                    std::fwrite(blob.data(), 1, blob.size(), f);
                    std::fclose(f);
                }
            }
        }
        first = opt.SnapshotAt;
    }

    auto start = std::chrono::steady_clock::now();
    auto fromStart = start;

    for (u32 frame = first; frame < opt.Frames; frame++)
    {
        if (opt.SnapshotAt && runIndex == 0 && frame == opt.SnapshotAt)
        {
            auto t0 = std::chrono::steady_clock::now();
            if (!Netplay::SaveSnapshot(ptrs.data(), setup.NumPlayers, Probe::Session(), ProbeSnapshot))
            {
                std::fprintf(stderr, "the snapshot failed\n");
                std::exit(1);
            }
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("run 0: snapshot of frame %u: %zu bytes in %.0f ms\n", frame, ProbeSnapshot.size(), ms);
            // what every PC already running does at a resync: load the snapshot back (the JIT
            // starts from an empty cache there, as on a PC that boots from the snapshot)
            if (!Netplay::LoadSnapshot(ptrs.data(), setup.NumPlayers, Probe::Session(), ProbeSnapshot.data(),
                                       ProbeSnapshot.size()))
            {
                std::fprintf(stderr, "run 0: the snapshot did not load back\n");
                std::exit(1);
            }
        }
        if (frame == opt.From) fromStart = std::chrono::steady_clock::now();
        for (int i = 0; i < Netplay::kMaxConsoles; i++)
            inputs[i] = ConsoleInput{};
        if (!inputsFor(frame, inputs))
        {
            hashes.clear();
            break;
        }

        Probe::Session().RunFrame(inputs);

        if (opt.Realtime)
        {
            auto due = start + std::chrono::nanoseconds(static_cast<long long>((frame + 1) * 16715000.0));
            std::this_thread::sleep_until(due);
        }

        u64* row = &hashes[static_cast<size_t>(frame) * setup.NumPlayers];
        for (int i = 0; i < setup.NumPlayers; i++)
        {
            row[i] = Netplay::HashConsole(*consoles[i]);
        }
        if (afterFrame) afterFrame(frame, row, setup.NumPlayers);

        if (runIndex == 0 && !opt.ShotPrefix.empty())
        {
            for (u32 shot : opt.ShotFrames)
            {
                if (shot != frame) continue;
                for (int i = 0; i < setup.NumPlayers; i++)
                {
                    char path[512];
                    std::snprintf(path, sizeof path, "%s_f%u_c%d.ppm", opt.ShotPrefix.c_str(),
                                  frame, i);
                    WriteShot(path, *consoles[i]);
                }
            }
        }
    }

    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("run %d: %u frames x %d consoles in %.1f s (%.1f fps, %.0f%% speed), "
                "%llu switches, %llu timeouts\n",
                runIndex, opt.Frames, setup.NumPlayers, secs, opt.Frames / secs,
                100.0 * opt.Frames / secs / 59.8261,
                static_cast<unsigned long long>(Probe::Session().Switches),
                static_cast<unsigned long long>(Probe::Session().Timeouts));
    if (opt.From > 0 && opt.From < opt.Frames)
    {
        double part = std::chrono::duration<double>(std::chrono::steady_clock::now() - fromStart).count();
        u32 n = opt.Frames - opt.From;
        std::printf("run %d: frames %u-%u: %.1f fps, %.0f%% speed\n", runIndex, opt.From,
                    opt.Frames - 1, n / part, 100.0 * n / part / 59.8261);
    }
    std::fflush(stdout);

    consoles.clear();
    Probe::CurrentSession.reset();
    return hashes;
}

static void WriteHashes(const std::string& path, const std::vector<u64>& hashes, int consoles, u32 frames)
{
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    for (u32 frame = 0; frame < frames && (static_cast<size_t>(frame) + 1) * consoles <= hashes.size(); frame++)
    {
        std::fprintf(f, "%u", frame);
        for (int i = 0; i < consoles; i++)
            std::fprintf(f, " %016llx", static_cast<unsigned long long>(
                hashes[static_cast<size_t>(frame) * consoles + i]));
        std::fprintf(f, "\n");
    }
    std::fclose(f);
}

static int RunLocal(const Options& opt, const std::vector<u8>& rom, const std::vector<ScriptEvent>& script)
{
    Netplay::StartInfo setup;
    setup.NumPlayers = opt.Consoles;
    setup.JIT = opt.Jit;
    for (int i = 0; i < opt.Consoles; i++)
    {
        setup.Names[i] = "PLAYER" + std::to_string(i + 1);
        if (i < static_cast<int>(opt.Saves.size()) && !opt.Saves[i].empty())
            setup.Saves[i] = ReadFile(opt.Saves[i]);
    }

    auto inputs = [&](u32 frame, ConsoleInput* out) {
        for (int i = 0; i < opt.Consoles; i++) out[i] = ScriptInput(script, i, frame);
        return true;
    };

    std::vector<std::vector<u64>> runs;
    for (int r = 0; r < opt.Runs; r++)
        runs.push_back(Run(opt, setup, rom, inputs, nullptr, r));

    if (!opt.HashOut.empty()) WriteHashes(opt.HashOut, runs[0], opt.Consoles, opt.Frames);

    int status = 0;
    for (int r = 1; r < opt.Runs; r++)
    {
        bool same = true;
        size_t firstK = opt.SnapshotAt && r > 0 ? static_cast<size_t>(opt.SnapshotAt) * opt.Consoles : 0;
        for (size_t k = firstK; k < runs[0].size() && same; k++)
        {
            if (runs[0][k] != runs[r][k])
            {
                std::printf("run %d differs from run 0 at frame %zu, console %zu\n", r,
                            k / opt.Consoles, k % opt.Consoles);
                same = false;
                status = 1;
            }
        }
        if (same) std::printf("run %d matches run 0 on every frame and console\n", r);
    }
    return status;
}

static int RunNet(const Options& opt, const std::vector<u8>& rom, const std::vector<ScriptEvent>& script)
{
    Netplay::RomHash romHash = Netplay::HashRom(rom.data(), static_cast<u32>(rom.size()));

    std::vector<u8> mySave;
    if (!opt.Saves.empty() && !opt.Saves[0].empty()) mySave = ReadFile(opt.Saves[0]);

    Session net;
    net.SetSimulatedLag(opt.LagMs);
    bool ok = opt.NetHost ? net.Host(opt.Port, opt.Name, romHash, std::move(mySave), opt.Bind)
                          : net.Join(opt.JoinAddress, opt.Port, opt.Name, romHash, std::move(mySave));
    if (!ok)
    {
        std::fprintf(stderr, "netplay: %s\n", net.Error().c_str());
        return 1;
    }

    // lobby
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (net.GetPhase() != Session::Phase::Running)
    {
        net.Poll(10);
        if (net.GetPhase() == Session::Phase::Ended)
        {
            std::fprintf(stderr, "netplay: %s\n", net.Error().c_str());
            return 1;
        }
        if (opt.NetHost && net.LobbyCount() >= opt.Consoles)
        {
            // let the guests' saves arrive (they follow WELCOME)
            for (int i = 0; i < 50; i++) net.Poll(10);
            if (!net.StartGame(opt.Delay, opt.Jit, 2026, 1, 1, 12, 0, 0))
            {
                std::fprintf(stderr, "netplay: %s\n", net.Error().c_str());
                return 1;
            }
        }
        if (std::chrono::steady_clock::now() > deadline)
        {
            std::fprintf(stderr, "netplay: nobody came\n");
            return 1;
        }
    }

    const Netplay::StartInfo& info = net.Start();
    int me = net.LocalSlot();
    std::printf("session: %d players, I am P%d, input delay %d\n", info.NumPlayers, me + 1,
                info.InputDelay);

    const Netplay::StartInfo& setup = info;

    auto inputs = [&](u32 frame, ConsoleInput* out) {
        // our input for the frame `delay` ahead, then every player's for this one
        net.SubmitInput(frame + info.InputDelay, ScriptInput(script, me, frame + info.InputDelay));
        auto waitStart = std::chrono::steady_clock::now();
        while (!net.HaveInputs(frame))
        {
            net.Poll(1);
            if (net.GetPhase() == Session::Phase::Ended)
            {
                std::fprintf(stderr, "netplay: %s\n", net.Error().c_str());
                return false;
            }
        }
        net.WaitMs += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - waitStart).count();
        net.TakeInputs(frame, out);
        net.Poll(0);
        return true;
    };
    auto afterFrame = [&](u32 frame, const u64* hashes, int count) {
        if ((frame + 1) % Netplay::kHashPeriod == 0) net.SubmitHashes(frame, hashes, count);
    };

    std::vector<u64> hashes = Run(opt, setup, rom, inputs, afterFrame, 0);

    // let the last hashes and inputs go out
    for (int i = 0; i < 100; i++) net.Poll(10);
    std::printf("waited %llu ms for remote input; %s\n",
                static_cast<unsigned long long>(net.WaitMs),
                net.Desynced() ? ("DESYNC at frame " + std::to_string(net.DesyncAt())).c_str()
                               : "no desync reported");
    if (!opt.HashOut.empty()) WriteHashes(opt.HashOut, hashes, setup.NumPlayers, opt.Frames);
    net.Leave();
    return hashes.empty() || net.Desynced() ? 1 : 0;
}

static void Usage()
{
    std::fprintf(stderr,
        "usage: melonDS-netprobe --rom FILE [--consoles N] [--frames N] [--runs N] [--from F]\n"
        "       [--snapshot F]  (run 0 saves every console at F, later runs load it there)\n"
        "       [--script FILE] [--save I=FILE]... [--hashes OUT] [--nojit]\n"
        "       [--shot PREFIX --shot-frame F]...\n"
        "   net: --host [--port P] --consoles N | --join ADDR [--port P]\n"
        "        [--name NAME] [--delay FRAMES] [--lag MS] [--save 0=OWN_SAVE]\n");
    std::exit(2);
}

int main(int argc, char** argv)
{
    Options opt;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) Usage();
            return argv[++i];
        };
        if (a == "--rom") opt.Rom = next();
        else if (a == "--consoles") opt.Consoles = std::atoi(next().c_str());
        else if (a == "--frames") opt.Frames = static_cast<u32>(std::atol(next().c_str()));
        else if (a == "--runs") opt.Runs = std::atoi(next().c_str());
        else if (a == "--from") opt.From = static_cast<u32>(std::atol(next().c_str()));
        else if (a == "--snapshot") opt.SnapshotAt = static_cast<u32>(std::atol(next().c_str()));
        else if (a == "--script") opt.Script = next();
        else if (a == "--hashes") opt.HashOut = next();
        else if (a == "--nojit") opt.Jit = false;
        else if (a == "--realtime") opt.Realtime = true;
        else if (a == "--shot") opt.ShotPrefix = next();
        else if (a == "--shot-frame") opt.ShotFrames.push_back(static_cast<u32>(std::atol(next().c_str())));
        else if (a == "--host") opt.NetHost = true;
        else if (a == "--join") opt.JoinAddress = next();
        else if (a == "--port") opt.Port = std::atoi(next().c_str());
        else if (a == "--bind") opt.Bind = next();
        else if (a == "--name") opt.Name = next();
        else if (a == "--delay") opt.Delay = std::atoi(next().c_str());
        else if (a == "--lag") opt.LagMs = std::atoi(next().c_str());
        else if (a == "--save")
        {
            std::string v = next();
            auto eq = v.find('=');
            if (eq == std::string::npos) Usage();
            int idx = std::atoi(v.substr(0, eq).c_str());
            if (idx < 0 || idx >= Netplay::kMaxConsoles) Usage();
            if (static_cast<int>(opt.Saves.size()) <= idx) opt.Saves.resize(idx + 1);
            opt.Saves[idx] = v.substr(eq + 1);
        }
        else if (a == "--verbose") Probe::LogThreshold = melonDS::Platform::Debug;
        else Usage();
    }
    if (opt.Rom.empty() || opt.Consoles < 1 || opt.Consoles > Netplay::kMaxConsoles) Usage();

    std::vector<u8> rom = ReadFile(opt.Rom);
    if (rom.empty())
    {
        std::fprintf(stderr, "cannot read %s\n", opt.Rom.c_str());
        return 1;
    }
    std::vector<ScriptEvent> script = LoadScript(opt.Script);

    if (opt.NetHost || !opt.JoinAddress.empty())
        return RunNet(opt, rom, script);
    return RunLocal(opt, rom, script);
}
