/* Explicit local unused arithmetic outputs in repair coverage. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "timing.h"
#include "analogue_repair.h"
#include "gtest/gtest.h"
#include <memory>
#include <string>
#include <functional>

USING_NEXTPNR_NAMESPACE
namespace {
class UnobservedArithmeticCoverage : public ::testing::TestWithParam<bool> {
  protected:
    std::unique_ptr<Context> ctx;
    CellInfo *source=nullptr,*root=nullptr,*carry=nullptr,*capture=nullptr,*ram=nullptr;
    NetInfo *clock=nullptr,*phase=nullptr,*unused=nullptr,*observed=nullptr;
    IdString unused_port,observed_port;
    std::string reason;
    NetInfo *net(const std::string &name) { return ctx->createNet(ctx->id(name)); }
    NetInfo *make_clock(const char *name,int shift) {
        auto *n=net(name);n->clkconstr=std::make_unique<ClockConstraint>();
        n->clkconstr->period=DelayPair(10000);n->clkconstr->high=n->clkconstr->low=DelayPair(5000);
        n->clkconstr->phase_group=ctx->id("family");n->clkconstr->phase_shift=shift;return n;
    }
    CellInfo *ff(const std::string &name,NetInfo *data=nullptr) {
        auto *f=ctx->createCell(ctx->id(name),id_MISTRAL_FF);
        for(auto p:{id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN})f->addInput(p);
        f->addOutput(id_Q);f->connectPort(id_Q,net(name+"$q"));
        f->connectPort(id_DATAIN,data?data:f->getPort(id_Q));f->connectPort(id_CLK,clock);
        f->pin_data[id_ENA].state=f->pin_data[id_ACLR].state=PIN_1;
        f->pin_data[id_SCLR].state=f->pin_data[id_SLOAD].state=PIN_0;return f;
    }
    CellInfo *buffer(const std::string &name,NetInfo *input) {
        auto *b=ctx->createCell(ctx->id(name),id_MISTRAL_BUF);b->addInput(id_A);b->connectPort(id_A,input);
        b->addOutput(id_Q);b->connectPort(id_Q,net(name+"$q"));return b;
    }
    void SetUp() override {
        ArchArgs args;args.device="5CSEBA6U23I7";ctx=std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")]=1e8;net("$PACKER_GND_NET");net("$PACKER_VCC_NET");
        clock=make_clock("clock",0);phase=make_clock("phase",2500);source=ff("source");
        root=ctx->createCell(ctx->id("root"),id_MISTRAL_ALUT2);root->params[id_LUT]=Property(14,4);
        root->addInput(id_A);root->connectPort(id_A,source->getPort(id_Q));root->addInput(id_B);root->pin_data[id_B].state=PIN_0;
        root->addOutput(id_Q);root->connectPort(id_Q,net("root$q"));auto *buf=buffer("buffer",root->getPort(id_Q));
        carry=ctx->createCell(ctx->id("carry"),id_MISTRAL_ALUT_ARITH);
        carry->params[id_LUT0]=Property(0x6996,16);carry->params[id_LUT1]=Property(0x9669,16);
        for(auto p:{id_A,id_B,id_C,id_D0,id_D1,id_CI}) {carry->addInput(p);if(p==id_A)carry->connectPort(p,buf->getPort(id_Q));else carry->pin_data[p].state=PIN_0;}
        for(auto p:{id_SO,id_CO}) {carry->addOutput(p);carry->connectPort(p,net(p==id_SO?"sum":"carry$out"));}
        unused_port=GetParam()?id_SO:id_CO;observed_port=GetParam()?id_CO:id_SO;
        unused=carry->getPort(unused_port);observed=carry->getPort(observed_port);capture=ff("capture",observed);
        ram=ctx->createCell(ctx->id("address_capture"),id_MISTRAL_M10K);
        ram->params[id_CFG_ABITS]=12;ram->params[id_CFG_DBITS]=1;ram->params[id_CFG_DUAL_CLOCK]=1;
        for(auto p:{id_CLK1,id_CLK2,id_B1EN,id_ACLR0,id_ACLR1,ctx->id("B1ADDR[0]")})ram->addInput(p);
        ram->connectPort(id_CLK1,clock);ram->connectPort(id_CLK2,phase);
        ram->connectPort(id_B1EN,ctx->nets.at(ctx->id("$PACKER_VCC_NET")).get());ram->pin_data[id_B1EN].state=PIN_1;
        ram->pin_data[id_ACLR0].state=ram->pin_data[id_ACLR1].state=PIN_0;
        ram->connectPort(ctx->id("B1ADDR[0]"),observed);
    }
    std::unique_ptr<TimingAnalyser> timing(bool skew) {
        ctx->assignArchInfo();auto t=std::make_unique<TimingAnalyser>(ctx.get());t->with_clock_skew=skew;t->setup(false,false,true);return t;
    }
    bool opt_in(TimingAnalyser &t,NetInfo *start=nullptr) {
        return critical_output_coverage_with_unobserved_arithmetic(ctx.get(),t,start?start:root->getPort(id_Q),&reason);
    }
    void mutation(const std::function<void()> &change,const std::function<void()> &restore) {
        // Keep a valid original native report while corrupting only the nominally
        // unobserved branch. Structural eligibility must catch the new observer.
        for(bool skew:{false,true}) {
            auto t=timing(skew);ASSERT_TRUE(opt_in(*t))<<reason;change();
            EXPECT_FALSE(opt_in(*t))<<"skew="<<skew<<" "<<reason;restore();
            EXPECT_TRUE(opt_in(*t))<<reason;
        }
    }
};

TEST_P(UnobservedArithmeticCoverage, OnlyExplicitWrapperAdmitsUnusedSiblingAndRetainsEveryPhaseCapture)
{
    ASSERT_EQ(unused->users.entries(),0u);ASSERT_TRUE(unused->wires.empty());
    for(bool skew:{false,true}) {
        auto t=timing(skew);EXPECT_TRUE(opt_in(*t))<<reason;
        EXPECT_FALSE(critical_lut6_swap_output_coverage(ctx.get(),*t,root->getPort(id_Q),&reason));
        for(auto endpoint:{CellPortKey(capture->name,id_DATAIN),CellPortKey(ram->name,ctx->id("B1ADDR[0]"))}) {
            std::vector<EndpointClockPairTiming> rows;ASSERT_TRUE(t->get_endpoint_clock_pair_timings(endpoint,rows));ASSERT_EQ(rows.size(),1u);
            EXPECT_TRUE(rows[0].setup_timed);EXPECT_TRUE(rows[0].hold_related);ASSERT_TRUE(rows[0].setup_window);
            EXPECT_EQ(*rows[0].setup_window,endpoint.cell==capture->name?10000:2500);
        }
        EXPECT_FALSE(opt_in(*t,unused)); // Never turn an empty root into a capture.
    }
}
TEST_P(UnobservedArithmeticCoverage, PublicClockGlobalRegionConstantProtectedAndRoutedLeavesReject)
{
    auto public_name=ctx->id("public");
    mutation([&]{ctx->ports[public_name]=PortInfo{public_name,unused,PORT_OUT,{}};},[&]{ctx->ports.erase(public_name);});
    mutation([&]{unused->is_global=true;},[&]{unused->is_global=false;});
    mutation([&]{unused->clkconstr=std::make_unique<ClockConstraint>();},[&]{unused->clkconstr.reset();});
    Region region;mutation([&]{unused->region=&region;},[&]{unused->region=nullptr;});
    mutation([&]{carry->region=&region;},[&]{carry->region=nullptr;});
    mutation([&]{unused->constant_value=ctx->id("constant");},[&]{unused->constant_value=IdString();});
    for(const char *name:{"keep","dont_touch","BEL","FES_SLOT"}) {
        auto key=ctx->id(name);mutation([&]{unused->attrs[key]=1;},[&]{unused->attrs.erase(key);});
        mutation([&]{carry->attrs[key]=1;},[&]{carry->attrs.erase(key);});
    }
    mutation([&]{unused->wires[WireId()]=PipMap{};},[&]{unused->wires.clear();});
}
TEST_P(UnobservedArithmeticCoverage, ExactNativeOutputDriverAndPinIdentityAreRequired)
{
    const auto driver=unused->driver;
    mutation([&]{unused->driver.cell=nullptr;},[&]{unused->driver=driver;});
    mutation([&]{unused->driver.port=observed_port;},[&]{unused->driver=driver;});
    mutation([&]{carry->ports.at(unused_port).net=observed;},[&]{carry->ports.at(unused_port).net=unused;});
    mutation([&]{carry->ports.at(unused_port).type=PORT_IN;},[&]{carry->ports.at(unused_port).type=PORT_OUT;});
    mutation([&]{carry->ports.at(unused_port).name=observed_port;},[&]{carry->ports.at(unused_port).name=unused_port;});
    for(auto state:{PIN_0,PIN_1,PIN_INV})mutation([&]{carry->pin_data[unused_port].state=state;},[&]{carry->pin_data[unused_port].state=PIN_SIG;});
    mutation([&]{carry->type=id_MISTRAL_BUF;},[&]{carry->type=id_MISTRAL_ALUT_ARITH;});
    auto original_name=unused->name;
    mutation([&]{unused->name=ctx->id("missing_net_identity");},[&]{unused->name=original_name;});
    auto cell_name=carry->name;
    mutation([&]{carry->name=ctx->id("missing_cell_identity");},[&]{carry->name=cell_name;});
}
TEST_P(UnobservedArithmeticCoverage, RealAndMissingIndexedObserversCannotHideInZeroUserLeaves)
{
    auto *hidden=ff("hidden");
    mutation([&]{hidden->connectPort(id_ACLR,unused);hidden->pin_data[id_ACLR].state=PIN_SIG;},[&]{hidden->disconnectPort(id_ACLR);hidden->pin_data[id_ACLR].state=PIN_1;});
    for(bool skew:{false,true}) {
        auto t=timing(skew);ASSERT_TRUE(opt_in(*t));
        hidden->disconnectPort(id_DATAIN);hidden->connectPort(id_DATAIN,unused);
        const auto slot=hidden->ports.at(id_DATAIN).user_idx;unused->users.remove(slot);ASSERT_EQ(unused->users.entries(),0u);
        EXPECT_FALSE(opt_in(*t))<<"live input port with omitted reciprocal indexed user must reject";
        auto replacement=unused->users.add(PortRef{hidden,id_DATAIN});hidden->ports.at(id_DATAIN).user_idx=replacement;
        hidden->disconnectPort(id_DATAIN);hidden->connectPort(id_DATAIN,hidden->getPort(id_Q));EXPECT_TRUE(opt_in(*t))<<reason;
    }
    auto *observer=ctx->createCell(ctx->id("extra_output_identity"),id_MISTRAL_BUF);observer->addOutput(id_Q);
    mutation([&]{observer->ports.at(id_Q).net=unused;},[&]{observer->ports.at(id_Q).net=nullptr;});
    observer->addInput(ctx->id("IO"));observer->ports.at(ctx->id("IO")).type=PORT_INOUT;
    mutation([&]{observer->ports.at(ctx->id("IO")).net=unused;},[&]{observer->ports.at(ctx->id("IO")).net=nullptr;});
    auto *unsupported=ctx->createCell(ctx->id("unsupported"),id_MISTRAL_OB);unsupported->addInput(id_I);
    mutation([&]{unsupported->connectPort(id_I,unused);},[&]{unsupported->disconnectPort(id_I);});
}
TEST_P(UnobservedArithmeticCoverage, EntirelyUnusedConeAndCutOrUnqualifiedCapturesStillReject)
{
    for(bool skew:{false,true}) {
        auto t=timing(skew);ASSERT_TRUE(opt_in(*t));
        TimingAnalyser setup_only(ctx.get());setup_only.setup_only=true;setup_only.setup();EXPECT_FALSE(opt_in(setup_only));
    }
    Context::SdcClockException cut;cut.from={"clock"};cut.to={"phase"};ctx->sdc_clock_exceptions.push_back(cut);
    for(bool skew:{false,true}){auto t=timing(skew);EXPECT_FALSE(opt_in(*t));}ctx->sdc_clock_exceptions.clear();
    capture->disconnectPort(id_DATAIN);capture->connectPort(id_DATAIN,capture->getPort(id_Q));ram->disconnectPort(ctx->id("B1ADDR[0]"));
    ASSERT_EQ(observed->users.entries(),0u);
    for(bool skew:{false,true}){auto t=timing(skew);EXPECT_FALSE(opt_in(*t))<<"a fully dead cone has no actual capture";}
}
TEST_P(UnobservedArithmeticCoverage, UnknownOrDeepParallelOutputBranchesRemainExcluded)
{
    auto *unused_buf=buffer("unused_buffer",root->getPort(id_Q));
    for(bool skew:{false,true}){auto t=timing(skew);EXPECT_FALSE(opt_in(*t))<<"only unused arithmetic outputs are in scope";}
    unused_buf->disconnectPort(id_A);
    for(bool skew:{false,true}) {
        auto t=timing(skew);ASSERT_TRUE(opt_in(*t));
        const std::string label=skew?"cycle_skew":"cycle_reference";
        auto *cycle=ctx->createCell(ctx->id(label),id_MISTRAL_ALUT2);cycle->params[id_LUT]=Property(14,4);
        cycle->addInput(id_A);cycle->addInput(id_B);cycle->addOutput(id_Q);
        cycle->connectPort(id_Q,net(label+"$q"));cycle->connectPort(id_A,root->getPort(id_Q));cycle->connectPort(id_B,cycle->getPort(id_Q));
        ctx->assignArchInfo();EXPECT_FALSE(opt_in(*t))<<"parallel combinational cycle must reject before capture admission";
        for(auto p:{id_A,id_B,id_Q})cycle->disconnectPort(p);
        const auto name=cycle->name;ctx->cells.erase(name);
        EXPECT_TRUE(opt_in(*t))<<reason;
    }
    auto *chain=root->getPort(id_Q);
    for(unsigned i=0;i<70;++i)chain=buffer("deep"+std::to_string(i),chain)->getPort(id_Q);
    auto *end=ff("deep_capture",chain);(void)end;
    for(bool skew:{false,true}){auto t=timing(skew);EXPECT_FALSE(opt_in(*t))<<"depth bound remains strict";}
}
INSTANTIATE_TEST_SUITE_P(UnusedSumOrCarry,UnobservedArithmeticCoverage,::testing::Bool());
} // namespace
