/* Independent factor reassociation proofs. SPDX-License-Identifier: ISC */
#include "reduction_reassociate.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <vector>

namespace {
namespace policy = reduction_reassociate;
using policy::Candidate;
using policy::Lut;
using policy::Pair;
using policy::Pin;

bool oracle_lut(const Lut &lut, const std::map<int, bool> &values, bool producer)
{
    unsigned row = 0;
    for (unsigned index = 0; index < lut.pins.size(); ++index) {
        const Pin &pin = lut.pins[index];
        bool value = false;
        if (pin.signal == policy::ONE) value = true;
        else if (pin.signal == policy::INTERNAL) value = producer;
        else if (pin.signal >= 0) value = values.at(pin.signal);
        value ^= pin.inverted;
        if (value) row |= 1u << index;
    }
    return (lut.mask & (uint64_t(1) << row)) != 0;
}
bool oracle_pair(const Pair &pair, const std::map<int, bool> &values)
{
    return oracle_lut(pair.consumer, values, oracle_lut(pair.producer, values, false));
}
void independent_proof(const Pair &before, const Pair &after)
{
    std::set<int> sources;
    for (auto *pair : {&before, &after})
        for (auto *lut : {&pair->producer, &pair->consumer})
            for (auto pin : lut->pins) if (pin.signal >= 0) sources.insert(pin.signal);
    ASSERT_LE(sources.size(), 11u);
    for (unsigned row = 0; row < (1u << sources.size()); ++row) {
        std::map<int, bool> values;
        unsigned index = 0;
        for (int source : sources) values[source] = (row & (1u << index++)) != 0;
        ASSERT_EQ(oracle_pair(before, values), oracle_pair(after, values)) << "assignment=" << row;
    }
}
Pair actual_pair()
{
    // Frozen PC/XT critical reduction, signal names replaced by plain IDs.
    // P = F & ~A & ~B & ~C & ~D & ~E; C = E & D & ~A & ~B & ~C.
    return {{{{0, false}, {1, false}, {2, false}, {3, false}, {4, false}, {5, false}}, uint64_t(1) << 32},
            {{{6, false}, {7, false}, {8, false}, {9, false}, {policy::INTERNAL, false}}, uint64_t(1) << 24}};
}
const Candidate *find_signal(const std::vector<Candidate> &candidates, int signal)
{
    auto it = std::find_if(candidates.begin(), candidates.end(),
                           [&](const Candidate &c) { return c.promoted_signal == signal; });
    return it == candidates.end() ? nullptr : &*it;
}
} // namespace

TEST(ReductionReassociate, CriticalReductionMovesRemoteFactorIntoSpareInput)
{
    const auto original = actual_pair();
    const auto candidates = policy::reassociate(original);
    ASSERT_EQ(candidates.size(), 6u);
    const auto *candidate = find_signal(candidates, 0);
    ASSERT_NE(candidate, nullptr);
    EXPECT_EQ(candidate->promoted_pin, 0u);
    EXPECT_EQ(candidate->consumer_pin, 5u);
    EXPECT_EQ(candidate->network.producer.pins.size(), 5u);
    EXPECT_EQ(candidate->network.consumer.pins.size(), 6u);
    EXPECT_EQ(candidate->network.producer.mask, uint64_t(1) << 16);
    EXPECT_EQ(candidate->network.consumer.mask, uint64_t(1) << 24);
    EXPECT_TRUE(std::none_of(candidate->network.producer.pins.begin(), candidate->network.producer.pins.end(),
                             [](const Pin &p) { return p.signal == 0; }));
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        EXPECT_EQ(candidates[index].promoted_pin, index);
        EXPECT_TRUE(policy::equivalent(original, candidates[index].network));
        independent_proof(original, candidates[index].network);
    }
}

TEST(ReductionReassociate, PinAndLinkInversionsSparseSignalsAndConstantInputs)
{
    auto original = actual_pair();
    const std::array<int, 10> ids{{2, 5, 9, 23, 31, 47, 103, 257, 511, std::numeric_limits<int>::max()}};
    for (Lut *lut : {&original.producer, &original.consumer})
        for (std::size_t index = 0; index < lut->pins.size(); ++index) {
            auto &pin = lut->pins[index];
            if (pin.signal >= 0) pin.signal = ids.at(pin.signal);
            pin.inverted = (index % 2) == 0;
        }
    original.producer.pins[1] = {policy::ONE, true};
    original.consumer.pins[1] = {policy::ZERO, false};
    ASSERT_TRUE(policy::valid(original));
    const auto candidates = policy::reassociate(original);
    ASSERT_EQ(candidates.size(), 5u);
    for (const auto &candidate : candidates) independent_proof(original, candidate.network);
}

TEST(ReductionReassociate, ExistingInvertedAliasReusesFullConsumerInput)
{
    auto original = actual_pair();
    original.consumer.pins[0] = {0, true};
    original.consumer.pins.push_back({10, false});
    original.consumer.mask = (uint64_t(1) << 25) | (uint64_t(1) << 57);
    original.consumer.pins[4].inverted = true;
    const auto candidates = policy::reassociate(original);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].promoted_signal, 0);
    EXPECT_EQ(candidates[0].consumer_pin, 0u);
    EXPECT_EQ(candidates[0].network.consumer.pins.size(), 6u);
    independent_proof(original, candidates[0].network);
}

TEST(ReductionReassociate, TiedAndInvertedDuplicateSignalsStayCorrelated)
{
    auto original = actual_pair();
    original.producer.pins[2] = original.producer.pins[1];
    original.producer.pins[2].inverted = true;
    original.producer.mask = uint64_t(1) << 36;
    original.consumer.pins[0] = original.consumer.pins[1];
    original.consumer.pins[0].inverted = true;
    original.consumer.mask = uint64_t(1) << 25;
    // Both tied pairs have opposite logical values on a reachable true row;
    // the proof therefore exercises correlation without a constant function.
    EXPECT_TRUE(oracle_pair(original, {{0, false}, {1, false}, {3, false}, {4, false},
                                       {5, true}, {7, false}, {8, false}, {9, true}}));
    auto candidates = policy::reassociate(original);
    ASSERT_EQ(candidates.size(), 4u);
    EXPECT_EQ(find_signal(candidates, 1), nullptr);
    for (const auto &candidate : candidates) independent_proof(original, candidate.network);
}

TEST(ReductionReassociate, ConstantOneCofactorAndArbitraryConsumerFunction)
{
    // Producer = A | (B ^ C), consumer = (internal & D) ^ E.
    Pair original{{{{0, false}, {1, false}, {2, false}}, 0xbe},
                  {{{policy::INTERNAL, false}, {3, false}, {4, false}}, 0x78}};
    auto candidates = policy::reassociate(original);
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].promoted_signal, 0);
    EXPECT_EQ(candidates[0].network.producer.mask, 0x6u);
    independent_proof(original, candidates[0].network);
}

TEST(ReductionReassociate, ExhaustiveSmallTruthTablesCoverAllCofactorPolarities)
{
    unsigned checked = 0;
    for (unsigned producer = 0; producer < 256; ++producer)
        for (unsigned consumer = 0; consumer < 16; ++consumer) {
            Pair original{{{{0, false}, {1, false}, {2, false}}, producer},
                          {{{policy::INTERNAL, (consumer & 1u) != 0}, {3, true}}, consumer}};
            for (const auto &candidate : policy::reassociate(original)) {
                independent_proof(original, candidate.network);
                ++checked;
            }
        }
    EXPECT_GT(checked, 1000u);
}

TEST(ReductionReassociate, HighestTruthTableBitAndElevenDistinctSourcesAreExact)
{
    auto original = actual_pair();
    original.producer.mask = uint64_t(1) << 63;
    original.consumer.mask = uint64_t(1) << 31;
    const auto candidates = policy::reassociate(original);
    ASSERT_EQ(candidates.size(), 6u);
    for (const auto &candidate : candidates) {
        EXPECT_EQ(candidate.network.producer.mask, uint64_t(1) << 31);
        EXPECT_EQ(candidate.network.consumer.mask, uint64_t(1) << 63);
        independent_proof(original, candidate.network);
    }
    // A six-input consumer with an unrelated eleventh raw source is valid,
    // but has no free physical input for a factor from the producer.
    original.consumer.pins.push_back({10, false});
    original.consumer.mask = uint64_t(1) << 63;
    EXPECT_TRUE(policy::valid(original));
    EXPECT_TRUE(policy::equivalent(original, original));
    EXPECT_TRUE(policy::reassociate(original).empty());
    independent_proof(original, original);
}

TEST(ReductionReassociate, VerifierRejectsFunctionMutationAndNewUnboundedInputs)
{
    const auto original = actual_pair();
    auto candidates = policy::reassociate(original);
    ASSERT_FALSE(candidates.empty());
    auto broken = candidates.front().network;
    broken.consumer.mask ^= 1;
    EXPECT_FALSE(policy::equivalent(original, broken));
    broken = candidates.front().network;
    broken.producer.mask ^= uint64_t(1) << 16;
    EXPECT_FALSE(policy::equivalent(original, broken));
    broken = original;
    for (auto *lut : {&broken.producer, &broken.consumer})
        for (auto &pin : lut->pins) if (pin.signal >= 0) pin.signal += 100;
    EXPECT_FALSE(policy::equivalent(original, broken));
}

TEST(ReductionReassociate, RejectsMalformedOrUnsupportedPairBeforeProof)
{
    const auto original = actual_pair();
    std::vector<Pair> bad;
    bad.push_back(original); bad.back().producer.pins[0].signal = policy::INTERNAL;
    bad.push_back(original); bad.back().consumer.pins[0].signal = policy::INTERNAL;
    bad.push_back(original); bad.back().consumer.pins[4].signal = 20;
    bad.push_back(original); bad.back().producer.pins[0].signal = -4;
    bad.push_back(original); bad.back().consumer.mask |= uint64_t(1) << 40;
    bad.push_back(original); bad.back().producer.pins.clear();
    bad.push_back(original); bad.back().consumer.pins.push_back({20, false}); bad.back().consumer.pins.push_back({21, false});
    for (const auto &pair : bad) {
        EXPECT_FALSE(policy::valid(pair));
        EXPECT_FALSE(policy::equivalent(pair, pair));
        EXPECT_TRUE(policy::reassociate(pair).empty());
    }
    auto small = original;
    small.producer = {{{0, false}, {1, false}}, 0x8};
    EXPECT_TRUE(policy::valid(small));
    EXPECT_TRUE(policy::reassociate(small).empty());
    auto full = original;
    full.consumer.pins.push_back({10, false});
    EXPECT_TRUE(policy::valid(full));
    EXPECT_TRUE(policy::reassociate(full).empty());
    auto parity = original;
    parity.producer = {{{0, false}, {1, false}, {2, false}}, 0x96};
    EXPECT_TRUE(policy::reassociate(parity).empty());
}
