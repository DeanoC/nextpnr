#include <limits>
#include <memory>
#include "gpurouter.h"
#include "gtest/gtest.h"
#include "nextpnr.h"
#include "ready_fallback_policy.h"
USING_NEXTPNR_NAMESPACE
TEST(ReadyFallbackTest, GuardAllowsOnlySetupGainWithoutNewHoldViolations)
{
    using namespace ready_fallback_policy;
    EXPECT_TRUE(endpoint_ok({100, 50}, {120, 0}));
    EXPECT_FALSE(endpoint_ok({100, 50}, {99, 50}));
    EXPECT_FALSE(endpoint_ok({100, 50}, {120, -1}));
    EXPECT_FALSE(endpoint_ok({100, -5}, {120, -6}));
    EXPECT_TRUE(endpoint_ok({100, -5}, {120, -5}));
    EXPECT_FALSE(endpoint_ok({100, 0}, {std::numeric_limits<int>::max(), 0}));
    EXPECT_FALSE(endpoint_ok({100, 0}, {120, std::numeric_limits<int>::lowest()}));
    EXPECT_TRUE(clock_ok(100, 100));
    EXPECT_FALSE(clock_ok(100, 99));
    EXPECT_FALSE(clock_ok(100, std::numeric_limits<float>::quiet_NaN()));
    EXPECT_TRUE(gain_ok(100, 120));
    EXPECT_FALSE(gain_ok(100, 119));
}
TEST(ReadyFallbackTest, DisabledIsIdentifierNeutral)
{
    ArchArgs a;
    a.device = "5CSEBA6U23I7";
    Context ctx(a);
    auto x = ctx.id("before-disabled");
    ctx.ready_fallback_pass(nullptr);
    ctx.ready_fallback_pass("");
    EXPECT_EQ(ctx.id("after-disabled").index, x.index + 1);
}
TEST(ReadyFallbackTest, FailedHpsFallbackReevaluatesWithoutSuccessOrLearning)
{
    ArchArgs a;
    a.device = "5CSEBA6U23I7";
    Context ctx(a);
    auto net = ctx.createNet(ctx.id("fallback-fixture"));
    WireId src(CycloneV::rnode_coords(CycloneV::GIN, 51, 64, 18)),
            mid(CycloneV::rnode_coords(CycloneV::H6, 46, 64, 32));
    PipId next;
    for (auto p : ctx.getPipsDownhill(mid))
        if (!ctx.getPipDstWire(p).is_nextpnr_created()) {
            next = p;
            break;
        }
    ASSERT_NE(next, PipId());
    auto dst = ctx.getPipDstWire(next);
    auto plug = [&](const char *n, PortType d, WireId w) {
        auto id = ctx.id(n), pin = ctx.id("pin");
        ctx.createRegionPlug(id, ctx.id("test_plug"), Loc());
        ctx.addPlugPin(id, pin, d, w);
        auto c = ctx.cells.at(id).get();
        c->connectPort(pin, net);
        return c;
    };
    plug("source", PORT_OUT, src);
    auto sink = plug("sink", PORT_IN, mid);
    ctx.bindWire(src, net, STRENGTH_WEAK);
    ctx.bindPip(PipId(src.node, mid.node), net, STRENGTH_WEAK);
    ctx.bitstream_configured = true;
    ctx.compute_analogue_arcs(false);
    const auto &user = *net->users.begin();
    ASSERT_FALSE(ctx.analogue_arc_cache.at(&user).ok);
    auto before = ctx.getNetinfoRouteDelay(net, user);
    EXPECT_EQ(before, 100);
    // Extend the real physical path and move only the fixture's logical tap.
    // This checks the evaluator contract, not a legal-placement transformation.
    ctx.bindPip(next, net, STRENGTH_WEAK);
    ctx.addPlugPin(sink->name, ctx.id("new_pin"), PORT_IN, dst);
    sink->disconnectPort(ctx.id("pin"));
    sink->connectPort(ctx.id("new_pin"), net);
    ctx.compute_analogue_arcs(false);
    const auto &after_user = *net->users.begin();
    ASSERT_FALSE(ctx.analogue_arc_cache.at(&after_user).ok);
    EXPECT_EQ(ctx.getNetinfoRouteDelay(net, after_user), before + ctx.getPipDelay(next).maxDelay());
    EXPECT_TRUE(ctx.pip_delay_observed.empty());
    for (auto &c : ctx.pip_type_calibration)
        EXPECT_EQ(c.hops, 0);
}

TEST(ReadyFallbackTest, CpuCandidatesReserveUnrelatedRoutingAndLeaveArchUnchanged)
{
    ArchArgs a;
    a.device = "5CSEBA6U23I7";
    Context ctx(a);
    auto net = ctx.createNet(ctx.id("candidate-fixture"));
    auto unrelated = ctx.createNet(ctx.id("reserved-fixture"));
    auto src = ctx.add_wire(1, 1, ctx.id("candidate_source"));
    auto old = ctx.add_wire(2, 1, ctx.id("candidate_original"));
    auto blocked = ctx.add_wire(2, 2, ctx.id("candidate_blocked"));
    auto alternative = ctx.add_wire(2, 3, ctx.id("candidate_alternative"));
    auto sink = ctx.add_wire(3, 1, ctx.id("candidate_sink"));
    std::vector<PipId> original;
    for (auto middle : {old, blocked, alternative}) {
        auto p = ctx.add_pip(src, middle), q = ctx.add_pip(middle, sink);
        if (middle == old)
            original = {p, q};
    }
    auto plug = [&](const char *n, PortType d, WireId w, NetInfo *ni) {
        auto name = ctx.id(n), pin = ctx.id("pin");
        ctx.createRegionPlug(name, ctx.id("test_plug"), Loc());
        ctx.addPlugPin(name, pin, d, w);
        ctx.cells.at(name)->connectPort(pin, ni);
    };
    plug("source", PORT_OUT, src, net);
    plug("sink", PORT_IN, sink, net);
    plug("reserved_source", PORT_OUT, blocked, unrelated);
    ctx.bindWire(src, net, STRENGTH_WEAK);
    for (auto p : original)
        ctx.bindPip(p, net, STRENGTH_WEAK);
    ctx.bindWire(blocked, unrelated, STRENGTH_WEAK);
    ctx.settings[ctx.id("target_freq")] = std::string("130000000");
    ctx.settings[ctx.id("timing_driven")] = std::string("0");
    GpuRouterCfg cfg(&ctx);
    cfg.cpu_backend = true;
    GpuCandidateRouter router(&ctx, cfg);
    auto user = net->users.enumerate().begin();
    auto candidates = router.candidates({{net, (*user).index}}, 4);
    ASSERT_EQ(candidates.size(), 1);
    ASSERT_FALSE(candidates[0].empty());
    bool found_alternative = false;
    for (auto &c : candidates[0])
        for (auto &w : c.wires) {
            EXPECT_NE(w.first, blocked);
            found_alternative |= w.first == alternative;
        }
    EXPECT_TRUE(found_alternative);
    EXPECT_EQ(ctx.getBoundWireNet(blocked), unrelated);
    EXPECT_EQ(net->wires.size(), 3);
    EXPECT_EQ(net->wires.at(old).pip, original[0]);
    EXPECT_EQ(net->wires.at(sink).pip, original[1]);
    // Router setup may record defaults and scratch net indexes, but candidate
    // generation itself must leave all architecture wire bindings untouched.
    EXPECT_EQ(ctx.getBoundWireNet(alternative), nullptr);
}
TEST(ReadyFallbackTest, RouteThroughInsertionLeavesPlacementCacheFromEarlierPhase)
{
    ArchArgs a;
    a.device = "5CSEBA6U23I7";
    Context ctx(a);
    ctx.createNet(ctx.id("$PACKER_GND_NET"));
    ctx.createNet(ctx.id("$PACKER_VCC_NET"));
    auto clock = ctx.createNet(ctx.id("clock"));
    auto datain = ctx.createNet(ctx.id("fabric_data"));
    auto ff = ctx.createCell(ctx.id("phase_ff"), id_MISTRAL_FF);
    for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN})
        ff->addInput(pin);
    ff->addOutput(id_Q);
    ff->connectPort(id_CLK, clock);
    ff->connectPort(id_DATAIN, datain);
    ff->pin_data[id_ENA].state = PIN_1;
    ff->pin_data[id_ACLR].state = PIN_1;
    ff->pin_data[id_SCLR].state = PIN_0;
    ff->pin_data[id_SLOAD].state = PIN_0;
    auto lut = ctx.createCell(ctx.id("opposite_lut"), id_MISTRAL_ALUT4);
    lut->params[id_LUT] = 0x1234;
    for (auto pin : {id_A, id_B, id_C, id_D}) {
        lut->addInput(pin);
        lut->connectPort(pin, ctx.createNet(ctx.idf("input_%s", pin.c_str(&ctx))));
    }
    lut->addOutput(id_Q);
    lut->connectPort(id_Q, ctx.createNet(ctx.id("lut_output")));
    ctx.assignArchInfo();
    BelId ff_bel, lut_bel;
    for (auto bel : ctx.getBelsByTile(30, 20)) {
        auto z = ctx.getBelLocation(bel).z;
        if (z == 2 && ctx.getBelType(bel) == id_MISTRAL_FF)
            ff_bel = bel;
        if (z == 1 && ctx.isValidBelForCellType(lut->type, bel))
            lut_bel = bel;
    }
    ASSERT_NE(ff_bel, BelId());
    ASSERT_NE(lut_bel, BelId());
    ctx.bindBel(ff_bel, ff, STRENGTH_WEAK);
    ctx.bindBel(lut_bel, lut, STRENGTH_WEAK);
    ASSERT_TRUE(ctx.isBelLocationValid(ff_bel));
    ASSERT_EQ(ff->ffInfo.datain, datain);
    auto data = ctx.bel_data(ff_bel).lab_data;
    ctx.reassign_alm_inputs(data.lab, data.alm);
    auto inserted = ctx.cells.at(ctx.id("phase_ff$ROUTETHRU")).get();
    ASSERT_EQ(ff->getPort(id_DATAIN), inserted->getPort(id_Q));
    ASSERT_EQ(inserted->getPort(id_A), datain);
    EXPECT_EQ(ff->ffInfo.datain, datain);
    EXPECT_NE(ff->ffInfo.datain, ff->getPort(id_DATAIN));
    EXPECT_FALSE(ctx.isBelLocationValid(ff_bel));
    EXPECT_FALSE(ctx.is_alm_legal(data.lab, data.alm));
    // Test-only refresh isolates the cause. The final route diagnostic must
    // preserve the live cached metadata and compare its baseline census.
    ctx.assign_ff_info(ff);
    ctx.update_bel(ff_bel);
    EXPECT_TRUE(ctx.isBelLocationValid(ff_bel));
}
