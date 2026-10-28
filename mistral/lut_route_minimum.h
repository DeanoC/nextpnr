#ifndef NEXTPNR_MISTRAL_LUT_ROUTE_MINIMUM_H
#define NEXTPNR_MISTRAL_LUT_ROUTE_MINIMUM_H

#include "lut_route_minimum_data.h"
#include <algorithm>
#include <cmath>
#include <limits>

NEXTPNR_NAMESPACE_BEGIN
namespace lut_route_minimum {

inline bool data_element(mistral::CycloneV::rnode_type_t type)
{
    using CV = mistral::CycloneV;
    switch (type) {
    case CV::GIN: case CV::GOUT: case CV::LD: case CV::TD:
    case CV::H3: case CV::H6: case CV::H14:
    case CV::V2: case CV::V4: case CV::V12:
        return true;
    default:
        return false;
    }
}

// Enclose the existing interval and its native minimum-scaled early value.
// Apply separately at each hop: scaling a sum containing negative increments
// can move its early bound later. Propagated voltage waves are unchanged.
inline bool enclose_minimum(double factor, mistral::AnalogSim::time_interval &interval)
{
    if (!std::isfinite(factor) || factor <= 0 || factor > 1 ||
        !std::isfinite(interval.mi) || !std::isfinite(interval.mx) || interval.mi > interval.mx)
        return false;
    const double early = interval.mi * factor;
    if (!std::isfinite(early))
        return false;
    interval.mi = std::min(interval.mi, early);
    interval.mx = std::max(interval.mx, early);
    return true;
}

inline bool outward_ps(double seconds, bool early, delay_t &result)
{
    const double value = early ? std::floor(seconds * 1e12) : std::ceil(seconds * 1e12);
    if (!std::isfinite(value) || value < std::numeric_limits<delay_t>::min() ||
        value > std::numeric_limits<delay_t>::max())
        return false;
    result = delay_t(value);
    return true;
}

} // namespace lut_route_minimum
NEXTPNR_NAMESPACE_END
#endif
