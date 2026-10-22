/* Fixed transparent helper users for explicit final-route repair. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "analogue_repair.h"
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
using Tree = std::map<WireId,std::pair<PipId,PlaceStrength>>;
using Config = std::map<std::tuple<int,int,int,int>,std::tuple<int,bool,uint32_t,std::vector<uint8_t>>>;
constexpr size_t reverse_port_limit = 1000000;
bool reject(std::string *reason,const char *text) { if(reason)*reason=text;return false; }
bool protected_attrs(Context *ctx,const dict<IdString,Property> &attrs)
{
    for(const auto &entry:attrs) {
        auto name=entry.first.str(ctx);
        if(name=="keep" || name=="dont_touch" || name=="BEL" || name=="FES_SLOT")return true;
    }
    return false;
}
bool live_cell(Context *ctx,CellInfo *cell)
{
    if(!cell)return false;
    auto it=ctx->cells.find(cell->name);
    return it!=ctx->cells.end() && it->second.get()==cell && cell->ctx==ctx;
}
bool live_net(Context *ctx,NetInfo *net)
{
    if(!net)return false;
    auto it=ctx->nets.find(net->name);
    return it!=ctx->nets.end() && it->second.get()==net;
}
bool fixed_cell(Context *ctx,CellInfo *cell)
{
    return live_cell(ctx,cell) && !cell->isPseudo() && !cell->region && cell->cluster==ClusterId() &&
        cell->constr_children.empty() && !cell->constr_x && !cell->constr_y && !cell->constr_z && !cell->constr_abs_z &&
        cell->bel!=BelId() && cell->belStrength<=STRENGTH_STRONG && ctx->getBoundBelCell(cell->bel)==cell &&
        !protected_attrs(ctx,cell->attrs);
}
bool plain_net(Context *ctx,NetInfo *net)
{
    if(!live_net(ctx,net) || net->region || net->is_global || net->clkconstr || net->constant_value!=IdString() ||
       protected_attrs(ctx,net->attrs))return false;
    for(const auto &port:ctx->ports)if(port.second.net==net)return false;
    return true;
}
bool user_slot(Context *ctx,NetInfo *net,const PortRef &user)
{
    if(!live_cell(ctx,user.cell))return false;
    auto p=user.cell->ports.find(user.port);
    if(p==user.cell->ports.end() || p->second.name!=user.port || p->second.type!=PORT_IN || p->second.net!=net ||
       !net->users.count(p->second.user_idx))return false;
    auto old=net->users.at(p->second.user_idx);
    return old.cell==user.cell && old.port==user.port;
}
bool tree_valid(Context *ctx,NetInfo *net)
{
    if(!plain_net(ctx,net) || net->wires.empty() || net->wires.size()>4096 || !live_cell(ctx,net->driver.cell))return false;
    auto port=net->driver.cell->ports.find(net->driver.port);
    if(port==net->driver.cell->ports.end() || port->second.name!=net->driver.port ||
       port->second.type!=PORT_OUT || port->second.net!=net)return false;
    auto root=ctx->getNetinfoSourceWire(net);
    if(root==WireId() || !net->wires.count(root) || net->wires.at(root).pip!=PipId())return false;
    for(const auto &entry:net->wires) {
        if(entry.second.strength>STRENGTH_STRONG || ctx->getBoundWireNet(entry.first)!=net ||
           (entry.second.pip!=PipId() && ctx->getBoundPipNet(entry.second.pip)!=net))return false;
        std::set<WireId> seen;
        for(auto w=entry.first;w!=root;) {
            auto at=net->wires.find(w);
            if(!seen.insert(w).second || at==net->wires.end() || at->second.pip==PipId() ||
               ctx->getPipDstWire(at->second.pip)!=w)return false;
            w=ctx->getPipSrcWire(at->second.pip);
        }
    }
    for(const auto &user:net->users) {
        if(!user_slot(ctx,net,user))return false;
        auto sinks=ctx->getNetinfoSinkWires(net,user);if(sinks.size()==0)return false;
        for(auto sink:sinks)if(!net->wires.count(sink))return false;
    }
    return true;
}
bool pins(CellInfo *cell,IdString port,IdString physical)
{
    auto it=cell->pin_data.find(port);
    return it!=cell->pin_data.end() && it->second.state==PIN_SIG && it->second.bel_pins==std::vector<IdString>{physical};
}
bool cached_controls(Context *ctx,CellInfo *ff)
{
    auto check=[&](IdString pin,const ControlSig &cached,const char *implicit=nullptr) {
        auto *net=ff->getPort(pin);
        if(net) {if(cached.net!=net)return false;}
        else if(implicit) {
            auto *constant=const_cast<NetInfo *>(cached.net);
            if(!live_net(ctx,constant) || constant->name.str(ctx)!=implicit)return false;
        } else if(cached.net)return false;
        return cached.inverted==(ff->get_pin_state(pin)==PIN_INV);
    };
    const auto &c=ff->ffInfo.ctrlset;
    return check(id_CLK,c.clk) && check(id_ENA,c.ena,"$PACKER_VCC_NET") && check(id_ACLR,c.aclr) &&
        check(id_SCLR,c.sclr) && check(id_SLOAD,c.sload,ff->getPort(id_SCLR)?"$PACKER_GND_NET":nullptr) &&
        ff->ffInfo.datain==ff->getPort(id_DATAIN) && ff->ffInfo.sdata==ff->getPort(id_SDATA);
}
struct Helper { CellInfo *buffer,*ff;NetInfo *output;uint32_t lab; };
bool inspect(Context *ctx,NetInfo *input,std::vector<Helper> &helpers,std::string *reason)
{
    if(!ctx || !ctx->attrs.count(id_step) || ctx->attrs.at(id_step).as_string()!="route" ||
       !ctx->bitstream_configured || !ctx->analogue_cache_valid || ctx->fes_any_slot_region_active ||
       ctx->fes_fence_active || ctx->fes_has_reserved_rect || ctx->fes_has_cram_region ||
       !ctx->fes_frozen_cells.empty() || !ctx->fes_frozen_pips.empty())
        return reject(reason,"buffer users require current ordinary configured routing");
    if(!plain_net(ctx,input) || !input->users.entries() || input->users.entries()>64 || input->users.capacity()>4096 ||
       !tree_valid(ctx,input))return reject(reason,"invalid buffer input net or native routed tree");
    std::set<NetInfo *> outputs;std::set<uint32_t> labs;
    for(const auto &user:input->users) {
        if(!user_slot(ctx,input,user))return reject(reason,"buffer input user identity mismatch");
        auto *buffer=user.cell;if(buffer->type!=id_MISTRAL_BUF)continue;
        if(user.port!=id_A || !fixed_cell(ctx,buffer) || buffer->ports.size()!=2 || !buffer->params.empty() ||
           buffer->pin_data.size()!=2 || buffer->getPort(id_A)!=input || !pins(buffer,id_Q,id_COMBOUT))
            return reject(reason,"requires an exact fixed native BUF A/Q shape");
        auto output=buffer->ports.find(id_Q);
        if(output==buffer->ports.end() || output->second.name!=id_Q || output->second.type!=PORT_OUT ||
           !plain_net(ctx,output->second.net) || output->second.net==input)
            return reject(reason,"invalid private buffer output");
        auto *q=output->second.net;
        if(q->driver.cell!=buffer || q->driver.port!=id_Q || q->users.entries()!=1 || q->users.capacity()>4096 ||
           !outputs.insert(q).second || !tree_valid(ctx,q))return reject(reason,"buffer Q is not one private routed FF input");
        const auto &place=ctx->bel_data(buffer->bel);
        if(!place.type.in(id_MISTRAL_COMB,id_MISTRAL_MCOMB))return reject(reason,"buffer has non-combinational BEL");
        const auto &where=place.lab_data;
        if(where.lab>=ctx->labs.size() || where.alm>=10 || where.idx>=2)return reject(reason,"invalid native helper location");
        const auto &alm=ctx->labs[where.lab].alms[where.alm];
        if(alm.lut_bels[where.idx]!=buffer->bel || alm.l6_mode || alm.carry_mode ||
           !(pins(buffer,id_A,where.idx?id_E1:id_E0) || pins(buffer,id_A,where.idx?id_F1:id_F0)))
            return reject(reason,"buffer does not use one fixed native half-private input");
        const auto &comb=buffer->combInfo;
        if(comb.lut_input_count!=1 || comb.used_lut_input_count!=1 || comb.lut_bits_count!=2 ||
           comb.lut_in[0]!=input || comb.comb_out!=q || comb.is_carry || comb.is_shared || comb.is_extended ||
           comb.carry_start || comb.carry_end || comb.chain_shared_input_count || comb.mlab_group!=-1)
            return reject(reason,"buffer native function cache is not transparent");
        auto capture=*q->users.begin();auto *ff=capture.cell;
        if(capture.port!=id_DATAIN || !fixed_cell(ctx,ff) || ff->type!=id_MISTRAL_FF ||
           ctx->getBelType(ff->bel)!=id_MISTRAL_FF || !pins(ff,id_DATAIN,id_DATAIN) ||
           ff->getPort(id_SDATA) || ff->getPort(id_SLOAD) || ff->get_pin_state(id_SLOAD)==PIN_1 ||
           ff->get_pin_state(id_SLOAD)==PIN_INV || !cached_controls(ctx,ff))
            return reject(reason,"buffer capture is not an unchanged local DATAIN register");
        const auto &f=ctx->bel_data(ff->bel).lab_data;
        if(f.lab!=where.lab || f.alm!=where.alm || f.idx>=4 || f.idx/2!=where.idx || alm.ff_bels[f.idx]!=ff->bel ||
           !ctx->wires_connected(alm.comb_out[where.idx],alm.ff_in[f.idx]) ||
           ctx->wires_connected(alm.sel_ef[where.idx],alm.ff_in[f.idx]))
            return reject(reason,"buffer Q does not use same-half native COMBOUT DATAIN");
        using CV=mistral::CycloneV;
        const std::array<CV::bmux_type_t,4> pkreg{CV::TPKREG1,CV::TPKREG0,CV::BPKREG1,CV::BPKREG0};
        CV::bmux_setting_t setting{};
        if(!ctx->cyclonev->bmux_get(ctx->labs[f.lab].is_mlab?CV::MLAB:CV::LAB,ff->bel.pos,pkreg[f.idx],f.alm,setting) || setting.s)
            return reject(reason,"configured FF data selector is not local COMBOUT");
        int clocks=0;DelayQuad delay;
        if(ctx->getPortTimingClass(buffer,id_A,clocks)!=TMG_COMB_INPUT || clocks ||
           ctx->getPortTimingClass(buffer,id_Q,clocks)!=TMG_COMB_OUTPUT || clocks ||
           !ctx->getCellDelay(buffer,id_A,id_Q,delay))return reject(reason,"buffer lacks a characterized transparent native arc");
        for(auto v:{delay.minRiseDelay(),delay.maxRiseDelay(),delay.minFallDelay(),delay.maxFallDelay()})
            if(!std::isfinite(double(v)) || v==std::numeric_limits<delay_t>::lowest() || v==std::numeric_limits<delay_t>::max())
                return reject(reason,"buffer native arc is not finite");
        helpers.push_back({buffer,ff,q,where.lab});labs.insert(where.lab);
    }
    if(helpers.empty())return reject(reason,"net has no native buffer users");
    // Complete reverse identities: input driver plus all indexed users, and
    // each private Q driver plus its sole DATAIN observer. Missing slots cannot
    // hide an extra observer from the routed tree or timing walk.
    std::map<NetInfo *,size_t> references;size_t work=0;
    for(const auto &entry:ctx->cells)for(const auto &port:entry.second->ports) {
        if(++work>reverse_port_limit)return reject(reason,"buffer reverse-port scan bound exceeded");
        if(port.second.net==input || outputs.count(port.second.net))++references[port.second.net];
    }
    if(references[input]!=size_t(input->users.entries())+1)
        return reject(reason,"buffer input has hidden or duplicate cell-port observers");
    for(auto *q:outputs)if(references[q]!=2)return reject(reason,"private buffer Q has hidden or duplicate cell-port observers");
    for(auto lab:labs)for(const auto &alm:ctx->labs[lab].alms) {
        for(auto bel:alm.lut_bels)if(ctx->getBoundBelCell(bel) && !ctx->isBelLocationValid(bel))
            return reject(reason,"original buffer LAB LUT legality failed");
        for(auto bel:alm.ff_bels)if(ctx->getBoundBelCell(bel) && !ctx->isBelLocationValid(bel))
            return reject(reason,"original buffer LAB FF legality failed");
    }
    return true;
}
Tree tree(NetInfo *net)
{Tree result;for(const auto &w:net->wires)result[w.first]={w.second.pip,w.second.strength};return result;}
bool same_users(const indexed_store<PortRef> &a,const indexed_store<PortRef> &b)
{
    if(a.entries()!=b.entries() || a.capacity()!=b.capacity())return false;
    auto x=a,y=b;
    for(auto user:x.enumerate())if(!y.count(user.index) || y.at(user.index).cell!=user.value.cell || y.at(user.index).port!=user.value.port)return false;
    for(int i=0;i<a.capacity()+1;++i)if(x.add(PortRef{})!=y.add(PortRef{}))return false;
    return true;
}
bool same_lab(const LABInfo &a,const LABInfo &b)
{
    if(a.is_mlab!=b.is_mlab || a.clk_wires!=b.clk_wires || a.ena_wires!=b.ena_wires || a.aclr_wires!=b.aclr_wires ||
       a.sclr_wire!=b.sclr_wire || a.sload_wire!=b.sload_wire || a.aclr_used!=b.aclr_used)return false;
    for(size_t i=0;i<a.alms.size();++i) {
        const auto &x=a.alms[i],&y=b.alms[i];
        if(x.comb_out!=y.comb_out || x.sel_clk!=y.sel_clk || x.sel_ena!=y.sel_ena || x.sel_aclr!=y.sel_aclr || x.sel_ef!=y.sel_ef ||
           x.ff_in!=y.ff_in || x.ff_out!=y.ff_out || x.lut_bels!=y.lut_bels || x.ff_bels!=y.ff_bels || x.l6_mode!=y.l6_mode ||
           x.carry_mode!=y.carry_mode || x.clk_ena_idx!=y.clk_ena_idx || x.aclr_idx!=y.aclr_idx || x.unique_input_count!=y.unique_input_count)return false;
    }
    return true;
}
Config static_config(Context *ctx,const std::map<uint32_t,LABInfo> &labs)
{
    using CV=mistral::CycloneV;std::set<std::pair<int,int>> tiles;
    for(const auto &entry:labs)tiles.emplace(entry.second.is_mlab?CV::MLAB:CV::LAB,entry.second.alms[0].lut_bels[0].pos.v);
    Config result;
    for(const auto &v:ctx->cyclonev->bmux_get())if(tiles.count({int(v.btype),v.pos.v}))
        result.emplace(std::make_tuple(int(v.btype),v.pos.v,int(v.mux),v.midx),std::make_tuple(v.type,v.def,v.s,v.r));
    return result;
}
} // namespace

struct CriticalFinalRouteBufferState {
    struct Cell {
        CellInfo *cell;IdString name,type,hierpath;BelId bel;PlaceStrength strength;ClusterId cluster;Region *region;PseudoCell *pseudo;
        decltype(CellInfo::ports) ports;decltype(CellInfo::attrs) attrs;decltype(CellInfo::params) params;ArchCellInfo arch;
        explicit Cell(CellInfo *c):cell(c),name(c->name),type(c->type),hierpath(c->hierpath),bel(c->bel),strength(c->belStrength),
            cluster(c->cluster),region(c->region),pseudo(c->pseudo_cell.get()),ports(c->ports),attrs(c->attrs),params(c->params),arch(*c){}
        bool same(Context *ctx) const
        {
            auto *c=cell;if(!live_cell(ctx,c) || c->name!=name || c->type!=type || c->hierpath!=hierpath || c->bel!=bel ||
                c->belStrength!=strength || c->cluster!=cluster || c->region!=region || c->pseudo_cell.get()!=pseudo || c->attrs!=attrs || c->params!=params ||
                c->ports.size()!=ports.size() || c->pin_data.size()!=arch.pin_data.size() ||
                c->constr_children!=arch.constr_children || c->constr_x!=arch.constr_x || c->constr_y!=arch.constr_y ||
                c->constr_z!=arch.constr_z || c->constr_abs_z!=arch.constr_abs_z)return false;
            for(const auto &p:ports) {
                auto q=c->ports.find(p.first);if(q==c->ports.end() || q->second.name!=p.second.name || q->second.net!=p.second.net ||
                    q->second.type!=p.second.type || q->second.user_idx!=p.second.user_idx)return false;
            }
            for(const auto &p:arch.pin_data) {
                auto q=c->pin_data.find(p.first);if(q==c->pin_data.end() || q->second.state!=p.second.state || q->second.bel_pins!=p.second.bel_pins)return false;
            }
            if(type==id_MISTRAL_FF)return c->ffInfo.ctrlset==arch.ffInfo.ctrlset && c->ffInfo.datain==arch.ffInfo.datain && c->ffInfo.sdata==arch.ffInfo.sdata;
            const auto &a=c->combInfo,&b=arch.combInfo;
            if(a.comb_out!=b.comb_out || a.lut_input_count!=b.lut_input_count || a.used_lut_input_count!=b.used_lut_input_count ||
               a.lut_bits_count!=b.lut_bits_count || a.chain_shared_input_count!=b.chain_shared_input_count || a.is_carry!=b.is_carry ||
               a.is_shared!=b.is_shared || a.is_extended!=b.is_extended || a.carry_start!=b.carry_start || a.carry_end!=b.carry_end ||
               a.mlab_group!=b.mlab_group)return false;
            if(a.lut_input_count<0 || a.lut_input_count>7)return false;
            for(int i=0;i<a.lut_input_count;++i)if(a.lut_in[i]!=b.lut_in[i])return false;
            return type!=id_MISTRAL_MLAB || (a.wclk==b.wclk && a.we==b.we);
        }
    };
    struct Net {
        NetInfo *net;IdString name,hierpath;PortRef driver;indexed_store<PortRef> users;Tree route;
        decltype(NetInfo::attrs) attrs;decltype(NetInfo::aliases) aliases;
        explicit Net(NetInfo *n):net(n),name(n->name),hierpath(n->hierpath),driver(n->driver),users(n->users),route(tree(n)),attrs(n->attrs),aliases(n->aliases){}
        bool same(Context *ctx,bool input) const
        {return live_net(ctx,net) && net->name==name && net->hierpath==hierpath && plain_net(ctx,net) && net->driver.cell==driver.cell && net->driver.port==driver.port &&
            same_users(net->users,users) && net->attrs==attrs && net->aliases==aliases && (input || tree(net)==route);}
    };
    NetInfo *input=nullptr;std::vector<Helper> helpers;std::map<uint32_t,LABInfo> labs;Config config;
    std::map<BelId,CellInfo *> occupants;std::vector<Cell> cells;std::vector<Net> nets;
    decltype(Context::net_aliases) aliases;size_t cell_count=0,net_count=0;
};

bool critical_final_route_buffer_users(Context *ctx,NetInfo *input,std::string *reason)
{std::vector<Helper> helpers;return inspect(ctx,input,helpers,reason);}

std::shared_ptr<const CriticalFinalRouteBufferState> critical_final_route_buffer_snapshot(Context *ctx,NetInfo *input,std::string *reason)
{
    auto state=std::make_shared<CriticalFinalRouteBufferState>();
    if(!inspect(ctx,input,state->helpers,reason))return {};
    state->input=input;state->aliases=ctx->net_aliases;state->cell_count=ctx->cells.size();state->net_count=ctx->nets.size();
    state->nets.emplace_back(input);
    for(const auto &h:state->helpers){state->labs.emplace(h.lab,ctx->labs[h.lab]);state->nets.emplace_back(h.output);}
    for(const auto &lab:state->labs)for(const auto &alm:lab.second.alms) {
        auto save=[&](BelId bel){auto *c=ctx->getBoundBelCell(bel);state->occupants.emplace(bel,c);if(c)state->cells.emplace_back(c);};
        for(auto bel:alm.lut_bels)save(bel);
        for(auto bel:alm.ff_bels)save(bel);
    }
    state->config=static_config(ctx,state->labs);return state;
}

bool critical_final_route_buffer_unchanged(Context *ctx,const CriticalFinalRouteBufferState &state,std::string *reason)
{
    std::vector<Helper> helpers;if(!inspect(ctx,state.input,helpers,reason))return false;
    if(helpers.size()!=state.helpers.size())return reject(reason,"helper inventory changed");
    for(size_t i=0;i<helpers.size();++i) {
        const auto &a=helpers[i],&b=state.helpers[i];
        if(a.buffer!=b.buffer || a.ff!=b.ff || a.output!=b.output || a.lab!=b.lab)return reject(reason,"native helper identities changed");
    }
    if(ctx->cells.size()!=state.cell_count || ctx->nets.size()!=state.net_count || ctx->net_aliases!=state.aliases)
        return reject(reason,"helper graph or alias inventory changed");
    for(const auto &lab:state.labs)if(lab.first>=ctx->labs.size() || !same_lab(ctx->labs[lab.first],lab.second))
        return reject(reason,"helper LAB native state changed");
    for(const auto &bel:state.occupants)if(ctx->getBoundBelCell(bel.first)!=bel.second)return reject(reason,"helper LAB occupant binding changed");
    for(const auto &cell:state.cells)if(!cell.same(ctx))return reject(reason,"helper LAB cell or physical pin state changed");
    for(const auto &net:state.nets)if(!net.same(ctx,net.net==state.input))return reject(reason,"helper input slots or private Q tree changed");
    if(static_config(ctx,state.labs)!=state.config)return reject(reason,"helper LAB configured masks or selectors changed");
    return true;
}
NEXTPNR_NAMESPACE_END
