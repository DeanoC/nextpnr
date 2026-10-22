/* Exhaustive bounded private two-node truth decomposition. SPDX-License-Identifier: ISC */
#include "private_pair_decomposition.h"
#include "gtest/gtest.h"
#include <random>

namespace {
namespace p = private_pair_decomposition;
p::Pair mda()
{
    return {{{{0,false},{1,false},{2,false}},0x08},
            {{{3,false},{4,false},{5,false},{6,false},{p::INTERNAL,false}},0x1554bffe}};
}
TEST(PrivatePairDecomposition, ExhaustiveMdaLateInputsReachRoot)
{
    auto original = mda(); auto candidates = p::decompose(original);
    ASSERT_FALSE(candidates.empty()); EXPECT_LE(candidates.size(),p::MAX_CANDIDATES);
    bool bypass = false;
    for (const auto &candidate : candidates) {
        EXPECT_TRUE(reduction_reassociate::equivalent(original,candidate.network));
        bool late_at_root = true;
        for (int late = 0; late < 3; ++late) {
            bool present = false;
            for (auto pin : candidate.network.consumer.pins) present |= pin.signal == late;
            late_at_root &= present;
        }
        bypass |= late_at_root;
    }
    EXPECT_TRUE(bypass);
    auto again = p::decompose(original); ASSERT_EQ(again.size(),candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        EXPECT_EQ(candidates[i].bound,again[i].bound);
        EXPECT_EQ(candidates[i].network.producer.mask,again[i].network.producer.mask);
        EXPECT_EQ(candidates[i].network.consumer.mask,again[i].network.consumer.mask);
    }
}
TEST(PrivatePairDecomposition, ComplementsInversionsAndSparseSignalIds)
{
    auto original = mda();
    const int remap[] = {104,8,51,993,0,28,74};
    for (auto *lut : {&original.producer,&original.consumer})
        for (auto &pin : lut->pins) if (pin.signal >= 0) {pin.inverted = (pin.signal % 2) != 0;pin.signal = remap[pin.signal];}
    auto candidates = p::decompose(original); ASSERT_FALSE(candidates.empty());
    ASSERT_EQ(candidates.size()%2,0);
    for (size_t i = 0; i < candidates.size(); i += 2) {
        EXPECT_EQ(candidates[i].bound,candidates[i+1].bound);
        EXPECT_EQ(candidates[i].network.producer.mask ^ candidates[i+1].network.producer.mask,0xffu);
        EXPECT_TRUE(reduction_reassociate::equivalent(original,candidates[i].network));
        EXPECT_TRUE(reduction_reassociate::equivalent(original,candidates[i+1].network));
    }
}
TEST(PrivatePairDecomposition, RejectsConstantsAliasesInessentialAndMalformedPairs)
{
    auto original = mda();
    auto bad = original;bad.consumer.pins[0].signal = 0;EXPECT_TRUE(p::decompose(bad).empty());
    bad = original;bad.producer.pins[0].signal = reduction_reassociate::ZERO;EXPECT_TRUE(p::decompose(bad).empty());
    bad = original;bad.producer.mask = 0;EXPECT_TRUE(p::decompose(bad).empty());
    bad = original;bad.consumer.mask = 0xffff;EXPECT_TRUE(p::decompose(bad).empty());
    bad = original;bad.consumer.pins.back().signal = 7;EXPECT_TRUE(p::decompose(bad).empty());
    bad = original;bad.producer.pins[0].signal = p::INTERNAL;EXPECT_TRUE(p::decompose(bad).empty());
    bad = original;bad.producer.pins.pop_back();EXPECT_TRUE(p::decompose(bad).empty());
    bad = original;bad.producer.mask |= uint64_t(1)<<8;EXPECT_TRUE(p::decompose(bad).empty());
}
TEST(PrivatePairDecomposition, DeterministicRandomTruthTablesPreserveEveryRow)
{
    std::mt19937 random(173);
    unsigned checked = 0;
    for (int trial = 0; trial < 128; ++trial) {
        auto original = mda();original.producer.mask = random()&255;original.consumer.mask = random();
        for (auto *lut : {&original.producer,&original.consumer})
            for (auto &pin : lut->pins) pin.inverted = (random()&1) != 0;
        auto candidates = p::decompose(original);EXPECT_LE(candidates.size(),p::MAX_CANDIDATES);
        for (const auto &candidate : candidates) {
            EXPECT_TRUE(reduction_reassociate::equivalent(original,candidate.network));++checked;
        }
    }
    EXPECT_GT(checked,100u);
}
} // namespace
