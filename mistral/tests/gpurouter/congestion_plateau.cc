// Standalone regression for the production GPU-router stopping policy.
#include "common/route/gpu/congestion_plateau.h"

#include <cstdio>

static int failures = 0;

static void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

// Return the iteration that requests an early escape, or zero when the
// configured budget expires. Occupancy is supplied independently of routing:
// these tests cover stopping policy, not whether a physical graph is routable.
static int plateau_until_budget(gpuroute::CongestionPlateau &policy, int overused, int budget, int boost_ceiling_at)
{
    for (int iter = 1; iter <= budget; ++iter) {
        if (iter >= boost_ceiling_at && policy.saturated(overused, 2))
            return iter;
        if (policy.tiny(overused))
            return iter;
    }
    return 0;
}

int main()
{
    gpuroute::CongestionPlateau policy;
    policy.begin(false);
    require(plateau_until_budget(policy, 1, 100, 80) == 0,
            "initial routing must continue beyond the tiny plateau and boost ceiling to its budget");
    policy.begin(false);
    require(plateau_until_budget(policy, 5, 100, 20) == 0,
            "initial routing must retain its budget when only the boost-ceiling shortcut applies");

    // Pre-routed/global hard reservations do not set ArcData::frozen. They
    // must not turn an initial negotiation into a timing-repair attempt.
    policy.begin(false);
    require(plateau_until_budget(policy, 2, 200, 1) == 0,
            "zero frozen arcs must retain the budget even with hard reservations");

    policy.begin(true);
    require(plateau_until_budget(policy, 1, 100, 80) == 30,
            "timing repair must retain its existing tiny-plateau shortcut");
    policy.begin(true);
    require(plateau_until_budget(policy, 5, 100, 20) == 20,
            "timing repair must retain the saturated-congestion shortcut");

    policy.begin(true);
    for (int i = 0; i < 29; ++i)
        require(!policy.tiny(1), "repair must not escape before thirty consecutive tiny iterations");
    require(!policy.tiny(0), "legal routing must clear the tiny-plateau counter");
    for (int i = 0; i < 29; ++i)
        require(!policy.tiny(4), "a recovered route must get a fresh plateau interval");
    require(!policy.tiny(5), "larger congestion must clear the tiny-plateau counter");
    require(plateau_until_budget(policy, 2, 30, 80) == 30,
            "small congestion must get thirty iterations after larger congestion");

    // Thawing occurs within an attempt, without begin(): the attempt retains
    // its repair budget even if its final frozen arc was released meanwhile.
    policy.begin(true);
    for (int i = 0; i < 20; ++i)
        require(!policy.tiny(1), "repair must still negotiate before thawing");
    require(plateau_until_budget(policy, 1, 10, 80) == 10,
            "thawing during repair must not disable its plateau escape");
    policy.clear_tiny();
    require(plateau_until_budget(policy, 1, 30, 80) == 30,
            "successful escape must restart the tiny-plateau interval");
    policy.begin(false);
    require(plateau_until_budget(policy, 1, 100, 1) == 0,
            "a later negotiation without frozen arcs must not inherit repair eligibility");
    policy.begin(true);
    require(!policy.saturated(0, 0) && !policy.saturated(9, 2) && !policy.saturated(1, 9),
            "the boost-ceiling shortcut must remain bounded to small nonzero congestion");

    if (failures != 0)
        return 1;
    std::puts("PASS: GPU congestion plateau policy");
    return 0;
}
