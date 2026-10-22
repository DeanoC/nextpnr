/* Exact private ALUT3 -> ALUT5 disjoint decomposition. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_PRIVATE_PAIR_DECOMPOSITION_H
#define MISTRAL_PRIVATE_PAIR_DECOMPOSITION_H
#include "reduction_reassociate.h"
#include <array>
#include <set>
namespace private_pair_decomposition {
using reduction_reassociate::Pair;
using reduction_reassociate::Lut;
using reduction_reassociate::Pin;
using reduction_reassociate::INTERNAL;
struct Candidate { Pair network; std::array<int, 3> bound; };
constexpr size_t MAX_CANDIDATES = 70;
// Exactly seven distinct essential external signals, each used once. Inversions
// are interpreted before composition; constants and aliases cannot meet this
// first-stage scope. No reachable-state assumptions enter the proof.
inline std::vector<Candidate> decompose(const Pair &before)
{
    namespace old = reduction_reassociate;
    if (!old::valid(before) || before.producer.pins.size() != 3 || before.consumer.pins.size() != 5) return {};
    std::vector<int> signals;
    for (const auto *lut : {&before.producer, &before.consumer})
        for (auto pin : lut->pins) {
            if (pin.signal == INTERNAL) continue;
            if (pin.signal < 0) return {};
            signals.push_back(pin.signal);
        }
    std::sort(signals.begin(), signals.end());
    if (signals.size() != 7 || std::adjacent_find(signals.begin(), signals.end()) != signals.end()) return {};
    std::array<bool, 128> truth{};
    for (unsigned row = 0; row < 128; ++row) truth[row] = old::detail::evaluate(before, signals, row);
    for (unsigned pin = 0; pin < 7; ++pin) {
        bool essential = false;
        for (unsigned row = 0; row < 128; ++row) essential |= truth[row] != truth[row ^ (1u << pin)];
        if (!essential) return {};
    }
    std::vector<Candidate> result;
    for (int a = 0; a < 5; ++a) for (int b = a + 1; b < 6; ++b) for (int c = b + 1; c < 7; ++c) {
        const std::array<int, 3> bound{a, b, c};
        std::vector<int> free;
        for (int i = 0; i < 7; ++i) if (i != a && i != b && i != c) free.push_back(i);
        std::array<uint16_t, 8> columns{};
        for (unsigned x = 0; x < 8; ++x) for (unsigned y = 0; y < 16; ++y) {
            unsigned row = 0;
            for (unsigned i = 0; i < 3; ++i) row |= ((x >> i) & 1u) << bound[i];
            for (unsigned i = 0; i < 4; ++i) row |= ((y >> i) & 1u) << free[i];
            columns[x] |= uint16_t(truth[row]) << y;
        }
        std::set<uint16_t> distinct(columns.begin(), columns.end());
        if (distinct.size() != 2) continue;
        const std::array<uint16_t, 2> classes{*distinct.begin(), *distinct.rbegin()};
        for (unsigned complement = 0; complement < 2; ++complement) {
            Candidate candidate;
            for (unsigned i = 0; i < 3; ++i) {
                candidate.bound[i] = signals[bound[i]];
                candidate.network.producer.pins.push_back({signals[bound[i]], false});
            }
            for (int i : free) candidate.network.consumer.pins.push_back({signals[i], false});
            candidate.network.consumer.pins.push_back({INTERNAL, false});
            for (unsigned x = 0; x < 8; ++x)
                candidate.network.producer.mask |= uint64_t(unsigned(columns[x] == classes[1]) ^ complement) << x;
            for (unsigned code = 0; code < 2; ++code)
                candidate.network.consumer.mask |= uint64_t(classes[code ^ complement]) << (16 * code);
            if (!old::equivalent(before, candidate.network)) return {};
            result.push_back(std::move(candidate));
        }
    }
    return result;
}
} // namespace private_pair_decomposition
#endif
