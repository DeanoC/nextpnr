/* Complete upstream origins for final routed-net rediscovery. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "timing.h"
#include "analogue_repair.h"
#include "gtest/gtest.h"
#include "jsonwrite.h"
#include <sstream>
#include <array>
#include <memory>
#include <map>
#include <cstdint>
#include <set>
#include <string>

USING_NEXTPNR_NAMESPACE
namespace {
const std::array<IdString,6> pins{id_A,id_B,id_C,id_D,id_E,id_F};
class FinalRouteSourceCoverage : public ::testing::Test {
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
        const std::array<IdString,5> types{id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,id_MISTRAL_ALUT4,id_MISTRAL_ALUT5,id_MISTRAL_ALUT6};
        auto *l=ctx->createCell(ctx->id(name),types.at(width-2));
        const unsigned bits=1u<<width;
        const uint64_t mask=bits==64?~uint64_t(1):(uint64_t(1)<<bits)-2;
        l->params[id_LUT]=Property(int64_t(mask),bits);
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
            EXPECT_EQ(critical_final_route_source_coverage(ctx.get(),timing,root->getPort(id_Q),&reason),expected)<<"skew="<<skew<<" "<<reason;
        }
    }
};

TEST_F(FinalRouteSourceCoverage, CompleteMultilevelNativeConePreservesEveryKnownPhaseAndStaticPin)
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

TEST_F(FinalRouteSourceCoverage, MixedKnownAndSilentlyOmittedSiblingOriginsFailClosed)
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

TEST_F(FinalRouteSourceCoverage, PublicKnownOriginsAsyncStartsAndExplicitCutsFailClosed)
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

TEST_F(FinalRouteSourceCoverage, RejectsIncompleteAnalysisAndMalformedSourceIdentity)
{
    ctx->assignArchInfo();TimingAnalyser fresh(ctx.get());
    EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),fresh,root->getPort(id_Q),&reason));
    TimingAnalyser full(ctx.get());full.setup(false,false,true);
    ASSERT_TRUE(critical_final_route_source_coverage(ctx.get(),full,root->getPort(id_Q),&reason))<<reason;
    TimingAnalyser setup_only(ctx.get());setup_only.setup_only=true;setup_only.setup();
    EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),setup_only,root->getPort(id_Q),&reason));
    auto *n=source[1]->getPort(id_Q);auto old=n->driver;n->driver.port=id_CLK;
    EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),full,root->getPort(id_Q),&reason));n->driver=old;
    n->driver.cell=nullptr;EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),full,root->getPort(id_Q),&reason));n->driver=old;
    const auto period=clock->clkconstr->period;clock->clkconstr->period=DelayPair(0);
    EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),full,root->getPort(id_Q),&reason));clock->clkconstr->period=period;
    const auto state=source[1]->pin_data[id_CLK].state;source[1]->pin_data[id_CLK].state=PIN_INV;
    EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),full,root->getPort(id_Q),&reason));source[1]->pin_data[id_CLK].state=state;
    for(auto forced:{PIN_0,PIN_1}){source[1]->pin_data[id_CLK].state=forced;EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),full,root->getPort(id_Q),&reason));}
    source[1]->pin_data[id_CLK].state=state;
    full.with_clock_skew=!full.with_clock_skew;EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),full,root->getPort(id_Q),&reason));
    full.with_clock_skew=!full.with_clock_skew;
    EXPECT_TRUE(critical_final_route_source_coverage(ctx.get(),full,root->getPort(id_Q),&reason))<<reason;
}

TEST_F(FinalRouteSourceCoverage, ReachableCyclesAndOverDepthConesFailClosed)
{
    ctx->assignArchInfo();TimingAnalyser timing(ctx.get());timing.setup(false,false,true);
    input(mix,id_B,mix->getPort(id_Q));ctx->assign_comb_info(mix);
    EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),timing,root->getPort(id_Q),&reason));
    input(mix,id_B,source[1]->getPort(id_Q));
    auto *n=mix->getPort(id_Q);for(unsigned i=0;i<70;++i)n=buffer("deep"+std::to_string(i),n)->getPort(id_Q);
    input(root,id_A,n);expect(false);
}
TEST_F(FinalRouteSourceCoverage, WideSharedOriginHonorsTotalWorkBound)
{
    // A shallow cone can still exceed the bounded complete user audit.
    for(unsigned n=0;n<4100;++n)buffer("fanout"+std::to_string(n),source[0]->getPort(id_Q));
    expect(false);
}


TEST_F(FinalRouteSourceCoverage, DirectRegisteredQAndEveryOrdinaryWidthKeepOldWrapperScopes)
{
    std::array<CellInfo*,5> widths{};
    for(int width=2;width<=6;++width) {
        auto *cell=lut("width"+std::to_string(width),width);widths[width-2]=cell;
        for(int p=0;p<width;++p)input(cell,pins[p],source[p]->getPort(id_Q));
        ff("width_capture"+std::to_string(width),cell->getPort(id_Q));
    }
    auto *memory_source=memory("root_scope_memory");
    auto *buf=buffer("root_scope_buffer",source[0]->getPort(id_Q));
    ff("memory_scope_capture",memory_source->getPort(id_B1DATA));ff("buffer_scope_capture",buf->getPort(id_Q));
    ctx->assignArchInfo();
    for(bool skew:{false,true}) {
        TimingAnalyser timing(ctx.get());timing.with_clock_skew=skew;timing.setup(false,false,true);
        EXPECT_TRUE(critical_final_route_source_coverage(ctx.get(),timing,source[0]->getPort(id_Q),&reason))<<reason;
        EXPECT_FALSE(critical_lut6_swap_source_coverage(ctx.get(),timing,source[0],&reason));
        EXPECT_FALSE(critical_lut_permutation_source_coverage(ctx.get(),timing,source[0],&reason));
        for(int width=2;width<=6;++width) {
            auto *cell=widths[width-2];
            EXPECT_TRUE(critical_final_route_source_coverage(ctx.get(),timing,cell->getPort(id_Q),&reason))<<width<<" "<<reason;
            EXPECT_EQ(critical_lut6_swap_source_coverage(ctx.get(),timing,cell,&reason),width==6)<<reason;
            EXPECT_EQ(critical_lut_permutation_source_coverage(ctx.get(),timing,cell,&reason),width<6)<<reason;
            EXPECT_TRUE(critical_lut6_swap_output_coverage(ctx.get(),timing,cell->getPort(id_Q),&reason))<<reason;
        }
        // Native BUF/M10K are legal inside an ordinary source cone, but are not
        // new root-driver types admitted by this separate wrapper.
        EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),timing,buf->getPort(id_Q),&reason));
        EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),timing,memory_source->getPort(id_B1DATA),&reason));
    }
}

TEST_F(FinalRouteSourceCoverage, MalformedRootNetPortStateAndIndexedUserIdentityFailClosed)
{
    ctx->assignArchInfo();
    for(bool skew:{false,true}) {
        TimingAnalyser timing(ctx.get());timing.with_clock_skew=skew;timing.setup(false,false,true);
        auto *q=root->getPort(id_Q);const auto driver=q->driver;
        ASSERT_TRUE(critical_final_route_source_coverage(ctx.get(),timing,q,&reason))<<reason;
        EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),timing,nullptr,&reason));
        q->driver.port=id_A;EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),timing,q,&reason));q->driver=driver;
        q->driver.cell=nullptr;EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),timing,q,&reason));q->driver=driver;
        auto &out=root->ports.at(id_Q);const auto old_name=out.name;out.name=id_A;
        EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),timing,q,&reason));out.name=old_name;
        const auto old_type=out.type;out.type=PORT_IN;
        EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),timing,q,&reason));out.type=old_type;
        const auto pin_state=root->get_pin_state(id_Q);root->pin_data[id_Q].state=PIN_INV;
        EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),timing,q,&reason));root->pin_data[id_Q].state=pin_state;
        const auto net_name=q->name;q->name=ctx->id("missing_root_identity");
        EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),timing,q,&reason));q->name=net_name;
        auto &user=q->users.at(capture->ports.at(id_DATAIN).user_idx);const auto user_port=user.port;user.port=id_ENA;
        EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),timing,q,&reason));user.port=user_port;
        auto *other=net("other_root_"+std::to_string(skew));other->driver=driver;
        EXPECT_FALSE(critical_final_route_source_coverage(ctx.get(),timing,other,&reason));other->driver=PortRef{};
        EXPECT_TRUE(critical_final_route_source_coverage(ctx.get(),timing,q,&reason))<<reason;
    }
}

TEST_F(FinalRouteSourceCoverage, DefaultZeroFinalHookLeavesUnroutedStateAndInternedIdsExact)
{
    ASSERT_EQ(ctx->critical_final_route_budget,0);
    // A nonzero earlier-route budget must not activate this separate hook.
    ctx->critical_route_budget=32;ctx->force=true;
    ASSERT_FALSE(ctx->attrs.count(id_step));ASSERT_FALSE(ctx->analogue_cache_valid);ASSERT_TRUE(ctx->analogue_arc_cache.empty());
    std::ostringstream before,after;std::string filename="unused-in-memory.json";
    ASSERT_TRUE(write_json_file(before,filename,ctx.get()));
    const auto ids=ctx->idstring_str_to_idx->size();const auto attrs=ctx->attrs;const auto settings=ctx->settings;
    std::map<IdString,std::map<IdString,std::pair<CellPinState,std::vector<IdString>>>> maps;
    for(const auto &entry:ctx->cells)for(const auto &pin:entry.second->pin_data)
        maps[entry.first][pin.first]={pin.second.state,pin.second.bel_pins};
    EXPECT_NO_THROW(ctx->repair_critical_routes(true));
    EXPECT_EQ(ctx->idstring_str_to_idx->size(),ids);EXPECT_EQ(ctx->attrs,attrs);EXPECT_EQ(ctx->settings,settings);
    EXPECT_FALSE(ctx->analogue_cache_valid);EXPECT_TRUE(ctx->analogue_arc_cache.empty());
    for(const auto &entry:ctx->cells) {
        EXPECT_EQ(entry.second->pin_data.size(),maps.at(entry.first).size());
        for(const auto &pin:entry.second->pin_data) {
            const auto &old=maps.at(entry.first).at(pin.first);EXPECT_EQ(pin.second.state,old.first);EXPECT_EQ(pin.second.bel_pins,old.second);
        }
    }
    ASSERT_TRUE(write_json_file(after,filename,ctx.get()));EXPECT_EQ(before.str(),after.str());
    EXPECT_EQ(ctx->idstring_str_to_idx->size(),ids);
}
} // namespace
