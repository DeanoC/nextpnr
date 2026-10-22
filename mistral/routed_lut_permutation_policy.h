/* Exact ordinary-LUT input transposition. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_ROUTED_LUT_PERMUTATION_POLICY_H
#define MISTRAL_ROUTED_LUT_PERMUTATION_POLICY_H
#include <cstdint>
#include <optional>

namespace routed_lut_permutation_policy {
inline std::optional<uint64_t> transpose(uint64_t mask, unsigned width, unsigned first, unsigned second)
{
    if (width < 2 || width > 5 || first >= width || second >= width || first == second) return std::nullopt;
    const unsigned rows = 1u << width;
    if (mask >> rows) return std::nullopt;
    uint64_t result = 0;
    for (unsigned row = 0; row < rows; ++row) {
        unsigned original = row;
        if (((row >> first) ^ (row >> second)) & 1) original ^= (1u << first) | (1u << second);
        result |= ((mask >> original) & 1) << row;
    }
    return result;
}
}
#endif
