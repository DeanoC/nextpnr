/* Route-through insertion maintains FF caches and native LAB legality. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "gtest/gtest.h"
#include <array>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

USING_NEXTPNR_NAMESPACE
namespace {
const std::array<IdString,5> inputs{id_A,id_B,id_C,id_D,id_E};
struct SavedCell {
    CellInfo *cell;
    decltype(CellInfo::ports) ports;
    decltype(CellInfo::params) params;
    decltype(CellInfo::attrs) attrs;
    ArchCellInfo arch;
    BelId bel;
    PlaceStrength strength;
    explicit SavedCell(CellInfo *c):cell(c),ports(c->ports),params(c->params),attrs(c->attrs),
        arch(static_cast<const ArchCellInfo &>(*c)),bel(c->bel),strength(c->belStrength){}
};
class RouteThroughCache : public ::testing::TestWithParam<std::tuple<bool,int>> {
  protected:
    std::unique_ptr<Context> ctx;
    uint32_t lab=0;
    std::vector<CellInfo*> ffs,luts;
    int half() const {return std::get<1>(GetParam());}
    NetInfo *net(const std::string &name){return ctx->createNet(ctx->id(name));}
    void build(bool synchronous_clear)
    {
        ArchArgs args;args.device="5CSEBA6U23I7";ctx=std::make_unique<Context>(args);
        net("$PACKER_GND_NET");net("$PACKER_VCC_NET");
        for(uint32_t i=0;i<ctx->labs.size();++i)if(ctx->labs[i].is_mlab==std::get<0>(GetParam())){lab=i;break;}
        auto *clock=net("clock"),*enable=net("enable"),*clear=net("clear");
        // Seven LUT5 + independent-FF ALMs consume exactly 42 LAB inputs.
        // Every FF can use an empty local LUT, but cannot use E/F because the
        // opposite LUT5 occupies those inputs. A stale cache breaks both gates.
        for(unsigned a=0;a<7;++a) {
            auto *lut=ctx->createCell(ctx->idf("companion%d",a),id_MISTRAL_ALUT5);
            lut->params[id_LUT]=Property(int64_t(0x69c3a5f0),32);
            for(unsigned p=0;p<inputs.size();++p){lut->addInput(inputs[p]);lut->connectPort(inputs[p],net("input"+std::to_string(a)+"_"+std::to_string(p)));}
            lut->pin_data[id_B].state=PIN_INV;
            lut->addOutput(id_Q);lut->connectPort(id_Q,net("companion_q"+std::to_string(a)));
            ctx->assign_comb_info(lut);ctx->assign_default_pinmap(lut);
            ctx->bindBel(ctx->labs[lab].alms[a].lut_bels[1-half()],lut,STRENGTH_WEAK);luts.push_back(lut);
            auto *ff=ctx->createCell(ctx->idf("register%d",a),id_MISTRAL_FF);
            for(auto p:{id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN})ff->addInput(p);
            ff->addOutput(id_Q);ff->connectPort(id_Q,net("register_q"+std::to_string(a)));
            ff->connectPort(id_CLK,clock);ff->connectPort(id_ENA,enable);ff->pin_data[id_ENA].state=PIN_INV;
            ff->pin_data[id_ACLR].state=PIN_1;ff->pin_data[id_SCLR].state=PIN_0;ff->pin_data[id_SLOAD].state=PIN_0;
            if(synchronous_clear){ff->connectPort(id_SCLR,clear);ff->pin_data[id_SCLR].state=PIN_SIG;}
            else{ff->connectPort(id_ACLR,clear);ff->pin_data[id_ACLR].state=PIN_INV;}
            auto *data=net("data"+std::to_string(a));ff->connectPort(id_DATAIN,data);
            auto alias=ctx->idf("observed_data%d",a);data->aliases.push_back(alias);ctx->net_aliases[alias]=data->name;
            ctx->assign_ff_info(ff);ctx->assign_default_pinmap(ff);
            ctx->bindBel(ctx->labs[lab].alms[a].ff_bels[2*half()],ff,STRENGTH_WEAK);ffs.push_back(ff);
        }
        ctx->assignArchInfo();ctx->assign_control_sets(lab);
        for(uint8_t a=0;a<10;++a)ctx->assign_alm_lut_inputs(lab,a);
    }
    int count() const {int total=0;for(const auto &a:ctx->labs[lab].alms)total+=a.unique_input_count;return total;}
    void exercise(bool synchronous_clear)
    {
        build(synchronous_clear);ASSERT_EQ(count(),42);ASSERT_TRUE(ctx->check_lab_input_count(lab));
        for(auto *c:ffs){ASSERT_TRUE(ctx->isBelLocationValid(c->bel));ASSERT_EQ(c->ffInfo.datain,c->getPort(id_DATAIN));}
        std::vector<SavedCell> old;for(const auto &c:ctx->cells)old.emplace_back(c.second.get());
        const auto aliases=ctx->net_aliases;const auto before_lab=ctx->labs[lab];
        std::array<uint64_t,7> masks{};std::array<IdString,7> helper_pins{};
        for(unsigned a=0;a<7;++a){masks[a]=ctx->compute_lut_mask(lab,a);helper_pins[a]=ctx->alm_route_through_pin(lab,a,half());}
        const auto old_cells=ctx->cells.size(),old_nets=ctx->nets.size();
        ctx->lab_pre_route();
        ASSERT_EQ(ctx->cells.size(),old_cells+7);ASSERT_EQ(ctx->nets.size(),old_nets+7);
        auto expected_aliases=aliases;
        for(auto *ff:ffs){auto *q=ff->getPort(id_DATAIN);expected_aliases[q->name]=q->name;}
        EXPECT_EQ(ctx->net_aliases,expected_aliases);EXPECT_EQ(count(),42);EXPECT_TRUE(ctx->check_lab_input_count(lab));
        for(const auto &saved:old) {
            auto *c=saved.cell;EXPECT_EQ(c->params,saved.params);EXPECT_EQ(c->attrs,saved.attrs);
            EXPECT_EQ(c->bel,saved.bel);EXPECT_EQ(c->belStrength,saved.strength);ASSERT_EQ(c->ports.size(),saved.ports.size());
            for(const auto &p:saved.ports) {
                const auto &now=c->ports.at(p.first);EXPECT_EQ(now.name,p.second.name);EXPECT_EQ(now.type,p.second.type);
                if(c->type!=id_MISTRAL_FF||p.first!=id_DATAIN){EXPECT_EQ(now.net,p.second.net);EXPECT_EQ(now.user_idx,p.second.user_idx);}
            }
            ASSERT_EQ(c->pin_data.size(),saved.arch.pin_data.size());
            for(const auto &p:saved.arch.pin_data){const auto &now=c->pin_data.at(p.first);EXPECT_EQ(now.state,p.second.state);EXPECT_EQ(now.bel_pins,p.second.bel_pins);}
            if(c->type==id_MISTRAL_FF){EXPECT_EQ(c->ffInfo.ctrlset,saved.arch.ffInfo.ctrlset);EXPECT_EQ(c->ffInfo.sdata,saved.arch.ffInfo.sdata);EXPECT_EQ(c->ffInfo.datain,c->getPort(id_DATAIN));}
            EXPECT_TRUE(ctx->isBelLocationValid(c->bel))<<ctx->nameOf(c);
        }
        for(unsigned a=0;a<7;++a) {
            auto *ff=ffs[a],*helper=ctx->getBoundBelCell(ctx->labs[lab].alms[a].lut_bels[half()]);
            ASSERT_NE(helper,nullptr);EXPECT_EQ(helper->type,id_MISTRAL_BUF);EXPECT_EQ(helper->name,ctx->idf("register%d$ROUTETHRU",a));
            EXPECT_TRUE(helper->params.empty());EXPECT_TRUE(helper->attrs.empty());EXPECT_EQ(helper->belStrength,STRENGTH_STRONG);
            auto *data=ctx->nets.at(ctx->id("data"+std::to_string(a))).get();
            EXPECT_EQ(helper->getPort(id_A),data);EXPECT_EQ(helper->getPort(id_Q),ff->getPort(id_DATAIN));
            EXPECT_EQ(helper->combInfo.comb_out,ff->ffInfo.datain);EXPECT_EQ(helper->pin_data.at(id_A).bel_pins,std::vector<IdString>{helper_pins[a]});
            EXPECT_EQ(helper->pin_data.at(id_A).state,PIN_SIG);EXPECT_EQ(helper->pin_data.at(id_Q).state,PIN_SIG);
            EXPECT_EQ(helper->pin_data.at(id_Q).bel_pins,std::vector<IdString>{id_COMBOUT});
            ASSERT_EQ(data->users.entries(),1);EXPECT_EQ(data->users.at(helper->ports.at(id_A).user_idx).cell,helper);
            EXPECT_EQ(data->users.at(helper->ports.at(id_A).user_idx).port,id_A);
            auto *q=helper->getPort(id_Q);EXPECT_EQ(q->driver.cell,helper);EXPECT_EQ(q->driver.port,id_Q);ASSERT_EQ(q->users.entries(),1);
            EXPECT_EQ(q->users.at(ff->ports.at(id_DATAIN).user_idx).cell,ff);EXPECT_EQ(q->users.at(ff->ports.at(id_DATAIN).user_idx).port,id_DATAIN);
            EXPECT_EQ(data->aliases,std::vector<IdString>{ctx->idf("observed_data%d",a)});
            EXPECT_TRUE(ctx->is_alm_legal(lab,a));EXPECT_TRUE(ctx->isBelLocationValid(helper->bel));
            const auto &was=before_lab.alms[a],&now=ctx->labs[lab].alms[a];
            EXPECT_EQ(now.unique_input_count,was.unique_input_count);EXPECT_EQ(now.clk_ena_idx,was.clk_ena_idx);EXPECT_EQ(now.aclr_idx,was.aclr_idx);
            EXPECT_EQ(now.sel_clk,was.sel_clk);EXPECT_EQ(now.sel_ena,was.sel_ena);EXPECT_EQ(now.sel_aclr,was.sel_aclr);EXPECT_EQ(now.sel_ef,was.sel_ef);
            EXPECT_EQ(now.l6_mode,was.l6_mode);EXPECT_EQ(now.carry_mode,was.carry_mode);
            const uint64_t stationary_mask=uint64_t(0xffffffff)<<(32*(1-half()));
            EXPECT_EQ(ctx->compute_lut_mask(lab,a)&stationary_mask,masks[a]&stationary_mask);
        }
        EXPECT_EQ(ctx->labs[lab].aclr_used,before_lab.aclr_used);ctx->check();
        // Re-entry must neither add helpers nor invalidate caches/counts.
        std::vector<SavedCell> prepared;for(const auto &c:ctx->cells)prepared.emplace_back(c.second.get());
        ctx->lab_pre_route();EXPECT_EQ(ctx->cells.size(),old_cells+7);EXPECT_EQ(ctx->nets.size(),old_nets+7);EXPECT_EQ(count(),42);
        for(const auto &saved:prepared) {
            auto *c=saved.cell;EXPECT_EQ(c->bel,saved.bel);EXPECT_EQ(c->belStrength,saved.strength);
            for(const auto &p:saved.ports){EXPECT_EQ(c->ports.at(p.first).net,p.second.net);EXPECT_EQ(c->ports.at(p.first).user_idx,p.second.user_idx);}
            for(const auto &p:saved.arch.pin_data){EXPECT_EQ(c->pin_data.at(p.first).state,p.second.state);EXPECT_EQ(c->pin_data.at(p.first).bel_pins,p.second.bel_pins);}
            EXPECT_TRUE(ctx->isBelLocationValid(c->bel));if(c->type==id_MISTRAL_FF){EXPECT_EQ(c->ffInfo.datain,c->getPort(id_DATAIN));EXPECT_EQ(c->ffInfo.ctrlset,saved.arch.ffInfo.ctrlset);}
        }
    }
};
TEST_P(RouteThroughCache, AtLimitLabWithOppositeLut5AndAsynchronousControlRemainsLegal){exercise(false);}
TEST_P(RouteThroughCache, AtLimitLabPreservesSynchronousClearPlaceholderAndPhysicalPins){exercise(true);}
INSTANTIATE_TEST_SUITE_P(BothTilesAndHalves,RouteThroughCache,::testing::Combine(::testing::Bool(),::testing::Values(0,1)));
} // namespace
