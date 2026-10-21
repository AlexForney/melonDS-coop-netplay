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

// melonDS-netprobe: does lockstep make linked consoles deterministic?
//
// Boots N consoles of a ROM (free BIOS, generated firmware with per-console MAC addresses as the
// frontend makes them, direct boot, a fixed clock), runs them through the Lockstep scheduler with
// a scripted input, hashes every console's main RAM every frame, repeats the whole run, and
// reports the first frame where two runs differ. Hashes can be written out to compare runs made
// in separate processes (or on separate machines).
//
// Script lines: "<frame> <console> <keys> [<frames held>]", keys joined with '+'
// (A B SELECT START RIGHT LEFT UP DOWN R L X Y), e.g. "900 0 START 4". '#' starts a comment.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "NDS.h"
#include "NDSCart.h"
#include "Args.h"
#include "SPI_Firmware.h"
#include "../Lockstep.h"
#include "Probe.h"

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

using namespace melonDS;
using melonDS::Netplay::ConsoleInput;
using melonDS::Netplay::Lockstep;

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
    std::vector<std::string> Saves;     // per console, optional
    int Consoles = 2;
    u32 Frames = 600;
    u32 From = 0;                       // also time the frames from here on
    int Runs = 2;
    bool Jit = true;
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

// Firmware as the frontend makes it for instance `id` (EmuInstance::customizeFirmware).
static Firmware MakeFirmware(int id)
{
    Firmware firmware(0);
    auto& user = firmware.GetEffectiveUserData();
    std::string name = "PLAYER" + std::to_string(id + 1);
    for (size_t i = 0; i < name.size() && i < 10; i++)
        user.Nickname[i] = static_cast<char16_t>(name[i]);
    user.NameLength = static_cast<u16>(name.size());

    if (id > 0)
    {
        auto& header = firmware.GetHeader();
        MacAddress mac = header.MacAddr;
        mac[3] += id;
        mac[4] += id * 0x44;
        mac[5] += id * 0x10;
        mac[0] &= 0xFC;
        header.MacAddr = mac;
        header.UpdateChecksum();
    }
    firmware.UpdateChecksums();
    return firmware;
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

// One full run: hashes[frame * consoles + console].
static std::vector<u64> Run(const Options& opt, const std::vector<u8>& rom,
                            const std::vector<ScriptEvent>& script, int runIndex)
{
    Probe::CurrentSession = std::make_unique<Lockstep>();
    std::vector<Probe::ConsoleContext> ctx(opt.Consoles);
    std::vector<std::unique_ptr<NDS>> consoles;
    std::string romName = BaseName(opt.Rom);

    for (int i = 0; i < opt.Consoles; i++)
    {
        ctx[i].ID = i;
        NDSArgs args {};
        args.Firmware = MakeFirmware(i);
        if (!opt.Jit) args.JIT = std::nullopt;

        auto nds = std::make_unique<NDS>(std::move(args), &ctx[i]);

        NDSCart::NDSCartArgs cartArgs {};
        if (i < static_cast<int>(opt.Saves.size()) && !opt.Saves[i].empty())
        {
            std::vector<u8> save = ReadFile(opt.Saves[i]);
            if (!save.empty())
            {
                cartArgs.SRAM = std::make_unique<u8[]>(save.size());
                std::memcpy(cartArgs.SRAM.get(), save.data(), save.size());
                cartArgs.SRAMLength = static_cast<u32>(save.size());
            }
        }
        auto cart = NDSCart::ParseROM(rom.data(), static_cast<u32>(rom.size()), &ctx[i],
                                      std::move(cartArgs));
        if (!cart)
        {
            std::fprintf(stderr, "the ROM did not parse\n");
            std::exit(1);
        }
        nds->SetNDSCart(std::move(cart));
        nds->Reset();
        nds->SetupDirectBoot(romName);
        nds->RTC.SetDateTime(2026, 1, 1, 12, 0, 0);
        nds->Start();

        Probe::Session().AddConsole(i, nds.get());
        consoles.push_back(std::move(nds));
    }

    std::vector<u64> hashes(static_cast<size_t>(opt.Frames) * opt.Consoles);
    ConsoleInput inputs[Netplay::kMaxConsoles];
    auto start = std::chrono::steady_clock::now();
    auto fromStart = start;

    for (u32 frame = 0; frame < opt.Frames; frame++)
    {
        if (frame == opt.From) fromStart = std::chrono::steady_clock::now();
        for (int i = 0; i < opt.Consoles; i++)
            inputs[i] = ConsoleInput{};
        for (const ScriptEvent& ev : script)
        {
            if (ev.Console < opt.Consoles && frame >= ev.Frame && frame < ev.Frame + ev.Hold)
                inputs[ev.Console].KeyMask &= ~ev.Keys;
        }

        Probe::Session().RunFrame(inputs);

        for (int i = 0; i < opt.Consoles; i++)
        {
            NDS& nds = *consoles[i];
            hashes[static_cast<size_t>(frame) * opt.Consoles + i] =
                XXH3_64bits(nds.MainRAM, nds.MainRAMMask + 1);
        }

        if (runIndex == 0 && !opt.ShotPrefix.empty())
        {
            for (u32 shot : opt.ShotFrames)
            {
                if (shot != frame) continue;
                for (int i = 0; i < opt.Consoles; i++)
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
                runIndex, opt.Frames, opt.Consoles, secs, opt.Frames / secs,
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

static void Usage()
{
    std::fprintf(stderr,
        "usage: melonDS-netprobe --rom FILE [--consoles N] [--frames N] [--runs N]\n"
        "       [--script FILE] [--save I=FILE]... [--hashes OUT] [--nojit]\n"
        "       [--shot PREFIX --shot-frame F]...\n");
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
        else if (a == "--script") opt.Script = next();
        else if (a == "--hashes") opt.HashOut = next();
        else if (a == "--nojit") opt.Jit = false;
        else if (a == "--shot") opt.ShotPrefix = next();
        else if (a == "--shot-frame") opt.ShotFrames.push_back(static_cast<u32>(std::atol(next().c_str())));
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

    std::vector<std::vector<u64>> runs;
    for (int r = 0; r < opt.Runs; r++)
        runs.push_back(Run(opt, rom, script, r));

    if (!opt.HashOut.empty())
    {
        std::FILE* f = std::fopen(opt.HashOut.c_str(), "w");
        if (f)
        {
            for (u32 frame = 0; frame < opt.Frames; frame++)
            {
                std::fprintf(f, "%u", frame);
                for (int i = 0; i < opt.Consoles; i++)
                    std::fprintf(f, " %016llx", static_cast<unsigned long long>(
                        runs[0][static_cast<size_t>(frame) * opt.Consoles + i]));
                std::fprintf(f, "\n");
            }
            std::fclose(f);
        }
    }

    int status = 0;
    for (int r = 1; r < opt.Runs; r++)
    {
        bool same = true;
        for (u32 frame = 0; frame < opt.Frames && same; frame++)
        {
            for (int i = 0; i < opt.Consoles; i++)
            {
                size_t k = static_cast<size_t>(frame) * opt.Consoles + i;
                if (runs[0][k] != runs[r][k])
                {
                    std::printf("run %d differs from run 0 at frame %u, console %d\n", r, frame, i);
                    same = false;
                    status = 1;
                    break;
                }
            }
        }
        if (same) std::printf("run %d matches run 0 on every frame and console\n", r);
    }
    return status;
}
