#include "gtest/gtest.h"
#include "heap_congestion.h"
#include <set>
#include <vector>

USING_NEXTPNR_NAMESPACE

TEST(HeapCongestionTest, UsesBetaForEffectiveCapacity)
{
    EXPECT_FALSE(heap_congestion_detail::scale_is_usable(150, 200, 0.5f));
    EXPECT_TRUE(heap_congestion_detail::scale_is_usable(150, 400, 0.5f));
}

TEST(HeapCongestionTest, PreservesSmallBelException)
{
    EXPECT_TRUE(heap_congestion_detail::scale_is_usable(2, 2, 0.5f));
    EXPECT_FALSE(heap_congestion_detail::scale_is_usable(4, 4, 0.5f));
}

TEST(HeapCongestionTest, RegionHoldsItsShareOfScaledCapacity)
{
    EXPECT_EQ(heap_congestion_detail::region_capacity(6, 8, 8, 0, 0), 6);
    EXPECT_EQ(heap_congestion_detail::region_capacity(6, 4, 8, 0, 0), 3);
    EXPECT_EQ(heap_congestion_detail::region_capacity(6, 0, 8, 0, 0), 0);
    EXPECT_EQ(heap_congestion_detail::region_capacity(6, 0, 0, 0, 0), 0);
}

TEST(HeapCongestionTest, RegionCapacitySubtractsFixedOccupantsInsidePartialRegion)
{
    // Of 16 BELs, 12 are in the region. The scaled tile has 12 BELs and four
    // fixed occupants lie inside the region.
    int capacity = heap_congestion_detail::region_capacity(12, 12, 16, 4, 4);
    EXPECT_EQ(capacity, 5);
    // At beta 0.5 the old estimate (8 free BELs, apportioned to 6) passed
    // three cells; the corrected estimate rejects that scale.
    EXPECT_FALSE(heap_congestion_detail::scale_is_usable(3, capacity, 0.5f));
}

TEST(HeapCongestionTest, RegionCapacityCannotExceedTileCapacityAfterFixedOccupants)
{
    // Fixed occupants outside the region do not reduce its proportional
    // share, but they still reduce the tile's total capacity.
    EXPECT_EQ(heap_congestion_detail::region_capacity(12, 12, 16, 4, 0), 8);
}

TEST(HeapCongestionTest, FixedRegionOccupancyOnlyReducesCapacityAtItsLocation)
{
    // A two-location region has one fixed occupant in the first location.
    // The occupant must not also reduce the capacity at the second location.
    const std::vector<int> scaled_bels = {8, 8};
    const std::vector<int> region_bels = {8, 8};
    const std::vector<int> location_bels = {8, 8};
    const std::vector<int> fixed_occupancy = {1, 0};
    const std::vector<int> region_fixed_occupancy = {1, 0};
    int region_capacity = 0;
    for (size_t location = 0; location < scaled_bels.size(); location++)
        region_capacity += heap_congestion_detail::region_capacity(
                scaled_bels.at(location), region_bels.at(location), location_bels.at(location),
                fixed_occupancy.at(location), region_fixed_occupancy.at(location));

    EXPECT_EQ(region_capacity, 15);
    EXPECT_TRUE(heap_congestion_detail::scale_is_usable(14, region_capacity, 0.95f));

    // Repeating the region-wide fixed count at both locations incorrectly
    // reduces the total to 14 and rejects this otherwise usable scale.
    int region_wide_fixed_count_applied_twice = 0;
    for (size_t location = 0; location < scaled_bels.size(); location++)
        region_wide_fixed_count_applied_twice += heap_congestion_detail::region_capacity(
                scaled_bels.at(location), region_bels.at(location), location_bels.at(location),
                fixed_occupancy.at(location), 1);
    EXPECT_EQ(region_wide_fixed_count_applied_twice, 14);
    EXPECT_FALSE(heap_congestion_detail::scale_is_usable(14, region_wide_fixed_count_applied_twice, 0.95f));
}

TEST(HeapCongestionTest, DetectsSharedBelsBetweenConstrainedRegions)
{
    std::set<int> left = {1, 2, 3, 4, 5, 6, 7, 8};
    std::set<int> right = {1, 2, 3, 4, 5, 6, 7, 8};
    std::set<int> disjoint = {9, 10};
    std::vector<std::vector<std::vector<int>>> bucket_locations = {{{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}}};

    EXPECT_TRUE(heap_congestion_detail::regions_overlap_bels(left, right, bucket_locations));
    EXPECT_FALSE(heap_congestion_detail::regions_overlap_bels(left, disjoint, bucket_locations));
    // Each region could independently pass with four cells and six scaled
    // BELs, although their combined eight cells cannot fit those shared BELs.
    EXPECT_TRUE(heap_congestion_detail::scale_is_usable(4, 6, 0.75f));
    EXPECT_FALSE(heap_congestion_detail::scale_is_usable(8, 6, 0.75f));
}

TEST(HeapCongestionTest, RejectsScaleThatStrandsRegionCells)
{
    // Five cells confined to one eight-BEL tile scaled to four BELs cannot
    // fit, however much spare capacity the rest of the device has.
    int scaled = heap_congestion_detail::region_capacity(4, 8, 8, 0, 0);
    EXPECT_FALSE(heap_congestion_detail::scale_is_usable(5, scaled, 0.5f));
    // A wide region with its own spare capacity keeps the scale.
    EXPECT_TRUE(heap_congestion_detail::scale_is_usable(5, 80, 0.5f));
}
