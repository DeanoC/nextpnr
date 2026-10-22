#include "gtest/gtest.h"
#include "heap_spread.h"
#include <algorithm>
#include <array>
#include <vector>

USING_NEXTPNR_NAMESPACE

TEST(HeapSpreadTest, SingletonStartsAtItsOwnPartitionBoundary)
{
    for (int start : {0, 1, 7, 100})
        EXPECT_DOUBLE_EQ(heap_spread_detail::small_partition_position(start, start, start + 1, 14, 25), 14);
}

TEST(HeapSpreadTest, TwoCellsUsePartitionLocalRanks)
{
    for (int start : {0, 1, 3, 100}) {
        EXPECT_DOUBLE_EQ(heap_spread_detail::small_partition_position(start, start, start + 2, 14, 25), 14);
        EXPECT_DOUBLE_EQ(heap_spread_detail::small_partition_position(start + 1, start, start + 2, 14, 25), 19.5);
    }
}

TEST(HeapSpreadTest, DegeneratePartitionRemainsOnItsOnlyCoordinate)
{
    for (int count : {1, 2})
        for (int start : {0, 3, 100})
            for (int index = start; index < start + count; ++index)
                EXPECT_DOUBLE_EQ(heap_spread_detail::small_partition_position(index, start, start + count, 41, 41), 41);
}

TEST(HeapSpreadTest, UnrelatedPrefixDoesNotChangePartitionCoordinates)
{
    for (int count : {1, 2})
        for (int prefix = 0; prefix < 32; ++prefix)
            for (int rank = 0; rank < count; ++rank)
                EXPECT_DOUBLE_EQ(heap_spread_detail::small_partition_position(prefix + rank, prefix, prefix + count,
                                                                              13.25, 41.75),
                                 heap_spread_detail::small_partition_position(rank, 0, count, 13.25, 41.75));
}

TEST(HeapSpreadTest, TranslatingPartitionTranslatesItsCells)
{
    for (int count : {1, 2})
        for (int rank = 0; rank < count; ++rank) {
            double base = heap_spread_detail::small_partition_position(7 + rank, 7, 7 + count, 13.25, 41.75);
            EXPECT_DOUBLE_EQ(heap_spread_detail::small_partition_position(7 + rank, 7, 7 + count, 29.25, 57.75),
                             base + 16);
        }
}

TEST(HeapSpreadTest, CellsStayInTheChildWhoseCountsIncludeThem)
{
    // Parent clamping alone cannot fix an out-of-child coordinate. The legacy
    // absolute ranks 3 and 4 produce 30.5 and 36, both still inside parent [0,63].
    constexpr int start = 3, end = 5, child_low = 14, child_high = 25;
    int child_cells = 0, legacy_child_cells = 0;
    for (int index = start; index < end; ++index) {
        double raw = heap_spread_detail::small_partition_position(index, start, end, child_low, child_high);
        EXPECT_GE(raw, child_low);
        EXPECT_LE(raw, child_high);
        int tile = std::min(63, std::max(0, int(raw)));
        child_cells += tile >= child_low && tile <= child_high;
        double legacy_raw = child_low + index * (double(child_high - child_low) / (end - start));
        int legacy_tile = std::min(63, std::max(0, int(legacy_raw)));
        legacy_child_cells += legacy_tile >= child_low && legacy_tile <= child_high;
    }
    EXPECT_EQ(child_cells, end - start);
    EXPECT_EQ(legacy_child_cells, 0);
}

TEST(HeapSpreadTest, ZeroStartRetainsLegacyCoordinates)
{
    for (int count : {1, 2})
        for (int index = 0; index < count; ++index) {
            double legacy = 13.25 + index * ((41.75 - 13.25) / count);
            EXPECT_DOUBLE_EQ(heap_spread_detail::small_partition_position(index, 0, count, 13.25, 41.75), legacy);
        }
}

namespace {
struct Cell
{
    int bucket, weight;
};

void transfer(const std::vector<Cell> &cells, int &pivot, std::vector<int> &left, std::vector<int> &right)
{
    heap_spread_detail::move_right_cell_to_left(cells, pivot, left, right,
                                               [](const Cell &cell) { return cell.bucket; },
                                               [](const Cell &cell) { return cell.weight; });
}

std::vector<int> recount(const std::vector<Cell> &cells, int begin, int end, int buckets)
{
    std::vector<int> counts(buckets, 0);
    for (int i = begin; i < end; ++i)
        counts.at(cells.at(i).bucket) += cells.at(i).weight;
    return counts;
}
} // namespace

TEST(HeapSpreadTest, RightTransferCountsTheMovedCellsBucketAndWeight)
{
    // A three-cell chain from bucket 1 moves left; the old pivot is a single
    // bucket-0 cell. Charging its bucket would make right bucket 0 negative.
    const std::vector<Cell> cells = {{0, 1}, {1, 3}, {0, 2}};
    int pivot = 0;
    std::vector<int> left = {1, 0}, right = {2, 3};
    transfer(cells, pivot, left, right);
    EXPECT_EQ(pivot, 1);
    EXPECT_EQ(left, (std::vector<int>{1, 3}));
    EXPECT_EQ(right, (std::vector<int>{2, 0}));
}

TEST(HeapSpreadTest, RepeatedMixedTransfersMatchActualPartitionMembership)
{
    const std::vector<Cell> cells = {{0, 2}, {1, 3}, {2, 1}, {1, 2}, {0, 4}};
    int pivot = 0;
    auto left = recount(cells, 0, 1, 3);
    auto right = recount(cells, 1, int(cells.size()), 3);
    const auto totals = recount(cells, 0, int(cells.size()), 3);
    while (pivot + 1 < int(cells.size())) {
        transfer(cells, pivot, left, right);
        EXPECT_EQ(left, recount(cells, 0, pivot + 1, 3));
        EXPECT_EQ(right, recount(cells, pivot + 1, int(cells.size()), 3));
        for (int bucket = 0; bucket < 3; ++bucket) {
            EXPECT_GE(left.at(bucket), 0);
            EXPECT_GE(right.at(bucket), 0);
            EXPECT_EQ(left.at(bucket) + right.at(bucket), totals.at(bucket));
        }
    }
}

TEST(HeapSpreadTest, HomogeneousTransferRetainsLegacyCounts)
{
    const std::vector<Cell> cells = {{0, 1}, {0, 5}, {0, 2}};
    int pivot = 0;
    std::vector<int> left = {1}, right = {7};
    transfer(cells, pivot, left, right);
    EXPECT_EQ(pivot, 1);
    EXPECT_EQ(left, (std::vector<int>{6}));
    EXPECT_EQ(right, (std::vector<int>{2}));
}
