#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include "gtest/gtest.h"
#include "nextpnr.h"
USING_NEXTPNR_NAMESPACE
TEST(ReadyTimingTest, HpsReadyPlacementPredictionUsesActualOutputPinWireWhenEnabled)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    const char *prior = std::getenv("NEXTPNR_MISTRAL_HPS_READY_PIN_PREDICT");
    std::string saved = prior ? prior : "";
    bool had_prior = prior != nullptr;
    unsetenv("NEXTPNR_MISTRAL_HPS_READY_PIN_PREDICT");
    Context baseline(args);
    BelId source = baseline.getBelByLocation(Loc(52, 53, 0));
    BelId sink = baseline.getBelByLocation(Loc(30, 26, 48));
    ASSERT_NE(source, BelId());
    ASSERT_NE(sink, BelId());
    ASSERT_EQ(baseline.getBelType(source), baseline.id("cyclonev_hps_interface_fpga2sdram"));
    WireId wire = baseline.getBelPinWire(source, baseline.id("cmd_ready_1"));
    ASSERT_NE(wire, WireId());
    EXPECT_EQ(wire.node.t(), CycloneV::GIN);
    EXPECT_EQ(wire.node.x(), 51);
    EXPECT_EQ(wire.node.y(), 64);
    EXPECT_EQ(baseline.predictDelay(source, baseline.id("cmd_ready_1"), sink, baseline.id("A")), 4370);
    setenv("NEXTPNR_MISTRAL_HPS_READY_PIN_PREDICT", "1", 1);
    Context enabled(args);
    EXPECT_EQ(enabled.predictDelay(source, enabled.id("cmd_ready_1"), sink, enabled.id("A")), 5435);
    EXPECT_EQ(enabled.predictDelay(source, enabled.id("cmd_ready_2"), sink, enabled.id("A")), 4370);
    setenv("NEXTPNR_MISTRAL_HPS_READY_PIN_PREDICT", "", 1);
    Context empty(args);
    EXPECT_EQ(empty.predictDelay(source, empty.id("cmd_ready_1"), sink, empty.id("A")), 4370);
    if (had_prior)
        setenv("NEXTPNR_MISTRAL_HPS_READY_PIN_PREDICT", saved.c_str(), 1);
    else
        unsetenv("NEXTPNR_MISTRAL_HPS_READY_PIN_PREDICT");
}
TEST(ReadyTimingTest, ActualHpsReadyInputWaveFailureIsExplained)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    auto net = ctx.createNet(ctx.id("ready_trace_fixture"));
    WireId src(CycloneV::rnode_coords(CycloneV::GIN, 51, 64, 18)),
            dst(CycloneV::rnode_coords(CycloneV::H6, 46, 64, 32));
    auto port = [&](const char *n, PortType dir, WireId wire) {
        auto id = ctx.id(n), pin = ctx.id(dir == PORT_OUT ? "cmd_ready_1" : "pin");
        ctx.createRegionPlug(id, ctx.id(dir == PORT_OUT ? "cyclonev_hps_interface_fpga2sdram" : "trace_plug"), Loc());
        ctx.addPlugPin(id, pin, dir, wire);
        auto c = ctx.cells.at(id).get();
        c->connectPort(pin, net);
        return c;
    };
    port("source", PORT_OUT, src);
    auto sink = port("sink", PORT_IN, dst);
    ctx.bindWire(src, net, STRENGTH_WEAK);
    ctx.bindPip(PipId(src.node, dst.node), net, STRENGTH_WEAK);
    Arch::AnalogueTrace trace;
    std::vector<Arch::AnalogueHop> hops;
    DelayQuad d;
    EXPECT_FALSE(ctx.analogue_arc_delay(net, {sink, ctx.id("pin")}, d, &hops, &trace));
    DelayQuad untraced;
    std::vector<Arch::AnalogueHop> untraced_hops;
    EXPECT_FALSE(ctx.analogue_arc_delay(net, {sink, ctx.id("pin")}, untraced, &untraced_hops));
    ASSERT_EQ(untraced_hops.size(), hops.size());
    for (size_t i = 0; i < hops.size(); ++i) {
        EXPECT_EQ(untraced_hops[i].pip, hops[i].pip);
        EXPECT_EQ(untraced_hops[i].table, hops[i].table);
        EXPECT_EQ(untraced_hops[i].rise, hops[i].rise);
        EXPECT_EQ(untraced_hops[i].fall, hops[i].fall);
    }
    EXPECT_STREQ(trace.reason, "empty_input_wave");
    EXPECT_TRUE(trace.route_complete);
    EXPECT_EQ(trace.failure_hop, 0);
    ASSERT_EQ(trace.hops.size(), 1);
    EXPECT_FALSE(trace.hops[0].completed);
    EXPECT_EQ(trace.hops[0].input_rise_samples, 0);
    EXPECT_EQ(trace.hops[0].input_fall_samples, 0);
    EXPECT_TRUE(hops.empty());
    // Exercise the complete exporter on the real first-hop failure without a
    // full route/bitgen fixture. This source fails before circuit simulation.
    ctx.bitstream_configured = true;
    ctx.analogue_cache_valid = true;
    for (auto &user : net->users)
        ctx.analogue_arc_cache.emplace(&user, Arch::AnalogueArc{DelayQuad(), false});
    auto pip = PipId(src.node, dst.node);
    // Artificial legacy seed tests exporter behavior; the collector no longer
    // creates this entry from the unfinished hop.
    ctx.pip_delay_observed[pip] = 0;
    ctx.pip_delay_calibrated = true;
    auto &cal = ctx.pip_type_calibration[CycloneV::GIN];
    cal.table_ps = ctx.getPipDelayTable(pip).maxDelay();
    cal.analogue_ps = 0;
    cal.hops = 1;
    const char *configured = std::getenv("MISTRAL_READY_TRACE_TEST_PREFIX");
    std::string prefix = configured ? configured
                                    : testing::TempDir() + "mistral-ready-trace-" +
                                              std::to_string(reinterpret_cast<uintptr_t>(&ctx));
    ctx.dump_ready_timing(prefix.c_str());
    auto read = [&](const char *suffix) {
        std::ifstream f(prefix + suffix);
        std::ostringstream text;
        text << f.rdbuf();
        return text.str();
    };
    EXPECT_NE(read(".arcs.tsv").find("empty_input_wave"), std::string::npos);
    EXPECT_NE(read(".hops.tsv").find("observed_pip"), std::string::npos);
    EXPECT_NE(read(".state.json").find("\"failed_recomputations\":1"), std::string::npos);
    EXPECT_EQ(ctx.pip_delay_observed.at(pip), 0);
    // A failed cached override seeds Context's quad accumulator even though
    // the scalar API uses the complete positive fallback sum.
    ctx.pip_delay_observed[pip] = 123;
    cal.analogue_ps = 123;
    const auto &actual_user = *net->users.begin();
    auto positive = ctx.getNetinfoRouteDelayQuad(net, actual_user);
    EXPECT_EQ(positive.minDelay(), 0);
    EXPECT_EQ(positive.maxDelay(), 123);
    EXPECT_EQ(ctx.getNetinfoRouteDelay(net, actual_user), 123);
    ctx.dump_ready_timing((prefix + ".positive").c_str());
    EXPECT_EQ(ctx.pip_delay_observed.at(pip), 123);
    if (!configured)
        for (auto suffix : {".arcs.tsv", ".hops.tsv", ".observed.tsv", ".types.tsv", ".state.json"})
            std::remove((prefix + ".positive" + suffix).c_str());
    if (!configured)
        for (auto suffix : {".arcs.tsv", ".hops.tsv", ".observed.tsv", ".types.tsv", ".state.json"})
            std::remove((prefix + suffix).c_str());
}
TEST(ReadyTimingTest, ProvenanceFollowsActualDelayBranches)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    PipId p(CycloneV::rnode_coords(CycloneV::H6, 46, 64, 32), CycloneV::rnode_coords(CycloneV::WM, 46, 64, 0));
    EXPECT_STREQ(ctx.pip_delay_provenance(p), "uncalibrated_table");
    ctx.pip_delay_observed[p] = 0;
    ctx.pip_delay_calibrated = true;
    EXPECT_STREQ(ctx.pip_delay_provenance(p), "observed_pip");
    EXPECT_EQ(ctx.getPipDelay(p).maxDelay(), 0);
    ctx.pip_delay_observed.clear();
    EXPECT_STREQ(ctx.pip_delay_provenance(p), "uncalibrated_table");
    auto &cal = ctx.pip_type_calibration[CycloneV::H6];
    cal.table_ps = 100;
    cal.analogue_ps = 200;
    cal.hops = 1;
    ctx.pip_delay_prior = 1.25f;
    EXPECT_STREQ(ctx.pip_delay_provenance(p), "per_type_scaled");
    EXPECT_EQ(ctx.getPipDelay(p).maxDelay(), delay_t(ctx.getPipDelayTable(p).maxDelay() * 2.5f));
    PipId zero(CycloneV::rnode_coords(CycloneV::WM, 46, 64, 0), p.src);
    auto &z = ctx.pip_type_calibration[CycloneV::WM];
    z.table_ps = 100;
    z.analogue_ps = 160;
    z.hops = 2;
    EXPECT_STREQ(ctx.pip_delay_provenance(zero), "per_type_placeholder");
    EXPECT_EQ(ctx.getPipDelay(zero).maxDelay(), 100);
}
TEST(ReadyTimingTest, DisabledIsIdentifierNeutral)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    auto before = ctx.id("ready-trace-before");
    ctx.dump_ready_timing(nullptr);
    ctx.dump_ready_timing("");
    auto after = ctx.id("ready-trace-after");
    EXPECT_EQ(after.index, before.index + 1);
    EXPECT_TRUE(ctx.pip_delay_observed.empty());
    EXPECT_TRUE(ctx.analogue_arc_cache.empty());
}
