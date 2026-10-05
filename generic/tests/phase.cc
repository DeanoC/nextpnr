/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  Deano Calver <deano@geometric.so>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "gtest/gtest.h"
#include "log.h"
#include "nextpnr.h"
#include "timing.h"

#include <sstream>

USING_NEXTPNR_NAMESPACE

// Exercise the common analyser without a device database or routing randomness.
TEST(Timing, RelatedClockPhase)
{
    for (int phase : {0, 10, 20, 30}) {
        for (ClockEdge launch : {RISING_EDGE, FALLING_EDGE}) {
            for (ClockEdge capture : {RISING_EDGE, FALLING_EDGE}) {
                for (bool skew : {false, true}) {
                    SCOPED_TRACE(::testing::Message() << "phase=" << phase << " launch=" << launch
                                                      << " capture=" << capture << " skew=" << skew);
                    Context ctx(ArchArgs{});
                    ctx.settings[ctx.id("target_freq")] = 25e6;
                    auto clk = ctx.createNet(ctx.id("clk"));
                    clk->clkconstr = std::make_unique<ClockConstraint>();
                    clk->clkconstr->period = DelayPair(40);
                    clk->clkconstr->high = DelayPair(20);
                    clk->clkconstr->low = DelayPair(20);
                    clk->clkconstr->phase_group = ctx.id("pll");
                    auto capture_clk = ctx.createNet(ctx.id("capture_clk"));
                    capture_clk->clkconstr = std::make_unique<ClockConstraint>(*clk->clkconstr);
                    capture_clk->clkconstr->phase_shift = phase;
                    auto data = ctx.createNet(ctx.id("data"));
                    auto source = ctx.createCell(ctx.id("source"), ctx.id("FF"));
                    auto sink = ctx.createCell(ctx.id("sink"), ctx.id("FF"));
                    auto c = ctx.id("CLK"), q = ctx.id("Q"), d = ctx.id("D");
                    source->addInput(c);
                    source->addOutput(q);
                    sink->addInput(c);
                    sink->addInput(d);
                    source->connectPort(c, clk);
                    sink->connectPort(c, capture_clk);
                    source->connectPort(q, data);
                    sink->connectPort(d, data);
                    ctx.addCellTimingClock(source->name, c);
                    ctx.addCellTimingClock(sink->name, c);
                    ctx.addCellTimingClockToOut(source->name, q, c, 2);
                    ctx.addCellTimingSetupHold(sink->name, d, c, 1, 0);
                    ctx.cellTiming[source->name].clockingInfo[q][0].edge = launch;
                    ctx.cellTiming[sink->name].clockingInfo[d][0].edge = capture;
                    TimingAnalyser timing(&ctx);
                    timing.with_clock_skew = skew;
                    timing.setup();
                    timing.set_route_delay(CellPortKey(sink->name, d), DelayPair(5));
                    timing.set_route_delay(CellPortKey(source->name, c), DelayPair(3));
                    timing.set_route_delay(CellPortKey(sink->name, c), DelayPair(4));
                    timing.run(false, false, true, true);
                    // Explicit edge-distance table, independent of the analyser's wrapping logic.
                    int interval;
                    if (phase == 0)
                        interval = launch == capture ? 40 : 20;
                    else if (phase == 10)
                        interval = launch == capture ? 10 : 30;
                    else if (phase == 20)
                        interval = launch == capture ? 20 : 40;
                    else
                        interval = launch == capture ? 30 : 10;
                    // 2 ns clock-to-Q + 5 ns data route + 1 ns setup,
                    // reduced by the optional 1 ns positive capture-clock skew.
                    float delay = skew ? 7 : 8;
                    EXPECT_FLOAT_EQ(timing.get_setup_slack(CellPortKey(sink->name, d)), interval - delay);
                    auto &result = timing.get_timing_result();
                    EXPECT_NEAR(result.clock_fmax.at(clk->name).achieved, 1000 * interval / 40 / delay, 1e-4);
                    EXPECT_FLOAT_EQ(result.clock_paths.at(clk->name).max_delay, interval);
                    EXPECT_EQ(result.slack_histogram.count(int((interval - delay) * 1000)), 1u);
                    EXPECT_TRUE(result.xclock_paths.empty());
                    EXPECT_TRUE(result.min_delay_violations.empty());

                    // The previous capture edge is (40 - interval) ns before launch.
                    // A hold requirement one ns beyond data arrival must violate;
                    // one ns below that boundary must pass.
                    int hold_boundary = 7 + (40 - interval) - (skew ? 1 : 0);
                    for (int excess : {-1, 1}) {
                        ctx.cellTiming[sink->name].clockingInfo[d][0].hold = DelayPair(hold_boundary + excess);
                        TimingAnalyser hold_timing(&ctx);
                        hold_timing.with_clock_skew = skew;
                        hold_timing.setup();
                        hold_timing.set_route_delay(CellPortKey(sink->name, d), DelayPair(5));
                        hold_timing.set_route_delay(CellPortKey(source->name, c), DelayPair(3));
                        hold_timing.set_route_delay(CellPortKey(sink->name, c), DelayPair(4));
                        hold_timing.run(false, false, false, true);
                        EXPECT_EQ(hold_timing.get_timing_result().min_delay_violations.size(), excess > 0 ? 1u : 0u);
                    }
                }
            }
        }
    }
}

namespace {

void add_clock(Context &ctx, NetInfo *net, delay_t period)
{
    net->clkconstr = std::make_unique<ClockConstraint>();
    net->clkconstr->period = DelayPair(period);
    net->clkconstr->high = DelayPair(period / 2);
    net->clkconstr->low = DelayPair(period / 2);
}

// Two clocks from one root through unequal combinational delays, plus one
// data path from launch to capture. The reported path uses cell delays: the
// route is unplaced, so the clock-to-clock segment is what the gate sees.
void build_skewed_clocks(Context &ctx, delay_t delay_a, delay_t delay_b)
{
    auto ref = ctx.createNet(ctx.id("ref"));
    auto clk_a = ctx.createNet(ctx.id("clk_a"));
    auto clk_b = ctx.createNet(ctx.id("clk_b"));
    auto data = ctx.createNet(ctx.id("data"));
    auto osc = ctx.createCell(ctx.id("osc"), ctx.id("OSC"));
    auto buf_a = ctx.createCell(ctx.id("buf_a"), ctx.id("BUF"));
    auto buf_b = ctx.createCell(ctx.id("buf_b"), ctx.id("BUF"));
    auto source = ctx.createCell(ctx.id("source"), ctx.id("FF"));
    auto sink = ctx.createCell(ctx.id("sink"), ctx.id("FF"));
    auto y = ctx.id("Y"), i = ctx.id("I"), o = ctx.id("O");
    auto c = ctx.id("CLK"), q = ctx.id("Q"), d = ctx.id("D");
    osc->addOutput(y);
    osc->connectPort(y, ref);
    buf_a->addInput(i);
    buf_a->addOutput(o);
    buf_b->addInput(i);
    buf_b->addOutput(o);
    buf_a->connectPort(i, ref);
    buf_a->connectPort(o, clk_a);
    buf_b->connectPort(i, ref);
    buf_b->connectPort(o, clk_b);
    ctx.addCellTimingDelay(buf_a->name, i, o, delay_a);
    ctx.addCellTimingDelay(buf_b->name, i, o, delay_b);
    source->addInput(c);
    source->addOutput(q);
    sink->addInput(c);
    sink->addInput(d);
    source->connectPort(c, clk_a);
    source->connectPort(q, data);
    sink->connectPort(c, clk_b);
    sink->connectPort(d, data);
    ctx.addCellTimingClock(source->name, c);
    ctx.addCellTimingClock(sink->name, c);
    ctx.addCellTimingClockToOut(source->name, q, c, 2);
    ctx.addCellTimingSetupHold(sink->name, d, c, 1, 0);
    add_clock(ctx, clk_a, 10);
    add_clock(ctx, clk_b, 10);
    TimingAnalyser timing(&ctx);
    timing.setup();
    timing.run(false, false, false, true);
    ctx.timing_result = timing.get_timing_result();
}

bool clock_to_clock_segment(const TimingResult &result)
{
    for (const auto &path : result.xclock_paths)
        for (const auto &segment : path.segments)
            if (segment.type == CriticalPath::Segment::Type::CLK_TO_CLK && !is_zero_delay(segment.delay))
                return true;
    return false;
}

bool timing_gate(Context &ctx, std::string &log)
{
    std::ostringstream captured;
    log_streams.emplace_back(&captured, LogLevel::LOG_MSG);
    had_nonfatal_error = false;
    bool met = ctx.log_timing_results(ctx.timing_result, false, true, false, true);
    log_streams.pop_back();
    log = captured.str();
    return met && !had_nonfatal_error;
}

} // namespace

// A shared driver gives the crossing a clock-to-clock segment. Without a cut
// that segment fails a 100 MHz target. set_false_path must not.
TEST(Timing, CutRelatedClockDoesNotFailGate)
{
    Context open(ArchArgs{});
    open.settings[open.id("target_freq")] = 100e6;
    build_skewed_clocks(open, 20, 1);
    EXPECT_TRUE(clock_to_clock_segment(open.timing_result));
    std::string open_log;
    EXPECT_FALSE(timing_gate(open, open_log));
    EXPECT_NE(open_log.find("Max frequency for"), std::string::npos) << open_log;
    EXPECT_NE(open_log.find("FAIL"), std::string::npos) << open_log;

    Context cut(ArchArgs{});
    cut.settings[cut.id("target_freq")] = 100e6;
    BaseCtx::SdcClockException exception;
    exception.false_path = true;
    exception.from = {"clk_a"};
    exception.to = {"clk_b"};
    cut.sdc_clock_exceptions.push_back(exception);
    build_skewed_clocks(cut, 20, 1);
    EXPECT_TRUE(clock_to_clock_segment(cut.timing_result));
    std::string cut_log;
    EXPECT_TRUE(timing_gate(cut, cut_log)) << cut_log;
    EXPECT_EQ(cut_log.find("FAIL"), std::string::npos) << cut_log;
    EXPECT_NE(cut_log.find("Max delay"), std::string::npos) << cut_log;
}

// 2 ns clock-to-Q + 27 ns route + 1 ns setup on a 20 ns clock. -setup 2
// makes setup slack +10 ns. The single-cycle histogram value is -10 ns.
TEST(Timing, MulticycleSlackHistogram)
{
    Context ctx(ArchArgs{});
    ctx.settings[ctx.id("target_freq")] = 50e6;
    auto clk = ctx.createNet(ctx.id("clk"));
    add_clock(ctx, clk, 20);
    auto data = ctx.createNet(ctx.id("data"));
    auto source = ctx.createCell(ctx.id("source"), ctx.id("FF"));
    auto sink = ctx.createCell(ctx.id("sink"), ctx.id("FF"));
    auto c = ctx.id("CLK"), q = ctx.id("Q"), d = ctx.id("D");
    source->addInput(c);
    source->addOutput(q);
    sink->addInput(c);
    sink->addInput(d);
    source->connectPort(c, clk);
    source->connectPort(q, data);
    sink->connectPort(c, clk);
    sink->connectPort(d, data);
    ctx.addCellTimingClock(source->name, c);
    ctx.addCellTimingClock(sink->name, c);
    ctx.addCellTimingClockToOut(source->name, q, c, 2);
    ctx.addCellTimingSetupHold(sink->name, d, c, 1, 0);
    BaseCtx::SdcClockException exception;
    exception.false_path = false;
    exception.setup_multiplier = 2;
    exception.from = {"clk"};
    exception.to = {"clk"};
    ctx.sdc_clock_exceptions.push_back(exception);

    TimingAnalyser timing(&ctx);
    timing.setup();
    timing.set_route_delay(CellPortKey(sink->name, d), DelayPair(27));
    timing.run(false, false, true, true);
    EXPECT_FLOAT_EQ(timing.get_setup_slack(CellPortKey(sink->name, d)), 10);
    const auto &histogram = timing.get_timing_result().slack_histogram;
    EXPECT_EQ(histogram.count(10000), 1u);
    EXPECT_EQ(histogram.count(-10000), 0u);
}
