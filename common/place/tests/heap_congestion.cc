#include "gtest/gtest.h"
#include "heap_congestion.h"

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
    EXPECT_EQ(heap_congestion_detail::region_capacity(6, 8, 8), 6);
    EXPECT_EQ(heap_congestion_detail::region_capacity(6, 4, 8), 3);
    EXPECT_EQ(heap_congestion_detail::region_capacity(6, 0, 8), 0);
    EXPECT_EQ(heap_congestion_detail::region_capacity(6, 0, 0), 0);
}

TEST(HeapCongestionTest, RejectsScaleThatStrandsRegionCells)
{
    // Five cells confined to one eight-BEL tile scaled to four BELs cannot
    // fit, however much spare capacity the rest of the device has.
    int scaled = heap_congestion_detail::region_capacity(4, 8, 8);
    EXPECT_FALSE(heap_congestion_detail::scale_is_usable(5, scaled, 0.5f));
    // A wide region with its own spare capacity keeps the scale.
    EXPECT_TRUE(heap_congestion_detail::scale_is_usable(5, 80, 0.5f));
}
