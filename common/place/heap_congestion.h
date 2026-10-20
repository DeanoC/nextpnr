/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  Deano Calver <deano@geometric.so>
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
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef HEAP_CONGESTION_H
#define HEAP_CONGESTION_H

#include <cstdint>
#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

namespace heap_congestion_detail {

// Whether `cells` fit in `scaled_bels` under the beta threshold that
// SpreaderRegion::overused applies, including its small-capacity exception.
inline bool scale_is_usable(int cells, int scaled_bels, float beta)
{
    int required_capacity = cells + cells / 20;
    // Match SpreaderRegion::overused: small capacities do not use beta.
    float effective_capacity = scaled_bels < 4 ? scaled_bels : beta * scaled_bels;
    return effective_capacity >= required_capacity;
}

// The share of a location's scaled capacity that lies in a region holding
// `region_bels` of its `location_bels` BELs.
inline int region_capacity(int scaled_bels, int region_bels, int location_bels)
{
    if (location_bels == 0)
        return 0;
    return int(int64_t(scaled_bels) * region_bels / location_bels);
}

} // namespace heap_congestion_detail

NEXTPNR_NAMESPACE_END

#endif
