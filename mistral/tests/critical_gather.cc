#include "gtest/gtest.h"
#include "critical_gather.h"

USING_NEXTPNR_NAMESPACE

namespace {

GatherClockSlack met(long slack) { return {true, slack}; }
GatherClockSlack missed(long slack) { return {true, slack}; }
GatherClockSlack gone() { return {}; }

} // namespace

TEST(CriticalGatherAccept, KeepsAMoveThatImprovesWorstSlack)
{
    std::vector<GatherClockSlack> before{missed(-7000), met(800)};
    std::vector<GatherClockSlack> after{missed(-4000), met(200)};
    EXPECT_TRUE(critical_gather_accepts(-7000, before, -4000, after, 20));
}

TEST(CriticalGatherAccept, RejectsAGainBelowTheMinimum)
{
    std::vector<GatherClockSlack> before{missed(-100)};
    std::vector<GatherClockSlack> after{missed(-81)};
    EXPECT_FALSE(critical_gather_accepts(-100, before, -81, after, 20));
    EXPECT_TRUE(critical_gather_accepts(-100, before, -80, after, 20));
}

TEST(CriticalGatherAccept, RejectsBreakingAClockThatAlreadyMet)
{
    std::vector<GatherClockSlack> before{missed(-5000), met(50)};
    std::vector<GatherClockSlack> after{missed(-1000), missed(-1)};
    EXPECT_FALSE(critical_gather_accepts(-5000, before, -1000, after, 20));
}

TEST(CriticalGatherAccept, AllowsAnotherFailingClockToGetWorse)
{
    std::vector<GatherClockSlack> before{missed(-5000), missed(-100)};
    std::vector<GatherClockSlack> after{missed(-2000), missed(-400)};
    EXPECT_TRUE(critical_gather_accepts(-5000, before, -2000, after, 20));
}

TEST(CriticalGatherAccept, RejectsAClockThatDisappears)
{
    std::vector<GatherClockSlack> before{missed(-5000), met(100)};
    std::vector<GatherClockSlack> after{missed(-2000), gone()};
    EXPECT_FALSE(critical_gather_accepts(-5000, before, -2000, after, 20));
}

TEST(CriticalGatherAccept, IgnoresAClockThatWasNotThereBefore)
{
    std::vector<GatherClockSlack> before{gone(), missed(-5000)};
    std::vector<GatherClockSlack> after{met(10), missed(-2000)};
    EXPECT_TRUE(critical_gather_accepts(-5000, before, -2000, after, 20));
}

TEST(CriticalGatherAccept, RejectsAMismatchedClockList)
{
    std::vector<GatherClockSlack> before{missed(-5000)};
    std::vector<GatherClockSlack> after{missed(-2000), met(10)};
    EXPECT_FALSE(critical_gather_accepts(-5000, before, -2000, after, 20));
}

TEST(CriticalGatherAccept, TreatsZeroSlackAsAlreadyMet)
{
    std::vector<GatherClockSlack> before{missed(-5000), met(0)};
    std::vector<GatherClockSlack> after{missed(-2000), missed(-1)};
    EXPECT_FALSE(critical_gather_accepts(-5000, before, -2000, after, 20));
}
