#include "gtest/gtest.h"
#include "nextpnr.h"
#include "log.h"
#include "placer_heap.h"
#include "heap_pin_offset.h"
#include <fstream>
#include <memory>

NEXTPNR_NAMESPACE_BEGIN
void configure_hps_pin_geometry(Context *, PlacerHeapCfg &, const char *);
NEXTPNR_NAMESPACE_END
USING_NEXTPNR_NAMESPACE

namespace {
struct LinearSystem {
    double a[2][2]={{0,0},{0,0}}, b[2]={0,0};
    int rhs_calls=0;
    void add_coeff(int row,int col,double value) { a[row][col]+=value; }
    void add_rhs(int row,double value) { ++rhs_calls; b[row]+=value; }
};
}
TEST(HeAPPinOffset, FixedAndSolvedOffsetsStampCorrectEquations)
{
    LinearSystem fixed;
    heap_stamp_port_term(fixed,0,0,true,0,0,1);
    heap_stamp_port_term(fixed,0,1,false,53,11,-1);
    EXPECT_DOUBLE_EQ(fixed.b[0]/fixed.a[0][0],64);
    LinearSystem solved;
    heap_stamp_port_term(solved,0,0,true,53,11,1);
    heap_stamp_port_term(solved,0,1,false,53,0,-1);
    EXPECT_DOUBLE_EQ(solved.b[0]/solved.a[0][0],42);
    LinearSystem both;
    heap_stamp_port_term(both,0,0,true,0,11,1);
    heap_stamp_port_term(both,0,1,true,0,-3,-1);
    heap_stamp_port_term(both,1,1,true,0,-3,1);
    heap_stamp_port_term(both,1,0,true,0,11,-1);
    both.add_coeff(0,0,1); both.add_rhs(0,20); // anchor first cell
    double det=both.a[0][0]*both.a[1][1]-both.a[0][1]*both.a[1][0];
    ASSERT_NE(det,0);
    EXPECT_DOUBLE_EQ((both.b[0]*both.a[1][1]-both.a[0][1]*both.b[1])/det,20);
    EXPECT_DOUBLE_EQ((both.a[0][0]*both.b[1]-both.b[0]*both.a[1][0])/det,34);
}
TEST(HeAPPinOffset, PositionAndZeroOffsetPreserveCoordinates)
{
    EXPECT_EQ(heap_pin_position(53,11),64);
    EXPECT_EQ(heap_pin_position(-2,-11),-13); // analytic endpoints are not clamped
    EXPECT_EQ(heap_pin_position(53,0),53);
    LinearSystem zero;
    heap_stamp_port_term(zero,0,0,true,7,0,2);
    heap_stamp_port_term(zero,0,1,false,13,0,-2);
    EXPECT_DOUBLE_EQ(zero.a[0][0],2); EXPECT_DOUBLE_EQ(zero.b[0],26);
    EXPECT_EQ(zero.rhs_calls,1); // no additional zero-valued RHS stamps
}

class HpsPinGeometryTest : public ::testing::Test {
  protected:
    std::unique_ptr<Context> ctx;
    CellInfo *hps;
    BelId bel;
    void SetUp() override {
        ArchArgs args; args.device="5CSEBA6U23I7"; ctx=std::make_unique<Context>(args);
        ctx->settings[ctx->id("placerHeap/alpha")]=std::string("0.1");
        ctx->settings[ctx->id("placerHeap/timingWeight")]=10;
        ctx->settings[ctx->id("timing_driven")]=true;
        hps=ctx->createCell(ctx->id("arbitrary_hps_name"),id_cyclonev_hps_interface_fpga2sdram);
        for (auto b:ctx->getBels()) if (ctx->getBelType(b)==hps->type) { ASSERT_EQ(bel,BelId()); bel=b; }
        ASSERT_NE(bel,BelId());
        for (const char *name:{"cmd_ready_1","rd_data_0[0]","cmd_data_1[0]","cmd_port_clk_1","bad_mapping","unmapped","folded","cfg_port_width[0]","rd_data_0[1]"}) {
            auto p=ctx->id(name); auto n=ctx->createNet(ctx->idf("%s_net",name));
            if (std::string(name)=="cmd_ready_1" || std::string(name)=="rd_data_0[0]" || std::string(name)=="rd_data_0[1]") hps->addOutput(p); else hps->addInput(p);
            hps->connectPort(p,n);
            if (std::string(name)=="cmd_ready_1" || std::string(name)=="rd_data_0[0]") {
                auto sink=ctx->createCell(ctx->idf("%s_sink",name),id_MISTRAL_ALUT2);
                sink->addInput(id_A); sink->connectPort(id_A,n);
            }
        }
        auto constant=ctx->createCell(ctx->id("test_constant"),id_MISTRAL_CONST);
        constant->addOutput(id_Q); constant->connectPort(id_Q,hps->getPort(ctx->id("cfg_port_width[0]")));
        ctx->assignArchInfo();
        hps->pin_data[ctx->id("bad_mapping")].bel_pins={ctx->id("not_a_physical_pin")};
        hps->pin_data[ctx->id("unmapped")].bel_pins.clear();
        hps->pin_data[ctx->id("folded")].state=PIN_1;
    }
};
TEST_F(HpsPinGeometryTest, UsesMappedPhysicalPinsWithoutCellBinding)
{
    PlacerHeapCfg cfg(ctx.get());
    std::string prefix=::testing::TempDir()+"mistral-hps-pin-geometry";
    configure_hps_pin_geometry(ctx.get(),cfg,prefix.c_str());
    ASSERT_TRUE(bool(cfg.get_port_offset));
    ASSERT_EQ(hps->bel,BelId());
    auto base=ctx->getBelLocation(bel);
    for (const char *name:{"cmd_ready_1","rd_data_0[0]","cmd_data_1[0]"}) {
        auto p=ctx->id(name); auto wire=ctx->getBelPinWire(bel,p); ASSERT_NE(wire,WireId());
        auto offset=cfg.get_port_offset({hps,p});
        EXPECT_EQ(offset.x,int(wire.node.x())-base.x); EXPECT_EQ(offset.y,int(wire.node.y())-base.y);
    }
    auto ready=cfg.get_port_offset({hps,ctx->id("cmd_ready_1")});
    EXPECT_EQ(ready.x,-1); EXPECT_EQ(ready.y,11);
    ctx->bindBel(bel,hps,STRENGTH_WEAK);
    EXPECT_EQ(cfg.get_port_offset({hps,ctx->id("cmd_ready_1")}).y,11);
    ctx->unbindBel(bel);
    EXPECT_EQ(cfg.get_port_offset({hps,ctx->id("cmd_ready_1")}).y,11);
    for (const char *name:{"cmd_port_clk_1","bad_mapping","unmapped","folded","cfg_port_width[0]","rd_data_0[1]"}) {
        auto offset=cfg.get_port_offset({hps,ctx->id(name)}); EXPECT_EQ(offset.x,0); EXPECT_EQ(offset.y,0);
    }
    std::ifstream manifest(prefix+".pins.tsv");
    std::string rows((std::istreambuf_iterator<char>(manifest)),{});
    for (const char *reason:{"clock","missing_wire","unmapped","folded_constant","constant_net","unused_output"}) EXPECT_NE(rows.find(reason),std::string::npos);
}
TEST_F(HpsPinGeometryTest, DisabledDoesNotAllocateIdentifiersOrCallback)
{
    PlacerHeapCfg cfg(ctx.get());
    auto before=ctx->id("before_geometry_off").index;
    configure_hps_pin_geometry(ctx.get(),cfg,nullptr);
    configure_hps_pin_geometry(ctx.get(),cfg,"");
    EXPECT_FALSE(bool(cfg.get_port_offset));
    EXPECT_EQ(ctx->id("after_geometry_off").index,before+1);
}

TEST_F(HpsPinGeometryTest, RejectsEnabledNoOp)
{
    PlacerHeapCfg cfg(ctx.get());
    for (auto &p:hps->pin_data) p.second.state=PIN_1;
    std::string prefix=::testing::TempDir()+"mistral-hps-noop";
    EXPECT_THROW(configure_hps_pin_geometry(ctx.get(),cfg,prefix.c_str()),log_execution_error_exception);
    EXPECT_FALSE(bool(cfg.get_port_offset));
}
