#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include "gtest/gtest.h"
#include "analogue_repair.h"
#include "nextpnr.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE

// Analogue improvement of a passing design scores a sink by its setup slack
// margin over the worst path of its own clock pairs, so that the critical
// paths of every clock rank alike and no clock's critical path is slowed.
class SetupSlackMarginTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;

    NetInfo *clock(const std::string &name, delay_t period)
    {
        auto *net = ctx->createNet(ctx->id(name));
        net->is_global = true;
        net->clkconstr = std::make_unique<ClockConstraint>();
        net->clkconstr->period = DelayPair(period);
        net->clkconstr->high = DelayPair(period / 2);
        net->clkconstr->low = DelayPair(period - period / 2);
        return net;
    }

    CellInfo *ff(const std::string &name, NetInfo *clk, int x, int y)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN})
            cell->addInput(pin);
        cell->addOutput(id_Q);
        cell->connectPort(id_CLK, clk);
        cell->pin_data[id_CLK].state = PIN_SIG;
        cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        ctx->assignArchInfo();
        // The first LAB at or right of (x, y); some columns hold no LABs
        for (int lab_x = x; lab_x < x + 8; lab_x++)
            for (auto bel : ctx->getBelsByTile(lab_x, y)) {
                if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(cell->type, bel))
                    continue;
                ctx->bindBel(bel, cell, STRENGTH_USER);
                if (ctx->isBelLocationValid(bel))
                    return cell;
                ctx->unbindBel(bel);
            }
        ADD_FAILURE() << "No legal FF BEL for " << name << " near " << x << ',' << y;
        return cell;
    }

    void connect(CellInfo *from, std::initializer_list<CellInfo *> to)
    {
        auto *net = ctx->createNet(ctx->id(from->name.str(ctx.get()) + "$q"));
        from->connectPort(id_Q, net);
        for (auto *cell : to)
            cell->connectPort(id_DATAIN, net);
    }

    void SetUp() override
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 1e8;
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
    }
};

TEST_F(SetupSlackMarginTest, RanksTheCriticalPathOfEveryClockAlike)
{
    // A 10 ns clock launches one net with a slow and a fast sink; a 40 ns
    // clock has a single path. Each FF sits in its own LAB.
    auto *fast_clk = clock("fast_clk", 10000), *slow_clk = clock("slow_clk", 40000);
    auto *a = ff("a", fast_clk, 10, 10), *a_slow = ff("a_slow", fast_clk, 12, 10);
    auto *a_fast = ff("a_fast", fast_clk, 14, 10);
    auto *b = ff("b", slow_clk, 10, 20), *b_end = ff("b_end", slow_clk, 12, 20);
    connect(a, {a_slow, a_fast});
    connect(b, {b_end});

    TimingAnalyser tmg(ctx.get());
    tmg.setup();
    const CellPortKey slow(a_slow->name, id_DATAIN), fast(a_fast->name, id_DATAIN), other(b_end->name, id_DATAIN);
    auto time = [&](delay_t fast_delay) {
        tmg.set_route_delay(slow, DelayPair(3000));
        tmg.set_route_delay(fast, DelayPair(fast_delay));
        tmg.set_route_delay(other, DelayPair(5000));
        tmg.run(false, false, false, true);
        return AnalogueClockTiming(tmg.get_timing_result());
    };
    const auto before = time(1000);

    // The slow sink and the 40 ns path are each their clock pair's critical
    // path, although the 40 ns path has far more slack.
    EXPECT_EQ(tmg.get_setup_slack_margin(slow), 0);
    EXPECT_EQ(tmg.get_setup_slack_margin(other), 0);
    EXPECT_GT(tmg.get_setup_slack(other), tmg.get_setup_slack(slow) + 25000);
    // The fast sink of the same net is its route delay difference away from
    // that critical path.
    EXPECT_EQ(tmg.get_setup_slack_margin(fast), 2000);
    EXPECT_EQ(tmg.get_setup_slack(fast) - tmg.get_setup_slack(slow), 2000);

    // Slowing the fast sink by less than its margin leaves every clock's
    // worst slack and Fmax as they were; by more, its clock loses both.
    EXPECT_FALSE(time(2900).worse_than(before));
    const auto slower = time(3100);
    EXPECT_TRUE(slower.worse_than(before));
    EXPECT_LT(slower.setup.at(fast_clk->name), before.setup.at(fast_clk->name));
    EXPECT_LT(slower.fmax.at(fast_clk->name), before.fmax.at(fast_clk->name));
    EXPECT_EQ(slower.setup.at(slow_clk->name), before.setup.at(slow_clk->name));
}

TEST_F(SetupSlackMarginTest, PortOnlyOnAnUntimedCrossingHasNoMargin)
{
    // Two unrelated clocks: the crossing is not timed, so its endpoint is on
    // no timed pair.
    auto *clk1 = clock("clk1", 10000), *clk2 = clock("clk2", 13000);
    auto *a = ff("a", clk1, 10, 10), *c = ff("c", clk2, 12, 10);
    connect(a, {c});
    a->connectPort(id_DATAIN, a->getPort(id_Q));
    TimingAnalyser tmg(ctx.get());
    tmg.setup();
    EXPECT_EQ(tmg.get_setup_slack_margin(CellPortKey(c->name, id_DATAIN)), std::numeric_limits<delay_t>::max());
    EXPECT_EQ(tmg.get_setup_slack_margin(CellPortKey(a->name, id_DATAIN)), 0);
}

namespace {
AnalogueClockTiming clocks(Context *ctx, std::initializer_list<std::tuple<const char *, delay_t, delay_t, float>> rows)
{
    AnalogueClockTiming t;
    for (auto &row : rows) {
        IdString clock = ctx->id(std::get<0>(row));
        t.setup[clock] = std::get<1>(row);
        t.hold[clock] = std::get<2>(row);
        t.fmax[clock] = std::get<3>(row);
    }
    return t;
}
} // namespace

TEST(AnalogueClockTiming, AnyClockLosingSetupSlackFmaxOrHoldSlackIsWorse)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    auto before = clocks(&ctx, {{"sys", 300, 100, 52.5f}, {"pixel", 4000, -50, 110.0f}});
    EXPECT_FALSE(before.worse_than(before));
    EXPECT_FALSE(before.better_than(before));
    // One clock gaining is better and not worse.
    auto gain = clocks(&ctx, {{"sys", 900, 100, 54.0f}, {"pixel", 4000, -50, 110.0f}});
    EXPECT_FALSE(gain.worse_than(before));
    EXPECT_TRUE(gain.better_than(before));
    // A gain on one clock does not pay for a loss on another.
    EXPECT_TRUE(clocks(&ctx, {{"sys", 900, 100, 54.0f}, {"pixel", 3999, -50, 110.0f}}).worse_than(before));
    EXPECT_TRUE(clocks(&ctx, {{"sys", 900, 100, 54.0f}, {"pixel", 4000, -50, 109.9f}}).worse_than(before));
    // Positive hold slack may shrink but not go negative; negative hold
    // slack may not get any worse.
    EXPECT_FALSE(clocks(&ctx, {{"sys", 300, 0, 52.5f}, {"pixel", 4000, -50, 110.0f}}).worse_than(before));
    EXPECT_TRUE(clocks(&ctx, {{"sys", 300, -1, 52.5f}, {"pixel", 4000, -50, 110.0f}}).worse_than(before));
    EXPECT_TRUE(clocks(&ctx, {{"sys", 300, 100, 52.5f}, {"pixel", 4000, -51, 110.0f}}).worse_than(before));
    EXPECT_FALSE(clocks(&ctx, {{"sys", 300, 100, 52.5f}, {"pixel", 4000, -40, 110.0f}}).worse_than(before));
    // A clock that is no longer timed is worse; a new one is ignored.
    EXPECT_TRUE(clocks(&ctx, {{"sys", 900, 100, 54.0f}}).worse_than(before));
    auto extra = clocks(&ctx, {{"sys", 300, 100, 52.5f}, {"pixel", 4000, -50, 110.0f}, {"audio", -10, -10, 1.0f}});
    EXPECT_FALSE(extra.worse_than(before));
    EXPECT_FALSE(extra.better_than(before));
}
