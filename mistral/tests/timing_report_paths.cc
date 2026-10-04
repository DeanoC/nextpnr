#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include "gtest/gtest.h"
#include "json11.hpp"
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

int64_t path_delay(const CriticalPath &path)
{
    int64_t result = 0;
    for (const auto &segment : path.segments) result += segment.delay;
    return result;
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

    void bind_clock_source(NetInfo *net, const std::string &name)
    {
        auto *driver = ctx->createCell(ctx->id(name), id_MISTRAL_CLKBUF);
        driver->addOutput(id_Q); driver->connectPort(id_Q, net);
        ctx->assign_default_pinmap(driver);
        for (auto bel : ctx->getBels()) {
            if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(driver->type, bel)) continue;
            ctx->bindBel(bel, driver, STRENGTH_USER);
            if (ctx->isBelLocationValid(bel)) return;
            ctx->unbindBel(bel);
        }
        FAIL() << "No legal native clock source BEL";
    }

    NetInfo *other_clock(const std::string &name)
    {
        auto *net = ctx->createNet(ctx->id(name)); net->is_global = true;
        net->clkconstr = std::make_unique<ClockConstraint>(*clock->clkconstr);
        bind_clock_source(net, name + "$source");
        return net;
    }

    NetInfo *physically_related_clock(const std::string &name)
    {
        auto *root = ctx->createNet(ctx->id(name + "$root"));
        bind_clock_source(root, name + "$root_source");
        auto branch = [&](NetInfo *output, const std::string &branch_name) {
            auto *cell = ctx->createCell(ctx->id(branch_name), id_MISTRAL_ALUT2);
            cell->params[id_LUT] = Property(int64_t(0xA), 4);
            cell->addInput(id_A); cell->addInput(id_B); cell->addOutput(id_Q);
            cell->connectPort(id_A, root); cell->connectPort(id_B, root); cell->connectPort(id_Q, output);
            ctx->assignArchInfo();
            for (auto bel : ctx->getBels()) {
                if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(cell->type, bel)) continue;
                ctx->bindBel(bel, cell, STRENGTH_USER);
                if (ctx->isBelLocationValid(bel)) return;
                ctx->unbindBel(bel);
            }
            FAIL() << "No legal related-clock branch BEL for " << branch_name;
        };
        auto *related = ctx->createNet(ctx->id(name)); related->is_global = true;
        related->clkconstr = std::make_unique<ClockConstraint>(*clock->clkconstr);
        branch(clock, name + "$primary_branch");
        branch(related, name + "$related_branch");
        return related;
    }

    CellInfo *other_launch(NetInfo *net)
    {
        auto *cell = ff("independent_launch");
        cell->disconnectPort(id_CLK); cell->connectPort(id_CLK, net);
        cell->connectPort(id_DATAIN, cell->getPort(id_Q));
        ctx->assignArchInfo(); place(cell, 25, 23);
        return cell;
    }

    CriticalPath native_setup_path(TimingAnalyser &timing, CellInfo *endpoint,
                                  const EndpointClockPairTiming &pair)
    {
        auto reports = legacy(timing.get_timing_result());
        auto extra = timing.get_report_setup_paths(16384);
        reports.insert(reports.end(), extra.begin(), extra.end());
        for (const auto &path : reports) {
            if (path.segments.empty() || path.segments.back().to != std::make_pair(endpoint->name, id_DATAIN)) continue;
            if (path.clock_pair.start.clock == pair.launch.clock && path.clock_pair.start.edge == pair.launch.edge &&
                path.clock_pair.end.clock == pair.capture.clock && path.clock_pair.end.edge == pair.capture.edge)
                return path;
        }
        ADD_FAILURE() << "Missing actual native setup path for returned endpoint clock pair";
        return CriticalPath();
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

TEST_F(TimingReportPathsTest, JsonReportCarriesCompleteHoldSlackAndExplicitModel)
{
    timing_analysis(ctx.get(), false, true, false, false, true);
    ASSERT_TRUE(ctx->timing_result.clock_setup_slack.count(clock->name));
    ASSERT_TRUE(ctx->timing_result.clock_hold_slack.count(clock->name));
    auto parse_report = [&]() {
        std::ostringstream out; ctx->writeJsonReport(out);
        std::string error; auto document = json11::Json::parse(out.str(), error);
        EXPECT_TRUE(error.empty()) << error;
        return document;
    };
    auto document = parse_report();
    EXPECT_FALSE(document["timing_summary"]["final_analogue_model"].bool_value());
    auto clock_summary = document["timing_summary"]["clocks"][clock->name.str(ctx.get())];
    EXPECT_TRUE(clock_summary["setup_wns_ns"].is_number());
    EXPECT_TRUE(clock_summary["hold_wns_ns"].is_number());
    EXPECT_DOUBLE_EQ(clock_summary["setup_wns_ns"].number_value(),
                     ctx->getDelayNS(ctx->timing_result.clock_setup_slack.at(clock->name)));
    EXPECT_DOUBLE_EQ(clock_summary["hold_wns_ns"].number_value(),
                     ctx->getDelayNS(ctx->timing_result.clock_hold_slack.at(clock->name)));
    // Related-clock-only timing can have complete enforced slacks without a
    // same-clock Fmax record; the final report must retain that evidence.
    ctx->timing_result.clock_fmax.clear();
    document = parse_report();
    EXPECT_TRUE(document["fmax"].object_items().empty());
    clock_summary = document["timing_summary"]["clocks"][clock->name.str(ctx.get())];
    EXPECT_TRUE(clock_summary["setup_wns_ns"].is_number());
    EXPECT_TRUE(clock_summary["hold_wns_ns"].is_number());
    ctx->timing_result_is_final_analogue = true;
    EXPECT_TRUE(parse_report()["timing_summary"]["final_analogue_model"].bool_value());
}

TEST_F(TimingReportPathsTest, PhysicallyRelatedOnlyLaunchClockCarriesFinalSlacks)
{
    auto *related_clock = physically_related_clock("related_clock");
    auto *related_launch = other_launch(related_clock);
    related_launch->disconnectPort(id_DATAIN);
    auto *endpoint = rising.front(); auto *logic = cone.at(endpoint->name);
    for (auto pin : {id_A, id_B}) {
        logic->disconnectPort(pin); logic->connectPort(pin, related_launch->getPort(id_Q));
    }
    ctx->assignArchInfo();
    timing_analysis(ctx.get(), false, true, false, false, true);
    ASSERT_FALSE(ctx->timing_result.clock_fmax.count(related_clock->name));
    ASSERT_TRUE(ctx->timing_result.clock_setup_slack.count(related_clock->name));
    ASSERT_TRUE(ctx->timing_result.clock_hold_slack.count(related_clock->name));
    std::ostringstream out; ctx->writeJsonReport(out);
    std::string error; auto document = json11::Json::parse(out.str(), error);
    ASSERT_TRUE(error.empty()) << error;
    const auto summary = document["timing_summary"]["clocks"][related_clock->name.str(ctx.get())];
    EXPECT_TRUE(summary["setup_wns_ns"].is_number());
    EXPECT_TRUE(summary["hold_wns_ns"].is_number());
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

TEST_F(TimingReportPathsTest, KnownUnrelatedEndpointHasFiniteTransferBoundsWithoutAnInventedWindow)
{
    bind_clock_source(clock, "primary_clock_source");
    auto *independent_clock = other_clock("independent_clock");
    auto *independent = other_launch(independent_clock);
    auto *endpoint = rising.front(); auto *logic = cone.at(endpoint->name);
    for (auto pin : {id_A, id_B}) {
        logic->disconnectPort(pin); logic->connectPort(pin, independent->getPort(id_Q));
    }
    ctx->assignArchInfo();
    TimingAnalyser timing(ctx.get()); timing.setup(false, false, true);
    EXPECT_EQ(timing.get_setup_slack(CellPortKey(endpoint->name, id_DATAIN)),
              float(std::numeric_limits<delay_t>::max()));
    std::vector<EndpointClockPairTiming> rows;
    const auto graph = ctx->checksum();
    ASSERT_TRUE(timing.get_endpoint_clock_pair_timings(CellPortKey(endpoint->name, id_DATAIN), rows));
    ASSERT_EQ(rows.size(), 1u);
    const auto &row = rows.front();
    EXPECT_EQ(row.launch.clock, independent_clock->name); EXPECT_EQ(row.capture.clock, clock->name);
    EXPECT_FALSE(row.setup_timed); EXPECT_FALSE(row.hold_related);
    EXPECT_FALSE(row.setup_window.has_value()); EXPECT_FALSE(row.setup_margin.has_value());
    EXPECT_FALSE(row.hold_margin.has_value());
    EXPECT_EQ(row.max_path_delay, path_delay(native_setup_path(timing, endpoint, row)));
    // The two aliases take distinct characterized LUT arcs. The shortest
    // transfer uses B, independently of the longest native setup backpointer.
    DelayQuad fastest;
    ASSERT_TRUE(ctx->getCellDelay(logic, id_B, id_Q, fastest));
    const auto capture = ctx->getPortClockingInfo(endpoint, id_DATAIN, 0);
    const auto start = ctx->getPortClockingInfo(independent, id_Q, 0);
    const int64_t shortest = int64_t(start.clockToQ.minDelay()) +
        ctx->getNetinfoRouteDelay(independent->getPort(id_Q), PortRef{logic, id_B}) + fastest.minDelay() +
        ctx->getNetinfoRouteDelay(logic->getPort(id_Q), PortRef{endpoint, id_DATAIN}) - capture.hold.maxDelay();
    EXPECT_EQ(row.min_path_delay, shortest);
    EXPECT_LT(row.min_path_delay, row.max_path_delay);
    EXPECT_EQ(ctx->checksum(), graph);
    ctx->check();
}

TEST_F(TimingReportPathsTest, MixedEndpointRetainsEveryTimedAndUnrelatedClockPair)
{
    bind_clock_source(clock, "primary_clock_source");
    auto *independent_clock = other_clock("independent_clock");
    auto *independent = other_launch(independent_clock);
    auto *endpoint = rising.front(); auto *logic = cone.at(endpoint->name);
    logic->disconnectPort(id_B); logic->connectPort(id_B, independent->getPort(id_Q));
    ctx->assignArchInfo();
    TimingAnalyser timing(ctx.get()); timing.with_clock_skew = true; timing.setup(false, false, true);
    std::vector<EndpointClockPairTiming> rows;
    ASSERT_TRUE(timing.get_endpoint_clock_pair_timings(CellPortKey(endpoint->name, id_DATAIN), rows));
    ASSERT_EQ(rows.size(), 2u);
    std::set<std::pair<IdString, IdString>> coverage;
    size_t constrained = 0, unrelated = 0;
    for (const auto &row : rows) {
        EXPECT_TRUE(coverage.emplace(row.launch.clock, row.capture.clock).second);
        EXPECT_EQ(row.capture.clock, clock->name);
        EXPECT_EQ(row.max_path_delay, path_delay(native_setup_path(timing, endpoint, row)));
        if (row.launch.clock == clock->name) {
            ++constrained;
            ASSERT_TRUE(row.setup_timed); ASSERT_TRUE(row.setup_window.has_value());
            ASSERT_TRUE(row.setup_margin.has_value()); ASSERT_TRUE(row.hold_margin.has_value());
            EXPECT_EQ(*row.setup_window, 10000);
            EXPECT_EQ(*row.setup_margin, *row.setup_window - row.max_path_delay);
            EXPECT_EQ(*row.hold_margin, row.min_path_delay);
            EXPECT_FLOAT_EQ(timing.get_setup_slack(CellPortKey(endpoint->name, id_DATAIN)),
                            float(*row.setup_margin));
        } else {
            ++unrelated;
            EXPECT_EQ(row.launch.clock, independent_clock->name);
            EXPECT_FALSE(row.setup_timed); EXPECT_FALSE(row.hold_related);
            EXPECT_FALSE(row.setup_window.has_value()); EXPECT_FALSE(row.setup_margin.has_value());
            EXPECT_FALSE(row.hold_margin.has_value());
        }
    }
    EXPECT_EQ(constrained, 1u); EXPECT_EQ(unrelated, 1u);
    std::vector<EndpointClockPairTiming> repeat;
    ASSERT_TRUE(timing.get_endpoint_clock_pair_timings(CellPortKey(endpoint->name, id_DATAIN), repeat));
    ASSERT_EQ(repeat.size(), rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(repeat[i].launch, rows[i].launch); EXPECT_EQ(repeat[i].capture, rows[i].capture);
        EXPECT_EQ(repeat[i].max_path_delay, rows[i].max_path_delay);
        EXPECT_EQ(repeat[i].min_path_delay, rows[i].min_path_delay);
    }
    ctx->check();
}

TEST_F(TimingReportPathsTest, PhaseRelatedEndpointRetainsItsActualSetupAndHoldWindows)
{
    bind_clock_source(clock, "primary_clock_source");
    clock->clkconstr->phase_group = ctx->id("shared_pll_phase");
    auto *phase_clock = other_clock("phase_clock");
    phase_clock->clkconstr->phase_shift = 1000;
    ctx->settings[ctx->id("timing/ignoreRelClk")] = true;
    auto *endpoint = rising.front();
    endpoint->disconnectPort(id_CLK); endpoint->connectPort(id_CLK, phase_clock);
    ctx->assignArchInfo();
    TimingAnalyser timing(ctx.get()); timing.with_clock_skew = true; timing.setup(false, false, true);
    std::vector<EndpointClockPairTiming> rows;
    ASSERT_TRUE(timing.get_endpoint_clock_pair_timings(CellPortKey(endpoint->name, id_DATAIN), rows));
    ASSERT_EQ(rows.size(), 1u);
    const auto &row = rows.front();
    EXPECT_EQ(row.launch.clock, clock->name); EXPECT_EQ(row.capture.clock, phase_clock->name);
    ASSERT_TRUE(row.setup_timed); ASSERT_TRUE(row.hold_related);
    ASSERT_TRUE(row.setup_window.has_value()); ASSERT_TRUE(row.setup_margin.has_value());
    ASSERT_TRUE(row.hold_margin.has_value());
    EXPECT_EQ(*row.setup_window, 1000); // rising launch to the actual +1ns capture edge
    EXPECT_EQ(row.max_path_delay, path_delay(native_setup_path(timing, endpoint, row)));
    EXPECT_EQ(*row.setup_margin, 1000 - row.max_path_delay);
    EXPECT_EQ(*row.hold_margin, row.min_path_delay);
    EXPECT_FLOAT_EQ(timing.get_setup_slack(CellPortKey(endpoint->name, id_DATAIN)), float(*row.setup_margin));
    ASSERT_TRUE(timing.get_timing_result().clock_setup_slack.count(clock->name));
    // ignoreRelClk does not exempt this timed phase relation from the setup/Fmax gate.
    EXPECT_EQ(timing.get_timing_result().clock_setup_slack.at(clock->name), *row.setup_margin);
    EXPECT_GT(row.min_path_delay, 9000); // previous capture edge contributes period minus interval
    ctx->check();
}

TEST_F(TimingReportPathsTest, RelatedClockOnlyReportUsesNetConstraintsAndFailsClosedWhenAllowed)
{
    auto *phase_clock = other_clock("phase_clock_only");
    phase_clock->clkconstr->period = DelayPair(20000);
    TimingResult result;
    CriticalPath report;
    report.clock_pair.start = ClockEvent{clock->name, RISING_EDGE};
    report.clock_pair.end = ClockEvent{phase_clock->name, RISING_EDGE};
    report.max_delay = 10000;
    CriticalPath::Segment segment;
    segment.type = CriticalPath::Segment::Type::CLK_TO_CLK;
    segment.delay = 13333;
    report.segments.push_back(segment);
    result.xclock_paths.push_back(report);
    ASSERT_TRUE(result.clock_fmax.empty());

    ctx->settings[ctx->id("timing/allowFail")] = true;
    EXPECT_FALSE(ctx->log_timing_results(result, false, true, false, true));

    result.clock_fmax[phase_clock->name] = ClockFmax{75.0f, 50.0f};
    EXPECT_FALSE(ctx->log_timing_results(result, false, true, false, true));
    result.clock_fmax[clock->name] = ClockFmax{75.0f, 100.0f};
    EXPECT_FALSE(ctx->log_timing_results(result, false, true, false, true));

    ctx->settings[ctx->id("timing/ignoreRelClk")] = true;
    EXPECT_TRUE(ctx->log_timing_results(result, false, true, false, true));
}

TEST_F(TimingReportPathsTest, AllowedHoldViolationStillFailsTheTimingGate)
{
    TimingResult result;
    result.min_delay_violations.emplace_back();
    ctx->settings[ctx->id("timing/allowFail")] = true;
    EXPECT_FALSE(ctx->log_timing_results(result, false, true, false, true));
    EXPECT_TRUE(ctx->log_timing_results(result, false, false, false, false));

    CriticalPath cross_clock_hold;
    cross_clock_hold.clock_pair.start = ClockEvent{clock->name, RISING_EDGE};
    cross_clock_hold.clock_pair.end = ClockEvent{ctx->id("other_clock"), RISING_EDGE};
    result.min_delay_violations = {cross_clock_hold};
    ctx->settings[ctx->id("timing/ignoreRelClk")] = true;
    EXPECT_TRUE(ctx->log_timing_results(result, false, false, false, true));
}

TEST_F(TimingReportPathsTest, UnknownClocksAsyncLaunchesAndIncompleteAnalysisFailClosed)
{
    bind_clock_source(clock, "primary_clock_source");
    auto *independent_clock = other_clock("independent_clock");
    auto *independent = other_launch(independent_clock);
    auto *endpoint = rising.front(); auto *logic = cone.at(endpoint->name);
    logic->disconnectPort(id_B); logic->connectPort(id_B, independent->getPort(id_Q));
    ctx->assignArchInfo();
    std::vector<EndpointClockPairTiming> rows;
    {
        TimingAnalyser complete(ctx.get()); complete.setup(false, false, true);
        ASSERT_TRUE(complete.get_endpoint_clock_pair_timings(CellPortKey(endpoint->name, id_DATAIN), rows));
        ASSERT_EQ(rows.size(), 2u);
    }
    auto known = std::move(independent_clock->clkconstr);
    {
        TimingAnalyser unknown(ctx.get()); unknown.setup(false, false, true);
        EXPECT_FALSE(unknown.get_endpoint_clock_pair_timings(CellPortKey(endpoint->name, id_DATAIN), rows));
        EXPECT_TRUE(rows.empty());
    }
    independent_clock->clkconstr = std::move(known);
    {
        TimingAnalyser changed_flags(ctx.get()); changed_flags.setup(false, false, true);
        ASSERT_TRUE(changed_flags.get_endpoint_clock_pair_timings(CellPortKey(endpoint->name, id_DATAIN), rows));
        changed_flags.with_clock_skew = true;
        EXPECT_FALSE(changed_flags.get_endpoint_clock_pair_timings(CellPortKey(endpoint->name, id_DATAIN), rows));
        EXPECT_TRUE(rows.empty()); // flags cannot relabel an already completed unskewed analysis
    }
    {
        TimingAnalyser incomplete(ctx.get()); incomplete.setup_only = true; incomplete.setup();
        EXPECT_FALSE(incomplete.get_endpoint_clock_pair_timings(CellPortKey(endpoint->name, id_DATAIN), rows));
        EXPECT_TRUE(rows.empty());
    }
    // PLL lock is an actual asynchronous startpoint in the Mistral model.
    auto *asynchronous = ctx->createCell(ctx->id("asynchronous_lock"), id_altera_pll);
    asynchronous->addOutput(id_locked);
    auto *lock = ctx->createNet(ctx->id("asynchronous_lock_signal"));
    asynchronous->connectPort(id_locked, lock);
    logic->disconnectPort(id_B); logic->connectPort(id_B, lock);
    ctx->assignArchInfo();
    {
        TimingAnalyser mixed(ctx.get()); mixed.setup(false, false, true);
        EXPECT_FALSE(mixed.get_endpoint_clock_pair_timings(CellPortKey(endpoint->name, id_DATAIN), rows));
        EXPECT_TRUE(rows.empty()); // the remaining finite synchronous pair cannot hide the async branch
        EXPECT_FALSE(mixed.get_endpoint_clock_pair_timings(CellPortKey(launch->name, id_Q), rows));
        EXPECT_TRUE(rows.empty());
    }
    ctx->check();
}
