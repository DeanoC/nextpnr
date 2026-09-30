#include "gtest/gtest.h"
#include "local_remap_cut_policy.h"
#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <set>
#include <vector>

using namespace local_remap_cut_policy;

namespace {
// Evaluate the root recursively from raw external values, independently of the
// composer's topological table and reduced truth-row encoding.
bool evaluate(const std::vector<Node> &nodes, const std::array<bool, 32> &inputs, std::size_t index)
{
    const auto &node = nodes[index];
    unsigned row = 0;
    for (std::size_t pin = 0; pin < node.pins.size(); ++pin) {
        const auto &input = node.pins[pin];
        bool value;
        if (input.source >= 0) value = inputs[input.source];
        else if (input.source == ZERO) value = false;
        else if (input.source == ONE) value = true;
        else value = evaluate(nodes, inputs, std::size_t(-3 - input.source));
        if (value != input.inverted) row |= 1u << pin;
    }
    return (node.mask >> row) & 1u;
}

void expect_equivalent(const std::vector<Node> &nodes)
{
    const auto result = compose(nodes);
    ASSERT_TRUE(result.valid);
    std::set<int> external;
    for (const auto &node : nodes)
        for (const auto &pin : node.pins)
            if (pin.source >= 0) external.insert(pin.source);
    const std::vector<int> signals(external.begin(), external.end());
    std::vector<int> essential;
    for (std::size_t bit = 0; bit < signals.size(); ++bit) {
        bool affects_output = false;
        for (unsigned row = 0; row < (1u << signals.size()); ++row) {
            std::array<bool, 32> inputs{};
            for (std::size_t index = 0; index < signals.size(); ++index)
                inputs[signals[index]] = (row >> index) & 1u;
            const bool before = evaluate(nodes, inputs, nodes.size() - 1);
            inputs[signals[bit]] = !inputs[signals[bit]];
            affects_output |= before != evaluate(nodes, inputs, nodes.size() - 1);
        }
        if (affects_output) essential.push_back(signals[bit]);
    }
    EXPECT_EQ(result.signals, essential);
    for (unsigned row = 0; row < (1u << signals.size()); ++row) {
        std::array<bool, 32> inputs{};
        for (std::size_t index = 0; index < signals.size(); ++index)
            inputs[signals[index]] = (row >> index) & 1u;
        unsigned reduced_row = 0;
        for (std::size_t index = 0; index < result.signals.size(); ++index)
            reduced_row |= unsigned(inputs[result.signals[index]]) << index;
        EXPECT_EQ((result.mask >> reduced_row) & 1u, evaluate(nodes, inputs, nodes.size() - 1))
                << "assignment " << row;
    }
    if (result.signals.size() < 6) {
        EXPECT_EQ(result.mask >> (1u << result.signals.size()), 0u);
    }
}
}

TEST(LocalRemapCut, ComposesThreeNodeAsymmetricFunction)
{
    const std::vector<Node> nodes = {
        {0x8, {{0,false},{4,false}}},
        {0x2, {{2,false},{node_source(0),false}}},
        {0xe400, {{0,false},{3,false},{1,false},{node_source(1),false}}},
    };
    const auto result = compose(nodes);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.signals, (std::vector<int>{0,1,2,3,4}));
    EXPECT_EQ(result.mask, 0x5000d080u);
    const std::vector<unsigned> expected_pairs = {4,2,6,4,2};
    for (unsigned bit = 0; bit < 5; ++bit) {
        unsigned pairs = 0;
        for (unsigned row = 0; row < 32; ++row) {
            if (!(row & (1u << bit)))
                pairs += ((result.mask >> row) ^ (result.mask >> (row | (1u << bit)))) & 1u;
        }
        EXPECT_EQ(pairs, expected_pairs[bit]);
    }
    expect_equivalent(nodes);
}

TEST(LocalRemapCut, ExhaustiveTwoInputTablesAndPinPolarities)
{
    for (uint64_t inner = 0; inner < 16; ++inner)
        for (uint64_t outer = 0; outer < 16; ++outer)
            for (unsigned polarity = 0; polarity < 16; ++polarity) {
                const std::vector<Node> nodes = {
                    {inner, {{9,bool(polarity & 1)},{2,bool(polarity & 2)}}},
                    {outer, {{node_source(0),bool(polarity & 4)},{31,bool(polarity & 8)}}},
                };
                SCOPED_TRACE(::testing::Message() << "inner " << inner << ", outer " << outer
                                                << ", polarity " << polarity);
                expect_equivalent(nodes);
            }
}

TEST(LocalRemapCut, ExhaustiveAssignmentsForRandomTwoAndThreeNodeCuts)
{
    std::mt19937_64 random(0x6c75745f637574ULL);
    const std::vector<int> ids = {0,1,4,9,16,31};
    for (unsigned signal_count = 2; signal_count <= 6; ++signal_count)
        for (unsigned sample = 0; sample < 128; ++sample) {
            const unsigned node_count = 2 + sample % 2;
            std::vector<Node> nodes;
            for (unsigned node = 0; node < node_count; ++node) {
                Node value{random(), {}};
                const unsigned width = 2 + unsigned(random() % 5);
                for (unsigned pin = 0; pin < width; ++pin) {
                    int source;
                    if (node && pin == 0) source = node_source(int(node - 1));
                    else {
                        const unsigned choice = unsigned(random() % (signal_count + 2 + node));
                        if (choice < signal_count) source = ids[choice];
                        else if (choice == signal_count) source = ZERO;
                        else if (choice == signal_count + 1) source = ONE;
                        else source = node_source(int(choice - signal_count - 2));
                    }
                    value.pins.push_back({source, bool(random() & 1u)});
                }
                nodes.push_back(value);
            }
            SCOPED_TRACE(::testing::Message() << "signals " << signal_count << ", sample " << sample);
            expect_equivalent(nodes);
        }
}

TEST(LocalRemapCut, ResolvesSharedAliasesInversionsAndConstants)
{
    // (!a XOR 1) AND !a is zero; the final LUT inverts it and ORs with !0.
    const std::vector<Node> constant = {
        {0x6, {{7,true},{ONE,false}}},
        {0x8, {{node_source(0),true},{7,false}}},
        {0xe, {{node_source(1),true},{ZERO,true}}},
    };
    const auto one = compose(constant);
    ASSERT_TRUE(one.valid);
    EXPECT_TRUE(one.signals.empty());
    EXPECT_EQ(one.mask, 1u);
    expect_equivalent(constant);
    // The aliased first LUT is a & !a. The root reduces to !b.
    const std::vector<Node> single = {
        {0x8, {{31,false},{31,true}}},
        {0xe, {{node_source(0),false},{4,true}}},
    };
    const auto inverted = compose(single);
    ASSERT_TRUE(inverted.valid);
    EXPECT_EQ(inverted.signals, (std::vector<int>{4}));
    EXPECT_EQ(inverted.mask, 1u);
    expect_equivalent(single);
    const auto zero = compose({{0xf, {{ZERO,false},{ONE,true}}},
                               {0x8, {{node_source(0),false},{ZERO,false}}}});
    ASSERT_TRUE(zero.valid);
    EXPECT_TRUE(zero.signals.empty());
    EXPECT_EQ(zero.mask, 0u);
}

TEST(LocalRemapCut, PrunesMiddleInputsAndRepacksSortedTruthRows)
{
    // The first LUT ignores its second pin; the root is a XOR c.
    const std::vector<Node> nodes = {
        {0xa, {{2,false},{9,false}}},
        {0x6, {{node_source(0),false},{31,false}}},
    };
    const auto result = compose(nodes);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.signals, (std::vector<int>{2,31}));
    EXPECT_EQ(result.mask, 0x6u);
    expect_equivalent(nodes);
    // High bits above a small LUT's active rows are not part of its function.
    const auto high = compose({{std::numeric_limits<uint64_t>::max(), {{ZERO,false},{ONE,false}}},
                               {0x8, {{node_source(0),false},{4,false}}}});
    ASSERT_TRUE(high.valid);
    EXPECT_EQ(high.signals, (std::vector<int>{4}));
    EXPECT_EQ(high.mask, 0x2u);
}

TEST(LocalRemapCut, ComposesParallelNodesAndPrunesReconvergentAliases)
{
    const std::vector<Node> nodes = {
        {0x6, {{9,true},{2,false}}},
        {0xe, {{0,false},{31,true}}},
        {0x96, {{node_source(0),false},{node_source(1),true},{node_source(0),true}}},
    };
    // x XOR !y XOR !x = y; both inputs of the first node disappear.
    const auto result = compose(nodes);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.signals, (std::vector<int>{0,31}));
    EXPECT_EQ(result.mask, 0xbu);
    expect_equivalent(nodes);
}

TEST(LocalRemapCut, PreservesSixInputBit63AndRejectsOversizeSupportBeforePruning)
{
    const std::vector<Node> nodes = {
        {uint64_t(1) << 63, {{0,false},{1,false},{4,false},{9,false},{16,false},{31,false}}},
        {0x8, {{node_source(0),false},{ONE,false}}},
    };
    const auto result = compose(nodes);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.signals, (std::vector<int>{0,1,4,9,16,31}));
    EXPECT_EQ(result.mask, uint64_t(1) << 63);
    expect_equivalent(nodes);
    const auto oversized = compose({
        {0, {{0,false},{1,false},{2,false},{3,false},{4,false},{5,false}}},
        {0, {{node_source(0),false},{6,false}}},
    });
    EXPECT_FALSE(oversized.valid);
    EXPECT_TRUE(oversized.signals.empty());
    EXPECT_EQ(oversized.mask, 0u);
}

TEST(LocalRemapCut, RejectsMalformedNodesReferencesAndExternalBounds)
{
    const Node first{0x8, {{0,false},{1,false}}};
    const Node root{0x8, {{node_source(0),false},{2,false}}};
    for (const auto &nodes : std::vector<std::vector<Node>>{
             {}, {first}, {first,root,root,root},
             {{0,{}},root}, {{0,{{0,false}}},root},
             {{0,std::vector<Pin>(7, {0,false})},root},
             {first,{0,{}}}, {first,{0,{{0,false}}}},
             {first,{0,std::vector<Pin>(7, {0,false})}},
         }) {
        const auto result = compose(nodes);
        EXPECT_FALSE(result.valid);
        EXPECT_TRUE(result.signals.empty());
        EXPECT_EQ(result.mask, 0u);
    }
    for (int source : {32, std::numeric_limits<int>::max(), std::numeric_limits<int>::min(),
                       node_source(0), node_source(1), node_source(2), node_source(99)}) {
        const auto result = compose({{0x8, {{source,false},{0,false}}},root});
        EXPECT_FALSE(result.valid) << "first-node source " << source;
    }
    for (int source : {32, std::numeric_limits<int>::max(), std::numeric_limits<int>::min(),
                       node_source(1), node_source(2), node_source(99)}) {
        const auto result = compose({first,{0x8, {{source,false},{2,false}}}});
        EXPECT_FALSE(result.valid) << "second-node source " << source;
    }
    for (int source : {node_source(2), node_source(99), std::numeric_limits<int>::min()})
        EXPECT_FALSE(compose({first,root,{0x8, {{source,false},{3,false}}}}).valid);
    // A cycle is invalid even when the last node could otherwise be evaluated.
    EXPECT_FALSE(compose({{0x8, {{node_source(1),false},{0,false}}},root}).valid);
    EXPECT_EQ(node_source(-1), std::numeric_limits<int>::min());
    EXPECT_EQ(node_source(std::numeric_limits<int>::min()), std::numeric_limits<int>::min());
    EXPECT_EQ(node_source(std::numeric_limits<int>::max()), std::numeric_limits<int>::min());
}
