#ifndef NEXTPNR_HEAP_HARD_BLOCK_COST_H
#define NEXTPNR_HEAP_HARD_BLOCK_COST_H
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace heap_hard_block_detail {
// Use ordinary HeAP's native float criticality multiplier. Keep the existing
// window weight for phase urgency, and retain fractions after fanout division;
// only the complete site cost is rounded by the assignment planner.
inline bool timing_weight(int64_t window_weight, size_t fanout, float criticality, float timing_weight,
                          float exponent, double &result)
{
    result = 0;
    if (window_weight <= 0 || window_weight > 1048576 || fanout == 0 ||
        !std::isfinite(criticality) || criticality < 0 || criticality > 1 ||
        !std::isfinite(timing_weight) || timing_weight < 0 || !std::isfinite(exponent) || exponent < 0)
        return false;
    const double multiplier = 1.0 + timing_weight * std::pow(criticality, exponent);
    const double weight = double(window_weight) / double(fanout) * multiplier;
    if (!std::isfinite(multiplier) || multiplier < 1 || !std::isfinite(weight) || weight <= 0 ||
        weight >= double(int64_t(1) << 40))
        return false;
    result = weight;
    return true;
}
} // namespace heap_hard_block_detail
#endif
