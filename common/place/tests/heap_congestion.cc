#include "gtest/gtest.h"
#include "heap_congestion.h"

USING_NEXTPNR_NAMESPACE

TEST(HeapCongestionTest, UsesBetaForEffectiveCapacity)
{
    EXPECT_FALSE(heap_congestion_detail::scale_is_usable(false, 150, 200, 0.5f));
    EXPECT_TRUE(heap_congestion_detail::scale_is_usable(false, 150, 400, 0.5f));
}

TEST(HeapCongestionTest, PreservesSmallBelException)
{
    EXPECT_TRUE(heap_congestion_detail::scale_is_usable(false, 2, 2, 0.5f));
    EXPECT_FALSE(heap_congestion_detail::scale_is_usable(false, 4, 4, 0.5f));
}

TEST(HeapCongestionTest, DisablesScalingForRegionConstrainedCells)
{
    EXPECT_FALSE(heap_congestion_detail::scale_is_usable(true, 2, 8, 0.5f));
}
