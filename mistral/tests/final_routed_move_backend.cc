/* Independent native boundary for final ordinary-LUT moves. SPDX-License-Identifier: ISC
 * OUT-only fixture draft. It uses full native STA for coverage only, no GPU or full router.
 * The existing complete transaction snapshot is reused; emitted truth and static
 * native configuration checks below are independent of the production wrapper.
 */
#include "analogue_repair.h"
#include "cyclonev.h"
#include "gtest/gtest.h"
#include "nextpnr.h"
#include "timing.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
USING_NEXTPNR_NAMESPACE
namespace {
using CV=mistral::CycloneV;
const std::array<IdString,6> pins{id_A,id_B,id_C,id_D,id_E,id_F};
struct Snapshot {
    struct Cell {
        CellInfo *identity;
        IdString type;
        BelId bel;
        PlaceStrength strength;
        ClusterId cluster;
        Region *region; PseudoCell *pseudo; IdString hierpath;
        decltype(CellInfo::attrs) attrs;
        decltype(CellInfo::params) params;
        decltype(CellInfo::ports) ports;
        ArchCellInfo architecture;
    };
    struct Net {
        NetInfo *identity;
        Region *region; IdString constant,hierpath; bool global; ClockConstraint *clock;
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
    decltype(Context::settings) settings; size_t id_count,cache_size; bool configured,cache_valid;
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
    explicit Snapshot(Context *ctx) : labs(ctx->labs), aliases(ctx->net_aliases), attrs(ctx->attrs), settings(ctx->settings), id_count(ctx->idstring_idx_to_str->size()), cache_size(ctx->analogue_arc_cache.size()), configured(ctx->bitstream_configured), cache_valid(ctx->analogue_cache_valid), arcs(cache(ctx))
    {
        ctx->cyclonev->rbf_save(rbf);
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            cell_order.push_back(entry.first);
            cells.emplace(entry.first, Cell{cell, cell->type, cell->bel, cell->belStrength, cell->cluster, cell->region, cell->pseudo_cell.get(), cell->hierpath, cell->attrs, cell->params,
                                            cell->ports, *static_cast<ArchCellInfo *>(cell)});
        }
        for (const auto &entry : ctx->nets) {
            auto *net = entry.second.get();
            net_order.push_back(entry.first);
            Net saved{net, net->region,net->constant_value,net->hierpath,net->is_global,net->clkconstr.get(),net->driver, net->users, net->attrs, net->aliases, {}};
            for (const auto &wire : net->wires) saved.wires[wire.first]={wire.second.pip,wire.second.strength};
            nets.emplace(entry.first, std::move(saved));
        }
    }
    void expect_exact(Context *ctx, const std::set<NetInfo *> &changed_routes = {}) const
    {
        EXPECT_EQ(ctx->net_aliases, aliases);
        EXPECT_EQ(ctx->attrs, attrs);
        EXPECT_EQ(ctx->settings,settings);EXPECT_EQ(ctx->idstring_idx_to_str->size(),id_count);
        EXPECT_EQ(ctx->bitstream_configured,configured);EXPECT_EQ(ctx->analogue_cache_valid,cache_valid);
        EXPECT_EQ(ctx->analogue_arc_cache.size(),cache_size);
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
            EXPECT_EQ(cell->region,old.region);EXPECT_EQ(cell->pseudo_cell.get(),old.pseudo);EXPECT_EQ(cell->hierpath,old.hierpath);
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
                ASSERT_EQ(cell->combInfo.lut_input_count,arch.combInfo.lut_input_count);
                ASSERT_GE(cell->combInfo.lut_input_count,0);ASSERT_LE(size_t(cell->combInfo.lut_input_count),cell->combInfo.lut_in.size());
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
            EXPECT_EQ(net->region,old.region);EXPECT_EQ(net->constant_value,old.constant);EXPECT_EQ(net->hierpath,old.hierpath);
            EXPECT_EQ(net->is_global,old.global);EXPECT_EQ(net->clkconstr.get(),old.clock);
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

class FinalRoutedMove : public ::testing::TestWithParam<std::tuple<bool,int>> {
 protected:
    std::unique_ptr<Context> ctx;
    uint32_t lab=0,source_lab=0;
    CellInfo *moved=nullptr,*source_mate=nullptr,*target_mate=nullptr;
    CellInfo *source_ff=nullptr,*target_ff=nullptr;
    std::vector<NetInfo *> inputs;
    std::vector<CellInfo *> stationary;
    std::string reason;
    int half()const{return std::get<1>(GetParam());}
    BelId home()const{return ctx->labs[lab].alms[0].lut_bels[half()];}
    BelId target()const{return ctx->labs[lab].alms[8].lut_bels[half()];}
    CellInfo *lut(const std::string &name,int width,uint64_t mask,const std::vector<NetInfo *> &nets,BelId bel) {
        const IdString types[]{id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,id_MISTRAL_ALUT4,id_MISTRAL_ALUT5,id_MISTRAL_ALUT6};
        auto *c=ctx->createCell(ctx->id(name),types[width-2]);c->params[id_LUT]=Property(int64_t(mask),1u<<width);
        for(int i=0;i<width;++i){c->addInput(pins[i]);if(i<int(nets.size())&&nets[i]){c->connectPort(pins[i],nets[i]);c->pin_data[pins[i]].state=PIN_SIG;}else c->pin_data[pins[i]].state=PIN_0;}
        c->addOutput(id_Q);c->connectPort(id_Q,ctx->createNet(ctx->id(name+"$q")));
        ctx->assign_comb_info(c);ctx->assign_default_pinmap(c);ctx->bindBel(bel,c,STRENGTH_WEAK);return c;
    }
    CellInfo *ff(const std::string &name,CellInfo *local,BelId bel,bool controls=false) {
        auto *f=ctx->createCell(ctx->id(name),id_MISTRAL_FF);
        for(auto p:{id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN})f->addInput(p);
        f->connectPort(id_CLK,inputs[9]);f->connectPort(id_DATAIN,local->getPort(id_Q));
        f->pin_data[id_ENA].state=PIN_1;f->pin_data[id_ACLR].state=PIN_1;
        f->pin_data[id_SCLR].state=PIN_0;f->pin_data[id_SLOAD].state=PIN_0;
        if(controls){f->connectPort(id_ENA,inputs[7]);f->connectPort(id_ACLR,inputs[8]);f->pin_data[id_ENA].state=PIN_INV;f->pin_data[id_ACLR].state=PIN_INV;}
        f->addOutput(id_Q);f->connectPort(id_Q,ctx->createNet(ctx->id(name+"$q")));
        ctx->assign_ff_info(f);ctx->assign_default_pinmap(f);ctx->bindBel(bel,f,STRENGTH_WEAK);return f;
    }
    void build(bool moved_local_ff=false, bool empty_source=false, bool empty_target=false, bool ff_observer=false) {
        inputs.clear();stationary.clear();reason.clear();source_mate=target_mate=source_ff=target_ff=nullptr;
        ArchArgs a;a.device="5CSEBA6U23I7";ctx=std::make_unique<Context>(a);ctx->settings[ctx->id("target_freq")]=1e8;
        ctx->createNet(ctx->id("$PACKER_GND_NET"));ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        std::vector<std::pair<int,uint32_t>> sites;
        for(uint32_t i=0;i<ctx->labs.size();++i)if(ctx->labs[i].is_mlab==std::get<0>(GetParam())) {
            auto l=ctx->getBelLocation(ctx->labs[i].alms[0].lut_bels[0]);sites.emplace_back(std::abs(l.x-24)+std::abs(l.y-3),i);
        }
        std::sort(sites.begin(),sites.end());ASSERT_GE(sites.size(),2u);lab=sites[0].second;source_lab=sites[1].second;
        std::vector<CellInfo *> stimuli;
        for(int i=0;i<10;++i){auto *c=lut("stimulus"+std::to_string(i),2,8,{},ctx->labs[source_lab].alms[i].lut_bels[0]);stimuli.push_back(c);inputs.push_back(c->getPort(id_Q));}
        inputs[9]->clkconstr=std::make_unique<ClockConstraint>();inputs[9]->clkconstr->period=DelayPair(10000);inputs[9]->clkconstr->high=inputs[9]->clkconstr->low=DelayPair(5000);
        for(int i=0;i<7;++i)inputs[i]=ff("a0"+std::to_string(i),stimuli[i],ctx->labs[source_lab].alms[i].ff_bels[0])->getPort(id_Q);
        moved=lut("zz_moved",3,0xd2,{inputs[0],inputs[1],inputs[2]},home());moved->pin_data[id_B].state=PIN_INV;
        if(!empty_source) {
            source_mate=lut("source_mate",3,0x8e,{inputs[3],inputs[4],inputs[5]},ctx->labs[lab].alms[0].lut_bels[1-half()]);
            source_mate->pin_data[id_C].state=PIN_INV;
            source_ff=ff("source_mate_ff",source_mate,ctx->labs[lab].alms[0].ff_bels[2*(1-half())],true);
        }
        if(!empty_target) {
            target_mate=lut("target_mate",3,0x96,{inputs[3],inputs[4],inputs[5]},ctx->labs[lab].alms[8].lut_bels[1-half()]);
            target_mate->pin_data[id_A].state=PIN_INV;
            target_ff=ff("target_mate_ff",target_mate,ctx->labs[lab].alms[8].ff_bels[2*(1-half())],true);
        }
        if(moved_local_ff)ff("moved_local_ff",moved,ctx->labs[lab].alms[0].ff_bels[2*half()],true);
        for(int i=0;i<6;++i){auto *c=lut("stationary"+std::to_string(i),2,14,{inputs[0],inputs[6]},ctx->labs[lab].alms[i+1].lut_bels[0]);stationary.push_back(c);
            ff("stationary_capture"+std::to_string(i),c,ctx->labs[lab].alms[i+1].ff_bels[0],true);}
        if(ff_observer) ff("opposite_half_observer",stationary.front(),ctx->labs[lab].alms[0].ff_bels[2*(1-half())],true);
        auto *capture=lut("capture",2,8,{moved->getPort(id_Q),inputs[6]},ctx->labs[lab].alms[7].lut_bels[0]);
        ff("output_capture",capture,ctx->labs[lab].alms[7].ff_bels[0],true);
        lut("outside",2,6,{inputs[6],inputs[7]},ctx->labs[lab].alms[9].lut_bels[0]);
        auto alias=ctx->id("shared_input_alias");ctx->net_aliases[alias]=inputs[0]->name;inputs[0]->aliases.push_back(alias);
        for(auto *n:inputs){auto x=n->users.add(PortRef{}),y=n->users.add(PortRef{}),z=n->users.add(PortRef{});n->users.remove(y);n->users.remove(x);n->users.remove(z);}
        ctx->assignArchInfo();ctx->lab_pre_route();
        for(const auto &c:ctx->cells){ASSERT_TRUE(ctx->isBelLocationValid(c.second->bel))<<ctx->nameOf(c.second.get());}
        int visits=5000000;ASSERT_TRUE(critical_local_route_net(ctx.get(),inputs[9],visits));
        for(auto *net:{inputs[7],inputs[8]}){ASSERT_TRUE(critical_local_route_net(ctx.get(),net,visits));}
        for(const auto &n:ctx->nets)if(n.second->driver.cell){ASSERT_TRUE(critical_local_route_net(ctx.get(),n.second.get(),visits))<<ctx->nameOf(n.second.get());}
        for(const char *key:{"keep","dont_touch","BEL","FES_SLOT"})ctx->id(key);
        ctx->attrs[id_step]=std::string("route");ctx->configure_bitstream();ctx->check();
        ASSERT_TRUE(ctx->analogue_cache_valid);ASSERT_FALSE(Snapshot::cache(ctx.get()).empty());ASSERT_EQ(ctx->getBoundBelCell(target()),nullptr);
        ASSERT_EQ(inputs[0]->users.entries(),7);if(source_ff){ASSERT_EQ(source_ff->ffInfo.datain,source_mate->getPort(id_Q));}if(target_ff){ASSERT_EQ(target_ff->ffInfo.datain,target_mate->getPort(id_Q));}
    }
    void SetUp()override{build();}
    bool trial(const std::function<bool()> &accept,int budget=500000,BelId destination=BelId()) {
        reason.clear();return critical_final_routed_move_trial(ctx.get(),moved,destination==BelId()?target():destination,budget,accept,&reason);
    }
    std::set<NetInfo *> incident()const{return {inputs[0],inputs[1],inputs[2],moved->getPort(id_Q)};}
    using Tree=std::map<WireId,std::pair<PipId,PlaceStrength>>;
    Tree path(NetInfo *n,CellInfo *sink,IdString pin)const {
        Tree result;auto root=ctx->getNetinfoSourceWire(n);
        for(auto w:ctx->getNetinfoSinkWires(n,PortRef{sink,pin})) {
            std::set<WireId> seen;
            for(;;){if(!seen.insert(w).second||!n->wires.count(w))throw std::runtime_error("missing/cyclic sink tree");auto e=n->wires.at(w);result[w]={e.pip,e.strength};
                if(w==root){if(e.pip!=PipId())throw std::runtime_error("root has PIP");break;}
                if(e.pip==PipId()||ctx->getPipDstWire(e.pip)!=w)throw std::runtime_error("broken PIP");w=ctx->getPipSrcWire(e.pip);}
        }
        if(result.empty())throw std::runtime_error("no actual sink wires");return result;
    }
    void all_connected()const {for(auto *n:incident())for(const auto &u:n->users){EXPECT_FALSE(path(n,u.cell,u.port).empty());}}
    void coverage() {
        for(bool skew:{false,true}){TimingAnalyser timing(ctx.get());timing.with_clock_skew=skew;timing.setup(false,false,true);
            EXPECT_TRUE(critical_lut_permutation_source_coverage(ctx.get(),timing,moved,&reason))<<reason;
            for(auto *net:incident()){EXPECT_TRUE(critical_lut6_swap_output_coverage(ctx.get(),timing,net,&reason))<<ctx->nameOf(net)<<": "<<reason;}
        }
    }
    using ConfigKey=std::tuple<int,int,int,int>;
    using ConfigValue=std::tuple<int,bool,uint32_t,std::vector<uint8_t>>;
    using Config=std::map<ConfigKey,ConfigValue>;
    Config config(const CV &model)const {
        Config r;for(const auto &s:model.bmux_get())if(s.btype==CV::LAB||s.btype==CV::MLAB)
            r[{int(s.btype),s.pos.v,int(s.mux),s.midx}]={s.type,s.def,s.s,s.r};return r;
    }
    ConfigKey mask_key(BelId b)const {auto l=ctx->bel_data(b).lab_data;return {ctx->labs[l.lab].is_mlab?CV::MLAB:CV::LAB,b.pos.v,CV::LUT_MASK,l.alm};}
    std::unique_ptr<CV> decoded()const {
        std::vector<uint8_t> r;ctx->cyclonev->rbf_save(r);auto m=std::unique_ptr<CV>(CV::get_model("5CSEBA6U23I7"));
        if(!m||r.empty())throw std::runtime_error("empty RBF/model");m->rbf_load(r.data(),uint32_t(r.size()));return m;
    }
    uint64_t mask(const CV &m,BelId b)const {
        auto l=ctx->bel_data(b).lab_data;CV::bmux_setting_t s;
        if(!m.bmux_get(ctx->labs[l.lab].is_mlab?CV::MLAB:CV::LAB,b.pos,CV::LUT_MASK,l.alm,s)||s.type!=CV::MT_RAM||s.r.size()!=8)throw std::runtime_error("missing native LUT mask");
        uint64_t v=0;for(unsigned i=0;i<8;++i)v|=uint64_t(s.r[i])<<(8*i);return v;
    }
    unsigned emitted_index(BelId b,unsigned row)const {
        static const std::array<unsigned,64> mlab{{0,1,4,5,8,9,12,13,29,28,25,24,21,20,17,16,2,3,6,7,10,11,14,15,31,30,27,26,23,22,19,18,32,33,36,37,40,41,44,45,61,60,57,56,53,52,49,48,34,35,38,39,42,43,46,47,63,62,59,58,55,54,51,50}};
        return ctx->labs[ctx->bel_data(b).lab_data.lab].is_mlab?mlab.at(row):row;
    }
    unsigned address_bit(IdString pin)const {
        if(pin==id_A)return 0;if(pin==id_B)return 1;if(pin.in(id_C,id_D))return 2;
        if(pin.in(id_E0,id_E1))return 3;if(pin.in(id_F0,id_F1))return 4;throw std::runtime_error("unknown physical pin");
    }
    void truth(const CV &m,CellInfo *cell)const {
        auto l=ctx->bel_data(cell->bel).lab_data;auto bits=mask(m,cell->bel);uint64_t lut=uint64_t(cell->params.at(id_LUT).as_int64());
        std::vector<NetInfo *> nets;
        for(unsigned i=0;i<3;++i)if(std::find(nets.begin(),nets.end(),cell->getPort(pins[i]))==nets.end())nets.push_back(cell->getPort(pins[i]));
        ASSERT_EQ(nets.size(),3u);
        // Every native half address, including free A/B coordinates, is checked.
        for(unsigned row=0;row<32;++row){unsigned logical=0;
            for(unsigned i=0;i<3;++i){const auto &p=cell->pin_data.at(pins[i]);ASSERT_EQ(p.bel_pins.size(),1u);
                bool raw=!bool((row>>address_bit(p.bel_pins.front()))&1);logical|=unsigned(raw^(p.state==PIN_INV))<<i;}
            EXPECT_EQ((bits>>emitted_index(cell->bel,row+32*l.idx))&1,1u^((lut>>logical)&1))<<"physical row "<<row;
        }
        // Independent raw-net oracle: physical address is !raw for PIN_SIG and
        // PIN_INV alike. PIN_INV is already folded into the emitted LUT mask.
        for(unsigned raw=0;raw<8;++raw){unsigned row=0,logical=0;
            for(unsigned i=0;i<3;++i){const auto &p=cell->pin_data.at(pins[i]);bool value=(raw>>i)&1;
                row|=unsigned(!value)<<address_bit(p.bel_pins.front());logical|=unsigned(value^(p.state==PIN_INV))<<i;}
            EXPECT_EQ(1u^((bits>>emitted_index(cell->bel,row+32*l.idx))&1),(lut>>logical)&1)<<"raw row "<<raw;
        }
    }
    void flip(BelId b,unsigned row) {
        auto l=ctx->bel_data(b).lab_data;auto v=mask(*ctx->cyclonev,b)^(uint64_t(1)<<emitted_index(b,row));
        ASSERT_TRUE(ctx->cyclonev->bmux_r_set(ctx->labs[l.lab].is_mlab?CV::MLAB:CV::LAB,b.pos,CV::LUT_MASK,l.alm,v));
    }
    void restore_rbf(const Snapshot &s){ctx->cyclonev->rbf_load(s.rbf.data(),uint32_t(s.rbf.size()));}
    void restored(const Snapshot &s){s.expect_exact(ctx.get());EXPECT_EQ(moved->bel,home());EXPECT_EQ(ctx->getBoundBelCell(target()),nullptr);}
    Snapshot expected_after(const Snapshot &s)const {
        Snapshot e=s;e.cells.at(moved->name).bel=target();
        // Three distinct private inputs move; cohabiting FFs read only the mate.
        EXPECT_EQ(s.labs[lab].alms[0].unique_input_count,6);EXPECT_EQ(s.labs[lab].alms[8].unique_input_count,3);
        e.labs[lab].alms[0].unique_input_count=3;e.labs[lab].alms[8].unique_input_count=6;return e;
    }
    const Snapshot *accepted_before=nullptr;
    void stationary_masks_and_config(const CV &old,const CV &now,bool source_empty=false,bool target_empty=false)const {
        auto a=config(old),b=config(now);ASSERT_EQ(a.size(),b.size());
        for(auto bel:{home(),target()}){
            uint64_t stationary_bits=0;for(unsigned row=0;row<32;++row)stationary_bits|=uint64_t(1)<<emitted_index(bel,row+32*(1-half()));
            if(!(bel==home()?source_empty:target_empty)) EXPECT_EQ(mask(old,bel)&stationary_bits,mask(now,bel)&stationary_bits);
            if(bel==home()?source_empty:target_empty) {
                auto l=ctx->bel_data(bel).lab_data;auto block=ctx->labs[l.lab].is_mlab?CV::MLAB:CV::LAB;
                for(auto mux:{CV::TMODE,CV::BMODE}){ConfigKey k{block,bel.pos.v,mux,l.alm};a.erase(k);b.erase(k);}
            }
            a.erase(mask_key(bel));b.erase(mask_key(bel));
        }
        // Independently derive output selector values from actual decoded RBFs
        // and the pre/post routed output trees. Only selected-half Q outputs vary.
        const std::array<CV::bmux_type_t,6> muxes{CV::TDFF0,CV::TDFF1,CV::TDFF1L,CV::BDFF0,CV::BDFF1,CV::BDFF1L};
        const std::array<CV::port_type_t,6> ports{CV::FFT0,CV::FFT1,CV::FFT1L,CV::FFB0,CV::FFB1,CV::FFB1L};
        const auto &prior=accepted_before->nets.at(moved->getPort(id_Q)->name).wires;
        for(auto bel:{home(),target()})for(int i=3*half();i<3*(half()+1);++i) {
            auto l=ctx->bel_data(bel).lab_data;auto block=ctx->labs[l.lab].is_mlab?CV::MLAB:CV::LAB;
            auto wire=ctx->get_port(block,bel.pos.x(),bel.pos.y(),l.alm,ports[i]);
            bool was=prior.count(wire),is=moved->getPort(id_Q)->wires.count(wire);
            if(!was&&!is)continue;
            ConfigKey key{block,bel.pos.v,muxes[i],l.alm};
            EXPECT_EQ(a.at(key),(ConfigValue{CV::MT_MUX,!was,uint32_t(was?CV::NLUT:CV::REG),{}}));
            EXPECT_EQ(b.at(key),(ConfigValue{CV::MT_MUX,!is,uint32_t(is?CV::NLUT:CV::REG),{}}));
            a.erase(key);b.erase(key);
        }
        EXPECT_EQ(a,b); // PKREG, clock, stationary output selectors and modes exact.
    }
};

TEST_P(FinalRoutedMove, EmittedTruthMatesControlsAndStationaryTreesSurviveAcceptedMove)
{
    coverage();ASSERT_FALSE(HasFailure());
    Snapshot before(ctx.get());accepted_before=&before;auto old=decoded();for(auto *c:{moved,source_mate,target_mate})truth(*old,c);
    std::vector<Tree> paths;for(auto *s:stationary)paths.push_back(path(inputs[0],s,id_A));
    bool reached=false;ASSERT_TRUE(trial([&]{reached=true;return true;}))<<reason;ASSERT_TRUE(reached);
    EXPECT_EQ(ctx->getBoundBelCell(home()),nullptr);EXPECT_EQ(ctx->getBoundBelCell(target()),moved);
    auto now=decoded();for(auto *c:{moved,source_mate,target_mate})truth(*now,c);stationary_masks_and_config(*old,*now);
    auto expected=expected_after(before);expected.expect_exact(ctx.get(),incident());
    for(size_t i=0;i<stationary.size();++i){EXPECT_EQ(path(inputs[0],stationary[i],id_A),paths[i]);}
    all_connected();ctx->check();coverage();
}
TEST_P(FinalRoutedMove, FalseAndThrowRestoreEveryRouteMapControlRbfAndCache)
{
    const Snapshot before(ctx.get());bool reached=false;
    EXPECT_FALSE(trial([&]{reached=true;EXPECT_EQ(moved->bel,target());return false;}));ASSERT_TRUE(reached)<<reason;restored(before);
    EXPECT_THROW(trial([&]()->bool{EXPECT_EQ(moved->bel,target());throw std::runtime_error("accept fault");}),std::runtime_error);restored(before);
}
TEST_P(FinalRoutedMove, EveryOldHalfMaskBitRejectsBeforeCallbackWithoutMutation)
{
    const Snapshot original(ctx.get());
    for(unsigned row=0;row<32;++row){SCOPED_TRACE(row);flip(home(),row+32*half());const Snapshot malformed(ctx.get());bool reached=false;
        EXPECT_FALSE(trial([&]{reached=true;return true;}));EXPECT_FALSE(reached);malformed.expect_exact(ctx.get());EXPECT_EQ(ctx->getBoundBelCell(target()),nullptr);
        restore_rbf(original);restored(original);
    }
    const auto good=moved->params.at(id_LUT);
    for(const auto &bad:{Property(std::string("11010010")),Property::from_string("1101001x"),Property::from_string("1101001z"),Property(0xd2,7),Property(0xd2,9)}) {
        moved->params[id_LUT]=bad;const Snapshot malformed(ctx.get());bool reached=false;
        EXPECT_FALSE(trial([&]{reached=true;return true;}));EXPECT_FALSE(reached);malformed.expect_exact(ctx.get());
        moved->params[id_LUT]=good;restored(original);
    }
}
TEST_P(FinalRoutedMove, NewMovedAndBothStationaryMaskFaultsRejectAndRestore)
{
    const Snapshot before(ctx.get());
    // These are representative post-route bit faults, not all new mask bits.
    for(int which=0;which<3;++which){SCOPED_TRACE(which);bool reached=false;
        EXPECT_FALSE(trial([&]{reached=true;auto b=which==2?home():target();unsigned h=which==0?half():1-half();flip(b,32*h+7);return true;}));
        ASSERT_TRUE(reached)<<reason;restored(before);
    }
}
TEST_P(FinalRoutedMove, ConfiguredSelectorAndCacheFaultsRejectAndRestore)
{
    const Snapshot before(ctx.get());const auto loc=ctx->bel_data(target_ff->bel).lab_data;
    const std::array<CV::bmux_type_t,4> pkreg{CV::TPKREG1,CV::TPKREG0,CV::BPKREG1,CV::BPKREG0};
    CV::bmux_setting_t setting{};const auto block=ctx->labs[loc.lab].is_mlab?CV::MLAB:CV::LAB;
    ASSERT_TRUE(ctx->cyclonev->bmux_get(block,target_ff->bel.pos,pkreg[loc.idx],loc.alm,setting));ASSERT_EQ(setting.s,0u);
    bool called=false;
    EXPECT_FALSE(trial([&]{called=true;EXPECT_TRUE(ctx->cyclonev->bmux_b_set(block,target_ff->bel.pos,pkreg[loc.idx],loc.alm,true));return true;}));
    ASSERT_TRUE(called)<<reason;restored(before);
    EXPECT_THROW(trial([&]()->bool{EXPECT_TRUE(ctx->cyclonev->bmux_b_set(block,target_ff->bel.pos,pkreg[loc.idx],loc.alm,true));throw std::runtime_error("configured selector fault");}),std::runtime_error);
    restored(before);
    bool reached=false;
    EXPECT_FALSE(trial([&]{reached=true;ctx->analogue_cache_valid=false;return true;}));ASSERT_TRUE(reached)<<reason;restored(before);
    reached=false;
    EXPECT_FALSE(trial([&]{reached=true;ctx->analogue_arc_cache.clear();return true;}));ASSERT_TRUE(reached)<<reason;restored(before);
}
TEST_P(FinalRoutedMove, MovedOutputSelectorFaultsRejectAndRestore)
{
    const Snapshot before(ctx.get());
    const std::array<CV::bmux_type_t,6> muxes{CV::TDFF0,CV::TDFF1,CV::TDFF1L,CV::BDFF0,CV::BDFF1,CV::BDFF1L};
    for(auto bel:{home(),target()})for(int i=3*half();i<3*(half()+1);++i) {
        bool reached=false;auto l=ctx->bel_data(bel).lab_data;auto block=ctx->labs[l.lab].is_mlab?CV::MLAB:CV::LAB;
        EXPECT_FALSE(trial([&]{reached=true;CV::bmux_setting_t setting{};
            EXPECT_TRUE(ctx->cyclonev->bmux_get(block,bel.pos,muxes[i],l.alm,setting));
            EXPECT_TRUE(ctx->cyclonev->bmux_m_set(block,bel.pos,muxes[i],l.alm,setting.s==CV::REG?CV::NLUT:CV::REG));return true;}));
        ASSERT_TRUE(reached)<<reason;restored(before);
    }
}
TEST_P(FinalRoutedMove, MalformedOriginalOwnedSelectorRejectsBeforeMutation)
{
    const Snapshot before(ctx.get());
    const std::array<CV::bmux_type_t,6> muxes{CV::TDFF0,CV::TDFF1,CV::TDFF1L,CV::BDFF0,CV::BDFF1,CV::BDFF1L};
    auto bel=home();auto l=ctx->bel_data(bel).lab_data;
    auto block=ctx->labs[l.lab].is_mlab?CV::MLAB:CV::LAB;unsigned tested=0;
    for(int i=3*half();i<3*(half()+1);++i) {
        CV::bmux_setting_t setting{};
        ASSERT_TRUE(ctx->cyclonev->bmux_get(block,bel.pos,muxes[i],l.alm,setting));
        if(setting.s!=CV::NLUT)continue;
        ASSERT_TRUE(ctx->cyclonev->bmux_m_set(block,bel.pos,muxes[i],l.alm,CV::REG));
        const Snapshot malformed(ctx.get());bool reached=false;
        EXPECT_FALSE(trial([&]{reached=true;return true;}));EXPECT_FALSE(reached);
        malformed.expect_exact(ctx.get());
        ASSERT_TRUE(ctx->cyclonev->bmux_m_set(block,bel.pos,muxes[i],l.alm,CV::bmux_type_t(setting.s)));
        before.expect_exact(ctx.get());++tested;
    }
    EXPECT_GT(tested,0u);
}
TEST_P(FinalRoutedMove, TinyAndInvalidBudgetsNeverLeakPlacementOrTrees)
{
    const Snapshot before(ctx.get());
    for(int budget:{-1,0,500001,1}){SCOPED_TRACE(budget);bool reached=false;
        EXPECT_FALSE(trial([&]{reached=true;return true;},budget));EXPECT_FALSE(reached);restored(before);
    }
}
TEST_P(FinalRoutedMove, VacatedHalfLocalFfConsumerRejectsBeforeMutation)
{
    build(true);ASSERT_FALSE(HasFatalFailure());const Snapshot before(ctx.get());bool reached=false;
    EXPECT_FALSE(trial([&]{reached=true;return true;}));EXPECT_FALSE(reached);
    EXPECT_NE(reason.find("local COMBOUT"),std::string::npos);restored(before);
}
TEST_P(FinalRoutedMove, ProtectedWrongHalfAndOccupiedDestinationsRejectExactly)
{
    const Snapshot before(ctx.get());bool reached=false;
    auto callback=[&]{reached=true;return true;};
    // Occupied same-half is distinct from a physically empty wrong-half target.
    EXPECT_FALSE(trial(callback,500000,home()));EXPECT_FALSE(reached);restored(before);
    BelId wrong;
    for(uint32_t l=0;l<ctx->labs.size();++l)if(l!=lab&&l!=source_lab&&ctx->labs[l].is_mlab==std::get<0>(GetParam())) {wrong=ctx->labs[l].alms[0].lut_bels[1-half()];break;}
    ASSERT_NE(wrong,BelId());ASSERT_EQ(ctx->getBoundBelCell(wrong),nullptr);
    EXPECT_FALSE(trial(callback,500000,wrong));EXPECT_FALSE(reached);restored(before);
    auto old=moved->belStrength;moved->belStrength=STRENGTH_STRONG;Snapshot protected_state(ctx.get());
    EXPECT_FALSE(trial(callback));EXPECT_FALSE(reached);protected_state.expect_exact(ctx.get());moved->belStrength=old;restored(before);
    auto key=ctx->id("keep");moved->attrs[key]=1;Snapshot kept(ctx.get());EXPECT_FALSE(trial(callback));EXPECT_FALSE(reached);kept.expect_exact(ctx.get());moved->attrs.erase(key);
    // Protection keys are interned during fixture construction; no ID reset is used.
    before.expect_exact(ctx.get());
}
TEST_P(FinalRoutedMove, MissingReciprocalUserRejectsBeforeAnyPreview)
{
    auto *net=inputs[1];const auto slot=moved->ports.at(id_B).user_idx;
    auto users=net->users;net->users.remove(slot);const Snapshot malformed(ctx.get());bool reached=false;
    EXPECT_FALSE(trial([&]{reached=true;return true;}));EXPECT_FALSE(reached);malformed.expect_exact(ctx.get());
    EXPECT_EQ(ctx->getBoundBelCell(target()),nullptr);EXPECT_EQ(moved->bel,home());
    net->users=std::move(users);ctx->configure_bitstream();ctx->check();
}
TEST_P(FinalRoutedMove, ZeroFinalBudgetDoesNotTouchEvenAnUnroutedContext)
{
    // This hook must return before IDs, timing, configuration, or native preview.
    ctx->critical_final_move_budget=0;ctx->critical_move_route_budget=16;
    ctx->attrs[id_step]=std::string("place");ctx->bitstream_configured=false;ctx->analogue_cache_valid=false;
    const Snapshot before(ctx.get());ctx->repair_final_critical_moves();before.expect_exact(ctx.get());
    EXPECT_EQ(ctx->getBoundBelCell(target()),nullptr);
}

TEST_P(FinalRoutedMove, CanonicalEmptyAlmsPreserveMovedTruthAndNativeDefaults)
{
    for(auto flags:{std::make_pair(true,false),std::make_pair(false,true),std::make_pair(true,true)}) {
        SCOPED_TRACE(::testing::PrintToString(flags));build(false,flags.first,flags.second);ASSERT_FALSE(HasFatalFailure());
        coverage();ASSERT_FALSE(HasFailure());const Snapshot before(ctx.get());accepted_before=&before;auto old=decoded();truth(*old,moved);
        std::vector<Tree> paths;for(auto *cell:stationary)paths.push_back(path(inputs[0],cell,id_A));
        bool reached=false;
        ASSERT_TRUE(trial([&]{reached=true;return true;}))<<reason;ASSERT_TRUE(reached);
        auto now=decoded();truth(*now,moved);if(source_mate)truth(*now,source_mate);if(target_mate)truth(*now,target_mate);
        for(auto bel:{home(),target()}) {
            auto l=ctx->bel_data(bel).lab_data;auto block=ctx->labs[l.lab].is_mlab?CV::MLAB:CV::LAB;
            bool empty=bel==home()&&flags.first;CV::bmux_setting_t setting{};
            ASSERT_TRUE(now->bmux_get(block,bel.pos,CV::TMODE,l.alm,setting));
            EXPECT_EQ(setting.s,uint32_t(empty?CV::D_E:CV::E_0));EXPECT_EQ(setting.def,empty);
            ASSERT_TRUE(now->bmux_get(block,bel.pos,CV::BMODE,l.alm,setting));
            EXPECT_EQ(setting.s,uint32_t(empty?CV::C_E:CV::E_1));EXPECT_EQ(setting.def,empty);
            if(empty) EXPECT_EQ(mask(*now,bel),0u);
            else if(bel==target()&&flags.second) {
                uint64_t unused=0;for(unsigned row=0;row<32;++row)unused|=uint64_t(1)<<emitted_index(bel,row+32*(1-half()));
                EXPECT_EQ(mask(*now,bel)&unused,unused);
            }
        }
        stationary_masks_and_config(*old,*now,flags.first,flags.second);
        Snapshot expected=before;expected.cells.at(moved->name).bel=target();
        auto h=ctx->bel_data(home()).lab_data,t=ctx->bel_data(target()).lab_data;
        expected.labs[h.lab].alms[h.alm].unique_input_count=flags.first?0:3;
        expected.labs[t.lab].alms[t.alm].unique_input_count=flags.second?3:6;
        expected.expect_exact(ctx.get(),incident());
        for(size_t i=0;i<stationary.size();++i)EXPECT_EQ(path(inputs[0],stationary[i],id_A),paths[i]);
        all_connected();ctx->check();coverage();
    }
}
TEST_P(FinalRoutedMove, MalformedOriginalEmptyTargetRejectsBeforeMutation)
{
    build(false,false,true);ASSERT_FALSE(HasFatalFailure());const Snapshot before(ctx.get());
    auto bel=target();auto l=ctx->bel_data(bel).lab_data;auto block=ctx->labs[l.lab].is_mlab?CV::MLAB:CV::LAB;
    for(int fault=0;fault<3;++fault) {
        SCOPED_TRACE(fault);
        if(fault==0)flip(bel,7);
        else ASSERT_TRUE(ctx->cyclonev->bmux_m_set(block,bel.pos,fault==1?CV::TMODE:CV::BMODE,l.alm,fault==1?CV::E_0:CV::E_1));
        const Snapshot malformed(ctx.get());bool reached=false;
        EXPECT_FALSE(trial([&]{reached=true;return true;}));EXPECT_FALSE(reached);malformed.expect_exact(ctx.get());
        ctx->configure_bitstream();before.expect_exact(ctx.get());
    }
}
TEST_P(FinalRoutedMove, CanonicalEmptyFalseAndThrowRestoreEveryState)
{
    for(auto flags:{std::make_pair(true,false),std::make_pair(false,true),std::make_pair(true,true)}) {
        build(false,flags.first,flags.second);ASSERT_FALSE(HasFatalFailure());const Snapshot before(ctx.get());bool reached=false;
        EXPECT_FALSE(trial([&]{reached=true;return false;}));ASSERT_TRUE(reached)<<reason;restored(before);
        EXPECT_THROW(trial([&]()->bool{throw std::runtime_error("empty callback fault");}),std::runtime_error);restored(before);
    }
}
TEST_P(FinalRoutedMove, CanonicalEmptyMaskAndModeFaultsRejectAndRestore)
{
    build(false,true,true);ASSERT_FALSE(HasFatalFailure());const Snapshot before(ctx.get());
    for(int fault=0;fault<6;++fault) {
        SCOPED_TRACE(fault);bool reached=false;
        EXPECT_FALSE(trial([&]{reached=true;auto bel=fault<3?home():target();auto l=ctx->bel_data(bel).lab_data;
            auto block=ctx->labs[l.lab].is_mlab?CV::MLAB:CV::LAB;
            if(fault==0) flip(bel,0);
            else if(fault==3) flip(bel,32*(1-half())+7);
            else {auto mux=fault%3==1?CV::TMODE:CV::BMODE;
                auto value=fault<3?(mux==CV::TMODE?CV::E_0:CV::E_1):(mux==CV::TMODE?CV::D_E:CV::C_E);
                EXPECT_TRUE(ctx->cyclonev->bmux_m_set(block,bel.pos,mux,l.alm,value));}
            return true;}));
        ASSERT_TRUE(reached)<<reason;restored(before);
    }
}
TEST_P(FinalRoutedMove, FfOnlySourceNeverGetsEmptyModeException)
{
    build(false,true,true,true);ASSERT_FALSE(HasFatalFailure());const Snapshot before(ctx.get());bool reached=false;
    EXPECT_FALSE(trial([&]{reached=true;auto l=ctx->bel_data(home()).lab_data;auto block=ctx->labs[l.lab].is_mlab?CV::MLAB:CV::LAB;
        EXPECT_TRUE(ctx->cyclonev->bmux_m_set(block,home().pos,CV::TMODE,l.alm,CV::D_E));return true;}));
    ASSERT_TRUE(reached)<<reason;restored(before);
    ASSERT_TRUE(trial([]{return true;}))<<reason;auto now=decoded();auto l=ctx->bel_data(home()).lab_data;
    CV::bmux_setting_t setting{};ASSERT_TRUE(now->bmux_get(ctx->labs[l.lab].is_mlab?CV::MLAB:CV::LAB,home().pos,CV::TMODE,l.alm,setting));
    EXPECT_EQ(setting.s,uint32_t(CV::E_0));EXPECT_NE(mask(*now,home()),0u);
}
TEST_P(FinalRoutedMove, BoundInternalObserversPreventEmptyException)
{
    for(bool saved_empty_target:{false,true}) {
    SCOPED_TRACE(saved_empty_target);build(false,!saved_empty_target,saved_empty_target);ASSERT_FALSE(HasFatalFailure());
    auto bel=saved_empty_target?target():home();auto l=ctx->bel_data(bel).lab_data;
    const auto &alm=ctx->labs[l.lab].alms[l.alm];
    std::vector<WireId> wires{alm.comb_out[1-half()],alm.ff_in[2*(1-half())],alm.ff_out[2*(1-half())],
        ctx->getBelPinWire(bel,id_CI),ctx->getBelPinWire(bel,id_CO),
        ctx->getBelPinWire(bel,id_SHAREIN),ctx->getBelPinWire(bel,id_SHAREOUT)};
    for(auto wire:wires) {
        ASSERT_NE(wire,WireId());ASSERT_TRUE(ctx->checkWireAvail(wire));
        // Deliberate nonincident disconnected observer: detection-only malformed
        // route fixture. A conservative empty qualification must reject it.
        ctx->bindWire(wire,inputs[6],STRENGTH_WEAK);ctx->configure_bitstream();
        const Snapshot malformed(ctx.get());bool reached=false;
        EXPECT_FALSE(trial([&]{reached=true;return true;}));EXPECT_FALSE(reached);malformed.expect_exact(ctx.get());
        ctx->unbindWire(wire);ctx->configure_bitstream();
    }
    }
}

INSTANTIATE_TEST_SUITE_P(LabAndMlabBothHalves,FinalRoutedMove,::testing::Combine(::testing::Bool(),::testing::Values(0,1)));
} // namespace
