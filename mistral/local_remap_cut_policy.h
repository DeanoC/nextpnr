/* Bounded LUT-cut composition policy. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LOCAL_REMAP_CUT_POLICY_H
#define MISTRAL_LOCAL_REMAP_CUT_POLICY_H
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace local_remap_cut_policy {
constexpr int ZERO = -1, ONE = -2;
// Invalid indices produce a source that no bounded cut can reference.
constexpr int node_source(int index)
{
    return index >= 0 && index <= std::numeric_limits<int>::max() - 2
                   ? -3 - index : std::numeric_limits<int>::min();
}
struct Pin { int source; bool inverted; };
struct Node { uint64_t mask; std::vector<Pin> pins; };
struct Result {
    bool valid = false;
    uint64_t mask = 0;
    std::vector<int> signals;
};

// Compose the last of two or three topologically ordered LUTs. Each LUT has
// two through six pins; source IDs are 0..31, constants, or earlier nodes.
// Aliases share one variable. At most six external variables are permitted
// before pruning, and mask bits outside each LUT's active rows are ignored.
// The result contains only essential source IDs, sorted in truth-row order.
// A constant result has no signals and its single row is mask 0 or 1.
inline Result compose(const std::vector<Node> &nodes)
{
    Result result;
    if (nodes.size() < 2 || nodes.size() > 3) return result;
    std::array<bool, 32> external{};
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        if (nodes[index].pins.size() < 2 || nodes[index].pins.size() > 6) return result;
        for (const auto &pin : nodes[index].pins) {
            if (pin.source >= 0) {
                if (pin.source >= int(external.size())) return result;
                external[pin.source] = true;
            } else if (pin.source < ONE &&
                       (index == 0 || pin.source < -2 - int(index))) {
                return result;
            }
        }
    }
    std::vector<int> signals;
    std::array<int, 32> source_index{};
    for (std::size_t source = 0; source < external.size(); ++source) {
        if (!external[source]) continue;
        source_index[source] = int(signals.size());
        signals.push_back(int(source));
    }
    if (signals.size() > 6) return result;
    uint64_t table = 0;
    const unsigned rows = 1u << signals.size();
    for (unsigned row = 0; row < rows; ++row) {
        std::array<bool, 3> values{};
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            unsigned lut_row = 0;
            for (std::size_t pin_index = 0; pin_index < nodes[index].pins.size(); ++pin_index) {
                const auto &pin = nodes[index].pins[pin_index];
                bool value = pin.source == ONE;
                if (pin.source >= 0) value = (row >> source_index[pin.source]) & 1u;
                else if (pin.source < ONE) value = values[-3 - pin.source];
                lut_row |= unsigned(value ^ pin.inverted) << pin_index;
            }
            values[index] = (nodes[index].mask >> lut_row) & 1u;
        }
        if (values[nodes.size() - 1]) table |= uint64_t(1) << row;
    }
    std::vector<unsigned> essential;
    for (unsigned index = 0; index < signals.size(); ++index) {
        const unsigned bit = 1u << index;
        for (unsigned row = 0; row < rows; ++row) {
            if (!(row & bit) && (((table >> row) ^ (table >> (row | bit))) & 1u)) {
                essential.push_back(index);
                break;
            }
        }
    }
    for (unsigned index : essential) result.signals.push_back(signals[index]);
    for (unsigned row = 0; row < (1u << essential.size()); ++row) {
        unsigned original = 0;
        for (std::size_t index = 0; index < essential.size(); ++index)
            original |= ((row >> index) & 1u) << essential[index];
        if ((table >> original) & 1u) result.mask |= uint64_t(1) << row;
    }
    result.valid = true;
    return result;
}
}
#endif
