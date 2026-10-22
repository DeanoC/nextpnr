/* Exhaustive six-input symmetry eligibility. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_ROUTED_LUT6_SWAP_POLICY_H
#define MISTRAL_ROUTED_LUT6_SWAP_POLICY_H
#include <cstdint>

namespace routed_lut6_swap_policy {
inline bool symmetric(uint64_t mask, unsigned first, unsigned second)
{
    if (first >= 6 || second >= 6 || first == second) return false;
    for (unsigned row = 0; row < 64; ++row) {
        unsigned other = row;
        if (((row >> first) ^ (row >> second)) & 1)
            other ^= (1u << first) | (1u << second);
        if (((mask >> row) ^ (mask >> other)) & 1) return false;
    }
    return true;
}
}
#endif
