#include <algorithm>
#include <memory>
#include "gtest/gtest.h"
#include "nextpnr.h"
#include "log.h"
#include "ff_core_reference_data.h"

USING_NEXTPNR_NAMESPACE

class FfCoreTimingTest : public ::testing::Test
{
  protected:
    using CV = mistral::CycloneV;
    std::unique_ptr<Context> ctx;
    CellInfo *ff;
    NetInfo *clock, *data;
    BelId bel;
    uint32_t lab_index;
    int slot = 0;
    NetInfo *driven(const char *name)
    {
        auto *net = ctx->createNet(ctx->id(name));
        auto *driver = ctx->createCell(ctx->idf("%s$driver",name),id_MISTRAL_BUF);
        driver->addInput(id_A); driver->addOutput(id_Q); driver->connectPort(id_Q,net);
        driver->pin_data[id_Q].bel_pins = {id_COMBOUT};
        ctx->assign_comb_info(driver);
        return net;
    }
    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->createNet(ctx->id("$PACKER_GND_NET")); ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        ff = ctx->createCell(ctx->id("ff"),id_MISTRAL_FF);
        for (auto p : {id_CLK,id_DATAIN,id_ENA,id_SCLR,id_ACLR,id_SLOAD,id_SDATA}) {
            ff->addInput(p); ff->pin_data[p].bel_pins = {p};
        }
        ff->addOutput(id_Q); ff->pin_data[id_Q].bel_pins = {id_Q};
        clock=driven("clock"); data=driven("data");
        ff->connectPort(id_CLK,clock); ff->connectPort(id_DATAIN,data);
        ff->pin_data[id_CLK].state=PIN_SIG; ff->pin_data[id_DATAIN].state=PIN_SIG;
        ff->pin_data[id_ENA].state=PIN_1; ff->pin_data[id_ACLR].state=PIN_1;
        ff->pin_data[id_SCLR].state=PIN_0; ff->pin_data[id_SLOAD].state=PIN_0;
        clock->clkconstr=std::make_unique<ClockConstraint>();
        clock->clkconstr->period=DelayPair{10000};
        clock->clkconstr->high=clock->clkconstr->low=DelayPair{5000};
    }
    void enable() { ctx->settings[ctx->id("mistral/ff_core_reference")]=Property::State::S1; }
    TimingClockingInfo info(IdString p=id_Q) { return ctx->getPortClockingInfo(ff,p,0); }
    void reject() { EXPECT_THROW(info(),log_execution_error_exception); }
    void bit(CV::bmux_type_t field,bool value,int index=0)
    {
        ASSERT_TRUE(ctx->cyclonev->bmux_b_set(ctx->labs[lab_index].is_mlab?CV::MLAB:CV::LAB,bel.pos,field,index,value));
    }
    void mux(CV::bmux_type_t field,CV::bmux_type_t value,int index=0)
    {
        ASSERT_TRUE(ctx->cyclonev->bmux_m_set(ctx->labs[lab_index].is_mlab?CV::MLAB:CV::LAB,bel.pos,field,index,value));
    }
    void pip(WireId from,WireId to,NetInfo *net) { ctx->bindPip(PipId(from.node,to.node),net,STRENGTH_USER); }
    void physical(int ff_slot=0,bool mlab=false)
    {
        slot=ff_slot;
        auto found=std::find_if(ctx->labs.begin(),ctx->labs.end(),[&](const LABInfo &v){return v.is_mlab==mlab;});
        ASSERT_NE(found,ctx->labs.end()); lab_index=std::distance(ctx->labs.begin(),found);
        auto &lab=*found; auto &alm=lab.alms[0]; bel=alm.ff_bels[slot];
        ctx->lab_ff4=true;
        ctx->assign_ff_info(ff); ctx->bindBel(bel,ff,STRENGTH_USER);
        ctx->bindBel(alm.lut_bels[slot/2],data->driver.cell,STRENGTH_USER);
        ctx->bindBel(lab.alms[9].lut_bels[0],clock->driver.cell,STRENGTH_USER);
        ctx->bindWire(alm.comb_out[slot/2],data,STRENGTH_USER);
        pip(alm.comb_out[slot/2],alm.ff_in[slot],data);
        const auto block=mlab?CV::MLAB:CV::LAB;
        const auto root=ctx->get_port(block,bel.pos.x(),bel.pos.y(),-1,CV::CLKIN,0);
        ctx->bindWire(root,clock,STRENGTH_USER);
        pip(root,lab.clk_wires[0],clock);
        pip(lab.clk_wires[0],alm.sel_clk[(slot==0||slot==3)?0:1],clock);
        mux((slot==0||slot==3)?CV::TCLK_SEL:CV::BCLK_SEL,CV::CLK1,0);
        mux(CV::CLK1_SEL,CV::CLKA); bit(CV::CLK1_INV,false); bit(CV::EN1_EN,false);
        bit(CV::SCLR_DIS,true); bit(CV::SLOAD_EN,false);
        const CV::bmux_type_t pkreg[]={CV::TPKREG1,CV::TPKREG0,CV::BPKREG1,CV::BPKREG0};
        bit(pkreg[slot],false,0);
        ASSERT_TRUE(ctx->cyclonev->bmux_n_set(block,bel.pos,(slot==0||slot==3)?CV::TCLR_SEL:CV::BCLR_SEL,0,0));
        mux(CV::ACLR0_SEL,CV::ACLR0); bit(CV::ACLR0_INV,false);
        ctx->bitstream_configured=true;
    }
    const ff_core_reference::Bounds &expected(unsigned mode)
    {
        for (const auto &entry:ff_core_reference::modes) if (entry.key==mode) return entry.corners[0];
        ADD_FAILURE()<<"missing test mode "<<mode; return ff_core_reference::modes[0].corners[0];
    }
};

TEST_F(FfCoreTimingTest, DefaultRetainsHistoricalModel)
{
    auto q=info(), d=info(id_DATAIN);
    EXPECT_EQ(q.clockToQ.minDelay(),731); EXPECT_EQ(q.clockToQ.maxDelay(),731);
    EXPECT_EQ(d.setup.maxDelay(),-196); EXPECT_EQ(d.hold.maxDelay(),270);
}
TEST_F(FfCoreTimingTest, UnplacedBoundsDistinguishEarlyAndLate)
{
    enable(); auto q=info(),d=info(id_DATAIN);
    EXPECT_LT(q.clockToQ.minDelay(),q.clockToQ.maxDelay());
    EXPECT_GT(q.clockToQ.minDelay(),0); EXPECT_LT(q.clockToQ.maxDelay(),1000);
    EXPECT_NE(d.setup.maxDelay(),-196); EXPECT_NE(d.hold.maxDelay(),270);
}
TEST_F(FfCoreTimingTest, ClockPolarityIsPreserved)
{
    enable(); auto rising=info(); ff->pin_data[id_CLK].state=PIN_INV;
    auto falling=info(); EXPECT_EQ(falling.edge,FALLING_EDGE); EXPECT_EQ(falling.clock_port,id_CLK);
    EXPECT_NE(rising.clockToQ.maxDelay(),falling.clockToQ.maxDelay());
}
TEST_F(FfCoreTimingTest, MissingAndUnsupportedWaveformsRejected)
{
    enable(); clock->clkconstr.reset(); reject();
    clock->clkconstr=std::make_unique<ClockConstraint>();
    clock->clkconstr->period=DelayPair{8000}; clock->clkconstr->high=clock->clkconstr->low=DelayPair{4000}; reject();
}
TEST_F(FfCoreTimingTest, ActiveOverridesAndUndrivenControlsRejected)
{
    enable(); ff->connectPort(id_SLOAD,driven("load")); ff->pin_data[id_SLOAD].state=PIN_SIG; reject();
    ff->disconnectPort(id_SLOAD); ff->pin_data[id_SLOAD].state=PIN_0;
    ff->connectPort(id_SDATA,driven("sync_data")); reject(); ff->disconnectPort(id_SDATA);
    ff->connectPort(id_ENA,ctx->createNet(ctx->id("undriven"))); ff->pin_data[id_ENA].state=PIN_SIG; reject();
}
TEST_F(FfCoreTimingTest, UnobservedControlCombinationRejected)
{
    enable(); ctx->settings[ctx->id("mistral/ff_reset_reference")]=Property::State::S1;
    ff->connectPort(id_ACLR,driven("clear")); ff->pin_data[id_ACLR].state=PIN_SIG;
    ff->connectPort(id_SCLR,driven("sync_clear")); ff->pin_data[id_SCLR].state=PIN_SIG;
    reject();
}
TEST_F(FfCoreTimingTest, InactiveCalibrationPortsRetainTheirApproximation)
{
    enable(); EXPECT_EQ(info(id_SCLR).setup.maxDelay(),-196);
    EXPECT_EQ(info(id_ACLR).hold.maxDelay(),270);
}
TEST_F(FfCoreTimingTest, PhysicalClockIndexDoesNotFollowVirtualClockAlias)
{
    enable(); physical(); const auto &b=expected(4); auto q=info();
    EXPECT_EQ(q.clockToQ.minRiseDelay(),b.qr_min); EXPECT_EQ(q.clockToQ.maxRiseDelay(),b.qr_max);
    EXPECT_EQ(info(id_DATAIN).setup.maxDelay(),b.setup[0]);
}
TEST_F(FfCoreTimingTest, PhysicalMismatchRejected)
{
    enable(); physical(); bit(CV::CLK1_INV,true); reject(); bit(CV::CLK1_INV,false);
    mux(CV::CLK1_SEL,CV::CLKB); reject(); mux(CV::CLK1_SEL,CV::CLKA);
    bit(CV::EN1_EN,true); reject(); bit(CV::EN1_EN,false);
    bit(CV::TPKREG1,true,0); reject();
}
TEST_F(FfCoreTimingTest, SynchronousClearUsesItsMeasuredPhysicalSelector)
{
    enable(); physical();
    mux(CV::TCLK_SEL,CV::CLK0,0); mux(CV::CLK0_SEL,CV::CLKA);
    bit(CV::CLK0_INV,false); bit(CV::EN0_EN,false);
    auto *clear=driven("sync_clear"); ff->connectPort(id_SCLR,clear); ff->pin_data[id_SCLR].state=PIN_SIG;
    bit(CV::SCLR_DIS,false); bit(CV::TSCLR_DIS,false,0); bit(CV::SCLR_INV,false);
    mux(CV::SCLR_SEL,CV::DIN3);
    auto input=ctx->get_port(CV::LAB,bel.pos.x(),bel.pos.y(),-1,CV::DATAIN,3);
    ctx->bindWire(input,clear,STRENGTH_USER);
    const auto &b=expected(ff_core_reference::SyncClear|ff_core_reference::SyncDin3);
    EXPECT_EQ(info(id_SCLR).setup.maxDelay(),b.setup[4]);
    mux(CV::SCLR_SEL,CV::DIN1); reject();
}
TEST_F(FfCoreTimingTest, MirroredInputSelectionAndBothBlockTypes)
{
    for (int index=0;index<4;index++) {
        if (index) SetUp(); enable(); bool mlab=index>=2; physical(index,mlab);
        auto &alm=ctx->labs[lab_index].alms[0]; auto block=mlab?CV::MLAB:CV::LAB;
        const auto &b=expected(4|(mlab?1:0));
        EXPECT_EQ(info(id_DATAIN).setup.maxDelay(),b.setup[0]);
        ctx->unbindPip(PipId(alm.comb_out[index/2].node,alm.ff_in[index].node));
        auto input=ctx->get_port(block,bel.pos.x(),bel.pos.y(),0,index/2?CV::E1:CV::E0);
        ctx->bindWire(input,data,STRENGTH_USER);
        pip(input,alm.sel_ef[index/2],data); pip(alm.sel_ef[index/2],alm.ff_in[index],data);
        const CV::bmux_type_t pkreg[]={CV::TPKREG1,CV::TPKREG0,CV::BPKREG1,CV::BPKREG0};
        bit(pkreg[index],true,0); mux(index/2?CV::BEF_SEL:CV::TEF_SEL,CV::E,0);
        EXPECT_EQ(info(id_DATAIN).setup.maxDelay(),b.setup[1]);
        mux(index/2?CV::BEF_SEL:CV::TEF_SEL,CV::F,0); reject();
    }
}
