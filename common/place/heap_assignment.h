#ifndef HEAP_ASSIGNMENT_H
#define HEAP_ASSIGNMENT_H
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace heap_assignment_detail {
// Rectangular minimum-cost matching. Caller supplies stable row/column order.
// Negative costs forbid edges. Bounds keep every potential/subtraction exact.
inline bool solve(const std::vector<std::vector<int64_t>> &cost, std::vector<int> &assignment)
{
    assignment.clear();
    const size_t n = cost.size(), m = n ? cost.front().size() : 0;
    constexpr int64_t limit = int64_t(1) << 40, inf = int64_t(1) << 60;
    if (!n || n > 1024 || m < n || m > 4096 || n * m > 1048576) return false;
    for (const auto &row : cost) {
        if (row.size() != m) return false;
        for (auto value : row) if (value >= limit) return false;
    }
    std::vector<int64_t> u(n + 1), v(m + 1);
    std::vector<size_t> matched(m + 1), way(m + 1);
    for (size_t i = 1; i <= n; ++i) {
        matched[0] = i;
        size_t j0 = 0;
        std::vector<int64_t> slack(m + 1, inf);
        std::vector<bool> used(m + 1, false);
        do {
            used[j0] = true;
            size_t i0 = matched[j0], j1 = 0;
            int64_t delta = inf;
            for (size_t j = 1; j <= m; ++j) {
                if (used[j]) continue;
                if (cost[i0 - 1][j - 1] >= 0) {
                    int64_t current = cost[i0 - 1][j - 1] - u[i0] - v[j];
                    if (current < slack[j]) { slack[j] = current; way[j] = j0; }
                }
                // Strict comparisons and ascending column order resolve ties.
                if (slack[j] < delta) { delta = slack[j]; j1 = j; }
            }
            if (delta == inf) return false;
            for (size_t j = 0; j <= m; ++j) {
                if (used[j]) { u[matched[j]] += delta; v[j] -= delta; }
                else if (slack[j] != inf) slack[j] -= delta;
            }
            j0 = j1;
        } while (matched[j0]);
        do { size_t previous = way[j0]; matched[j0] = matched[previous]; j0 = previous; } while (j0);
    }
    assignment.assign(n, -1);
    for (size_t j = 1; j <= m; ++j)
        if (matched[j]) assignment[matched[j] - 1] = int(j - 1);
    return std::all_of(assignment.begin(), assignment.end(), [](int j) { return j >= 0; });
}
} // namespace heap_assignment_detail
#endif
