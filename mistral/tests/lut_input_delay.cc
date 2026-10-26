#include <memory>
#include "gtest/gtest.h"
#include "nextpnr.h"
#include "log.h"

USING_NEXTPNR_NAMESPACE

class LutInputDelayTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *net;
    CellInfo *lut, *control;
    WireId source{CycloneV::rnode_coords(CycloneV::LD, 22, 8, 19)};
    WireId input{CycloneV::rnode_coords(CycloneV::GOUT, 22, 8, 3)};
    PipId route, terminal;

    void SetUp() override
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        net = ctx->createNet(ctx->id("data"));
        auto src = ctx->id("source");
        ctx->createRegionPlug(src, ctx->id("test_source"), Loc());
        ctx->addPlugPin(src, id_Q, PORT_OUT, source);
        ctx->cells.at(src)->connectPort(id_Q, net);

        lut = ctx->createCell(ctx->id("lut"), id_MISTRAL_ALUT2);
        lut->addInput(id_A);
        lut->addOutput(id_Q);
        lut->connectPort(id_A, net);
        lut->pin_data[id_A].bel_pins = {id_B};
        lut->pin_data[id_Q].bel_pins = {id_COMBOUT};
        ctx->bindBel(ctx->getBelByLocation(Loc(22, 8, 0)), lut, STRENGTH_STRONG);
        ASSERT_EQ(ctx->getBelPinWire(lut->bel, id_B), input);

        // Exercise the existing synthetic FF/control sink boundary through
        // the same physical GOUT circuit. Its stage must remain counted once.
        auto endpoint = ctx->add_wire(22, 8, ctx->id("control_sink"));
        auto ctl = ctx->id("control");
        ctx->createRegionPlug(ctl, id_MISTRAL_FF, Loc());
        ctx->addPlugPin(ctl, id_ENA, PORT_IN, endpoint);
        control = ctx->cells.at(ctl).get();
        control->connectPort(id_ENA, net);
        ctx->bindWire(source, net, STRENGTH_WEAK);
        route = PipId(source.node, input.node);
        ctx->bindPip(route, net, STRENGTH_WEAK);
        terminal = ctx->add_pip(input, endpoint);
        ctx->bindPip(terminal, net, STRENGTH_WEAK);
        ctx->cyclonev->rnode_link(ctx->cyclonev->rc2ri(source.node), ctx->cyclonev->rc2ri(input.node));
    }

    const PortRef &user(CellInfo *cell, IdString pin) { return net->users.at(cell->ports.at(pin).user_idx); }

    void same(const DelayQuad &a, const DelayQuad &b)
    {
        EXPECT_NEAR(a.minRiseDelay(), b.minRiseDelay(), 1);
        EXPECT_NEAR(a.maxRiseDelay(), b.maxRiseDelay(), 1);
        EXPECT_NEAR(a.minFallDelay(), b.minFallDelay(), 1);
        EXPECT_NEAR(a.maxFallDelay(), b.maxFallDelay(), 1);
    }
};

TEST_F(LutInputDelayTest, PlacementChargesMuxWithoutInventingEarlyMargin)
{
    DelayQuad cell, logic;
    ASSERT_TRUE(ctx->getCellDelay(lut, id_A, id_Q, cell));
    ASSERT_TRUE(ctx->get_lut_pin_delay(id_B, false, logic));
    EXPECT_EQ(cell.maxDelay() - logic.maxDelay(), 175);
    EXPECT_EQ(cell.minDelay(), logic.minDelay());
    EXPECT_EQ(ctx->get_lut_input_delay(control, id_ENA).maxDelay(), 0);
}

TEST_F(LutInputDelayTest, AnalogueIncludesTerminalStageAndPreservesControlArc)
{
    // This native LD -> GOUT path is retained in the #236 Quartus replica.
    // A bare route stops around 110 ps; the block-input mux adds >100 ps.
    ctx->bitstream_configured = true;
    DelayQuad data, stage, ctl, no_stage;
    std::vector<Arch::AnalogueHop> hops;
    ASSERT_TRUE(ctx->analogue_arc_delay(net, user(lut, id_A), data, &hops, &stage));
    ASSERT_TRUE(ctx->analogue_arc_delay(net, user(control, id_ENA), ctl, nullptr, &no_stage));
    EXPECT_GT(stage.minDelay(), 0);
    EXPECT_GT(stage.maxDelay(), 100);
    EXPECT_GT(data.maxDelay(), 200);
    same(data, ctl);
    same(no_stage, DelayQuad(0));
    ASSERT_EQ(hops.size(), 1);
    EXPECT_EQ(hops.front().pip, route);

    ctx->compute_analogue_arcs(true);
    DelayQuad cell, logic;
    ASSERT_TRUE(ctx->getCellDelay(lut, id_A, id_Q, cell));
    ASSERT_TRUE(ctx->get_lut_pin_delay(id_B, false, logic));
    auto routed = ctx->getNetinfoRouteDelayQuad(net, user(lut, id_A));
    same(routed + cell, data + logic);
    same(ctx->getNetinfoRouteDelayQuad(net, user(control, id_ENA)), ctl);
    same(ctx->get_lut_input_delay(lut, id_A), stage);
    EXPECT_EQ(ctx->pip_delay_observed.size(), 2);
    EXPECT_FALSE(ctx->pip_delay_observed.count(PipId()));

    // Planning cells can carry the incumbent's BEL and port indices, but
    // do not own its route observation.
    CellInfo planned(ctx.get(), ctx->id("planned"), id_MISTRAL_ALUT2);
    planned.bel = lut->bel;
    planned.ports = lut->ports;
    same(ctx->get_lut_input_delay(&planned, id_A), DelayQuad(0, 175));

    // Rip-up invalidation drops the measured cell-input term with its route.
    ctx->analogue_arc_cache.erase(&user(lut, id_A));
    same(ctx->get_lut_input_delay(lut, id_A), DelayQuad(0, 175));
}

TEST_F(LutInputDelayTest, FfSynchronousClearReferenceCountsTerminalBufferOnTheNet)
{
    // A physical GOUT sink stops before its buffer. ENA's existing virtual
    // suffix already includes it; the FF SCLR reference must do so explicitly.
    auto name=ctx->id("synchronous_clear");
    ctx->createRegionPlug(name,id_MISTRAL_FF,Loc());
    ctx->addPlugPin(name,id_SCLR,PORT_IN,input);
    auto *sink=ctx->cells.at(name).get(); sink->connectPort(id_SCLR,net);
    ctx->bitstream_configured=true;
    DelayQuad baseline,reference,existing,no_cell_stage;
    ASSERT_TRUE(ctx->analogue_arc_delay(net,user(control,id_ENA),existing,nullptr,nullptr));
    ASSERT_TRUE(ctx->analogue_arc_delay(net,user(sink,id_SCLR),baseline,nullptr,nullptr));
    EXPECT_GT(existing.maxDelay()-baseline.maxDelay(),100);
    ctx->settings[ctx->id("mistral/ff_core_reference")]=Property::State::S1;
    ASSERT_TRUE(ctx->analogue_arc_delay(net,user(sink,id_SCLR),reference,nullptr,&no_cell_stage));
    same(reference,existing);
    same(no_cell_stage,DelayQuad(0));
    // The FF setup/hold envelope starts after this buffer, so unlike LUT
    // timing it must remain in the routed net delay rather than be subtracted.
    DelayQuad route;
    ASSERT_TRUE(ctx->getArcDelayOverride(net,user(sink,id_SCLR),route));
    same(route,reference);
}

TEST_F(LutInputDelayTest, FfSynchronousClearReferenceRejectsMissingSourceWave)
{
    ctx->cells.at(ctx->id("source"))->disconnectPort(id_Q);
    auto source_name=ctx->id("unseeded_source");
    ctx->createRegionPlug(source_name,ctx->id("test_source"),Loc());
    ctx->addPlugPin(source_name,id_Q,PORT_OUT,input);
    ctx->cells.at(source_name)->connectPort(id_Q,net);
    auto sink_name=ctx->id("synchronous_clear");
    ctx->createRegionPlug(sink_name,id_MISTRAL_FF,Loc());
    ctx->addPlugPin(sink_name,id_SCLR,PORT_IN,input);
    auto *sink=ctx->cells.at(sink_name).get(); sink->connectPort(id_SCLR,net);
    ctx->bitstream_configured=true;
    DelayQuad delay;
    // The ordinary zero-length route keeps its previous behavior. GOUT is
    // not a characterized launch source for the opt-in terminal observation.
    ASSERT_TRUE(ctx->analogue_arc_delay(net,user(sink,id_SCLR),delay,nullptr,nullptr));
    EXPECT_EQ(delay.maxDelay(),0);
    ctx->settings[ctx->id("mistral/ff_core_reference")]=Property::State::S1;
    EXPECT_THROW(ctx->analogue_arc_delay(net,user(sink,id_SCLR),delay,nullptr,nullptr),log_execution_error_exception);
}

TEST_F(LutInputDelayTest, FfSynchronousClearReferenceRejectsAPathThatMissesItsDriver)
{
    ctx->cells.at(ctx->id("source"))->disconnectPort(id_Q);
    auto source_name=ctx->id("other_source");
    ctx->createRegionPlug(source_name,ctx->id("test_source"),Loc());
    ctx->addPlugPin(source_name,id_Q,PORT_OUT,WireId{CycloneV::rnode_coords(CycloneV::LD,22,8,18)});
    ctx->cells.at(source_name)->connectPort(id_Q,net);
    auto sink_name=ctx->id("synchronous_clear");
    ctx->createRegionPlug(sink_name,id_MISTRAL_FF,Loc());
    ctx->addPlugPin(sink_name,id_SCLR,PORT_IN,input);
    auto *sink=ctx->cells.at(sink_name).get(); sink->connectPort(id_SCLR,net);
    ctx->bitstream_configured=true;
    DelayQuad delay;
    // The old routed prefix is simulatable, but it is not this net's driver.
    ASSERT_TRUE(ctx->analogue_arc_delay(net,user(sink,id_SCLR),delay,nullptr,nullptr));
    ctx->settings[ctx->id("mistral/ff_core_reference")]=Property::State::S1;
    EXPECT_THROW(ctx->analogue_arc_delay(net,user(sink,id_SCLR),delay,nullptr,nullptr),log_execution_error_exception);
}
