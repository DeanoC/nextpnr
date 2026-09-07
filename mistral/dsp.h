/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  Deano Calver
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
 *  IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MISTRAL_DSP_H
#define MISTRAL_DSP_H

#include <array>

#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

struct MistralDspLane
{
    int a_group;
    int b_group;
    int result_offset;
};

// Cyclone V three-multiplier mode packs three 9x9 products into the 27-bit
// A/B inputs and the low 54 logical result bits. The physical RESULT port has
// a one-bit hole at 36, so the third product starts at port 37. The order
// follows Mistral's documented AX/AY packing table: the low lane is the
// existing single-mode mapping and the two upper lanes use groups 6/8 and
// 7/9.
constexpr std::array<MistralDspLane, 3> mistral_dsp_lanes{{
        {0, 2, 0},
        {6, 8, 18},
        {7, 9, 37},
}};

NEXTPNR_NAMESPACE_END

#endif
