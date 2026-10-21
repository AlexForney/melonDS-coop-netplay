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

#include "Consoles.h"

#include <cstring>
#include "NDS.h"
#include "NDSCart.h"
#include "FreeBIOS.h"
#include "GPU3D_Soft.h"
#include "Lockstep.h"
#include "Savestate.h"

#include <zstd.h>

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

namespace melonDS::Netplay
{

Firmware MakeFirmware(int slot, const std::string& name)
{
    Firmware firmware(0);
    auto& user = firmware.GetEffectiveUserData();
    std::string nick = name.empty() ? "PLAYER" + std::to_string(slot + 1) : name;
    std::size_t len = nick.size() < kNameLength ? nick.size() : kNameLength;
    for (std::size_t i = 0; i < len; i++)
        user.Nickname[i] = static_cast<char16_t>(nick[i]);
    user.NameLength = static_cast<u16>(len);

    if (slot > 0)
    {
        // EmuInstance::customizeFirmware's per-instance MAC
        auto& header = firmware.GetHeader();
        MacAddress mac = header.MacAddr;
        mac[3] += slot;
        mac[4] += slot * 0x44;
        mac[5] += slot * 0x10;
        mac[0] &= 0xFC;
        header.MacAddr = mac;
        header.UpdateChecksum();
    }
    firmware.UpdateChecksums();
    return firmware;
}

std::optional<JITArgs> SessionJIT(bool enabled)
{
    if (!enabled) return std::nullopt;
    return JITArgs {};
}

static void Boot(NDS& nds, const StartInfo& info, const std::string& romName)
{
    nds.Reset();
    nds.SetupDirectBoot(romName);
    nds.RTC.SetDateTime(info.Year, info.Month, info.Day, info.Hour, info.Minute, info.Second);
    nds.Start();
}

std::unique_ptr<NDS> MakeMirror(const StartInfo& info, int slot, const u8* rom, u32 romLength,
                                const std::string& romName, void* userdata)
{
    NDSArgs args {};
    args.Firmware = MakeFirmware(slot, info.Names[slot]);
    args.JIT = SessionJIT(info.JIT);
    auto nds = std::make_unique<NDS>(std::move(args), userdata);

    NDSCart::NDSCartArgs cartArgs {};
    const std::vector<u8>& save = info.Saves[slot];
    if (!save.empty())
    {
        cartArgs.SRAM = std::make_unique<u8[]>(save.size());
        std::memcpy(cartArgs.SRAM.get(), save.data(), save.size());
        cartArgs.SRAMLength = static_cast<u32>(save.size());
    }
    auto cart = NDSCart::ParseROM(rom, romLength, userdata, std::move(cartArgs));
    if (!cart) return nullptr;
    nds->SetNDSCart(std::move(cart));
    Boot(*nds, info, romName);
    return nds;
}

void SetUpLocal(NDS& nds, const StartInfo& info, int slot, const std::string& romName)
{
    nds.SetARM9BIOS(bios_arm9_bin);
    nds.SetARM7BIOS(bios_arm7_bin);
    nds.SetFirmware(MakeFirmware(slot, info.Names[slot]));
    nds.SetJITArgs(SessionJIT(info.JIT));
    nds.SPU.SetInterpolation(AudioInterpolation::None);
    nds.SPU.SetDegrade10Bit(AudioBitDepth::Auto);
    nds.GPU.SetRenderer3D(std::make_unique<SoftRenderer>());
    nds.AREngine.Cheats.clear();

    const std::vector<u8>& save = info.Saves[slot];
    if (!save.empty())
        nds.SetNDSSave(save.data(), static_cast<u32>(save.size()));
    Boot(nds, info, romName);
}

u64 HashConsole(NDS& nds)
{
    return XXH3_64bits(nds.MainRAM, nds.MainRAMMask + 1);
}

RomHash HashRom(const u8* rom, u32 length)
{
    RomHash out {};
    XXH128_hash_t h = XXH3_128bits(rom, length);
    std::memcpy(out.data(), &h.low64, 8);
    std::memcpy(out.data() + 8, &h.high64, 8);
    return out;
}

// Snapshot, before compression: "NPSS", u32 count, then per console u32 length + its savestate,
// then u32 length + the link's state.
static constexpr u32 kSnapshotMagic = 0x5353504E;

static void PutBlock(std::vector<u8>& out, const void* data, u32 len)
{
    for (int i = 0; i < 4; i++) out.push_back(static_cast<u8>(len >> (8 * i)));
    const u8* p = static_cast<const u8*>(data);
    out.insert(out.end(), p, p + len);
}

bool SaveSnapshot(NDS* const* consoles, int count, const Lockstep& link, std::vector<u8>& out)
{
    std::vector<u8> raw;
    u32 head[2] = {kSnapshotMagic, static_cast<u32>(count)};
    raw.insert(raw.end(), reinterpret_cast<u8*>(head), reinterpret_cast<u8*>(head) + sizeof head);
    for (int s = 0; s < count; s++)
    {
        Savestate state;
        if (state.Error || !consoles[s]->DoSavestate(&state) || state.Error) return false;
        PutBlock(raw, state.Buffer(), state.Length());
    }
    std::vector<u8> linkState = link.SaveState();
    PutBlock(raw, linkState.data(), static_cast<u32>(linkState.size()));

    out.resize(ZSTD_compressBound(raw.size()));
    std::size_t n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), 3);
    if (ZSTD_isError(n)) return false;
    out.resize(n);
    return true;
}

bool LoadSnapshot(NDS* const* consoles, int count, Lockstep& link, const u8* data, std::size_t len)
{
    unsigned long long rawLen = ZSTD_getFrameContentSize(data, len);
    if (rawLen == ZSTD_CONTENTSIZE_ERROR || rawLen == ZSTD_CONTENTSIZE_UNKNOWN || rawLen > (256u << 20))
        return false;
    std::vector<u8> raw(rawLen);
    std::size_t n = ZSTD_decompress(raw.data(), raw.size(), data, len);
    if (ZSTD_isError(n) || n != rawLen) return false;

    std::size_t pos = 0;
    auto get32 = [&](u32& v) {
        if (pos + 4 > raw.size()) return false;
        std::memcpy(&v, &raw[pos], 4);
        pos += 4;
        return true;
    };
    u32 magic, stored;
    if (!get32(magic) || !get32(stored) || magic != kSnapshotMagic || stored != static_cast<u32>(count))
        return false;
    for (int s = 0; s < count; s++)
    {
        u32 blockLen;
        if (!get32(blockLen) || pos + blockLen > raw.size()) return false;
        Savestate state(&raw[pos], blockLen, false);
        if (state.Error || !consoles[s]->DoSavestate(&state) || state.Error) return false;
        pos += blockLen;
    }
    u32 linkLen;
    if (!get32(linkLen) || pos + linkLen > raw.size()) return false;
    return link.LoadState(&raw[pos], linkLen);
}

}
