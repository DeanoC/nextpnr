/* Native transparent BUF user admission and route-only rollback. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "analogue_repair.h"
#include "gtest/gtest.h"
#include "timing.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>
USING_NEXTPNR_NAMESPACE
namespace {
using CV=mistral::CycloneV;
const std::array<IdString,6> pins{id_A,id_B,id_C,id_D,id_E,id_F};
// Complete independent rollback oracle retained from the validated source-exit fixture.
struct Snapshot {
    struct Cell {
        CellInfo *identity;
        IdString type;
        BelId bel;
        PlaceStrength strength;
        ClusterId cluster;
        decltype(CellInfo::attrs) attrs;
        decltype(CellInfo::params) params;
        decltype(CellInfo::ports) ports;
        ArchCellInfo architecture;
    };
    struct Net {
        NetInfo *identity;
        PortRef driver;
        indexed_store<PortRef> users;
        decltype(NetInfo::attrs) attrs;
        std::vector<IdString> aliases;
        std::map<WireId, std::pair<PipId, PlaceStrength>> wires;
    };
    std::map<IdString, Cell> cells;
    std::map<IdString, Net> nets;
    std::vector<IdString> cell_order, net_order;
    std::vector<LABInfo> labs;
    decltype(Context::net_aliases) aliases;
    decltype(Context::attrs) attrs;
    std::vector<uint8_t> rbf;
    using ArcKey = std::tuple<IdString, IdString, IdString>;
    using ArcValue = std::tuple<bool, std::array<delay_t,4>, std::array<delay_t,4>>;
    std::map<ArcKey, ArcValue> arcs;
    static std::array<delay_t,4> quad(const DelayQuad &q)
    { return {q.minRiseDelay(), q.maxRiseDelay(), q.minFallDelay(), q.maxFallDelay()}; }
    static std::map<ArcKey, ArcValue> cache(Context *ctx)
    {
        std::map<ArcKey, ArcValue> result;
        for (const auto &entry : ctx->nets) for (const auto &user : entry.second->users) {
            auto it=ctx->analogue_arc_cache.find(&user);
            if(it!=ctx->analogue_arc_cache.end()) result[{entry.first,user.cell->name,user.port}]={it->second.ok,quad(it->second.delay),quad(it->second.lut_input_delay)};
        }
        return result;
    }
    explicit Snapshot(Context *ctx) : labs(ctx->labs), aliases(ctx->net_aliases), attrs(ctx->attrs), arcs(cache(ctx))
    {
        ctx->cyclonev->rbf_save(rbf);
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            cell_order.push_back(entry.first);
            cells.emplace(entry.first, Cell{cell, cell->type, cell->bel, cell->belStrength, cell->cluster, cell->attrs, cell->params,
                                            cell->ports, *static_cast<ArchCellInfo *>(cell)});
        }
        for (const auto &entry : ctx->nets) {
            auto *net = entry.second.get();
            net_order.push_back(entry.first);
            Net saved{net, net->driver, net->users, net->attrs, net->aliases, {}};
            for (const auto &wire : net->wires) saved.wires[wire.first]={wire.second.pip,wire.second.strength};
            nets.emplace(entry.first, std::move(saved));
        }
    }
    void expect_exact(Context *ctx, const std::set<NetInfo *> &changed_routes = {}) const
    {
        EXPECT_EQ(ctx->net_aliases, aliases);
        EXPECT_EQ(ctx->attrs, attrs);
        EXPECT_TRUE(ctx->analogue_cache_valid);
        if (changed_routes.empty()) {
            std::vector<uint8_t> now;ctx->cyclonev->rbf_save(now);EXPECT_EQ(now,rbf);EXPECT_EQ(cache(ctx),arcs);
        } else {
            auto unchanged_arcs=cache(ctx),old_arcs=arcs;
            for(auto *net:changed_routes) {
                for(auto it=unchanged_arcs.begin();it!=unchanged_arcs.end();)if(std::get<0>(it->first)==net->name)it=unchanged_arcs.erase(it);else ++it;
                for(auto it=old_arcs.begin();it!=old_arcs.end();)if(std::get<0>(it->first)==net->name)it=old_arcs.erase(it);else ++it;
            }
            EXPECT_EQ(unchanged_arcs,old_arcs);
        }
        ASSERT_EQ(ctx->cells.size(), cells.size());
        ASSERT_EQ(ctx->nets.size(), nets.size());
        std::vector<IdString> current_cells, current_nets;
        for (const auto &entry : ctx->cells) current_cells.push_back(entry.first);
        for (const auto &entry : ctx->nets) current_nets.push_back(entry.first);
        EXPECT_EQ(current_cells, cell_order);
        EXPECT_EQ(current_nets, net_order);
        for (const auto &entry : cells) {
            auto *cell = ctx->cells.at(entry.first).get();
            const auto &old = entry.second;
            SCOPED_TRACE(entry.first.str(ctx));
            EXPECT_EQ(cell, old.identity);
            EXPECT_EQ(cell->type, old.type);
            EXPECT_EQ(cell->bel, old.bel);
            EXPECT_EQ(cell->belStrength, old.strength);
            if (old.bel != BelId()) { EXPECT_EQ(ctx->getBoundBelCell(old.bel), cell); }
            EXPECT_EQ(cell->attrs, old.attrs);
            EXPECT_EQ(cell->params, old.params);
            ASSERT_EQ(cell->ports.size(), old.ports.size());
            for (const auto &p : old.ports) {
                ASSERT_TRUE(cell->ports.count(p.first));
                const auto &now = cell->ports.at(p.first);
                EXPECT_EQ(now.name, p.second.name);
                EXPECT_EQ(now.type, p.second.type);
                EXPECT_EQ(now.net, p.second.net);
                EXPECT_EQ(now.user_idx, p.second.user_idx);
            }
            const auto &arch = old.architecture;
            ASSERT_EQ(cell->pin_data.size(), arch.pin_data.size());
            for (const auto &pin : arch.pin_data) {
                ASSERT_TRUE(cell->pin_data.count(pin.first));
                EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
                EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
            }
            EXPECT_EQ(cell->cluster, old.cluster);
            EXPECT_EQ(cell->constr_x, arch.constr_x);
            EXPECT_EQ(cell->constr_y, arch.constr_y);
            EXPECT_EQ(cell->constr_z, arch.constr_z);
            EXPECT_EQ(cell->constr_abs_z, arch.constr_abs_z);
            EXPECT_EQ(cell->constr_children, arch.constr_children);
            if (ctx->is_comb_cell(cell->type)) {
                for (int i = 0; i < cell->combInfo.lut_input_count; ++i)
                    EXPECT_EQ(cell->combInfo.lut_in[i], arch.combInfo.lut_in[i]);
                EXPECT_EQ(cell->combInfo.comb_out, arch.combInfo.comb_out);
                EXPECT_EQ(cell->combInfo.lut_input_count, arch.combInfo.lut_input_count);
                EXPECT_EQ(cell->combInfo.used_lut_input_count, arch.combInfo.used_lut_input_count);
                EXPECT_EQ(cell->combInfo.lut_bits_count, arch.combInfo.lut_bits_count);
                EXPECT_EQ(cell->combInfo.chain_shared_input_count, arch.combInfo.chain_shared_input_count);
                EXPECT_EQ(cell->combInfo.is_carry, arch.combInfo.is_carry);
                EXPECT_EQ(cell->combInfo.is_shared, arch.combInfo.is_shared);
                EXPECT_EQ(cell->combInfo.is_extended, arch.combInfo.is_extended);
                EXPECT_EQ(cell->combInfo.carry_start, arch.combInfo.carry_start);
                EXPECT_EQ(cell->combInfo.carry_end, arch.combInfo.carry_end);
                EXPECT_EQ(cell->combInfo.mlab_group, arch.combInfo.mlab_group);
                if (cell->type == id_MISTRAL_MLAB) {
                    EXPECT_EQ(cell->combInfo.wclk, arch.combInfo.wclk);
                    EXPECT_EQ(cell->combInfo.we, arch.combInfo.we);
                }
            } else if (cell->type == id_MISTRAL_FF) {
                EXPECT_EQ(cell->ffInfo.ctrlset, arch.ffInfo.ctrlset);
                EXPECT_EQ(cell->ffInfo.datain, arch.ffInfo.datain);
                EXPECT_EQ(cell->ffInfo.sdata, arch.ffInfo.sdata);
            }
        }
        for (const auto &entry : nets) {
            auto *net = ctx->nets.at(entry.first).get();
            const auto &old = entry.second;
            EXPECT_EQ(net, old.identity);
            EXPECT_EQ(net->attrs, old.attrs);EXPECT_EQ(net->aliases,old.aliases);
            std::map<WireId,std::pair<PipId,PlaceStrength>> current;
            for(const auto &wire:net->wires) {
                current[wire.first]={wire.second.pip,wire.second.strength};
                EXPECT_EQ(ctx->getBoundWireNet(wire.first),net);
                if(wire.second.pip!=PipId()) { EXPECT_EQ(ctx->getBoundPipNet(wire.second.pip),net); }
            }
            if(!changed_routes.count(net)) { EXPECT_EQ(current,old.wires); }
            EXPECT_EQ(net->driver.cell, old.driver.cell);
            EXPECT_EQ(net->driver.port, old.driver.port);
            ASSERT_EQ(net->users.entries(), old.users.entries());
            auto stored_users = old.users;
            for (auto user : stored_users.enumerate()) {
                ASSERT_TRUE(net->users.count(user.index));
                EXPECT_EQ(net->users.at(user.index).cell, user.value.cell);
                EXPECT_EQ(net->users.at(user.index).port, user.value.port);
            }
            auto actual = net->users, expected = old.users;
            ASSERT_EQ(actual.capacity(), expected.capacity());
            // Include holes/free-list order, not only active user identities.
            for (int i = 0; i < old.users.capacity() + 8; ++i)
                EXPECT_EQ(actual.add(PortRef{}), expected.add(PortRef{}));
        }
        ASSERT_EQ(ctx->labs.size(), labs.size());
        for (size_t lab = 0; lab < labs.size(); ++lab) {
            const auto &now = ctx->labs[lab], &old = labs[lab];
            EXPECT_EQ(now.is_mlab, old.is_mlab);
            EXPECT_EQ(now.clk_wires, old.clk_wires);
            EXPECT_EQ(now.ena_wires, old.ena_wires);
            EXPECT_EQ(now.aclr_wires, old.aclr_wires);
            EXPECT_EQ(now.sclr_wire, old.sclr_wire);
            EXPECT_EQ(now.sload_wire, old.sload_wire);
            EXPECT_EQ(now.aclr_used, old.aclr_used);
            for (size_t alm = 0; alm < old.alms.size(); ++alm) {
                const auto &a = now.alms[alm], &b = old.alms[alm];
                EXPECT_EQ(a.comb_out, b.comb_out);
                EXPECT_EQ(a.sel_clk, b.sel_clk);
                EXPECT_EQ(a.sel_ena, b.sel_ena);
                EXPECT_EQ(a.sel_aclr, b.sel_aclr);
                EXPECT_EQ(a.sel_ef, b.sel_ef);
                EXPECT_EQ(a.ff_in, b.ff_in);
                EXPECT_EQ(a.ff_out, b.ff_out);
                EXPECT_EQ(a.lut_bels, b.lut_bels);
                EXPECT_EQ(a.ff_bels, b.ff_bels);
                EXPECT_EQ(a.l6_mode, b.l6_mode);
                EXPECT_EQ(a.carry_mode, b.carry_mode);
                EXPECT_EQ(a.clk_ena_idx, b.clk_ena_idx);
                EXPECT_EQ(a.aclr_idx, b.aclr_idx);
                EXPECT_EQ(a.unique_input_count, b.unique_input_count);
            }
        }
    }
};

const std::array<unsigned,64> mlab_order{
    0,1,4,5,8,9,12,13,29,28,25,24,21,20,17,16,
    2,3,6,7,10,11,14,15,31,30,27,26,23,22,19,18,
    32,33,36,37,40,41,44,45,61,60,57,56,53,52,49,48,
    34,35,38,39,42,43,46,47,63,62,59,58,55,54,51,50};
uint64_t emitted_mask(Context *ctx,const CV &decoded,CellInfo *cell)
{
    const auto &loc=ctx->bel_data(cell->bel).lab_data;CV::bmux_setting_t s{};
    if(!decoded.bmux_get(ctx->labs[loc.lab].is_mlab?CV::MLAB:CV::LAB,cell->bel.pos,CV::LUT_MASK,loc.alm,s)||s.type!=CV::MT_RAM||s.r.size()!=8)throw std::runtime_error("missing emitted LUT mask");
    uint64_t mask=0;for(unsigned i=0;i<8;++i)mask|=uint64_t(s.r[i])<<(8*i);return mask;
}
unsigned physical_address_bit(Context *ctx,CellInfo *cell,IdString logical)
{
    int half=ctx->bel_data(cell->bel).lab_data.idx;const auto &map=cell->pin_data.at(logical).bel_pins;
    if (map.size() != 1)
        throw std::runtime_error("invalid emitted pin map");
    auto p = map.front();
    if (p == id_A) return 0;
    if (p == id_B) return 1;
    if (p == (half ? id_D : id_C)) return 2;
    if (p == (half ? id_E1 : id_E0)) return 3;
    if (p == (half ? id_F1 : id_F0)) return 4;
    throw std::runtime_error("invalid physical half pin");
}
unsigned emitted_address(Context *ctx,CellInfo *cell,unsigned address)
{
    const auto &loc=ctx->bel_data(cell->bel).lab_data;unsigned row=address+32*loc.idx;return ctx->labs[loc.lab].is_mlab?mlab_order.at(row):row;
}
void check_emitted_truth(Context *ctx,const CV &decoded,CellInfo *cell)
{
    const auto &loc=ctx->bel_data(cell->bel).lab_data;const auto block=ctx->labs[loc.lab].is_mlab?CV::MLAB:CV::LAB;
    CV::bmux_setting_t top,bottom;ASSERT_TRUE(decoded.bmux_get(block,cell->bel.pos,CV::TMODE,loc.alm,top));ASSERT_TRUE(decoded.bmux_get(block,cell->bel.pos,CV::BMODE,loc.alm,bottom));EXPECT_EQ(top.s,uint32_t(CV::E_0));EXPECT_EQ(bottom.s,uint32_t(CV::E_1));
    const auto mask=emitted_mask(ctx,decoded,cell);std::set<unsigned> used;
    for(int i=0;i<cell->combInfo.lut_input_count;++i)ASSERT_TRUE(used.insert(physical_address_bit(ctx,cell,pins[i])).second);
    for(unsigned addr=0;addr<32;++addr){unsigned logical=0;for(int i=0;i<cell->combInfo.lut_input_count;++i){bool v=((addr>>physical_address_bit(ctx,cell,pins[i]))&1)!= (cell->get_pin_state(pins[i])!=PIN_INV);logical|=unsigned(v)<<i;}
        EXPECT_EQ((mask>>emitted_address(ctx,cell,addr))&1,1^((uint64_t(cell->params.at(id_LUT).as_int64())>>logical)&1))<<ctx->nameOf(cell)<<":"<<addr;
    }
}

std::unique_ptr<CV> decoded(Context *ctx)
{
    std::vector<uint8_t> bytes;ctx->cyclonev->rbf_save(bytes);if(bytes.empty())throw std::runtime_error("empty native RBF");
    auto result=std::unique_ptr<CV>(CV::get_model("5CSEBA6U23I7"));if(!result)throw std::runtime_error("native model missing");
    result->rbf_load(bytes.data(),uint32_t(bytes.size()));return result;
}

class FinalRouteBuffer : public ::testing::TestWithParam<std::tuple<bool,int>> {
  protected:
    std::unique_ptr<Context> ctx;
    uint32_t helper_lab=0,source_lab=0,sink_lab=0;
    NetInfo *input=nullptr,*clock=nullptr;
    std::vector<NetInfo*> signals;
    std::vector<CellInfo*> helpers,ffs,mates;
    std::string reason;
    int half() const {return std::get<1>(GetParam());}
    CellInfo *lut(const std::string &name,int width,const std::vector<NetInfo*> &nets,BelId bel)
    {
        auto *c=ctx->createCell(ctx->id(name),width==5?id_MISTRAL_ALUT5:id_MISTRAL_ALUT2);
        c->params[id_LUT]=Property(width==5?int64_t(0x96696996):int64_t(6),1u<<width);
        for(int i=0;i<width;++i){c->addInput(pins[i]);if(i<int(nets.size()))c->connectPort(pins[i],nets[i]);else c->pin_data[pins[i]].state=PIN_0;}
        c->addOutput(id_Q);c->connectPort(id_Q,ctx->createNet(ctx->id(name+"$q")));
        ctx->assign_comb_info(c);ctx->assign_default_pinmap(c);ctx->bindBel(bel,c,STRENGTH_WEAK);return c;
    }
    CellInfo *ff(const std::string &name,NetInfo *data,BelId bel,bool controls=false,bool synchronous_clear=false)
    {
        auto *c=ctx->createCell(ctx->id(name),id_MISTRAL_FF);
        for(auto p:{id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN})c->addInput(p);
        c->connectPort(id_CLK,clock);c->connectPort(id_DATAIN,data);
        c->pin_data[id_ENA].state=PIN_1;c->pin_data[id_ACLR].state=PIN_1;
        c->pin_data[id_SCLR].state=PIN_0;c->pin_data[id_SLOAD].state=PIN_0;
        if(controls){c->connectPort(id_ENA,signals[5]);c->pin_data[id_ENA].state=PIN_INV;}
        if(synchronous_clear){c->connectPort(id_SCLR,signals[4]);c->pin_data[id_SCLR].state=PIN_SIG;}
        c->addOutput(id_Q);c->connectPort(id_Q,ctx->createNet(ctx->id(name+"$q")));
        ctx->assign_ff_info(c);ctx->assign_default_pinmap(c);ctx->bindBel(bel,c,STRENGTH_WEAK);return c;
    }
    void SetUp() override
    {
        ArchArgs args;args.device="5CSEBA6U23I7";ctx=std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")]=1e8;
        ctx->createNet(ctx->id("$PACKER_GND_NET"));ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        std::vector<std::pair<int,uint32_t>> places;
        for(uint32_t i=0;i<ctx->labs.size();++i)if(ctx->labs[i].is_mlab==std::get<0>(GetParam())) {
            auto p=ctx->getBelLocation(ctx->labs[i].alms[0].lut_bels[0]);places.emplace_back(std::abs(p.x-24)+std::abs(p.y-3),i);
        }
        std::sort(places.begin(),places.end());ASSERT_GE(places.size(),3u);
        helper_lab=places[0].second;source_lab=places[1].second;sink_lab=places[2].second;
        clock=lut("clock_source",2,{},ctx->labs[source_lab].alms[9].lut_bels[0])->getPort(id_Q);
        clock->clkconstr=std::make_unique<ClockConstraint>();clock->clkconstr->period=DelayPair(10000);
        clock->clkconstr->high=clock->clkconstr->low=DelayPair(5000);
        for(int i=0;i<6;++i) {
            auto *d=lut("source_logic"+std::to_string(i),2,{},ctx->labs[source_lab].alms[i].lut_bels[0]);
            signals.push_back(ff("source_register"+std::to_string(i),d->getPort(id_Q),ctx->labs[source_lab].alms[i].ff_bels[0])->getPort(id_Q));
        }
        input=signals[0];
        for(int i=0;i<2;++i) {
            auto &alm=ctx->labs[helper_lab].alms[i];
            mates.push_back(lut("mate"+std::to_string(i),5,{signals[1],signals[2],signals[3],signals[4],signals[5]},alm.lut_bels[1-half()]));
            // All four helper-LAB registers share one dynamic ENA. Mixing the
            // default VCC enable with this fabric CLK and SCLR exhausts DINs.
            ff("mate_register"+std::to_string(i),mates.back()->getPort(id_Q),alm.ff_bels[2*(1-half())],true);
            ffs.push_back(ff("capture"+std::to_string(i),input,alm.ff_bels[2*half()],true,i==1));
        }
        auto *ordinary=lut("ordinary_user",2,{input,signals[1]},ctx->labs[sink_lab].alms[0].lut_bels[0]);
        ff("ordinary_capture",ordinary->getPort(id_Q),ctx->labs[sink_lab].alms[0].ff_bels[0]);
        auto alias=ctx->id("input_alias");ctx->net_aliases[alias]=input->name;input->aliases.push_back(alias);
        for(auto *n:signals){auto a=n->users.add(PortRef{}),b=n->users.add(PortRef{}),c=n->users.add(PortRef{});n->users.remove(b);n->users.remove(a);n->users.remove(c);}
        ctx->assignArchInfo();ctx->lab_pre_route();
        for(auto *f:ffs){auto *b=f->getPort(id_DATAIN)->driver.cell;ASSERT_NE(b,nullptr);ASSERT_EQ(b->type,id_MISTRAL_BUF);helpers.push_back(b);}
        ASSERT_EQ(helpers.size(),2u);ASSERT_NE(helpers[0],helpers[1]);
        // Rename the genuine inserted helpers. Eligibility must use the native
        // identities, never the packer's spelling or $ROUTETHRU suffix.
        for(size_t i=0;i<helpers.size();++i) {
            auto *b=helpers[i];auto owned=std::move(ctx->cells.at(b->name));ctx->cells.erase(b->name);
            b->name=ctx->id("transparent_native_"+std::to_string(i));ctx->cells[b->name]=std::move(owned);
        }
        for(const auto &c:ctx->cells)ASSERT_TRUE(ctx->isBelLocationValid(c.second->bel))<<ctx->nameOf(c.second.get());
        int visits=5000000;ASSERT_TRUE(critical_local_route_net(ctx.get(),clock,visits));
        for(auto *f:ffs)for(auto p:{id_ENA,id_SCLR})if(auto *n=f->getPort(p)) {
            ASSERT_TRUE(critical_local_route_net(ctx.get(),n,visits));
        }
        for(const auto &entry:ctx->nets)if(entry.second->driver.cell) {
            ASSERT_TRUE(critical_local_route_net(ctx.get(),entry.second.get(),visits))<<ctx->nameOf(entry.second.get());
        }
        ctx->attrs[id_step]=std::string("route");ctx->configure_bitstream();ctx->check();
        ASSERT_TRUE(ctx->analogue_cache_valid);const auto cache=Snapshot::cache(ctx.get());ASSERT_FALSE(cache.empty());
        bool characterized=false;for(const auto &entry:cache)characterized|=std::get<0>(entry.second);ASSERT_TRUE(characterized);
        ASSERT_TRUE(critical_final_route_buffer_users(ctx.get(),input,&reason))<<reason;
    }
    GpuRouteTree current(NetInfo *net)
    {GpuRouteTree result;for(const auto &w:net->wires)result.wires.emplace_back(w.first,w.second.pip);return result;}
    GpuRouteTree changed_tree()
    {
        const auto baseline=Snapshot(ctx.get());auto state=critical_final_route_buffer_snapshot(ctx.get(),input,&reason);
        if(!state)throw std::runtime_error(reason);
        auto original=current(input);unsigned checked=0;
        // A bounded free extra branch exercises an actual physical change; no
        // timing improvement is inferred from this transaction-only fixture.
        for(const auto &wire:original.wires)for(auto pip:ctx->getPipsDownhill(wire.first)) {
            if(!ctx->checkPipAvailForNet(pip,input)||!ctx->checkWireAvail(ctx->getPipDstWire(pip)))continue;
            if(++checked>32)throw std::runtime_error("fixture free-branch search exhausted");
            auto candidate=original;candidate.wires.emplace_back(ctx->getPipDstWire(pip),pip);bool usable=false;
            if(critical_route_trial(ctx.get(),input,candidate,[&]{usable=critical_final_route_buffer_unchanged(ctx.get(),*state,&reason);return false;}))
                throw std::runtime_error("false measurement callback retained a tree");
            baseline.expect_exact(ctx.get());
            if(usable)return candidate;
        }
        throw std::runtime_error("fixture has no preserving free route branch");
    }
    void no_mutation_reject()
    {
        const Snapshot before(ctx.get());const auto ids=ctx->idstring_idx_to_str->size();
        EXPECT_FALSE(critical_final_route_buffer_users(ctx.get(),input,&reason));
        EXPECT_FALSE(critical_final_route_buffer_snapshot(ctx.get(),input,&reason));
        EXPECT_EQ(ctx->idstring_idx_to_str->size(),ids);before.expect_exact(ctx.get());
    }
};

TEST_P(FinalRouteBuffer, RealRenamedHelpersHaveTransparentEmittedFunctionAndReadOnlyAdmission)
{
    const Snapshot before(ctx.get());const auto ids=ctx->idstring_idx_to_str->size();
    auto state=critical_final_route_buffer_snapshot(ctx.get(),input,&reason);ASSERT_TRUE(state)<<reason;
    EXPECT_TRUE(critical_final_route_buffer_unchanged(ctx.get(),*state,&reason))<<reason;
    EXPECT_EQ(ctx->idstring_idx_to_str->size(),ids);before.expect_exact(ctx.get());
    auto native=decoded(ctx.get());
    for(size_t i=0;i<helpers.size();++i) {
        auto *b=helpers[i];const unsigned bit=physical_address_bit(ctx.get(),b,id_A);const auto mask=emitted_mask(ctx.get(),*native,b);
        for(unsigned physical=0;physical<32;++physical) {
            const bool raw=!bool((physical>>bit)&1);
            EXPECT_EQ(bool(1^((mask>>emitted_address(ctx.get(),b,physical))&1)),raw);
        }
        check_emitted_truth(ctx.get(),*native,mates[i]);
        const auto &where=ctx->bel_data(ffs[i]->bel).lab_data;
        const std::array<CV::bmux_type_t,4> pkreg{CV::TPKREG1,CV::TPKREG0,CV::BPKREG1,CV::BPKREG0};
        CV::bmux_setting_t setting{};ASSERT_TRUE(native->bmux_get(ctx->labs[where.lab].is_mlab?CV::MLAB:CV::LAB,ffs[i]->bel.pos,pkreg[where.idx],where.alm,setting));
        EXPECT_EQ(setting.s,0u);
    }
    for(bool skew:{false,true}) {
        TimingAnalyser t(ctx.get());t.with_clock_skew=skew;t.setup(false,false,true);
        EXPECT_TRUE(critical_final_route_source_coverage(ctx.get(),t,input,&reason))<<reason;
        EXPECT_TRUE(critical_lut6_swap_output_coverage(ctx.get(),t,input,&reason))<<reason;
    }
}

TEST_P(FinalRouteBuffer, ChangedInputTreePreservesHelpersAndFalseOrExceptionRestoresConfiguredCache)
{
    const auto candidate=changed_tree();const Snapshot before(ctx.get());auto state=critical_final_route_buffer_snapshot(ctx.get(),input,&reason);ASSERT_TRUE(state);
    bool called=false;
    EXPECT_FALSE(critical_route_trial(ctx.get(),input,candidate,[&]{called=true;EXPECT_TRUE(critical_final_route_buffer_unchanged(ctx.get(),*state,&reason))<<reason;return false;}));
    EXPECT_TRUE(called);before.expect_exact(ctx.get());
    EXPECT_THROW(critical_route_trial(ctx.get(),input,candidate,[&]()->bool {
        EXPECT_TRUE(critical_final_route_buffer_unchanged(ctx.get(),*state,&reason))<<reason;
        throw std::runtime_error("injected after configured helper inspection");
    }),std::runtime_error);
    before.expect_exact(ctx.get());EXPECT_TRUE(critical_final_route_buffer_unchanged(ctx.get(),*state,&reason));
    // Physical fault injection after candidate configuration: the new guard
    // itself rejects a selector change. The existing transaction must restore
    // the original configured RBF, private Q trees and characterized cache on
    // both false and exception paths; no logical state is mutated here.
    const auto &capture=ctx->bel_data(ffs[0]->bel).lab_data;
    const std::array<CV::bmux_type_t,4> pkreg{CV::TPKREG1,CV::TPKREG0,CV::BPKREG1,CV::BPKREG0};
    const auto block=ctx->labs[capture.lab].is_mlab?CV::MLAB:CV::LAB;
    auto corrupt_selector=[&] {
        EXPECT_TRUE(critical_final_route_buffer_unchanged(ctx.get(),*state,&reason))<<reason;
        EXPECT_TRUE(ctx->cyclonev->bmux_b_set(block,ffs[0]->bel.pos,pkreg[capture.idx],capture.alm,true));
        EXPECT_FALSE(critical_final_route_buffer_unchanged(ctx.get(),*state,&reason));
        EXPECT_NE(reason.find("selector"),std::string::npos)<<reason;
    };
    called=false;
    EXPECT_FALSE(critical_route_trial(ctx.get(),input,candidate,[&] {
        called=true;corrupt_selector();return false;
    }));
    EXPECT_TRUE(called);before.expect_exact(ctx.get());
    EXPECT_TRUE(critical_final_route_buffer_unchanged(ctx.get(),*state,&reason))<<reason;
    called=false;
    EXPECT_THROW(critical_route_trial(ctx.get(),input,candidate,[&]()->bool {
        called=true;corrupt_selector();throw std::runtime_error("injected configured selector fault");
    }),std::runtime_error);
    EXPECT_TRUE(called);before.expect_exact(ctx.get());
    EXPECT_TRUE(critical_final_route_buffer_unchanged(ctx.get(),*state,&reason))<<reason;
    auto invalid=candidate;invalid.wires.clear();EXPECT_FALSE(critical_route_trial(ctx.get(),input,invalid,[]{return true;}));before.expect_exact(ctx.get());
    EXPECT_TRUE(critical_route_trial(ctx.get(),input,candidate,[&]{return critical_final_route_buffer_unchanged(ctx.get(),*state,&reason);}));
    before.expect_exact(ctx.get(),{input});EXPECT_TRUE(critical_final_route_buffer_unchanged(ctx.get(),*state,&reason));
    auto original=GpuRouteTree{};for(const auto &w:before.nets.at(input->name).wires)original.wires.emplace_back(w.first,w.second.first);
    EXPECT_TRUE(critical_route_trial(ctx.get(),input,original,[]{return true;}));before.expect_exact(ctx.get());
}

TEST_P(FinalRouteBuffer, MalformedPinsControlsProtectionAndHiddenObserversRejectWithoutMutation)
{
    auto *b=helpers[0],*f=ffs[0];auto *q=b->getPort(id_Q);
    auto state=b->pin_data.at(id_A).state;b->pin_data.at(id_A).state=PIN_INV;no_mutation_reject();b->pin_data.at(id_A).state=state;
    auto pin=b->pin_data.at(id_A).bel_pins;b->pin_data.at(id_A).bel_pins={id_A};no_mutation_reject();b->pin_data.at(id_A).bel_pins=pin;
    auto controls=f->ffInfo.ctrlset;f->ffInfo.ctrlset.ena.inverted=!controls.ena.inverted;no_mutation_reject();f->ffInfo.ctrlset=controls;
    f->pin_data[id_SLOAD].state=PIN_1;no_mutation_reject();f->pin_data[id_SLOAD].state=PIN_0;
    f->ports.at(id_SDATA).net=signals[1];no_mutation_reject();f->ports.at(id_SDATA).net=nullptr;
    const auto keep=ctx->id("keep");q->attrs[keep]=1;no_mutation_reject();q->attrs.erase(keep);
    auto strength=b->belStrength;b->belStrength=STRENGTH_LOCKED;no_mutation_reject();b->belStrength=strength;
    auto *hidden=ctx->createCell(ctx->id("hidden_observer"),id_MISTRAL_IO);hidden->addInput(id_I);
    for(auto *n:{q,input}) {hidden->ports.at(id_I).net=n;no_mutation_reject();hidden->ports.at(id_I).net=nullptr;}
    ctx->cells.erase(hidden->name);
    auto old=q->driver;q->driver={f,id_Q};no_mutation_reject();q->driver=old;
    EXPECT_TRUE(critical_final_route_buffer_users(ctx.get(),input,&reason))<<reason;
}

TEST_P(FinalRouteBuffer, LogicalAndConfiguredCorruptionAreDetectedWithoutClaimingLogicalRollback)
{
    auto state=critical_final_route_buffer_snapshot(ctx.get(),input,&reason);ASSERT_TRUE(state);const Snapshot before(ctx.get());
    // These deliberately malformed logical mutations are detection-only. The
    // route transaction does not promise to restore arbitrary cell changes.
    auto p=mates[0]->pin_data.at(id_A);mates[0]->pin_data.at(id_A).state=PIN_INV;
    EXPECT_FALSE(critical_final_route_buffer_unchanged(ctx.get(),*state,&reason));mates[0]->pin_data.at(id_A)=p;
    ++ctx->labs[helper_lab].alms[0].unique_input_count;EXPECT_FALSE(critical_final_route_buffer_unchanged(ctx.get(),*state,&reason));--ctx->labs[helper_lab].alms[0].unique_input_count;
    auto *q=helpers[0]->getPort(id_Q);auto w=q->wires.begin();auto oldstrength=w->second.strength;
    w->second.strength=oldstrength==STRENGTH_WEAK?STRENGTH_STRONG:STRENGTH_WEAK;
    EXPECT_FALSE(critical_final_route_buffer_unchanged(ctx.get(),*state,&reason));w->second.strength=oldstrength;
    const auto &f=ctx->bel_data(ffs[0]->bel).lab_data;
    const std::array<CV::bmux_type_t,4> pkreg{CV::TPKREG1,CV::TPKREG0,CV::BPKREG1,CV::BPKREG0};
    const auto block=ctx->labs[f.lab].is_mlab?CV::MLAB:CV::LAB;
    ASSERT_TRUE(ctx->cyclonev->bmux_b_set(block,ffs[0]->bel.pos,pkreg[f.idx],f.alm,true));
    const Snapshot malformed(ctx.get());EXPECT_FALSE(critical_final_route_buffer_unchanged(ctx.get(),*state,&reason));malformed.expect_exact(ctx.get());
    // Explicit test restoration; the predicate must not normalize a bad mux.
    ASSERT_TRUE(ctx->cyclonev->bmux_b_set(block,ffs[0]->bel.pos,pkreg[f.idx],f.alm,false));
    before.expect_exact(ctx.get());EXPECT_TRUE(critical_final_route_buffer_unchanged(ctx.get(),*state,&reason));
}

INSTANTIATE_TEST_SUITE_P(BothTilesAndHalves,FinalRouteBuffer,::testing::Combine(::testing::Bool(),::testing::Values(0,1)));
} // namespace
