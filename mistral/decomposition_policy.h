/* Bounded seven-input LUT decomposition policy. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_DECOMPOSITION_POLICY_H
#define MISTRAL_DECOMPOSITION_POLICY_H

#include "local_remap_cut_policy.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace decomposition_policy {
using local_remap_cut_policy::Node;
using local_remap_cut_policy::Pin;
using local_remap_cut_policy::ZERO;
using local_remap_cut_policy::ONE;
using local_remap_cut_policy::node_source;

struct Result {
    bool valid = false;
    std::array<uint64_t, 2> truth{};
    std::vector<int> signals;
};

struct Lut {
    std::vector<int> signals;
    uint64_t mask = 0;
};
inline bool operator<(const Lut &a, const Lut &b)
{
    return std::tie(a.signals, a.mask) < std::tie(b.signals, b.mask);
}
inline bool operator==(const Lut &a, const Lut &b)
{
    return a.signals == b.signals && a.mask == b.mask;
}

// LUT signal references use positions in Result::signals, not raw source IDs.
// Root references 7 and 8 select the corresponding encoder outputs.
struct Candidate {
    std::vector<Lut> encoders;
    Lut root;
    std::vector<int> bound, free;
    unsigned classes = 0, code_bits = 0;
    std::array<unsigned, 16> column_classes{};
    std::vector<unsigned> class_codes;
};

constexpr std::size_t MAX_PARTITIONS = 35;
constexpr std::size_t MAX_CANDIDATES = 840;

namespace detail {
inline bool bit(const std::array<uint64_t, 2> &truth, unsigned row)
{
    return (truth[row / 64] >> (row % 64)) & 1u;
}
inline void set(std::array<uint64_t, 2> &truth, unsigned row)
{
    truth[row / 64] |= uint64_t(1) << (row % 64);
}
inline bool essential(const std::array<uint64_t, 2> &truth, unsigned rows, unsigned index)
{
    const unsigned variable = 1u << index;
    for (unsigned row = 0; row < rows; ++row)
        if (!(row & variable) && bit(truth, row) != bit(truth, row | variable)) return true;
    return false;
}
inline Lut prune(Lut lut)
{
    std::vector<unsigned> support;
    const unsigned rows = 1u << lut.signals.size();
    for (unsigned index = 0; index < lut.signals.size(); ++index)
        for (unsigned row = 0; row < rows; ++row)
            if (!(row & (1u << index)) &&
                (((lut.mask >> row) ^ (lut.mask >> (row | (1u << index)))) & 1u)) {
                support.push_back(index);
                break;
            }
    Lut result;
    for (unsigned index : support) result.signals.push_back(lut.signals[index]);
    for (unsigned row = 0; row < (1u << support.size()); ++row) {
        unsigned original = 0;
        for (unsigned index = 0; index < support.size(); ++index)
            original |= ((row >> index) & 1u) << support[index];
        if ((lut.mask >> original) & 1u) result.mask |= uint64_t(1) << row;
    }
    return result;
}
inline unsigned source_row(unsigned row, const std::vector<int> &signals)
{
    unsigned result = 0;
    for (unsigned index = 0; index < signals.size(); ++index)
        result |= ((row >> signals[index]) & 1u) << index;
    return result;
}
inline bool matches(const Candidate &candidate, const Result &function)
{
    for (unsigned row = 0; row < 128; ++row) {
        std::array<bool, 2> code{};
        for (unsigned index = 0; index < candidate.encoders.size(); ++index) {
            const auto &lut = candidate.encoders[index];
            code[index] = (lut.mask >> source_row(row, lut.signals)) & 1u;
        }
        unsigned root_row = 0;
        for (unsigned index = 0; index < candidate.root.signals.size(); ++index) {
            const int signal = candidate.root.signals[index];
            const bool value = signal < 7 ? ((row >> signal) & 1u) : code[signal - 7];
            root_row |= unsigned(value) << index;
        }
        if (((candidate.root.mask >> root_row) & 1u) != bit(function.truth, row)) return false;
    }
    return true;
}
inline void normalize(Candidate &candidate)
{
    if (candidate.encoders.size() != 2 || !(candidate.encoders[1] < candidate.encoders[0])) return;
    std::swap(candidate.encoders[0], candidate.encoders[1]);
    for (auto &code : candidate.class_codes) code = ((code & 1u) << 1) | ((code >> 1) & 1u);
    auto new_signals = candidate.root.signals;
    for (auto &signal : new_signals)
        if (signal >= 7) signal = 15 - signal; // 7 <-> 8
    std::sort(new_signals.begin(), new_signals.end());
    uint64_t new_mask = 0;
    for (unsigned row = 0; row < (1u << new_signals.size()); ++row) {
        unsigned original = 0;
        for (unsigned index = 0; index < candidate.root.signals.size(); ++index) {
            int signal = candidate.root.signals[index];
            if (signal >= 7) signal = 15 - signal;
            const auto at = std::lower_bound(new_signals.begin(), new_signals.end(), signal) - new_signals.begin();
            original |= ((row >> at) & 1u) << index;
        }
        if ((candidate.root.mask >> original) & 1u) new_mask |= uint64_t(1) << row;
    }
    candidate.root = {std::move(new_signals), new_mask};
}
}

// Exactly four topologically ordered LUTs, each with two through six pins.
// At most seven distinct raw external source IDs (0..31) are accepted before
// pruning. Constants, inverted pins, aliases and reconvergence are exact.
inline Result compose(const std::vector<Node> &nodes)
{
    Result result;
    if (nodes.size() != 4) return result;
    std::array<bool, 32> external{};
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        if (nodes[index].pins.size() < 2 || nodes[index].pins.size() > 6) return result;
        for (const auto &pin : nodes[index].pins) {
            if (pin.source >= 0) {
                if (pin.source >= int(external.size())) return result;
                external[pin.source] = true;
            } else if (pin.source < ONE) {
                const int source = -3 - pin.source;
                if (source < 0 || source >= int(index)) return result;
            }
        }
    }
    std::vector<int> signals;
    std::array<unsigned, 32> source_index{};
    for (unsigned source = 0; source < external.size(); ++source)
        if (external[source]) {
            source_index[source] = unsigned(signals.size());
            signals.push_back(int(source));
        }
    if (signals.size() > 7) return result;
    std::array<uint64_t, 2> table{};
    const unsigned rows = 1u << signals.size();
    for (unsigned row = 0; row < rows; ++row) {
        std::array<bool, 4> values{};
        for (unsigned index = 0; index < nodes.size(); ++index) {
            unsigned lut_row = 0;
            for (unsigned at = 0; at < nodes[index].pins.size(); ++at) {
                const auto &pin = nodes[index].pins[at];
                bool value = pin.source == ONE;
                if (pin.source >= 0) value = (row >> source_index[pin.source]) & 1u;
                else if (pin.source < ONE) value = values[-3 - pin.source];
                lut_row |= unsigned(value ^ pin.inverted) << at;
            }
            values[index] = (nodes[index].mask >> lut_row) & 1u;
        }
        if (values.back()) detail::set(table, row);
    }
    std::vector<unsigned> support;
    for (unsigned index = 0; index < signals.size(); ++index)
        if (detail::essential(table, rows, index)) support.push_back(index);
    for (unsigned index : support) result.signals.push_back(signals[index]);
    for (unsigned row = 0; row < (1u << support.size()); ++row) {
        unsigned original = 0;
        for (unsigned index = 0; index < support.size(); ++index)
            original |= ((row >> index) & 1u) << support[index];
        if (detail::bit(table, original)) detail::set(result.truth, row);
    }
    result.valid = true;
    return result;
}

// Search all 35 bound-four/free-three partitions and every injective code.
// Unused codes produce zero. Encoders with fewer than two essential inputs
// are conservatively excluded; this policy does not synthesize padding or
// constants. Empty results therefore do not prove general undecomposability.
inline std::vector<Candidate> decompose(const Result &function)
{
    std::vector<Candidate> result;
    if (!function.valid || function.signals.size() != 7) return result;
    for (unsigned index = 0; index < 7; ++index)
        if (function.signals[index] < 0 || function.signals[index] > 31 ||
            (index && function.signals[index - 1] >= function.signals[index]) ||
            !detail::essential(function.truth, 128, index)) return result;
    using Signature = std::pair<std::vector<Lut>, Lut>;
    std::set<Signature> seen;
    for (int a = 0; a < 4; ++a)
        for (int b = a + 1; b < 5; ++b)
            for (int c = b + 1; c < 6; ++c)
                for (int d = c + 1; d < 7; ++d) {
                    Candidate base;
                    base.bound = {a, b, c, d};
                    for (int index = 0; index < 7; ++index)
                        if (std::find(base.bound.begin(), base.bound.end(), index) == base.bound.end())
                            base.free.push_back(index);
                    std::vector<unsigned> columns;
                    for (unsigned bound_row = 0; bound_row < 16; ++bound_row) {
                        unsigned column = 0;
                        for (unsigned free_row = 0; free_row < 8; ++free_row) {
                            unsigned row = 0;
                            for (unsigned index = 0; index < 4; ++index)
                                row |= ((bound_row >> index) & 1u) << base.bound[index];
                            for (unsigned index = 0; index < 3; ++index)
                                row |= ((free_row >> index) & 1u) << base.free[index];
                            if (detail::bit(function.truth, row)) column |= 1u << free_row;
                        }
                        auto found = std::find(columns.begin(), columns.end(), column);
                        base.column_classes[bound_row] = unsigned(found - columns.begin());
                        if (found == columns.end()) columns.push_back(column);
                    }
                    if (columns.size() < 2 || columns.size() > 4) continue;
                    base.classes = unsigned(columns.size());
                    base.code_bits = base.classes == 2 ? 1 : 2;
                    base.class_codes.resize(base.classes);
                    auto enumerate = [&](auto &&self, unsigned next, unsigned used) -> void {
                        if (next != base.classes) {
                            for (unsigned code = 0; code < (1u << base.code_bits); ++code)
                                if (!(used & (1u << code))) {
                                    base.class_codes[next] = code;
                                    self(self, next + 1, used | (1u << code));
                                }
                            return;
                        }
                        Candidate candidate = base;
                        for (unsigned bit = 0; bit < base.code_bits; ++bit) {
                            Lut encoder{base.bound, 0};
                            for (unsigned row = 0; row < 16; ++row)
                                if ((base.class_codes[base.column_classes[row]] >> bit) & 1u)
                                    encoder.mask |= uint64_t(1) << row;
                            encoder = detail::prune(std::move(encoder));
                            if (encoder.signals.size() < 2) return;
                            candidate.encoders.push_back(std::move(encoder));
                        }
                        candidate.root.signals = base.free;
                        for (unsigned bit = 0; bit < base.code_bits; ++bit)
                            candidate.root.signals.push_back(7 + int(bit));
                        for (unsigned row = 0; row < (1u << (3 + base.code_bits)); ++row) {
                            const unsigned code = row >> 3, free_row = row & 7u;
                            const auto found = std::find(base.class_codes.begin(), base.class_codes.end(), code);
                            if (found != base.class_codes.end() &&
                                ((columns[found - base.class_codes.begin()] >> free_row) & 1u))
                                candidate.root.mask |= uint64_t(1) << row;
                        }
                        candidate.root = detail::prune(std::move(candidate.root));
                        if (candidate.root.signals.size() < 2 || candidate.root.signals.size() > 6) return;
                        detail::normalize(candidate);
                        if (!detail::matches(candidate, function)) return;
                        if (seen.emplace(candidate.encoders, candidate.root).second)
                            result.push_back(std::move(candidate));
                    };
                    enumerate(enumerate, 0, 0);
                }
    auto rank = [](const Candidate &candidate) {
        std::size_t maximum = 0, total = 0;
        for (const auto &encoder : candidate.encoders) {
            maximum = std::max(maximum, encoder.signals.size());
            total += encoder.signals.size();
        }
        return std::make_tuple(maximum, total, candidate.encoders.size() + 1, candidate.root.signals.size());
    };
    std::sort(result.begin(), result.end(), [&](const Candidate &a, const Candidate &b) {
        return std::make_tuple(rank(a), a.encoders, a.root) < std::make_tuple(rank(b), b.encoders, b.root);
    });
    return result;
}
}
#endif
