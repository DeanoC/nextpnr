#include "gtest/gtest.h"
#include "archdefs.h"

USING_NEXTPNR_NAMESPACE

TEST(BelIdTest, OriginIsDistinctFromInvalid)
{
    BelId origin(CycloneV::xycoords(0, 0), 0);
    EXPECT_NE(origin, BelId());
    EXPECT_EQ(origin, BelId(CycloneV::xycoords(0, 0), 0));
    EXPECT_EQ(BelId(), BelId());
}
