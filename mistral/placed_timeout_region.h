#ifndef MISTRAL_PLACED_TIMEOUT_REGION_H
#define MISTRAL_PLACED_TIMEOUT_REGION_H
#include <array>
#include <cstdlib>
#include <utility>
#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN
// Diagnostic candidate domain only; scoring always keeps midpoint_distance.
struct TimeoutRegion
{
    std::pair<int, int> midpoint;
    std::array<std::pair<int, int>, 2> roots;
    bool expanded = false;
    int midpoint_distance(int x, int y) const { return std::abs(x - midpoint.first) + std::abs(y - midpoint.second); }
    int root_distance(int index, int x, int y) const
    {
        return std::abs(x - roots.at(index).first) + std::abs(y - roots.at(index).second);
    }
    bool contains(int x, int y) const
    {
        return midpoint_distance(x, y) <= 6 ||
               (expanded && (root_distance(0, x, y) <= 6 || root_distance(1, x, y) <= 6));
    }
};
NEXTPNR_NAMESPACE_END
#endif
