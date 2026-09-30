#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include "gtest/gtest.h"
#include "jsonwrite.h"
#include "log.h"
#include "nextpnr.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE

namespace {
using PathKey = std::tuple<IdString, int, IdString, int, IdString, IdString>;

PathKey key(const CriticalPath &path)
{
    const auto &pair = path.clock_pair;
    const auto &endpoint = path.segments.back().to;
    return {pair.start.clock, int(pair.start.edge), pair.end.clock, int(pair.end.edge),
            endpoint.first, endpoint.second};
}

std::string paths(Context *ctx, const std::vector<CriticalPath> &reports)
{
    std::ostringstream out;
    for (const auto &report : reports) {
        out << report.clock_pair.start.clock.str(ctx) << ':' << int(report.clock_pair.start.edge)
            << '>' << report.clock_pair.end.clock.str(ctx) << ':' << int(report.clock_pair.end.edge)
            << ':' << report.max_delay << '\n';
        for (const auto &segment : report.segments)
            out << int(segment.type) << ':' << segment.from.first.str(ctx) << '.' << segment.from.second.str(ctx)
                << '>' << segment.to.first.str(ctx) << '.' << segment.to.second.str(ctx)
                << ':' << segment.net.str(ctx) << ':' << segment.delay << '\n';
    }
    return out.str();
}

std::vector<CriticalPath> legacy(const TimingResult &result)
{
    std::vector<CriticalPath> reports;
    for (const auto &entry : result.clock_paths) reports.push_back(entry.second);
    reports.insert(reports.end(), result.xclock_paths.begin(), result.xclock_paths.end());
    return reports;
}
} // namespace

class TimingReportPathsTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *clock;
    CellInfo *launch;
    std::map<IdString, CellInfo *> cone;
    std::vector<CellInfo *> rising, falling;

    CellInfo *ff(const std::string &name, bool inverted = false)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN}) cell->addInput(pin);
        cell->addOutput(id_Q);
        cell->connectPort(id_CLK, clock);
        cell->pin_data[id_CLK].state = inverted ? PIN_INV : PIN_SIG;
        cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        cell->connectPort(id_Q, ctx->createNet(ctx->id(name + "$q")));
        return cell;
    }

    void place(CellInfo *cell, int x, int y)
    {
        for (auto bel : ctx->getBelsByTile(x, y)) {
            if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(cell->type, bel)) continue;
            ctx->bindBel(bel, cell, STRENGTH_USER);
            if (ctx->isBelLocationValid(bel)) return;
            ctx->unbindBel(bel);
        }
        FAIL() << "No legal report fixture BEL for " << cell->name.str(ctx.get()) << " at " << x << ',' << y;
    }

    void capture(const std::string &name, bool inverted)
    {
        auto *cell = ff(name, inverted);
        auto *logic = ctx->createCell(ctx->id(name + "$logic"), id_MISTRAL_ALUT2);
        logic->params[id_LUT] = Property(int64_t(0x8), 4);
        logic->addInput(id_A); logic->addInput(id_B); logic->addOutput(id_Q);
        logic->connectPort(id_A, launch->getPort(id_Q));
        logic->connectPort(id_B, launch->getPort(id_Q));
        logic->connectPort(id_Q, ctx->createNet(ctx->id(name + "$data")));
        cell->connectPort(id_DATAIN, logic->getPort(id_Q));
        ctx->assignArchInfo();
        // Pair every cone with its own directly reachable FF. First-free
        // independent binding can accidentally give one cone the other
        // half's FF and create a different 300ps route instead of a tie.
        bool bound = false;
        for (auto bel : ctx->getBelsByTile(30, inverted ? 21 : 20)) {
            const auto loc = ctx->getBelLocation(bel);
            if (loc.z % 6 != 0 || !ctx->checkBelAvail(bel) ||
                !ctx->isValidBelForCellType(logic->type, bel)) continue;
            auto ff_bel = ctx->getBelByLocation(Loc(loc.x, loc.y, loc.z + 2));
            if (ff_bel == BelId() || !ctx->checkBelAvail(ff_bel) ||
                !ctx->isValidBelForCellType(cell->type, ff_bel)) continue;
            ctx->bindBel(bel, logic, STRENGTH_USER);
            ctx->bindBel(ff_bel, cell, STRENGTH_USER);
            if (ctx->isBelLocationValid(bel) && ctx->isBelLocationValid(ff_bel)) {
                bound = true; break;
            }
            ctx->unbindBel(ff_bel); ctx->unbindBel(bel);
        }
        ASSERT_TRUE(bound) << "No legal paired LUT/FF report fixture at capture tile";
        cone.emplace(cell->name, logic);
        (inverted ? falling : rising).push_back(cell);
    }

    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 1e8;
        clock = ctx->createNet(ctx->id("report_clock"));
        clock->is_global = true;
        clock->clkconstr = std::make_unique<ClockConstraint>();
        clock->clkconstr->period = DelayPair(10000);
        clock->clkconstr->high = DelayPair(3000);
        clock->clkconstr->low = DelayPair(7000);
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        launch = ff("launch");
        launch->connectPort(id_DATAIN, launch->getPort(id_Q));
        ctx->assignArchInfo();
        place(launch, 24, 20);
        // Deliberately create tied endpoints out of lexical order.
        for (const auto &name : {"r_zeta", "r_beta", "r_alpha", "r_gamma"}) capture(name, false);
        for (const auto &name : {"f_zeta", "f_alpha"}) capture(name, true);
        ctx->assignArchInfo();
        for (const auto &entry : ctx->cells) ASSERT_TRUE(ctx->isBelLocationValid(entry.second->bel));
        ctx->check();
    }

    void expect_native_path(const CriticalPath &path)
    {
        ASSERT_FALSE(path.segments.empty());
        EXPECT_EQ(path.clock_pair.start.clock, clock->name);
        EXPECT_EQ(path.clock_pair.end.clock, clock->name);
        EXPECT_EQ(path.clock_pair.start.edge, RISING_EDGE);
        const auto endpoint = path.segments.back().to;
        EXPECT_EQ(endpoint.second, id_DATAIN);
        const bool inverted = ctx->cells.at(endpoint.first)->pin_data.at(id_CLK).state == PIN_INV;
        EXPECT_EQ(path.clock_pair.end.edge, inverted ? FALLING_EDGE : RISING_EDGE);
        EXPECT_EQ(path.max_delay, inverted ? 3000 : 10000);
        EXPECT_EQ(path.segments.front().type, CriticalPath::Segment::Type::CLK_TO_Q);
        EXPECT_EQ(path.segments.front().from, std::make_pair(launch->name, id_Q));
        EXPECT_EQ(path.segments.back().type, CriticalPath::Segment::Type::SETUP);
        auto *sink = ctx->cells.at(endpoint.first).get();
        EXPECT_EQ(path.segments.back().delay, ctx->getPortClockingInfo(sink, id_DATAIN, 0).setup.maxDelay());
        for (const auto &segment : path.segments) {
            ASSERT_TRUE(ctx->cells.count(segment.from.first));
            ASSERT_TRUE(ctx->cells.count(segment.to.first));
            auto *from = ctx->cells.at(segment.from.first).get();
            auto *to = ctx->cells.at(segment.to.first).get();
            ASSERT_TRUE(from->ports.count(segment.from.second));
            ASSERT_TRUE(to->ports.count(segment.to.second));
            if (segment.type == CriticalPath::Segment::Type::ROUTING) {
                auto *net = from->getPort(segment.from.second);
                ASSERT_NE(net, nullptr);
                EXPECT_EQ(to->getPort(segment.to.second), net);
                EXPECT_EQ(segment.net, net->name);
                EXPECT_EQ(segment.delay, ctx->getNetinfoRouteDelay(net, PortRef{to, segment.to.second}));
            } else if (segment.type == CriticalPath::Segment::Type::LOGIC) {
                EXPECT_EQ(from, to);
                DelayQuad delay;
                ASSERT_TRUE(ctx->getCellDelay(from, segment.from.second, segment.to.second, delay));
                EXPECT_EQ(segment.delay, delay.maxDelay());
            }
        }
        if (endpoint.first != launch->name) {
            ASSERT_TRUE(cone.count(endpoint.first));
            auto *logic = cone.at(endpoint.first);
            EXPECT_TRUE(std::any_of(path.segments.begin(), path.segments.end(), [&](const CriticalPath::Segment &s) {
                return s.type == CriticalPath::Segment::Type::LOGIC && s.from.first == logic->name;
            }));
        }
    }
};

TEST_F(TimingReportPathsTest, TiedEndpointsHaveBoundedNativePathsAndRealPhaseWindows)
{
    TimingAnalyser timing(ctx.get());
    timing.setup(false, false, true);
    auto &result = timing.get_timing_result();
    const auto old = paths(ctx.get(), legacy(result));
    ASSERT_EQ(result.clock_fmax.size(), 1u);
    EXPECT_FLOAT_EQ(result.clock_fmax.at(clock->name).constraint, 100.0f);
    for (const auto &group : {rising, falling}) {
        ASSERT_GE(group.size(), 2u);
        for (auto *cell : group)
            EXPECT_EQ(timing.get_setup_slack(CellPortKey(cell->name, id_DATAIN)),
                      timing.get_setup_slack(CellPortKey(group.front()->name, id_DATAIN)));
    }
    const auto before = ctx->checksum();
    EXPECT_TRUE(timing.get_report_setup_paths(1).empty());
    for (int count : {2, 3, 256, 16384}) {
        auto extra = timing.get_report_setup_paths(count);
        EXPECT_EQ(old, paths(ctx.get(), legacy(result)));
        EXPECT_TRUE(result.report_setup_paths.empty());
        std::set<PathKey> seen;
        std::map<int, int> totals;
        auto all = legacy(result);
        all.insert(all.end(), extra.begin(), extra.end());
        for (const auto &report : all) {
            expect_native_path(report);
            EXPECT_TRUE(seen.insert(key(report)).second);
            ++totals[int(report.clock_pair.end.edge)];
        }
        EXPECT_EQ(totals[int(RISING_EDGE)], std::min(count, 5));
        EXPECT_EQ(totals[int(FALLING_EDGE)], std::min(count, 2));
        // Each equal-delay cohort is sorted by native name after preserving
        // whichever endpoint the unchanged legacy summary selected.
        for (auto edge : {RISING_EDGE, FALLING_EDGE}) {
            std::vector<std::string> actual;
            for (const auto &report : extra) {
                const auto &name = report.segments.back().to.first;
                if (report.clock_pair.end.edge == edge && name != launch->name) actual.push_back(name.str(ctx.get()));
            }
            EXPECT_TRUE(std::is_sorted(actual.begin(), actual.end()));
        }
        EXPECT_EQ(paths(ctx.get(), extra), paths(ctx.get(), timing.get_report_setup_paths(count)));
    }
    EXPECT_EQ(ctx->checksum(), before);
    ctx->check();
}

TEST_F(TimingReportPathsTest, FinalRefreshClearsExtrasWithoutChangingLegacyOrNativeGraph)
{
    auto netlist = [&]() {
        std::ostringstream out; std::string filename = "report-fixture.json";
        EXPECT_TRUE(write_json_file(out, filename, ctx.get()));
        return out.str();
    };
    auto report = [&]() { std::ostringstream out; ctx->writeJsonReport(out); return out.str(); };
    timing_analysis(ctx.get(), false, true, false, false, true);
    const auto baseline = report(), graph = netlist();
    const auto base_legacy = paths(ctx.get(), legacy(ctx->timing_result));
    const auto holds = paths(ctx.get(), ctx->timing_result.min_delay_violations);
    const auto fmax = ctx->timing_result.clock_fmax.at(clock->name);
    ctx->timing_report_paths = 3;
    timing_analysis(ctx.get(), false, true, false, false, false);
    EXPECT_TRUE(ctx->timing_result.report_setup_paths.empty());
    timing_analysis(ctx.get(), false, false, false, false, true);
    ASSERT_FALSE(ctx->timing_result.report_setup_paths.empty());
    const auto extra = paths(ctx.get(), ctx->timing_result.report_setup_paths);
    EXPECT_EQ(base_legacy, paths(ctx.get(), legacy(ctx->timing_result)));
    EXPECT_EQ(holds, paths(ctx.get(), ctx->timing_result.min_delay_violations));
    EXPECT_FLOAT_EQ(fmax.achieved, ctx->timing_result.clock_fmax.at(clock->name).achieved);
    EXPECT_FLOAT_EQ(fmax.constraint, ctx->timing_result.clock_fmax.at(clock->name).constraint);
    EXPECT_EQ(graph, netlist());
    timing_analysis(ctx.get(), false, true, false, false, true);
    EXPECT_EQ(extra, paths(ctx.get(), ctx->timing_result.report_setup_paths));
    ctx->timing_report_paths = 1;
    timing_analysis(ctx.get(), false, true, false, false, true);
    EXPECT_TRUE(ctx->timing_result.report_setup_paths.empty());
    EXPECT_EQ(baseline, report());
    EXPECT_EQ(graph, netlist());
}

TEST_F(TimingReportPathsTest, InvalidPublicLimitsFailWithoutChangingResults)
{
    TimingAnalyser timing(ctx.get()); timing.setup(false, false, true);
    const auto before = paths(ctx.get(), legacy(timing.get_timing_result()));
    for (int count : {0, -1, 16385}) {
        EXPECT_THROW(timing.get_report_setup_paths(count), log_execution_error_exception);
        EXPECT_EQ(before, paths(ctx.get(), legacy(timing.get_timing_result())));
        EXPECT_TRUE(timing.get_timing_result().report_setup_paths.empty());
    }
    ctx->check();
}
