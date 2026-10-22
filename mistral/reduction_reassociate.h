/* Exact two-LUT factor reassociation policy. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_REDUCTION_REASSOCIATE_H
#define MISTRAL_REDUCTION_REASSOCIATE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace reduction_reassociate {
constexpr int ZERO = -1;
constexpr int ONE = -2;
constexpr int INTERNAL = -3;
constexpr std::size_t MAX_EXTERNAL_SIGNALS = 11;

// Nonnegative signal IDs identify actual external nets, including aliases.
// INTERNAL is the producer's raw Q and is allowed once in the consumer only.
// Pin inversion is applied before indexing the LUT's logical truth table.
struct Pin {
    int signal;
    bool inverted = false;
};
struct Lut {
    std::vector<Pin> pins;
    uint64_t mask = 0;
};
struct Pair {
    Lut producer;
    Lut consumer;
};
struct Candidate {
    Pair network;
    std::size_t promoted_pin = 0; // Original producer logical pin.
    std::size_t consumer_pin = 0; // New/reused consumer pin for this external net.
    int promoted_signal = -1;
};

namespace detail {
inline uint64_t full_mask(std::size_t width)
{
    return width == 6 ? ~uint64_t(0) : (uint64_t(1) << (1u << width)) - 1;
}
inline bool valid_lut(const Lut &lut, bool consumer)
{
    if (lut.pins.size() < 2 || lut.pins.size() > 6 || (lut.mask & ~full_mask(lut.pins.size())))
        return false;
    unsigned links = 0;
    for (const auto &pin : lut.pins) {
        if (pin.signal == INTERNAL) ++links;
        else if (pin.signal < 0 && pin.signal != ZERO && pin.signal != ONE) return false;
    }
    return links == (consumer ? 1u : 0u);
}
inline void append_signals(const Pair &pair, std::vector<int> &signals)
{
    for (const Lut *lut : {&pair.producer, &pair.consumer})
        for (const auto &pin : lut->pins)
            if (pin.signal >= 0) signals.push_back(pin.signal);
}
inline void unique_signals(std::vector<int> &signals)
{
    std::sort(signals.begin(), signals.end());
    signals.erase(std::unique(signals.begin(), signals.end()), signals.end());
}
inline bool evaluate(const Lut &lut, const std::vector<int> &signals, unsigned assignment, bool internal)
{
    unsigned row = 0;
    for (std::size_t pin = 0; pin < lut.pins.size(); ++pin) {
        const auto &source = lut.pins[pin];
        bool value = source.signal == INTERNAL ? internal : source.signal == ONE;
        if (source.signal >= 0) {
            auto found = std::lower_bound(signals.begin(), signals.end(), source.signal);
            value = (assignment >> std::size_t(found - signals.begin())) & 1u;
        }
        if (value != source.inverted) row |= 1u << pin;
    }
    return (lut.mask >> row) & 1u;
}
inline bool evaluate(const Pair &pair, const std::vector<int> &signals, unsigned assignment)
{
    return evaluate(pair.consumer, signals, assignment, evaluate(pair.producer, signals, assignment, false));
}
inline uint64_t cofactor(const Lut &lut, std::size_t pin, bool value)
{
    uint64_t mask = 0;
    const unsigned low = (1u << pin) - 1;
    for (unsigned row = 0; row < (1u << (lut.pins.size() - 1)); ++row) {
        unsigned original = (row & low) | ((row & ~low) << 1) | (unsigned(value) << pin);
        if ((lut.mask >> original) & 1u) mask |= uint64_t(1) << row;
    }
    return mask;
}
} // namespace detail

inline bool valid(const Pair &pair)
{
    if (!detail::valid_lut(pair.producer, false) || !detail::valid_lut(pair.consumer, true)) return false;
    std::vector<int> signals;
    detail::append_signals(pair, signals);
    detail::unique_signals(signals);
    return signals.size() <= MAX_EXTERNAL_SIGNALS;
}

// Exact proof over actual distinct source nets; constants, aliases and pin/link
// inversions are included. There are at most 2048 assignments. This proves only
// the consumer output: the Context caller must require a single-use producer Q.
inline bool equivalent(const Pair &before, const Pair &after)
{
    if (!valid(before) || !valid(after)) return false;
    std::vector<int> signals;
    detail::append_signals(before, signals);
    detail::append_signals(after, signals);
    detail::unique_signals(signals);
    if (signals.size() > MAX_EXTERNAL_SIGNALS) return false;
    for (unsigned row = 0; row < (1u << signals.size()); ++row)
        if (detail::evaluate(before, signals, row) != detail::evaluate(after, signals, row)) return false;
    return true;
}

// Lift one literal factor into the consumer when one producer cofactor is a
// constant and the other is nonconstant. The remaining producer stays an
// ordinary 2..5-input LUT. The consumer uses an existing alias or a spare input;
// arbitrary consumer functions are permitted. This is a logic-only proposal:
// placement, packing, routing and all native timing guards remain mandatory.
inline std::vector<Candidate> reassociate(const Pair &before)
{
    std::vector<Candidate> result;
    if (!valid(before) || before.producer.pins.size() < 3) return result;
    std::size_t link = 0;
    while (before.consumer.pins[link].signal != INTERNAL) ++link;
    for (std::size_t selected = 0; selected < before.producer.pins.size(); ++selected) {
        const auto promoted = before.producer.pins[selected];
        if (promoted.signal < 0) continue;
        // Require a real reduction in producer support, not just another copy
        // of the same raw source on a different logical pin.
        if (std::count_if(before.producer.pins.begin(), before.producer.pins.end(),
                          [&](const Pin &p) { return p.signal == promoted.signal; }) != 1) continue;
        const uint64_t low = detail::cofactor(before.producer, selected, false);
        const uint64_t high = detail::cofactor(before.producer, selected, true);
        const uint64_t full = detail::full_mask(before.producer.pins.size() - 1);
        const bool low_constant = low == 0 || low == full;
        const bool high_constant = high == 0 || high == full;
        if (low_constant == high_constant) continue;
        const bool constant_when = high_constant;
        const bool constant_value = (constant_when ? high : low) != 0;
        Candidate candidate;
        candidate.network = before;
        candidate.promoted_pin = selected;
        candidate.promoted_signal = promoted.signal;
        candidate.network.producer.pins.erase(candidate.network.producer.pins.begin() + selected);
        candidate.network.producer.mask = constant_when ? low : high;
        auto &consumer = candidate.network.consumer;
        auto alias = std::find_if(consumer.pins.begin(), consumer.pins.end(),
                                  [&](const Pin &p) { return p.signal == promoted.signal; });
        candidate.consumer_pin = std::size_t(alias - consumer.pins.begin());
        if (alias == consumer.pins.end()) {
            if (consumer.pins.size() == 6) continue;
            consumer.pins.push_back({promoted.signal, false});
        }
        consumer.mask = 0;
        for (unsigned row = 0; row < (1u << consumer.pins.size()); ++row) {
            const bool raw_source = bool((row >> candidate.consumer_pin) & 1u) !=
                                    consumer.pins[candidate.consumer_pin].inverted;
            const bool selected_value = raw_source != promoted.inverted;
            const bool reduced_output = bool((row >> link) & 1u) != consumer.pins[link].inverted;
            const bool old_output = selected_value == constant_when ? constant_value : reduced_output;
            unsigned original = row & ((1u << before.consumer.pins.size()) - 1);
            original &= ~(1u << link);
            if (old_output != before.consumer.pins[link].inverted) original |= 1u << link;
            if ((before.consumer.mask >> original) & 1u) consumer.mask |= uint64_t(1) << row;
        }
        if (equivalent(before, candidate.network)) result.push_back(std::move(candidate));
    }
    return result;
}
} // namespace reduction_reassociate
#endif
