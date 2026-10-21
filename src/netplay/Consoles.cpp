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

}
