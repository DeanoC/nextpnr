#include <memory>
#include <vector>
#include "gtest/gtest.h"
#include "nextpnr.h"
USING_NEXTPNR_NAMESPACE
class CompletedObservationsTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *net;
    WireId gin{CycloneV::rnode_coords(CycloneV::GIN, 51, 64, 18)}, h6{CycloneV::rnode_coords(CycloneV::H6, 46, 64, 32)};
    void SetUp() override
    {
        ArchArgs a;
        a.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(a);
        net = ctx->createNet(ctx->id("observation-fixture"));
        ctx->bitstream_configured = true;
    }
    CellInfo *port(const char *name, PortType dir, WireId w)
    {
        auto id = ctx->id(name), pin = ctx->id("pin");
        ctx->createRegionPlug(id, ctx->id("test_plug"), Loc());
        ctx->addPlugPin(id, pin, dir, w);
        auto c = ctx->cells.at(id).get();
        c->connectPort(pin, net);
        return c;
    }
    WireId physical(CycloneV::rnode_type_t type)
    {
        for (auto w : ctx->getWires())
            if (!w.is_nextpnr_created() && w.node.t() == type)
                return w;
        return WireId();
    }
    void path(const std::vector<WireId> &w)
    {
        ctx->bindWire(w.front(), net, STRENGTH_WEAK);
        for (size_t i = 1; i < w.size(); ++i) {
            auto p = ctx->add_pip(w[i - 1], w[i]);
            ctx->bindPip(p, net, STRENGTH_WEAK);
        }
    }
};
TEST_F(CompletedObservationsTest, RealFailedHpsHopNeverBecomesObservation)
{
    port("source", PORT_OUT, gin);
    port("sink", PORT_IN, h6);
    path({gin, h6});
    auto failed = PipId(gin.node, h6.node);
    ctx->compute_analogue_arcs(true);
    ASSERT_TRUE(ctx->analogue_cache_valid);
    ASSERT_EQ(ctx->analogue_arc_cache.size(), 1);
    EXPECT_FALSE(ctx->analogue_arc_cache.begin()->second.ok);
    EXPECT_TRUE(ctx->pip_delay_observed.empty());
    for (auto &c : ctx->pip_type_calibration) {
        EXPECT_EQ(c.hops, 0);
        EXPECT_EQ(c.table_ps, 0);
        EXPECT_EQ(c.analogue_ps, 0);
    }
    EXPECT_EQ(ctx->getPipDelay(failed).maxDelay(), 100);
    ctx->compute_analogue_arcs(true);
    EXPECT_TRUE(ctx->pip_delay_observed.empty());
    EXPECT_EQ(ctx->pip_type_calibration[CycloneV::GIN].hops, 0);
}
TEST_F(CompletedObservationsTest, FailedCachedOverridePreservesFallbackRouteMinimum)
{
    port("source", PORT_OUT, gin);
    auto sink = port("sink", PORT_IN, h6);
    path({gin, h6});
    const PortRef endpoint{sink, ctx->id("pin")};
    // This HPS source has no analogue input wave. Before caching, the
    // declined override leaves the route-table envelope intact.
    auto uncached = ctx->getNetinfoRouteDelayQuad(net, endpoint);
    ASSERT_GT(uncached.minDelay(), 0);
    ctx->compute_analogue_arcs(true);
    const auto &cached_endpoint = *net->users.begin();
    ASSERT_FALSE(ctx->analogue_arc_cache.at(&cached_endpoint).ok);
    // The cached failed arc writes its default zero delay before declining.
    // That value must be discarded, just as in the uncached calculation.
    auto cached = ctx->getNetinfoRouteDelayQuad(net, cached_endpoint);
    EXPECT_EQ(cached.rise.minDelay(), uncached.rise.minDelay());
    EXPECT_EQ(cached.rise.maxDelay(), uncached.rise.maxDelay());
    EXPECT_EQ(cached.fall.minDelay(), uncached.fall.minDelay());
    EXPECT_EQ(cached.fall.maxDelay(), uncached.fall.maxDelay());
    EXPECT_EQ(cached.maxDelay(), ctx->getNetinfoRouteDelay(net, cached_endpoint));
}
TEST_F(CompletedObservationsTest, CompletedZeroPrefixesSurviveLaterFailureAndRepeatedAggregation)
{
    auto generated = ctx->add_wire(1, 1, ctx->id("generated_prefix"));
    auto wm = physical(CycloneV::WM), gclk = physical(CycloneV::GCLK);
    ASSERT_NE(wm, WireId());
    ASSERT_NE(gclk, WireId());
    port("source", PORT_OUT, generated);
    auto short_sink = port("short_sink", PORT_IN, gclk);
    auto failed_sink = port("failed_sink", PORT_IN, h6);
    // Synthetic prefix connections exercise genuine generated/NO_DELAY/P2P modes;
    // the final physical GIN->H6 edge is the real failed HPS-ready hop.
    path({generated, wm, gclk, gin, h6});
    std::vector<PipId> valid{PipId(generated.node, wm.node), PipId(wm.node, gclk.node), PipId(gclk.node, gin.node)};
    auto failed = PipId(gin.node, h6.node);
    std::vector<Arch::AnalogueHop> hops;
    DelayQuad d;
    EXPECT_FALSE(ctx->analogue_arc_delay(net, {failed_sink, ctx->id("pin")}, d, &hops));
    ASSERT_EQ(hops.size(), 3);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(hops[i].pip, valid[i]);
        EXPECT_EQ(hops[i].rise, 0);
        EXPECT_EQ(hops[i].fall, 0);
    }
    ctx->compute_analogue_arcs(true);
    ASSERT_EQ(ctx->pip_delay_observed.size(), 3);
    for (auto p : valid)
        EXPECT_EQ(ctx->pip_delay_observed.at(p), 0);
    EXPECT_FALSE(ctx->pip_delay_observed.count(failed));
    EXPECT_EQ(ctx->pip_type_calibration[CycloneV::GIN].hops, 0);
    EXPECT_EQ(ctx->pip_type_calibration[CycloneV::WM].hops, 1);
    EXPECT_EQ(ctx->pip_type_calibration[CycloneV::GCLK].hops, 1);
    // Existing larger valid observations are retained by the maximum, rather
    // than overwritten by a repeated zero sample or double-counted per user.
    ctx->pip_delay_observed[valid[1]] = 17;
    ctx->pip_delay_observed[valid[2]] = 31;
    ctx->compute_analogue_arcs(true);
    EXPECT_EQ(ctx->pip_delay_observed.size(), 3);
    EXPECT_EQ(ctx->pip_delay_observed.at(valid[0]), 0);
    EXPECT_EQ(ctx->pip_delay_observed.at(valid[1]), 17);
    EXPECT_EQ(ctx->pip_delay_observed.at(valid[2]), 31);
    EXPECT_FALSE(ctx->pip_delay_observed.count(failed));
    EXPECT_EQ(ctx->pip_type_calibration[CycloneV::WM].hops, 1);
    EXPECT_EQ(ctx->pip_type_calibration[CycloneV::WM].analogue_ps, 17);
    EXPECT_EQ(ctx->pip_type_calibration[CycloneV::GCLK].hops, 1);
    EXPECT_EQ(ctx->pip_type_calibration[CycloneV::GCLK].analogue_ps, 31);
    std::vector<Arch::AnalogueHop> short_hops;
    EXPECT_TRUE(ctx->analogue_arc_delay(net, {short_sink, ctx->id("pin")}, d, &short_hops));
    EXPECT_EQ(short_hops.size(), 2);
    EXPECT_EQ(d.maxDelay(), 0);
}
TEST_F(CompletedObservationsTest, PhysicalLabCircuitHopKeepsPositiveCompletedObservation)
{
    // A real LAB output and one database-defined downhill edge, with its
    // output mux and routing mux configured, exercise both circuit edges.
    auto src = ctx->get_port(CycloneV::LAB, 1, 1, 0, CycloneV::FFT0);
    ASSERT_NE(src, WireId());
    ASSERT_FALSE(src.is_nextpnr_created());
    ASSERT_TRUE(ctx->cyclonev->bmux_m_set(CycloneV::LAB, CycloneV::xycoords(1, 1), CycloneV::TDFF0, 0, CycloneV::REG));
    PipId pip;
    for (auto p : ctx->getPipsDownhill(src)) {
        if (!ctx->getPipDstWire(p).is_nextpnr_created()) {
            pip = p;
            break;
        }
    }
    ASSERT_NE(pip, PipId());
    auto dst = ctx->getPipDstWire(pip);
    ctx->cyclonev->rnode_link(ctx->cyclonev->rc2ri(src.node), ctx->cyclonev->rc2ri(dst.node));
    port("source", PORT_OUT, src);
    auto sink = port("sink", PORT_IN, dst);
    path({src, dst});
    std::vector<Arch::AnalogueHop> hops;
    DelayQuad d;
    ASSERT_TRUE(ctx->analogue_arc_delay(net, {sink, ctx->id("pin")}, d, &hops));
    ASSERT_EQ(hops.size(), 1);
    EXPECT_EQ(hops[0].rise, 63);
    EXPECT_EQ(hops[0].fall, 83);
    EXPECT_EQ(d.rise.minDelay(), 60);
    EXPECT_EQ(d.rise.maxDelay(), 63);
    EXPECT_EQ(d.fall.minDelay(), 79);
    EXPECT_EQ(d.fall.maxDelay(), 83);
    std::cout << "LAB circuit " << ctx->nameOfWire(src) << " -> " << ctx->nameOfWire(dst) << " rise=" << hops[0].rise
              << " fall=" << hops[0].fall << " quad=" << d.rise.minDelay() << "," << d.rise.maxDelay() << ","
              << d.fall.minDelay() << "," << d.fall.maxDelay() << std::endl;
    DelayQuad untraced;
    std::vector<Arch::AnalogueHop> other;
    ASSERT_TRUE(ctx->analogue_arc_delay(net, {sink, ctx->id("pin")}, untraced, &other));
    ASSERT_EQ(other.size(), 1);
    EXPECT_EQ(other[0].rise, hops[0].rise);
    EXPECT_EQ(other[0].fall, hops[0].fall);
    EXPECT_EQ(untraced.maxDelay(), d.maxDelay());
    ctx->compute_analogue_arcs(true);
    ASSERT_EQ(ctx->pip_delay_observed.size(), 1);
    EXPECT_EQ(ctx->pip_delay_observed.at(pip), std::max(hops[0].rise, hops[0].fall));
    EXPECT_EQ(ctx->pip_type_calibration[src.node.t()].hops, 1);
}
