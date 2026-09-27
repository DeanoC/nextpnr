#include "gtest/gtest.h"
#include "nextpnr.h"
#include "log.h"
#include "placer_heap.h"
#include <fstream>
#include <memory>
#include <string>

NEXTPNR_NAMESPACE_BEGIN
void configure_hps_fixed_anchor(Context *, PlacerHeapCfg &, const char *);
void configure_hps_pin_geometry(Context *, PlacerHeapCfg &, const char *);
NEXTPNR_NAMESPACE_END
USING_NEXTPNR_NAMESPACE

class HpsFixedAnchorTest : public ::testing::Test {
  protected:
    std::unique_ptr<Context> ctx;
    CellInfo *hps;
    BelId bel;
    void SetUp() override {
        ArchArgs args; args.device="5CSEBA6U23I7"; ctx=std::make_unique<Context>(args);
        ctx->settings[ctx->id("placerHeap/alpha")]=std::string("0.1");
        ctx->settings[ctx->id("placerHeap/timingWeight")]=10;
        ctx->settings[ctx->id("timing_driven")]=false;
        ctx->settings[ctx->id("target_freq")]=130e6;
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        hps=ctx->createCell(ctx->id("arbitrary_hps"),id_cyclonev_hps_interface_fpga2sdram);
        auto ready=ctx->createNet(ctx->id("ready"));
        hps->addOutput(ctx->id("cmd_ready_1")); hps->connectPort(ctx->id("cmd_ready_1"),ready);
        auto valid=ctx->createNet(ctx->id("valid"));
        hps->addInput(ctx->id("cmd_valid_1")); hps->connectPort(ctx->id("cmd_valid_1"),valid);
        auto lut=ctx->createCell(ctx->id("sink_lut"),id_MISTRAL_ALUT2);
        lut->params[id_LUT]=0x8; lut->addInput(id_A); lut->addInput(id_B); lut->addOutput(id_Q);
        lut->connectPort(id_A,ready); lut->connectPort(id_Q,valid);
        auto anchor=ctx->createCell(ctx->id("fixed_source"),id_MISTRAL_FF);
        for (auto p:{id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN}) anchor->addInput(p);
        anchor->addOutput(id_Q); anchor->connectPort(id_DATAIN,valid);
        anchor->pin_data[id_ENA].state=PIN_1; anchor->pin_data[id_ACLR].state=PIN_1;
        anchor->pin_data[id_SCLR].state=PIN_0; anchor->pin_data[id_SLOAD].state=PIN_0;
        auto clock=ctx->createNet(ctx->id("clock")); clock->is_global=true;
        anchor->connectPort(id_CLK,clock);
        hps->addInput(ctx->id("cmd_port_clk_1")); hps->connectPort(ctx->id("cmd_port_clk_1"),clock);
        auto anchor_net=ctx->createNet(ctx->id("anchor_output")); anchor->connectPort(id_Q,anchor_net);
        lut->connectPort(id_B,anchor_net);
        ctx->assignArchInfo();
        for (auto b:ctx->getBels()) if (ctx->getBelType(b)==hps->type) { ASSERT_EQ(bel,BelId()); bel=b; }
        ASSERT_NE(bel,BelId());
        bool bound=false;
        for (auto b:ctx->getBelsByTile(30,20)) {
            if (!ctx->isValidBelForCellType(anchor->type,b)) continue;
            ctx->bindBel(b,anchor,STRENGTH_USER);
            if (ctx->isBelLocationValid(b)) { bound=true; break; }
            ctx->unbindBel(b);
        }
        ASSERT_TRUE(bound);
    }
    void run(bool enabled, bool pins) {
        PlacerHeapCfg cfg(ctx.get());
        std::string prefix=::testing::TempDir()+"mistral-hps-anchor-"+(pins?"pins":"plain");
        if (pins) configure_hps_pin_geometry(ctx.get(),cfg,prefix.c_str());
        configure_hps_fixed_anchor(ctx.get(),cfg,enabled?prefix.c_str():nullptr);
        auto observe=cfg.observe_diagnostic_cell;
        cfg.diagnostic_cell=hps;
        int rows=0, before=0, after=0;
        bool solved=false;
        cfg.observe_diagnostic_cell=[&](const char *phase,Loc xy,bool in_place,bool rows_known,bool row_assigned,bool locked) {
            if (observe) observe(phase,xy,in_place,rows_known,row_assigned,locked);
            std::string p=phase;
            if (rows_known) { ++rows; solved|=row_assigned; }
            if (enabled) {
                EXPECT_TRUE(locked); EXPECT_FALSE(in_place); if (rows_known) { EXPECT_FALSE(row_assigned); }
                EXPECT_EQ(xy.x,52); EXPECT_EQ(xy.y,53);
            }
            if (p=="before_refine") { ++before; EXPECT_EQ(hps->belStrength,enabled?STRENGTH_STRONG:STRENGTH_WEAK); }
            if (p=="refine_ready" || p=="after_refine") { EXPECT_EQ(hps->belStrength,STRENGTH_WEAK); }
            if (p=="after_refine") ++after;
        };
        ASSERT_TRUE(placer_heap(ctx.get(),cfg));
        EXPECT_GT(rows,0); EXPECT_EQ(before,1); EXPECT_EQ(after,1);
        EXPECT_EQ(solved,!enabled);
        EXPECT_EQ(hps->bel,bel); EXPECT_EQ(hps->belStrength,STRENGTH_WEAK);
        if (enabled) {
            for (const char *p:{"before_refine","after_refine"}) {
                std::ifstream f(prefix+"."+p+".bels.tsv");
                std::string text((std::istreambuf_iterator<char>(f)),{});
                EXPECT_NE(text.find("arbitrary_hps\t"),std::string::npos);
                EXPECT_NE(text.find("fixed_source\t"),std::string::npos);
            }
        }
    }
};
TEST_F(HpsFixedAnchorTest, RealPlacementAnchorsAndRestoresStrength) { run(true,false); }
TEST_F(HpsFixedAnchorTest, RealPlacementCombinesWithPhysicalOffsets) { run(true,true); }
TEST_F(HpsFixedAnchorTest, DisabledHpsActuallyParticipatesInSolve) { run(false,false); }
TEST_F(HpsFixedAnchorTest, DisabledAllocatesNoIdentifiersOrHooks) {
    PlacerHeapCfg cfg(ctx.get()); auto before=ctx->id("before_off").index;
    configure_hps_fixed_anchor(ctx.get(),cfg,nullptr); configure_hps_fixed_anchor(ctx.get(),cfg,"");
    EXPECT_EQ(ctx->id("after_off").index,before+1);
    EXPECT_EQ(cfg.diagnostic_cell,nullptr); EXPECT_FALSE(bool(cfg.observe_diagnostic_cell)); EXPECT_FALSE(bool(cfg.before_refine));
    EXPECT_FALSE(cfg.ioBufTypes.count(hps->type));
}
TEST_F(HpsFixedAnchorTest, RejectsBoundConstrainedAndMissingHps) {
    PlacerHeapCfg cfg(ctx.get()); auto prefix=(::testing::TempDir()+"mistral-hps-invalid");
    ctx->bindBel(bel,hps,STRENGTH_USER);
    EXPECT_THROW(configure_hps_fixed_anchor(ctx.get(),cfg,prefix.c_str()),log_execution_error_exception);
    EXPECT_EQ(hps->belStrength,STRENGTH_USER); ctx->unbindBel(bel);
    hps->attrs[ctx->id("BEL")]=ctx->getBelName(bel).str(ctx.get());
    EXPECT_THROW(configure_hps_fixed_anchor(ctx.get(),cfg,prefix.c_str()),log_execution_error_exception);
    hps->attrs.erase(ctx->id("BEL")); hps->cluster=hps->name;
    EXPECT_THROW(configure_hps_fixed_anchor(ctx.get(),cfg,prefix.c_str()),log_execution_error_exception);
    hps->cluster=ClusterId(); Region region; hps->region=&region;
    EXPECT_THROW(configure_hps_fixed_anchor(ctx.get(),cfg,prefix.c_str()),log_execution_error_exception);
    hps->region=nullptr;
    auto duplicate=ctx->createCell(ctx->id("duplicate_hps"),hps->type);
    EXPECT_THROW(configure_hps_fixed_anchor(ctx.get(),cfg,prefix.c_str()),log_execution_error_exception);
    duplicate->type=id_MISTRAL_ALUT2; hps->type=id_MISTRAL_ALUT2;
    EXPECT_THROW(configure_hps_fixed_anchor(ctx.get(),cfg,prefix.c_str()),log_execution_error_exception);
}
