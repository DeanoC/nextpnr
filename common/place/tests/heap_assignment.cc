#include <algorithm>
#include <functional>
#include <random>
#include "gtest/gtest.h"
#include "heap_assignment.h"
using heap_assignment_detail::solve;

TEST(HeapAssignment, ExhaustiveSmallMatricesAgainstBruteForce)
{
    std::mt19937 rng(417);
    for (int n = 1; n <= 4; ++n) for (int m = n; m <= 5; ++m) for (int trial = 0; trial < 100; ++trial) {
        std::vector<std::vector<int64_t>> cost(n, std::vector<int64_t>(m));
        for (auto &row : cost) for (auto &v : row) v = int(rng() % 9) - 1;
        int64_t best = std::numeric_limits<int64_t>::max();
        std::function<void(int, int, int64_t)> visit = [&](int i, int used, int64_t value) {
            if (i == n) { best = std::min(best, value); return; }
            for (int j = 0; j < m; ++j)
                if (!(used & (1 << j)) && cost[i][j] >= 0) visit(i + 1, used | (1 << j), value + cost[i][j]);
        };
        visit(0, 0, 0);
        std::vector<int> result, repeat;
        bool ok = solve(cost, result);
        ASSERT_EQ(ok, best != std::numeric_limits<int64_t>::max());
        if (!ok) { EXPECT_TRUE(result.empty()); continue; }
        ASSERT_TRUE(solve(cost, repeat)); EXPECT_EQ(result, repeat);
        int used = 0; int64_t value = 0;
        for (int i = 0; i < n; ++i) { ASSERT_GE(result[i], 0); EXPECT_FALSE(used & (1 << result[i]));
            used |= 1 << result[i]; value += cost[i][result[i]]; }
        EXPECT_EQ(value, best);
    }
}
TEST(HeapAssignment, RejectsInvalidAndBoundedInputs)
{
    std::vector<int> result{42};
    EXPECT_FALSE(solve({}, result)); EXPECT_TRUE(result.empty());
    EXPECT_FALSE(solve({{1}, {2}}, result));
    EXPECT_FALSE(solve({{1, 2}, {3}}, result));
    EXPECT_FALSE(solve({{int64_t(1) << 40}}, result));
    EXPECT_FALSE(solve({{-1, -1}}, result));
    EXPECT_FALSE(solve(std::vector<std::vector<int64_t>>(1025, std::vector<int64_t>(1025)), result));
}
TEST(HeapAssignment, UrgentEdgeCanDisplaceSlackBlock)
{
    std::vector<int> result;
    ASSERT_TRUE(solve({{4, 40, 80}, {2, 10, 20}}, result));
    EXPECT_EQ(result, (std::vector<int>{0, 1}));
    ASSERT_TRUE(solve({{1, 1, 1}, {1, 1, 1}}, result));
    EXPECT_EQ(result, (std::vector<int>{0, 1}));
}
