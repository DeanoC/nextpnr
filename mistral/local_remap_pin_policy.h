/* Bounded LUT input permutation policy. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LOCAL_REMAP_PIN_POLICY_H
#define MISTRAL_LOCAL_REMAP_PIN_POLICY_H
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace local_remap_pin_policy {
struct RankedOrder {
    // Entry p names the original input connected to the new LUT pin p.
    std::vector<int> input_for_pin;
    int score;
};

// matrix[source][pin] contains nonnegative costs for two through six inputs.
// Rank every order by its worst selected cost, then lexically for stable ties.
inline std::vector<RankedOrder> orders(const std::vector<std::vector<int>> &matrix)
{
    const auto count = matrix.size();
    if (count < 2 || count > 6) return {};
    for (const auto &row : matrix) {
        if (row.size() != count) return {};
        for (int cost : row) if (cost < 0) return {};
    }
    std::vector<int> order(count);
    for (std::size_t input = 0; input < count; ++input) order[input] = int(input);
    std::vector<RankedOrder> result;
    do {
        int score = 0;
        for (std::size_t pin = 0; pin < count; ++pin)
            score = std::max(score, matrix[order[pin]][pin]);
        result.push_back({order, score});
    } while (std::next_permutation(order.begin(), order.end()));
    std::sort(result.begin(), result.end(), [](const RankedOrder &a, const RankedOrder &b) {
        if (a.score != b.score) return a.score < b.score;
        return a.input_for_pin < b.input_for_pin;
    });
    return result;
}

// Truth-table permutation includes any polarity already encoded in the mask.
// Invalid orders leave out unchanged. Bits outside the active 2^n rows are ignored.
inline bool permute_mask(uint64_t mask, const std::vector<int> &input_for_pin, uint64_t &out)
{
    const auto count = input_for_pin.size();
    if (count < 2 || count > 6) return false;
    unsigned seen = 0;
    for (int input : input_for_pin) {
        if (input < 0 || input >= int(count) || (seen & (1u << input))) return false;
        seen |= 1u << input;
    }
    uint64_t result = 0;
    for (unsigned row = 0; row < (1u << count); ++row) {
        unsigned original = 0;
        for (std::size_t pin = 0; pin < count; ++pin)
            original |= ((row >> pin) & 1u) << input_for_pin[pin];
        if ((mask >> original) & 1u) result |= uint64_t(1) << row;
    }
    out = result;
    return true;
}
}
#endif
