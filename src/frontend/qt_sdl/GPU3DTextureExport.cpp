/*
    Copyright 2016-2026 melonDS team

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

#include "GPU3DTextureExport.h"

#include "GPU.h"
#include "GPU3D_Texcache.h"
#include "NDS.h"

#include <vector>

using namespace melonDS;

namespace melonDS
{

bool Decode3DTexture(NDS& nds, u32 texParam, u32 texPalette, std::vector<u8>& rgba,
                     int& width, int& height)
{
    const u32 fmt = (texParam >> 26) & 0x7;
    if (fmt == 0)
        return false;

    GPU& gpu = nds.GPU;
    auto texDirty = gpu.VRAMDirty_Texture.DeriveState(gpu.VRAMMap_Texture, gpu);
    auto palDirty = gpu.VRAMDirty_TexPal.DeriveState(gpu.VRAMMap_TexPal, gpu);
    gpu.MakeVRAMFlat_TextureCoherent(texDirty);
    gpu.MakeVRAMFlat_TexPalCoherent(palDirty);

    const u32 w = TextureWidth(texParam);
    const u32 h = TextureHeight(texParam);
    const u32 addr = (texParam & 0xFFFF) * 8;
    std::vector<u32> buf((size_t)w * h, 0);

    if (fmt == 7)
    {
        ConvertBitmapTexture<outputFmt_RGB6A5>(w, h, buf.data(), addr, gpu);
    }
    else if (fmt == 5)
    {
        u32 slot1 = 0x20000 + ((addr & 0x1FFFC) >> 1);
        if (addr >= 0x40000) slot1 += 0x10000;
        ConvertCompressedTexture<outputFmt_RGB6A5>(w, h, buf.data(), addr, slot1,
                                                   texPalette * 16, gpu);
    }
    else
    {
        u32 palAddr = texPalette * 16;
        if (fmt == 2) palAddr >>= 1;
        palAddr &= 0x1FFFF;
        const bool color0Transparent = (texParam & (1u << 29)) != 0;

        switch (fmt)
        {
        case 1: ConvertAXIYTexture<outputFmt_RGB6A5, 3, 5>(w, h, buf.data(), addr, palAddr, gpu); break;
        case 6: ConvertAXIYTexture<outputFmt_RGB6A5, 5, 3>(w, h, buf.data(), addr, palAddr, gpu); break;
        case 2: ConvertNColorsTexture<outputFmt_RGB6A5, 2>(w, h, buf.data(), addr, palAddr, color0Transparent, gpu); break;
        case 3: ConvertNColorsTexture<outputFmt_RGB6A5, 4>(w, h, buf.data(), addr, palAddr, color0Transparent, gpu); break;
        case 4: ConvertNColorsTexture<outputFmt_RGB6A5, 8>(w, h, buf.data(), addr, palAddr, color0Transparent, gpu); break;
        default: return false;
        }
    }

    width = (int)w;
    height = (int)h;
    rgba.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++)
    {
        const u32 px = buf[i];
        rgba[i * 4 + 0] = (u8)(((px & 0xFF) * 255) / 63);
        rgba[i * 4 + 1] = (u8)((((px >> 8) & 0xFF) * 255) / 63);
        rgba[i * 4 + 2] = (u8)((((px >> 16) & 0xFF) * 255) / 63);
        rgba[i * 4 + 3] = (u8)((((px >> 24) & 0x1F) * 255) / 31);
    }

    return true;
}

}
