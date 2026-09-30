/* Bounded four-leaf spatial probing. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_PLACED_REDUCTION_POLICY_H
#define MISTRAL_PLACED_REDUCTION_POLICY_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <set>
#include <utility>

namespace placed_reduction_policy {
using Lab = std::pair<int, int>;

class WideProbeBudget {
    using Tuple = std::array<Lab, 4>;
    std::set<Tuple> ordered_trials;
    std::map<Tuple, int> geometry_trials;
    int timed_trials = 0;

    static Tuple geometry(Tuple labs)
    {
        std::sort(labs.begin(), labs.end());
        return labs;
    }

  public:
    // Eligibility never consumes budget. The caller first checks full physical
    // legality, then admits the assignment immediately before timing it.
    bool eligible(const Tuple &labs) const
    {
        if (exhausted() || ordered_trials.count(labs)) return false;
        auto found = geometry_trials.find(geometry(labs));
        return found == geometry_trials.end() || found->second < 2;
    }

    bool admit_legal_ordered_tuple(const Tuple &labs)
    {
        if (!eligible(labs)) return false;
        ordered_trials.insert(labs);
        ++geometry_trials[geometry(labs)];
        ++timed_trials;
        return true;
    }

    bool exhausted() const { return timed_trials >= 16; }
    int timed_count() const { return timed_trials; }
    size_t geometry_count() const { return geometry_trials.size(); }
};
}

#endif
