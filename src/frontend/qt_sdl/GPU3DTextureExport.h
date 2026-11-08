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

#ifndef GPU3DTEXTUREEXPORT_H
#define GPU3DTEXTUREEXPORT_H

#include <vector>

#include "types.h"

namespace melonDS
{

class NDS;

// Decodes a 3D texture (TexParam/TexPalette) into RGBA8, reusing the
// renderer's texture decoder. Returns false for "no texture"/unsupported format.
bool Decode3DTexture(NDS& nds, u32 texParam, u32 texPalette, std::vector<u8>& rgba,
                     int& width, int& height);

}

#endif // GPU3DTEXTUREEXPORT_H
