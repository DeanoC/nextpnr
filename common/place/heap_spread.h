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

#ifndef HEAP_SPREAD_H
#define HEAP_SPREAD_H

#include <vector>
#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

namespace heap_spread_detail {

// The one/two-cell interpolation uses a rank within this partition, not
// within the full cut vector. The caller only invokes this for a nonempty
// partition and cells_start <= index < cells_end.
inline double small_partition_position(int index, int cells_start, int cells_end, double area_l, double area_r)
{
    return area_l + (index - cells_start) * ((area_r - area_l) / (cells_end - cells_start));
}

// The pivot is the last cell on the left. Transfer the first right cell's
// complete chain weight in its own bucket before advancing that boundary.
// Accessors keep the same operation testable without an architecture database.
template <typename Cells, typename Bucket, typename Size>
void move_right_cell_to_left(const Cells &cells, int &pivot, std::vector<int> &left, std::vector<int> &right,
                             Bucket bucket, Size size)
{
    const auto &cell = cells.at(pivot + 1);
    auto type = bucket(cell);
    int count = size(cell);
    left.at(type) += count;
    right.at(type) -= count;
    ++pivot;
}

} // namespace heap_spread_detail

NEXTPNR_NAMESPACE_END

#endif
