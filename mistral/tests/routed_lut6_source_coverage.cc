/* Complete upstream origins for routed LUT6 repair. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "timing.h"
#include "analogue_repair.h"
#include "gtest/gtest.h"
#include <array>
#include <memory>
#include <set>
#include <string>

USING_NEXTPNR_NAMESPACE
namespace {
const std::array<IdString,6> pins{id_A,id_B,id_C,id_D,id_E,id_F};
class RoutedLut6SourceCoverage : public ::testing::Test {
  protected:
    std::unique_ptr<Context> ctx;
    CellInfo *root=nullptr,*capture=nullptr,*mix=nullptr;
    std::array<CellInfo*,6> source{};
    NetInfo *clock=nullptr,*phase=nullptr;
    std::string reason;
    NetInfo *net(const std::string &n){return ctx->createNet(ctx->id(n));}
    NetInfo *make_clock(const char *name,int shift)
    {
        auto *n=net(name);n->clkconstr=std::make_unique<ClockConstraint>();
        n->clkconstr->period=DelayPair(10000);n->clkconstr->high=n->clkconstr->low=DelayPair(5000);
        n->clkconstr->phase_group=ctx->id("family");n->clkconstr->phase_shift=shift;return n;
    }
    CellInfo *ff(const std::string &name,NetInfo *data=nullptr,NetInfo *clk=nullptr)
    {
        auto *f=ctx->createCell(ctx->id(name),id_MISTRAL_FF);
        for(auto p:{id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN})f->addInput(p);
        f->addOutput(id_Q);f->connectPort(id_Q,net(name+"$q"));
        f->connectPort(id_DATAIN,data?data:f->getPort(id_Q));f->connectPort(id_CLK,clk?clk:clock);
        f->pin_data[id_ENA].state=f->pin_data[id_ACLR].state=PIN_1;
        f->pin_data[id_SCLR].state=f->pin_data[id_SLOAD].state=PIN_0;return f;
    }
    CellInfo *lut(const std::string &name,int width)
    {
        auto *l=ctx->createCell(ctx->id(name),width==2?id_MISTRAL_ALUT2:id_MISTRAL_ALUT6);
        l->params[id_LUT]=width==2?Property(8,4):Property(int64_t(0xfffffffffffffffeULL),64);
        for(int n=0;n<width;++n){l->addInput(pins[n]);l->pin_data[pins[n]].state=PIN_0;}
        l->addOutput(id_Q);l->connectPort(id_Q,net(name+"$q"));return l;
    }
    void input(CellInfo *c,IdString p,NetInfo *n,CellPinState state=PIN_SIG)
    {if(c->getPort(p))c->disconnectPort(p);if(n)c->connectPort(p,n);c->pin_data[p].state=state;}
    CellInfo *buffer(const std::string &name,NetInfo *n)
    {
        auto *b=ctx->createCell(ctx->id(name),id_MISTRAL_BUF);b->addInput(id_A);b->connectPort(id_A,n);
        b->addOutput(id_Q);b->connectPort(id_Q,net(name+"$q"));return b;
    }
    CellInfo *memory(const std::string &name)
    {
        auto *m=ctx->createCell(ctx->id(name),id_MISTRAL_M10K);
        m->params[id_CFG_ABITS]=12;m->params[id_CFG_DBITS]=1;m->params[id_CFG_DUAL_CLOCK]=1;
        for(auto p:{id_CLK1,id_CLK2,id_B1EN,id_ACLR0,id_ACLR1,ctx->id("B1ADDR[0]")})m->addInput(p);
        m->connectPort(id_CLK1,clock);m->connectPort(id_CLK2,phase);m->connectPort(ctx->id("B1ADDR[0]"),source[1]->getPort(id_Q));
        m->connectPort(id_B1EN,ctx->nets.at(ctx->id("$PACKER_VCC_NET")).get());m->pin_data[id_B1EN].state=PIN_1;
        m->pin_data[id_ACLR0].state=m->pin_data[id_ACLR1].state=PIN_0;m->addOutput(id_B1DATA);m->connectPort(id_B1DATA,net(name+"$q"));return m;
    }
    void SetUp() override
    {
        ArchArgs args;args.device="5CSEBA6U23I7";ctx=std::make_unique<Context>(args);ctx->settings[ctx->id("target_freq")]=1e8;
        net("$PACKER_GND_NET");net("$PACKER_VCC_NET");clock=make_clock("clock",0);phase=make_clock("phase",2500);
        for(unsigned n=0;n<6;++n)source[n]=ff("source"+std::to_string(n));
        root=lut("root",6);for(unsigned n=0;n<6;++n)input(root,pins[n],source[n]->getPort(id_Q));
        capture=ff("capture",root->getPort(id_Q));mix=lut("mixed_source",2);
        input(mix,id_A,source[0]->getPort(id_Q));input(mix,id_B,source[1]->getPort(id_Q));input(root,id_A,mix->getPort(id_Q));
    }
    void expect(bool expected)
    {
        ctx->assignArchInfo();
        for(bool skew:{false,true}) {
            TimingAnalyser timing(ctx.get());timing.with_clock_skew=skew;timing.setup(false,false,true);
            EXPECT_EQ(critical_lut6_swap_source_coverage(ctx.get(),timing,root,&reason),expected)<<"skew="<<skew<<" "<<reason;
        }
    }
};

TEST_F(RoutedLut6SourceCoverage, CompleteMultilevelNativeConePreservesEveryKnownPhaseAndStaticPin)
{
    expect(true);
    auto *m=memory("read_source");input(root,id_B,m->getPort(id_B1DATA));
    input(mix,id_A,source[0]->getPort(id_Q),PIN_INV);input(mix,id_B,nullptr,PIN_1);expect(true);
    input(mix,id_B,nullptr,PIN_0);expect(true);
    auto *constant=ctx->createCell(ctx->id("one"),id_MISTRAL_CONST);constant->params[id_LUT]=Property(1,1);
    constant->addOutput(id_Q);constant->connectPort(id_Q,net("one$q"));input(mix,id_B,constant->getPort(id_Q),PIN_1);expect(true);
    input(mix,id_B,constant->getPort(id_Q),PIN_0);expect(false);input(mix,id_B,constant->getPort(id_Q),PIN_1);
    auto *b=buffer("upstream_buffer",mix->getPort(id_Q));
    auto *a=ctx->createCell(ctx->id("upstream_carry"),id_MISTRAL_ALUT_ARITH);
    a->params[id_LUT0]=Property(0x6996,16);a->params[id_LUT1]=Property(0x9669,16);
    for(auto p:{id_A,id_B,id_C,id_D0,id_D1,id_CI}){a->addInput(p);a->pin_data[p].state=PIN_0;}
    input(a,id_A,b->getPort(id_Q));input(a,id_B,source[2]->getPort(id_Q),PIN_INV);input(a,id_D0,source[3]->getPort(id_Q));
    a->pin_data[id_D1].state=PIN_1;input(a,id_CI,source[4]->getPort(id_Q));
    for(auto p:{id_SO,id_CO}){a->addOutput(p);a->connectPort(p,net(p==id_SO?"sum":"carry"));}
    input(root,id_A,a->getPort(id_SO));expect(true);
    ctx->assignArchInfo();TimingAnalyser timing(ctx.get());timing.setup(false,false,true);
    std::vector<TimingAnalyser::SetupWindowHint> hints;ASSERT_TRUE(timing.get_setup_window_hints(CellPortKey(root->name,id_Q),hints));
    std::set<IdString> clocks;for(const auto &h:hints)clocks.insert(h.launch.clock);
    EXPECT_EQ(clocks,(std::set<IdString>{clock->name,phase->name}));
}

TEST_F(RoutedLut6SourceCoverage, MixedKnownAndSilentlyOmittedSiblingOriginsFailClosed)
{
    expect(true);
    auto *missing=ff("missing_clock");missing->disconnectPort(id_CLK);input(mix,id_B,missing->getPort(id_Q));expect(false);
    input(mix,id_B,net("undriven"));expect(false);
    auto *ignored=ctx->createCell(ctx->id("ignored_source"),id_MISTRAL_IB);ignored->addOutput(id_O);ignored->connectPort(id_O,net("ignored$q"));
    int n=0;ASSERT_EQ(ctx->getPortTimingClass(ignored,id_O,n),TMG_IGNORE);input(mix,id_B,ignored->getPort(id_O));expect(false);
    auto *public_net=net("public_undriven");ctx->ports[ctx->id("public_input")]=PortInfo{ctx->id("public_input"),public_net,PORT_IN,{}};
    input(mix,id_B,public_net);expect(false);
    input(mix,id_B,source[1]->getPort(id_Q));expect(true);
}

TEST_F(RoutedLut6SourceCoverage, PublicKnownOriginsAsyncStartsAndExplicitCutsFailClosed)
{
    auto *known=source[1]->getPort(id_Q);auto public_name=ctx->id("public_known");
    ctx->ports[public_name]=PortInfo{public_name,known,PORT_OUT,{}};expect(false);ctx->ports.erase(public_name);expect(true);
    auto *async=ctx->createCell(ctx->id("async_status"),id_altera_pll);async->addOutput(id_locked);async->connectPort(id_locked,net("locked"));
    input(mix,id_B,async->getPort(id_locked));expect(false);
    auto *m=memory("phase_memory");input(mix,id_B,m->getPort(id_B1DATA));expect(true);
    Context::SdcClockException cut;cut.from={"phase"};cut.to={"clock"};ctx->sdc_clock_exceptions.push_back(cut);expect(false);
    ctx->sdc_clock_exceptions.clear();expect(true);
    m->disconnectPort(id_CLK2);expect(false);m->connectPort(id_CLK2,phase);
    m->params[id_CFG_ASYNC_READ]=1;expect(false);
}

TEST_F(RoutedLut6SourceCoverage, RejectsIncompleteAnalysisAndMalformedSourceIdentity)
{
    ctx->assignArchInfo();TimingAnalyser full(ctx.get());full.setup(false,false,true);
    ASSERT_TRUE(critical_lut6_swap_source_coverage(ctx.get(),full,root,&reason))<<reason;
    TimingAnalyser setup_only(ctx.get());setup_only.setup_only=true;setup_only.setup();
    EXPECT_FALSE(critical_lut6_swap_source_coverage(ctx.get(),setup_only,root,&reason));
    auto *n=source[1]->getPort(id_Q);auto old=n->driver;n->driver.port=id_CLK;
    EXPECT_FALSE(critical_lut6_swap_source_coverage(ctx.get(),full,root,&reason));n->driver=old;
    n->driver.cell=nullptr;EXPECT_FALSE(critical_lut6_swap_source_coverage(ctx.get(),full,root,&reason));n->driver=old;
    const auto period=clock->clkconstr->period;clock->clkconstr->period=DelayPair(0);
    EXPECT_FALSE(critical_lut6_swap_source_coverage(ctx.get(),full,root,&reason));clock->clkconstr->period=period;
    const auto state=source[1]->pin_data[id_CLK].state;source[1]->pin_data[id_CLK].state=PIN_INV;
    EXPECT_FALSE(critical_lut6_swap_source_coverage(ctx.get(),full,root,&reason));source[1]->pin_data[id_CLK].state=state;
    for(auto forced:{PIN_0,PIN_1}){source[1]->pin_data[id_CLK].state=forced;EXPECT_FALSE(critical_lut6_swap_source_coverage(ctx.get(),full,root,&reason));}
    source[1]->pin_data[id_CLK].state=state;
    full.with_clock_skew=!full.with_clock_skew;EXPECT_FALSE(critical_lut6_swap_source_coverage(ctx.get(),full,root,&reason));
    full.with_clock_skew=!full.with_clock_skew;
    EXPECT_TRUE(critical_lut6_swap_source_coverage(ctx.get(),full,root,&reason))<<reason;
}

TEST_F(RoutedLut6SourceCoverage, ReachableCyclesAndOverDepthConesFailClosed)
{
    ctx->assignArchInfo();TimingAnalyser timing(ctx.get());timing.setup(false,false,true);
    input(mix,id_B,mix->getPort(id_Q));ctx->assign_comb_info(mix);
    EXPECT_FALSE(critical_lut6_swap_source_coverage(ctx.get(),timing,root,&reason));
    input(mix,id_B,source[1]->getPort(id_Q));
    auto *n=mix->getPort(id_Q);for(unsigned i=0;i<70;++i)n=buffer("deep"+std::to_string(i),n)->getPort(id_Q);
    input(root,id_A,n);expect(false);
}
TEST_F(RoutedLut6SourceCoverage, WideSharedOriginHonorsTotalWorkBound)
{
    // A shallow cone can still exceed the bounded complete user audit.
    for(unsigned n=0;n<4100;++n)buffer("fanout"+std::to_string(n),source[0]->getPort(id_Q));
    expect(false);
}
} // namespace
