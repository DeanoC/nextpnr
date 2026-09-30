/* Independent bounded decomposition tests. SPDX-License-Identifier: ISC */
#include "decomposition_policy.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace {
using decomposition_policy::Candidate;
using decomposition_policy::Lut;
using decomposition_policy::Node;
using decomposition_policy::Result;
using decomposition_policy::MAX_CANDIDATES;
using decomposition_policy::MAX_PARTITIONS;
using decomposition_policy::ZERO;
using decomposition_policy::ONE;
using decomposition_policy::node_source;
using decomposition_policy::decompose;

bool truth_bit(const Result &function, unsigned row)
{
    return ((function.truth.at(row >> 6) >> (row & 63u)) & 1u) != 0;
}

template <typename Function> Result function_table(Function function)
{
    Result result;
    result.valid = true;
    result.signals = {0, 1, 2, 3, 4, 5, 6};
    for (unsigned row = 0; row < 128; ++row)
        if (function(row)) result.truth[row >> 6] |= uint64_t(1) << (row & 63u);
    return result;
}

bool actual_oracle(unsigned row)
{
    const bool a = row & 1u, s = row & 2u, t = row & 4u, b = row & 8u;
    const bool r = row & 16u, h = row & 32u, q = row & 64u;
    // Separate R branches, independent of the four-node LUT implementation.
    return h && q && (r ? (!a && !s) : (!s || (!t && b)));
}

std::vector<Node> actual_nodes()
{
    // Boundary order [a,s,t,b,R,h,q], with reconvergent s and R.
    return {{0x8, {{1, false}, {4, false}}},
            {0x4c, {{0, false}, {6, false}, {4, false}}},
            {0xb, {{node_source(0), false}, {node_source(1), false}}},
            {0x7500, {{1, false}, {2, false}, {3, false}, {5, false}, {node_source(2), false}}}};
}

unsigned lut_row(const Lut &lut, const std::array<bool, 9> &values)
{
    unsigned row = 0;
    for (unsigned pin = 0; pin < lut.signals.size(); ++pin)
        if (values.at(lut.signals.at(pin))) row += 1u << pin;
    return row;
}

bool lut_value(const Lut &lut, const std::array<bool, 9> &values)
{
    return (lut.mask & (uint64_t(1) << lut_row(lut, values))) != 0;
}

std::array<bool, 9> encoder_values(const Candidate &candidate, unsigned row)
{
    std::array<bool, 9> values{};
    for (unsigned source = 0; source < 7; ++source) values[source] = (row & (1u << source)) != 0;
    for (unsigned encoder = 0; encoder < candidate.encoders.size(); ++encoder)
        values[7 + encoder] = lut_value(candidate.encoders.at(encoder), values);
    return values;
}

unsigned expanded_row(unsigned packed, const std::vector<int> &positions)
{
    unsigned result = 0;
    for (unsigned index = 0; index < positions.size(); ++index)
        if (packed & (1u << index)) result += 1u << positions.at(index);
    return result;
}

std::array<unsigned, 16> columns(const Result &function, const std::vector<int> &bound)
{
    std::vector<int> free;
    for (int index = 0; index < 7; ++index)
        if (std::find(bound.begin(), bound.end(), index) == bound.end()) free.push_back(index);
    std::array<unsigned, 16> result{};
    for (unsigned bound_row = 0; bound_row < 16; ++bound_row)
        for (unsigned free_row = 0; free_row < 8; ++free_row)
            if (truth_bit(function, expanded_row(bound_row, bound) | expanded_row(free_row, free)))
                result.at(bound_row) += 1u << free_row;
    return result;
}

std::vector<std::vector<int>> partitions()
{
    std::vector<std::vector<int>> result;
    // Independent subset enumeration, rather than the production nested loops.
    for (unsigned subset = 0; subset < 128; ++subset) {
        std::vector<int> members;
        for (int index = 0; index < 7; ++index)
            if (subset & (1u << index)) members.push_back(index);
        if (members.size() == 4) result.push_back(members);
    }
    return result;
}

std::string signature(const Candidate &candidate)
{
    std::ostringstream result;
    auto append = [&](const Lut &lut) {
        result << '[';
        for (int signal : lut.signals) result << signal << ',';
        result << ']' << std::hex << lut.mask << ';' << std::dec;
    };
    for (const auto &lut : candidate.encoders) append(lut);
    result << '/';
    append(candidate.root);
    return result.str();
}

auto rank(const Candidate &candidate)
{
    std::size_t maximum = 0, total = 0;
    for (const auto &encoder : candidate.encoders) {
        maximum = std::max(maximum, encoder.signals.size());
        total += encoder.signals.size();
    }
    return std::make_tuple(maximum, total, candidate.encoders.size() + 1, candidate.root.signals.size(),
                           candidate.encoders, candidate.root);
}

void expect_minimal_support(const Lut &lut)
{
    const unsigned rows = 1u << lut.signals.size();
    for (unsigned pin = 0; pin < lut.signals.size(); ++pin) {
        bool witness = false;
        for (unsigned row = 0; row < rows; ++row)
            witness |= ((lut.mask >> row) & 1u) != ((lut.mask >> (row ^ (1u << pin))) & 1u);
        EXPECT_TRUE(witness);
    }
    if (rows < 64) { EXPECT_EQ(lut.mask >> rows, 0u); }
}

void expect_candidates(const Result &function, const std::vector<Candidate> &candidates)
{
    EXPECT_LE(candidates.size(), MAX_CANDIDATES);
    std::set<std::string> unique;
    for (unsigned at = 0; at < candidates.size(); ++at) {
        const auto &candidate = candidates.at(at);
        EXPECT_TRUE(unique.insert(signature(candidate)).second);
        if (at) { EXPECT_FALSE(rank(candidate) < rank(candidates.at(at - 1))); }
        ASSERT_EQ(candidate.bound.size(), 4u);
        ASSERT_EQ(candidate.free.size(), 3u);
        std::set<int> boundary(candidate.bound.begin(), candidate.bound.end());
        boundary.insert(candidate.free.begin(), candidate.free.end());
        EXPECT_EQ(boundary, (std::set<int>{0, 1, 2, 3, 4, 5, 6}));
        const auto independent_columns = columns(function, candidate.bound);
        const std::set<unsigned> column_set(independent_columns.begin(), independent_columns.end());
        EXPECT_EQ(candidate.classes, column_set.size());
        ASSERT_GE(candidate.classes, 2u);
        ASSERT_LE(candidate.classes, 4u);
        EXPECT_EQ(candidate.code_bits, candidate.classes == 2 ? 1u : 2u);
        ASSERT_EQ(candidate.encoders.size(), candidate.code_bits);
        ASSERT_EQ(candidate.class_codes.size(), candidate.classes);
        std::set<unsigned> used_codes(candidate.class_codes.begin(), candidate.class_codes.end());
        EXPECT_EQ(used_codes.size(), candidate.classes);
        for (const auto &encoder : candidate.encoders) {
            ASSERT_GE(encoder.signals.size(), 2u);
            ASSERT_LE(encoder.signals.size(), 4u);
            for (int source : encoder.signals)
                EXPECT_NE(std::find(candidate.bound.begin(), candidate.bound.end(), source), candidate.bound.end());
            expect_minimal_support(encoder);
        }
        if (candidate.encoders.size() == 2) { EXPECT_FALSE(candidate.encoders[1] < candidate.encoders[0]); }
        ASSERT_GE(candidate.root.signals.size(), 2u);
        ASSERT_LE(candidate.root.signals.size(), 6u);
        expect_minimal_support(candidate.root);
        for (unsigned first = 0; first < 16; ++first) {
            ASSERT_LT(candidate.column_classes[first], candidate.classes);
            for (unsigned second = 0; second < 16; ++second)
                EXPECT_EQ(candidate.column_classes[first] == candidate.column_classes[second],
                          independent_columns[first] == independent_columns[second]);
            auto values = encoder_values(candidate, expanded_row(first, candidate.bound));
            unsigned code = unsigned(values[7]) + (candidate.code_bits == 2 ? 2u * unsigned(values[8]) : 0u);
            EXPECT_EQ(code, candidate.class_codes.at(candidate.column_classes[first]));
        }
        // Completion is a defined function even for unreachable encoder codes.
        for (unsigned code = 0; code < (1u << candidate.code_bits); ++code)
            if (!used_codes.count(code))
                for (unsigned free_row = 0; free_row < 8; ++free_row) {
                    std::array<bool, 9> values{};
                    for (unsigned index = 0; index < 3; ++index)
                        values.at(candidate.free[index]) = (free_row & (1u << index)) != 0;
                    values[7] = code & 1u;
                    values[8] = code & 2u;
                    EXPECT_FALSE(lut_value(candidate.root, values));
                }
        for (unsigned row = 0; row < 128; ++row)
            EXPECT_EQ(lut_value(candidate.root, encoder_values(candidate, row)), truth_bit(function, row))
                    << "candidate " << at << " row " << row;
    }
}

uint64_t random_word(uint64_t &state)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}
}

TEST(DecompositionPolicy, ActualFourNodeFunctionMatchesIndependentBranches)
{
    const auto composed = decomposition_policy::compose(actual_nodes());
    ASSERT_TRUE(composed.valid);
    EXPECT_EQ(composed.signals, (std::vector<int>{0, 1, 2, 3, 4, 5, 6}));
    unsigned true_rows = 0;
    for (unsigned row = 0; row < 128; ++row) {
        EXPECT_EQ(truth_bit(composed, row), actual_oracle(row)) << row;
        true_rows += truth_bit(composed, row);
    }
    EXPECT_EQ(true_rows, 14u);
    for (unsigned variable = 0; variable < 7; ++variable) {
        bool witness = false;
        for (unsigned row = 0; row < 128; ++row)
            witness |= actual_oracle(row) != actual_oracle(row ^ (1u << variable));
        EXPECT_TRUE(witness);
    }
}

TEST(DecompositionPolicy, NoncanonicalEncodingFindsThreeAndTwoInputEncoders)
{
    const auto function = function_table(actual_oracle);
    const auto candidates = decompose(function);
    ASSERT_FALSE(candidates.empty());
    expect_candidates(function, candidates);
    bool found = false;
    for (const auto &candidate : candidates) {
        if (candidate.bound != std::vector<int>{0, 1, 2, 3} || candidate.encoders.size() != 2) continue;
        int u = -1, v = -1;
        for (unsigned encoder = 0; encoder < 2; ++encoder) {
            if (candidate.encoders[encoder] == Lut{{1, 2, 3}, 0x75}) u = int(encoder);
            if (candidate.encoders[encoder] == Lut{{0, 1}, 0x1}) v = int(encoder);
        }
        if (u < 0 || v < 0) continue;
        found = true;
        EXPECT_EQ(candidate.classes, 3u);
        EXPECT_EQ(candidate.free, (std::vector<int>{4, 5, 6}));
        EXPECT_EQ(candidate.root.signals, (std::vector<int>{4, 5, 6, 7, 8}));
        EXPECT_EQ(candidate.root.mask, u == 0 ? uint64_t(0xc0004000) : uint64_t(0xc0400000));
        EXPECT_EQ(candidate.class_codes.at(candidate.column_classes[0]), 3u);
        // The absent code is zero-completed, rather than a state don't-care.
        EXPECT_EQ(std::set<unsigned>(candidate.class_codes.begin(), candidate.class_codes.end()),
                  u == 0 ? (std::set<unsigned>{0, 1, 3}) : (std::set<unsigned>{0, 2, 3}));
    }
    EXPECT_TRUE(found);
}

TEST(DecompositionPolicy, FoldedPinInversionsAndSparseSourceIdsPreserveActualFunction)
{
    auto nodes = actual_nodes();
    const std::array<int, 7> raw_sources{{2, 5, 9, 12, 19, 23, 31}};
    for (auto &node : nodes)
        for (auto &pin : node.pins)
            if (pin.source >= 0) pin.source = raw_sources.at(pin.source);
    for (unsigned index = 0; index < nodes.size(); ++index) {
        const unsigned pin = index % nodes[index].pins.size();
        const uint64_t original = nodes[index].mask;
        nodes[index].pins[pin].inverted = true;
        nodes[index].mask = 0;
        for (unsigned row = 0; row < (1u << nodes[index].pins.size()); ++row)
            if ((original >> (row ^ (1u << pin))) & 1u) nodes[index].mask |= uint64_t(1) << row;
    }
    const auto result = decomposition_policy::compose(nodes);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.signals, (std::vector<int>{2, 5, 9, 12, 19, 23, 31}));
    for (unsigned row = 0; row < 128; ++row) EXPECT_EQ(truth_bit(result, row), actual_oracle(row));
    const auto candidates = decompose(result);
    ASSERT_FALSE(candidates.empty());
    expect_candidates(result, candidates);
}

TEST(DecompositionPolicy, ActualAlternateFourClassPartitionIsRetained)
{
    const auto function = function_table(actual_oracle);
    const std::vector<int> bound{0, 1, 2, 6};
    const auto independent = columns(function, bound);
    EXPECT_EQ(std::set<unsigned>(independent.begin(), independent.end()).size(), 4u);
    const auto candidates = decompose(function);
    EXPECT_TRUE(std::any_of(candidates.begin(), candidates.end(), [&](const Candidate &candidate) {
        return candidate.bound == bound && candidate.classes == 4;
    }));
}

TEST(DecompositionPolicy, TwoClassParityExercisesEveryPartitionAndBothCodes)
{
    const auto function = function_table([](unsigned row) {
        bool parity = false;
        for (unsigned input = 0; input < 7; ++input) parity ^= (row & (1u << input)) != 0;
        return parity;
    });
    const auto candidates = decompose(function);
    ASSERT_EQ(candidates.size(), 70u);
    expect_candidates(function, candidates);
    std::map<std::vector<int>, unsigned> counts;
    for (const auto &candidate : candidates) {
        EXPECT_EQ(candidate.classes, 2u);
        EXPECT_EQ(candidate.encoders.size(), 1u);
        EXPECT_EQ(candidate.encoders[0].signals.size(), 4u);
        EXPECT_EQ(candidate.root.signals.size(), 4u);
        ++counts[candidate.bound];
    }
    EXPECT_EQ(counts.size(), MAX_PARTITIONS);
    for (const auto &bound : partitions()) EXPECT_EQ(counts.at(bound), 2u);
}

TEST(DecompositionPolicy, ConstantsInversionAliasReconvergenceAndRawPruning)
{
    std::vector<Node> nodes{{0x2, {{7, false}, {7, true}}},
                            {0x8, {{node_source(0), false}, {ONE, false}}},
                            {0xe, {{node_source(0), true}, {node_source(1), false}}},
                            {0x8, {{node_source(2), false}, {31, true}}}};
    auto result = decomposition_policy::compose(nodes);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.signals, (std::vector<int>{31}));
    EXPECT_EQ(result.truth, (std::array<uint64_t, 2>{1, 0}));
    EXPECT_TRUE(decompose(result).empty());
    nodes[1].pins[1].inverted = true;
    result = decomposition_policy::compose(nodes);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.signals, (std::vector<int>{7, 31}));
    EXPECT_EQ(result.truth, (std::array<uint64_t, 2>{1, 0}));
    nodes[1].pins[1] = {ZERO, true};
    result = decomposition_policy::compose(nodes);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.signals, (std::vector<int>{31}));
    EXPECT_EQ(result.truth, (std::array<uint64_t, 2>{1, 0}));
    nodes.back() = {0xa, {{node_source(2), false}, {ZERO, true}}};
    result = decomposition_policy::compose(nodes);
    ASSERT_TRUE(result.valid);
    EXPECT_TRUE(result.signals.empty());
    EXPECT_EQ(result.truth, (std::array<uint64_t, 2>{1, 0}));
    nodes = {{0xffffffffffffaaaaULL, {{0, false}, {1, false}, {2, false}, {3, false}}},
             {0xaaaa, {{4, false}, {5, false}, {6, false}, {node_source(0), false}}},
             {0xa, {{node_source(0), false}, {node_source(1), false}}},
             {0xa, {{node_source(2), false}, {ONE, false}}}};
    result = decomposition_policy::compose(nodes);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.signals, (std::vector<int>{0}));
    EXPECT_EQ(result.truth, (std::array<uint64_t, 2>{2, 0}));
}

TEST(DecompositionPolicy, BothTruthWordsAndLastLutRowRemainExact)
{
    const std::vector<Node> nodes{{uint64_t(1) << 63, {{0, false}, {1, false}, {2, false},
                                                      {3, false}, {4, false}, {5, false}}},
                                  {0xe, {{node_source(0), false}, {6, false}}},
                                  {0xa, {{node_source(1), false}, {ZERO, false}}},
                                  {0xa, {{node_source(2), false}, {ONE, false}}}};
    const auto result = decomposition_policy::compose(nodes);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.signals, (std::vector<int>{0, 1, 2, 3, 4, 5, 6}));
    EXPECT_EQ(result.truth, (std::array<uint64_t, 2>{uint64_t(1) << 63, ~uint64_t(0)}));
    EXPECT_FALSE(truth_bit(result, 62));
    EXPECT_TRUE(truth_bit(result, 63));
    EXPECT_TRUE(truth_bit(result, 64));
    EXPECT_TRUE(truth_bit(result, 127));
    expect_candidates(result, decompose(result));
}

TEST(DecompositionPolicy, InvalidTopologyWidthsAndSourceRangesFailClosed)
{
    const auto valid = actual_nodes();
    auto nodes = valid;
    nodes.pop_back();
    EXPECT_FALSE(decomposition_policy::compose(nodes).valid);
    nodes = valid;
    nodes.push_back(valid.back());
    EXPECT_FALSE(decomposition_policy::compose(nodes).valid);
    for (unsigned index = 0; index < 4; ++index) {
        for (int source : {node_source(int(index)), node_source(int(index) + 1),
                           node_source(4), node_source(std::numeric_limits<int>::max()),
                           std::numeric_limits<int>::min(), 32, std::numeric_limits<int>::max()}) {
            nodes = valid;
            nodes[index].pins[0].source = source;
            EXPECT_FALSE(decomposition_policy::compose(nodes).valid) << index << ' ' << source;
        }
        nodes = valid;
        nodes[index].pins.resize(1);
        EXPECT_FALSE(decomposition_policy::compose(nodes).valid);
        nodes = valid;
        nodes[index].pins.resize(7, {ZERO, false});
        EXPECT_FALSE(decomposition_policy::compose(nodes).valid);
    }
}

TEST(DecompositionPolicy, EightRawSignalsAreRejectedBeforePruning)
{
    const std::vector<Node> nodes{{0xa, {{0, false}, {1, false}, {2, false}, {3, false}, {4, false}, {5, false}}},
                                  {0x8, {{6, false}, {7, false}}},
                                  {0xa, {{node_source(0), false}, {node_source(1), false}}},
                                  {0xa, {{node_source(2), false}, {ZERO, false}}}};
    EXPECT_FALSE(decomposition_policy::compose(nodes).valid);
}

TEST(DecompositionPolicy, InvalidAndNonessentialSevenSignalResultsAreRejected)
{
    auto function = function_table(actual_oracle);
    function.valid = false;
    EXPECT_TRUE(decompose(function).empty());
    function = function_table(actual_oracle);
    function.signals.pop_back();
    EXPECT_TRUE(decompose(function).empty());
    function = function_table([](unsigned row) { return row & 1u; });
    EXPECT_TRUE(decompose(function).empty());
    for (const auto &signals : {std::vector<int>{0, 1, 2, 3, 4, 5, 32},
                                std::vector<int>{0, 1, 2, 3, 4, 5, 5},
                                std::vector<int>{0, 2, 1, 3, 4, 5, 6},
                                std::vector<int>{-1, 1, 2, 3, 4, 5, 6}}) {
        function = function_table(actual_oracle);
        function.signals = signals;
        EXPECT_TRUE(decompose(function).empty());
    }
}

TEST(DecompositionPolicy, MoreThanFourColumnsRejectsThatPartition)
{
    const auto function = function_table([](unsigned row) {
        const unsigned selector = (row >> 4) & 3u;
        return bool((row >> selector) & 1u) != bool(row & 64u);
    });
    const auto independent = columns(function, {0, 1, 2, 3});
    EXPECT_EQ(std::set<unsigned>(independent.begin(), independent.end()).size(), 16u);
    const auto candidates = decompose(function);
    expect_candidates(function, candidates);
    for (const auto &candidate : candidates) EXPECT_NE(candidate.bound, (std::vector<int>{0, 1, 2, 3}));
}

TEST(DecompositionPolicy, IndependentHardTruthTableHasNoAdmissiblePartition)
{
    Result function{true, {0x91cba897f03d625eULL, 0xe6b32d0af974581cULL}, {0, 1, 2, 3, 4, 5, 6}};
    for (const auto &bound : partitions()) {
        const auto independent = columns(function, bound);
        ASSERT_GT(std::set<unsigned>(independent.begin(), independent.end()).size(), 4u);
    }
    EXPECT_TRUE(decompose(function).empty());
}

TEST(DecompositionPolicy, OneInputEncoderIsExcludedButOtherEncodingsRemain)
{
    const auto function = function_table([](unsigned row) {
        const bool selected = (row & 16u) ? bool(row & 1u) : ((row & 14u) == 14u);
        return selected != (bool(row & 32u) != bool(row & 64u));
    });
    const auto candidates = decompose(function);
    ASSERT_FALSE(candidates.empty());
    expect_candidates(function, candidates);
    bool bound_retained = false;
    for (const auto &candidate : candidates)
        if (candidate.bound == std::vector<int>{0, 1, 2, 3}) {
            bound_retained = true;
            for (const auto &encoder : candidate.encoders) EXPECT_GE(encoder.signals.size(), 2u);
        }
    EXPECT_TRUE(bound_retained);
}

TEST(DecompositionPolicy, DeterministicRandomDecomposableFunctionsRecomposeIndependently)
{
    uint64_t state = 0x624ae921377bf05dULL;
    unsigned exercised = 0;
    for (unsigned trial = 0; trial < 24; ++trial) {
        const uint64_t u = random_word(state) & 0xffffu;
        const uint64_t v = random_word(state) & 0xffffu;
        const uint64_t root = random_word(state) & 0xffffffffu;
        const std::vector<Node> nodes{{u, {{0, false}, {1, false}, {2, false}, {3, false}}},
                                      {v, {{0, false}, {1, false}, {2, false}, {3, false}}},
                                      {root, {{4, false}, {5, false}, {6, false},
                                              {node_source(0), false}, {node_source(1), false}}},
                                      {0xa, {{node_source(2), false}, {ONE, true}}}};
        const auto composed = decomposition_policy::compose(nodes);
        ASSERT_TRUE(composed.valid);
        for (unsigned row = 0; row < 128; ++row) {
            unsigned compact = 0;
            for (unsigned index = 0; index < composed.signals.size(); ++index)
                if (row & (1u << composed.signals[index])) compact += 1u << index;
            const unsigned code = unsigned((u >> (row & 15u)) & 1u) +
                                  2u * unsigned((v >> (row & 15u)) & 1u);
            EXPECT_EQ(truth_bit(composed, compact), bool((root >> (((row >> 4) & 7u) + 8u * code)) & 1u));
        }
        if (composed.signals.size() != 7) {
            EXPECT_TRUE(decompose(composed).empty());
            continue;
        }
        ++exercised;
        const auto first = decompose(composed), second = decompose(composed);
        ASSERT_FALSE(first.empty());
        expect_candidates(composed, first);
        ASSERT_EQ(first.size(), second.size());
        for (unsigned index = 0; index < first.size(); ++index) {
            EXPECT_EQ(signature(first[index]), signature(second[index]));
            EXPECT_EQ(first[index].bound, second[index].bound);
            EXPECT_EQ(first[index].free, second[index].free);
            EXPECT_EQ(first[index].column_classes, second[index].column_classes);
            EXPECT_EQ(first[index].class_codes, second[index].class_codes);
        }
    }
    EXPECT_GT(exercised, 0u);
}

TEST(DecompositionPolicy, DeterministicArbitraryTablesRespectGlobalBoundAndDeduplication)
{
    uint64_t state = 0x1589a0f379e4dc21ULL;
    for (unsigned trial = 0; trial < 16; ++trial) {
        Result function{true, {random_word(state), random_word(state)}, {0, 1, 2, 3, 4, 5, 6}};
        const auto candidates = decompose(function);
        expect_candidates(function, candidates);
        const auto again = decompose(function);
        ASSERT_EQ(candidates.size(), again.size());
        for (unsigned index = 0; index < candidates.size(); ++index)
            EXPECT_EQ(signature(candidates[index]), signature(again[index]));
    }
}
