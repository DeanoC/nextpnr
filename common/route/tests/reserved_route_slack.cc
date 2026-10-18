#include "gtest/gtest.h"

#include "reserved_route_slack.h"

namespace {

gpuroute::ReservedRouteVerdict<int> judge(const std::vector<gpuroute::ReservedRouteArc<int>> &arcs)
{
    return gpuroute::judge_reserved_route(arcs);
}

// STA reports -100 ps on a 500 ps arc. Pure-delay repair makes it 200 ps
// (actual slack +200). The reserved route makes it 300 ps (actual slack +100).
// That route is an improvement on the original and a regression on the route
// already installed, so it is refused. The two actual slacks differ by the
// delay change; the pre-repair slack is only the shared baseline.
TEST(ReservedRouteSlack, SlowerThanThePureDelayRouteIsRefused)
{
    gpuroute::ReservedRouteArc<int> arc{0, 500, 200, 300, -100.f, true};
    auto verdict = judge({arc});
    EXPECT_FALSE(verdict.accept);
    EXPECT_FLOAT_EQ(verdict.incumbent_worst, 200.f);
    EXPECT_FLOAT_EQ(verdict.alternate_worst, 100.f);
    EXPECT_EQ(verdict.incumbent_sum, 200);
    EXPECT_EQ(verdict.alternate_sum, 300);
}

TEST(ReservedRouteSlack, FasterThanThePureDelayRouteIsKept)
{
    gpuroute::ReservedRouteArc<int> arc{0, 500, 400, 250, -100.f, true};
    auto verdict = judge({arc});
    EXPECT_TRUE(verdict.accept);
    EXPECT_FLOAT_EQ(verdict.incumbent_worst, 0.f);
    EXPECT_FLOAT_EQ(verdict.alternate_worst, 150.f);
}

// One arc: the pre-repair slack cancels, so the decision matches comparing the
// two candidate delays directly.
TEST(ReservedRouteSlack, OneArcMatchesTheDelayComparison)
{
    const int originals[] = {0, 100, 500};
    const int delays[] = {50, 100, 250, 400};
    for (int original : originals) {
        for (int incumbent : delays) {
            for (int alternate : delays) {
                gpuroute::ReservedRouteArc<int> arc{0, original, incumbent, alternate, -100.f, true};
                float stale = -100.f;
                float estimated = stale - float(alternate - incumbent);
                bool slack_worse = estimated < stale - 1.f;
                bool slack_better = estimated > stale + 1.f;
                bool delay_accept = !slack_worse && (slack_better || alternate < incumbent);
                EXPECT_EQ(judge({arc}).accept, delay_accept) << original << " " << incumbent << " " << alternate;
            }
        }
    }
}

// Two sinks. Pure-delay repair already fixed the arc whose stale slack was
// -200 ps. The reserved route slows the arc that is critical after that
// repair (-50 ps to -150 ps) and speeds the other. Worst slack falls. A
// comparison against the stale -200 ps would accept this regression.
TEST(ReservedRouteSlack, DoesNotAcceptARegressionAgainstStaleSlack)
{
    std::vector<gpuroute::ReservedRouteArc<int>> arcs{
            {0, 100, 100, 200, -50.f, true},
            {1, 500, 100, 50, -200.f, true},
    };
    auto verdict = judge(arcs);
    EXPECT_FALSE(verdict.accept);
    EXPECT_FLOAT_EQ(verdict.incumbent_worst, -50.f);
    EXPECT_FLOAT_EQ(verdict.alternate_worst, -150.f);
}

// The stale-critical arc is unchanged by pure-delay repair and gets faster on
// the reserved route. Another arc, already repaired, gets slower but stays
// well positive. Worst slack rises from -100 ps to -50 ps. Charging the
// reserved route for the pure-delay improvement would reject this.
TEST(ReservedRouteSlack, DoesNotRejectAnImprovementAgainstStaleSlack)
{
    std::vector<gpuroute::ReservedRouteArc<int>> arcs{
            {0, 400, 100, 250, -10.f, true},
            {1, 200, 200, 150, -100.f, true},
    };
    auto verdict = judge(arcs);
    EXPECT_TRUE(verdict.accept);
    EXPECT_FLOAT_EQ(verdict.incumbent_worst, -100.f);
    EXPECT_FLOAT_EQ(verdict.alternate_worst, -50.f);
}

// Two physical pins, one port. The slow pin is the maximum. The other pin
// slows on the reserved route but stays under the new maximum, so the port
// slack still improves.
TEST(ReservedRouteSlack, PortMaximumIgnoresAPinUnderTheMax)
{
    std::vector<gpuroute::ReservedRouteArc<int>> arcs{
            {0, 500, 200, 150, -100.f, true},
            {0, 100, 100, 140, -100.f, true},
    };
    auto verdict = judge(arcs);
    EXPECT_TRUE(verdict.accept);
    EXPECT_FLOAT_EQ(verdict.incumbent_worst, 200.f);
    EXPECT_FLOAT_EQ(verdict.alternate_worst, 250.f);
}

// A pre-routed sibling pin is not searched. It holds the port delay at 300 ps
// for both candidates, and the searched pin gets slower, so the tie keeps the
// pure-delay route.
TEST(ReservedRouteSlack, UnsearchedPinCapsThePort)
{
    std::vector<gpuroute::ReservedRouteArc<int>> arcs{
            {0, 500, 100, 250, -100.f, true},
            {0, 300, 300, 300, -100.f, false},
    };
    auto verdict = judge(arcs);
    EXPECT_FALSE(verdict.accept);
    EXPECT_FLOAT_EQ(verdict.incumbent_worst, 100.f);
    EXPECT_FLOAT_EQ(verdict.alternate_worst, 100.f);
    EXPECT_EQ(verdict.incumbent_sum, 100);
    EXPECT_EQ(verdict.alternate_sum, 250);
}

TEST(ReservedRouteSlack, TieKeepsTheLowerDelaySum)
{
    gpuroute::ReservedRouteArc<int> faster_sum{0, 100, 100, 99, 0.f, true};
    gpuroute::ReservedRouteArc<int> same{0, 100, 100, 100, 0.f, true};
    EXPECT_TRUE(judge({faster_sum}).accept);
    EXPECT_FALSE(judge({same}).accept);
}

TEST(ReservedRouteSlack, EmptyInputIsRefused)
{
    EXPECT_FALSE(judge({}).accept);
}

// generic's delay_t is float. The same baseline applies.
TEST(ReservedRouteSlack, FloatDelayUsesTheSameBaseline)
{
    gpuroute::ReservedRouteArc<float> arc{0, 500.f, 200.f, 300.f, -100.f, true};
    auto verdict = gpuroute::judge_reserved_route(std::vector<gpuroute::ReservedRouteArc<float>>{arc});
    EXPECT_FALSE(verdict.accept);
    EXPECT_FLOAT_EQ(verdict.incumbent_worst, 200.f);
    EXPECT_FLOAT_EQ(verdict.alternate_worst, 100.f);
}

} // namespace
