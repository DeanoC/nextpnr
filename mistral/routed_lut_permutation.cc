/* Bounded fixed-BEL ordinary-LUT input permutations after routing. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "analogue_repair.h"
#include "critical_cohort.h"
#include "private_pair_decomposition_native.h"
#include "routed_lut_permutation_policy.h"
#include "cyclonev.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
const std::array<IdString, 5> pins{id_A,id_B,id_C,id_D,id_E};
using Alm = std::pair<uint32_t,uint8_t>;
unsigned width_of(const CellInfo *cell)
{
    if (!cell) return 0;
    return cell->type == id_MISTRAL_ALUT2 ? 2 : cell->type == id_MISTRAL_ALUT3 ? 3 :
           cell->type == id_MISTRAL_ALUT4 ? 4 : cell->type == id_MISTRAL_ALUT5 ? 5 : 0;
}
bool protected_attrs(Context *ctx, const dict<IdString, Property> &attrs)
{
    for (const auto &attr : attrs) {
        const auto name = attr.first.str(ctx);
        if (name == "keep" || name == "dont_touch" || name == "BEL" || name == "FES_SLOT") return true;
    }
    return false;
}
bool movable(Context *ctx, CellInfo *cell)
{
    return cell && cell->bel != BelId() && cell->belStrength <= STRENGTH_WEAK && !cell->isPseudo() &&
           cell->cluster == ClusterId() && cell->constr_children.empty() && !cell->constr_x &&
           !cell->constr_y && !cell->constr_z && !cell->constr_abs_z && !cell->region &&
           !protected_attrs(ctx, cell->attrs);
}
Alm alm_of(Context *ctx, CellInfo *cell)
{
    const auto &data = ctx->bel_data(cell->bel).lab_data;
    return {data.lab, data.alm};
}
bool legal_labs(Context *ctx, const std::set<uint32_t> &labs)
{
    for (auto lab : labs) for (const auto &alm : ctx->labs.at(lab).alms) {
        for (auto bel : alm.lut_bels) if (ctx->getBoundBelCell(bel) && !ctx->isBelLocationValid(bel)) return false;
        for (auto bel : alm.ff_bels) if (ctx->getBoundBelCell(bel) && !ctx->isBelLocationValid(bel)) return false;
    }
    return true;
}
const std::array<unsigned, 64> mlab_bits{
    0,1,4,5,8,9,12,13,29,28,25,24,21,20,17,16,
    2,3,6,7,10,11,14,15,31,30,27,26,23,22,19,18,
    32,33,36,37,40,41,44,45,61,60,57,56,53,52,49,48,
    34,35,38,39,42,43,46,47,63,62,59,58,55,54,51,50};
unsigned physical_index(Context *ctx, Alm where, unsigned address)
{
    return ctx->labs.at(where.first).is_mlab ? mlab_bits.at(address) : address;
}
uint64_t half_mask(Context *ctx, CellInfo *cell)
{
    auto where = alm_of(ctx,cell); unsigned half = ctx->bel_data(cell->bel).lab_data.idx;
    uint64_t result = 0;
    for (unsigned address = 0; address < 32; ++address)
        result |= uint64_t(1) << physical_index(ctx,where,address+32*half);
    return result;
}
bool physical_function(Context *ctx, CellInfo *cell)
{
    const auto where = alm_of(ctx,cell);
    const int half = ctx->bel_data(cell->bel).lab_data.idx;
    const unsigned width = width_of(cell);
    const uint64_t physical = ctx->compute_lut_mask(where.first,where.second);
    const uint64_t logical = uint64_t(cell->params.at(id_LUT).as_int64());
    std::array<unsigned,5> bits{}; std::set<unsigned> distinct;
    for (unsigned i = 0; i < width; ++i) {
        const auto &pin = cell->pin_data.at(pins[i]);
        if (pin.bel_pins.size() != 1 || (pin.state != PIN_SIG && pin.state != PIN_INV)) return false;
        auto physical_pin = pin.bel_pins.front();
        if (physical_pin == id_A) bits[i] = 0;
        else if (physical_pin == id_B) bits[i] = 1;
        else if (physical_pin == (half ? id_D : id_C)) bits[i] = 2;
        else if (physical_pin == (half ? id_E1 : id_E0)) bits[i] = 3;
        else if (physical_pin == (half ? id_F1 : id_F0)) bits[i] = 4;
        else return false;
        if (!distinct.insert(bits[i]).second) return false;
    }
    for (unsigned address = 0; address < 32; ++address) {
        unsigned row = 0;
        for (unsigned i = 0; i < width; ++i)
            row |= unsigned(bool((address >> bits[i]) & 1) != (cell->get_pin_state(pins[i]) != PIN_INV)) << i;
        const unsigned index = physical_index(ctx,where,address+32*half);
        if (((physical >> index)&1) != (1 ^ ((logical >> row)&1))) return false;
    }
    return true;
}
bool same_pins(CellInfo *cell, const decltype(CellInfo::pin_data) &before)
{
    if (cell->pin_data.size() != before.size()) return false;
    for (const auto &pin : before) {
        auto found = cell->pin_data.find(pin.first);
        if (found == cell->pin_data.end() || pin.second.state != found->second.state ||
            pin.second.bel_pins != found->second.bel_pins) return false;
    }
    return true;
}
bool ordinary_context(Context *ctx)
{
    return !ctx->fes_any_slot_region_active && !ctx->fes_fence_active && !ctx->fes_has_reserved_rect &&
           !ctx->fes_has_cram_region && ctx->fes_frozen_cells.empty() && ctx->fes_frozen_pips.empty() &&
           ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() == "route";
}
using Route = std::map<WireId, std::pair<PipId, PlaceStrength>>;
std::vector<WireId> sink_wires(Context *ctx, NetInfo *net, const PortRef &user)
{
    auto physical=ctx->getNetinfoSinkWires(net,user);
    std::vector<WireId> result(physical.begin(),physical.end());
    std::sort(result.begin(),result.end());
    return result;
}
Route route_tree(NetInfo *net)
{
    Route result;
    for (const auto &wire : net->wires) result.emplace(wire.first, std::make_pair(wire.second.pip, wire.second.strength));
    return result;
}
bool valid_tree(Context *ctx, NetInfo *net, std::string &reason)
{
    auto fail = [&](const char *text) { reason = text; return false; };
    if (net->wires.empty() || net->wires.size() > 4096 || net->users.entries() > 64)
        return fail("LUT permutation net exceeds tree/user bound or is unrouted");
    const auto root = ctx->getNetinfoSourceWire(net);
    if (!net->wires.count(root) || net->wires.at(root).pip != PipId()) return fail("invalid LUT permutation root");
    for (const auto &wire : net->wires) {
        if (wire.second.strength > STRENGTH_STRONG || ctx->getBoundWireNet(wire.first) != net ||
            (wire.second.pip != PipId() && ctx->getBoundPipNet(wire.second.pip) != net))
            return fail("protected or foreign LUT permutation wire/pip");
        std::set<WireId> seen;
        for (auto cursor = wire.first; cursor != root;) {
            auto at = net->wires.find(cursor);
            if (!seen.insert(cursor).second || at == net->wires.end() || at->second.pip == PipId() ||
                ctx->getPipDstWire(at->second.pip) != cursor) return fail("malformed LUT permutation tree");
            cursor = ctx->getPipSrcWire(at->second.pip);
        }
    }
    for (const auto &user : net->users) {
        if (!user.cell || !ctx->cells.count(user.cell->name) || ctx->cells.at(user.cell->name).get()!=user.cell ||
            !user.cell->ports.count(user.port)) return fail("malformed LUT permutation user");
        const auto &port = user.cell->ports.at(user.port);
        if (port.name != user.port || port.net != net || port.type != PORT_IN || !net->users.count(port.user_idx) ||
            &net->users.at(port.user_idx) != &user)
            return fail("LUT permutation user-slot mismatch");
        auto sinks = sink_wires(ctx,net,user);
        if (sinks.empty()) return fail("unmapped LUT permutation sink");
        for (auto sink : sinks) if (!net->wires.count(sink)) return fail("unrouted LUT permutation sink");
    }
    return true;
}
struct View {
    CellInfo *target = nullptr;
    NetInfo *output = nullptr;
    unsigned width = 0;
    uint64_t mask = 0;
    std::vector<NetInfo *> signals;
    std::vector<store_index<PortRef>> slots;
    std::vector<CellPinState> states;
    std::set<Alm> alms;
    std::set<uint32_t> labs;
};
bool ordinary_net(Context *ctx, NetInfo *net)
{
    if (!net || !net->driver.cell || net->is_global || net->clkconstr || net->region ||
        net->constant_value != IdString() || protected_attrs(ctx, net->attrs)) return false;
    if (!ctx->nets.count(net->name) || ctx->nets.at(net->name).get()!=net ||
        !ctx->cells.count(net->driver.cell->name) || ctx->cells.at(net->driver.cell->name).get()!=net->driver.cell)
        return false;
    auto output=net->driver.cell->ports.find(net->driver.port);
    if (output==net->driver.cell->ports.end() || output->second.name!=net->driver.port ||
        output->second.type!=PORT_OUT || output->second.net!=net) return false;
    for (const auto &port : ctx->ports) if (port.second.net == net) return false;
    for (const auto &user : net->users) {
        if (!user.cell) return false;
        int clocks = 0;
        if (ctx->getPortTimingClass(user.cell,user.port,clocks) == TMG_CLOCK_INPUT) return false;
    }
    return true;
}
bool inspect(Context *ctx, CellInfo *cell, View &view, std::string &reason)
{
    auto fail = [&](const char *text) { reason = text; return false; };
    const unsigned width = width_of(cell);
    if (!ordinary_context(ctx) || !width || !movable(ctx,cell))
        return fail("requires an ordinary weak routed ALUT2..5");
    auto where = alm_of(ctx,cell);
    view.target=cell;view.width=width;view.alms={where};view.labs={where.first};view.output=cell->getPort(id_Q);
    const auto &alm = ctx->labs.at(where.first).alms.at(where.second);
    if (alm.l6_mode || alm.carry_mode || cell->combInfo.is_carry || cell->combInfo.is_extended ||
        cell->combInfo.lut_input_count != int(width) || cell->combInfo.used_lut_input_count != int(width) ||
        cell->combInfo.lut_bits_count != int(1u<<width)) return fail("requires an ordinary complete native LUT mode");
    for (auto bel : alm.lut_bels) if (auto *occupant=ctx->getBoundBelCell(bel))
        if (!width_of(occupant) || !movable(ctx,occupant)) return fail("protected or unsupported LUT mate");
    for (size_t i=0;i<alm.ff_bels.size();++i) if (auto *ff=ctx->getBoundBelCell(alm.ff_bels[i])) {
        auto *lut=ctx->getBoundBelCell(alm.lut_bels.at(i/2));
        if (!lut || ff->type!=id_MISTRAL_FF || !movable(ctx,ff) || ff->getPort(id_SDATA) ||
            !ff->getPort(id_DATAIN) || ff->getPort(id_DATAIN)!=lut->getPort(id_Q))
            return fail("cohabiting FF requires local COMBOUT DATAIN and no SDATA");
    }
    if (!legal_labs(ctx,view.labs)) return fail("original LAB is illegal");
    if (cell->ports.size()!=width+1 || cell->pin_data.size()!=width+1 || cell->params.size()!=1 ||
        !cell->params.count(id_LUT) || cell->params.at(id_LUT).is_string ||
        !cell->params.at(id_LUT).is_fully_def() || cell->params.at(id_LUT).size()!=(1u<<width) ||
        !cell->ports.count(id_Q) || cell->ports.at(id_Q).name!=id_Q || cell->ports.at(id_Q).type!=PORT_OUT ||
        !ordinary_net(ctx,view.output) || view.output->driver.cell!=cell || view.output->driver.port!=id_Q ||
        cell->get_pin_state(id_Q)!=PIN_SIG || !cell->pin_data.count(id_Q) ||
        cell->pin_data.at(id_Q).bel_pins!=std::vector<IdString>{id_COMBOUT} || cell->combInfo.comb_out!=view.output)
        return fail("unsupported exact LUT table/output");
    view.mask=uint64_t(cell->params.at(id_LUT).as_int64());
    std::set<NetInfo *> distinct;
    for (unsigned p=0;p<width;++p) {
        auto port=cell->ports.find(pins[p]);
        if (port==cell->ports.end() || port->second.name!=pins[p] || port->second.type!=PORT_IN ||
            !ordinary_net(ctx,port->second.net) || port->second.net==view.output ||
            !distinct.insert(port->second.net).second) return fail("requires distinct ordinary source nets");
        auto *net=port->second.net;auto *driver=net->driver.cell;
        if (!movable(ctx,driver) || driver->getPort(net->driver.port)!=net ||
            !(width_of(driver) || driver->type.in(id_MISTRAL_ALUT6,id_MISTRAL_BUF,id_MISTRAL_ALUT_ARITH,id_MISTRAL_FF,id_MISTRAL_M10K)))
            return fail("protected or unsupported source driver");
        const auto state=cell->get_pin_state(pins[p]);
        if ((state!=PIN_SIG && state!=PIN_INV) || !cell->pin_data.count(pins[p]) ||
            cell->combInfo.lut_in[p]!=net || !net->users.count(port->second.user_idx) ||
            net->users.at(port->second.user_idx).cell!=cell || net->users.at(port->second.user_idx).port!=pins[p])
            return fail("unsupported source state or indexed user identity");
        view.signals.push_back(net);view.slots.push_back(port->second.user_idx);view.states.push_back(state);
    }
    auto nets=view.signals;nets.push_back(view.output);
    for (auto *net:nets) if (!valid_tree(ctx,net,reason)) return false;
    return physical_function(ctx,cell) || fail("original native LUT function is invalid");
}
bool logical_function(const View &view)
{
    auto *cell=view.target;
    if (cell->ports.size()!=view.width+1 || cell->pin_data.size()!=view.width+1 || cell->params.size()!=1 ||
        !cell->params.count(id_LUT) || cell->params.at(id_LUT).is_string ||
        !cell->params.at(id_LUT).is_fully_def() || cell->params.at(id_LUT).size()!=(1u<<view.width) ||
        (!cell->ports.count(id_Q) || cell->ports.at(id_Q).name!=id_Q || cell->ports.at(id_Q).type!=PORT_OUT) ||
        cell->getPort(id_Q)!=view.output || cell->get_pin_state(id_Q)!=PIN_SIG ||
        !cell->pin_data.count(id_Q) || cell->pin_data.at(id_Q).bel_pins!=std::vector<IdString>{id_COMBOUT} ||
        cell->combInfo.comb_out!=view.output || cell->combInfo.lut_input_count!=int(view.width) ||
        cell->combInfo.used_lut_input_count!=int(view.width) || cell->combInfo.lut_bits_count!=int(1u<<view.width)) return false;
    std::vector<unsigned> source;
    std::set<NetInfo *> distinct;
    for (unsigned p=0;p<view.width;++p) {
        auto port=cell->ports.find(pins[p]);
        if (port==cell->ports.end() || port->second.name!=pins[p] || port->second.type!=PORT_IN ||
            !distinct.insert(port->second.net).second) return false;
        auto at=std::find(view.signals.begin(),view.signals.end(),port->second.net);
        if (at==view.signals.end()) return false;
        const unsigned original=unsigned(at-view.signals.begin());
        auto *net=*at;const auto state=cell->get_pin_state(pins[p]);
        if (port->second.user_idx!=view.slots[original] || !net->users.count(port->second.user_idx) ||
            net->users.at(port->second.user_idx).cell!=cell || net->users.at(port->second.user_idx).port!=pins[p] ||
            (state!=PIN_SIG && state!=PIN_INV) || cell->combInfo.lut_in[p]!=net) return false;
        source.push_back(original);
    }
    const uint64_t mask=uint64_t(cell->params.at(id_LUT).as_int64());
    for (unsigned row=0;row<(1u<<view.width);++row) {
        unsigned old_index=0,new_index=0;
        for (unsigned p=0;p<view.width;++p) {
            old_index|=unsigned(bool((row>>p)&1) != (view.states[p]==PIN_INV))<<p;
            new_index|=unsigned(bool((row>>source[p])&1) != (cell->get_pin_state(pins[p])==PIN_INV))<<p;
        }
        if (((view.mask>>old_index)&1)!=((mask>>new_index)&1)) return false;
    }
    return true;
}
bool same_lab(const LABInfo &a, const LABInfo &b)
{
    if (a.is_mlab != b.is_mlab || a.clk_wires != b.clk_wires || a.ena_wires != b.ena_wires ||
        a.aclr_wires != b.aclr_wires || a.sclr_wire != b.sclr_wire || a.sload_wire != b.sload_wire ||
        a.aclr_used != b.aclr_used) return false;
    for (size_t i = 0; i < a.alms.size(); ++i) {
        const auto &x = a.alms[i], &y = b.alms[i];
        if (x.comb_out != y.comb_out || x.sel_clk != y.sel_clk || x.sel_ena != y.sel_ena ||
            x.sel_aclr != y.sel_aclr || x.sel_ef != y.sel_ef || x.ff_in != y.ff_in || x.ff_out != y.ff_out ||
            x.lut_bels != y.lut_bels || x.ff_bels != y.ff_bels || x.l6_mode != y.l6_mode ||
            x.carry_mode != y.carry_mode || x.clk_ena_idx != y.clk_ena_idx || x.aclr_idx != y.aclr_idx ||
            x.unique_input_count != y.unique_input_count) return false;
    }
    return true;
}
// All actual configuration fields of the affected LABs, except the one ALM mask
// intentionally rewritten. This includes FF input/control selectors and modes.
using Config = std::map<std::tuple<int,int,int,int>, std::tuple<int,bool,uint32_t,std::vector<uint8_t>>>;
Config static_config(Context *ctx, const View &view)
{
    using CV = mistral::CycloneV;
    std::set<std::pair<int,int>> tiles;
    std::set<std::tuple<int,int,int>> masks;
    for (auto where : view.alms) {
        const auto &lab = ctx->labs.at(where.first);
        auto bel = lab.alms.at(where.second).lut_bels[0];
        int type = lab.is_mlab ? CV::MLAB : CV::LAB;
        tiles.emplace(type,bel.pos.v); masks.emplace(type,bel.pos.v,int(where.second));
    }
    Config result;
    for (const auto &value : ctx->cyclonev->bmux_get()) {
        if (!tiles.count({int(value.btype),value.pos.v})) continue;
        if (value.mux == CV::LUT_MASK && masks.count({int(value.btype),value.pos.v,value.midx})) continue;
        result.emplace(std::make_tuple(int(value.btype),value.pos.v,int(value.mux),value.midx),
                       std::make_tuple(value.type,value.def,value.s,value.r));
    }
    return result;
}
struct Transaction {
    struct Cell {
        CellInfo *cell; IdString type; BelId bel; PlaceStrength strength;
        decltype(CellInfo::ports) ports; decltype(CellInfo::params) params; decltype(CellInfo::attrs) attrs;
        ArchCellInfo architecture;
    };
    struct Net {
        NetInfo *net; indexed_store<PortRef> users; Route route;
        std::map<store_index<PortRef>,std::vector<WireId>> sinks;
        std::set<WireId> retained; bool changed = false;
    };
    Context *ctx; const View &view;
    std::vector<Cell> cells;
    std::vector<Net> nets;
    std::vector<LABInfo> labs;
    std::map<Alm,uint64_t> masks;
    Config config;
    bool dirty = false;
    Transaction(Context *ctx, const View &view) : ctx(ctx), view(view), labs(ctx->labs), config(static_config(ctx,view))
    {
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            if (cell->bel == BelId() || !(ctx->is_comb_cell(cell->type) || cell->type == id_MISTRAL_FF) ||
                !view.labs.count(alm_of(ctx,cell).first)) continue;
            cells.push_back({cell,cell->type,cell->bel,cell->belStrength,cell->ports,cell->params,cell->attrs,*static_cast<ArchCellInfo *>(cell)});
        }
        for (auto where : view.alms) masks.emplace(where,ctx->compute_lut_mask(where.first,where.second));
        auto signals = view.signals; signals.push_back(view.output);
        std::sort(signals.begin(),signals.end(),[&](auto *a,auto *b) { return a->name.str(ctx)<b->name.str(ctx); });
        for (auto *net : signals) {
            Net saved{net,net->users,route_tree(net),{}, {}, false};
            for (const auto &user : net->users) {
                auto sinks = sink_wires(ctx,net,user);
                saved.sinks.emplace(user.cell->ports.at(user.port).user_idx,std::move(sinks));
            }
            nets.push_back(std::move(saved));
        }
    }
    void restore()
    {
        if (!dirty) return;
        for (auto &saved : nets) if (saved.changed) ctx->ripupNet(saved.net->name);
        for (auto &saved : cells) {
            saved.cell->type=saved.type; saved.cell->ports=saved.ports; saved.cell->params=saved.params; saved.cell->attrs=saved.attrs;
            *static_cast<ArchCellInfo *>(saved.cell)=saved.architecture;
        }
        for (auto &saved : nets) saved.net->users=indexed_store<PortRef>(saved.users);
        ctx->labs=labs;
        for (const auto &saved : nets) if (saved.changed) {
            for (const auto &wire : saved.route) if (wire.second.first==PipId()) ctx->bindWire(wire.first,saved.net,wire.second.second);
            for (const auto &wire : saved.route) if (wire.second.first!=PipId()) ctx->bindPip(wire.second.first,saved.net,wire.second.second);
        }
        ctx->configure_bitstream();
        dirty=false;
    }
    // Keep only original direct COMBOUT exit edges that encode protected-LAB
    // output selectors. No new wire, PIP, strength or route ordering is chosen.
    bool retain_source_exits(Net &saved, std::string &reason)
    {
        using CV = mistral::CycloneV;
        auto fail = [&](const char *text) { reason=text; return false; };
        auto *driver=saved.net->driver.cell;
        if (!(width_of(driver) || driver->type.in(id_MISTRAL_ALUT6,id_MISTRAL_BUF,id_MISTRAL_ALUT_ARITH)))
            return true;
        const auto where=alm_of(ctx,driver);
        if (!view.labs.count(where.first)) return true;
        const auto &lab=ctx->labs.at(where.first);
        const auto &alm=lab.alms.at(where.second);
        const unsigned half=ctx->bel_data(driver->bel).lab_data.idx;
        if (half>1) return fail("invalid source-exit ALM half");
        const auto root=ctx->getNetinfoSourceWire(saved.net);
        if (root!=alm.comb_out.at(half)) return true; // e.g. an arithmetic carry output
        const std::array<CV::port_type_t,6> ports{CV::FFT0,CV::FFT1,CV::FFT1L,CV::FFB0,CV::FFB1,CV::FFB1L};
        const std::array<CV::bmux_type_t,6> muxes{CV::TDFF0,CV::TDFF1,CV::TDFF1L,CV::BDFF0,CV::BDFF1,CV::BDFF1L};
        const auto type=lab.is_mlab?CV::MLAB:CV::LAB;
        const auto pos=driver->bel.pos;
        for (unsigned i=3*half;i<3*half+3;++i) {
            const auto exit=ctx->get_port(type,pos.x(),pos.y(),where.second,ports[i]);
            auto old=saved.route.find(exit);
            if (old==saved.route.end()) continue;
            const auto pip=old->second.first;
            if (pip==PipId() || ctx->getPipSrcWire(pip)!=root || ctx->getPipDstWire(pip)!=exit ||
                old->second.second>STRENGTH_STRONG || ctx->getBoundWireNet(exit)!=saved.net ||
                ctx->getBoundPipNet(pip)!=saved.net)
                return fail("original source-exit PIP/wire ownership or strength changed");
            CV::bmux_setting_t setting;
            if (!ctx->cyclonev->bmux_get(type,pos,muxes[i],where.second,setting) ||
                setting.type!=CV::MT_MUX || setting.s!=CV::NLUT)
                return fail("original source-exit native selector is not NLUT");
            saved.retained.insert(exit);
        }
        return true;
    }
    bool valid(bool configured, std::string &reason)
    {
        auto fail=[&](const char *text) { reason=text; return false; };
        if (ctx->labs.size()!=labs.size() || !legal_labs(ctx,view.labs)) return fail("LUT permutation LAB legality changed");
        for (size_t i=0;i<labs.size();++i) if (!same_lab(labs[i],ctx->labs[i])) return fail("LUT permutation LAB modes/counts/control selectors changed");
        for (const auto &saved : cells) {
            auto *cell=saved.cell;
            if (cell->bel!=saved.bel || cell->belStrength!=saved.strength || cell->type!=saved.type || cell->attrs!=saved.attrs)
                return fail("LUT permutation occupant placement/type/attributes changed");
            if (cell==view.target) continue;
            if (cell->params!=saved.params || !same_pins(cell,saved.architecture.pin_data)) return fail("stationary occupant mask or native pins changed");
            if (cell->ports.size()!=saved.ports.size()) return fail("stationary occupant ports changed");
            for (const auto &port : saved.ports) {
                auto found=cell->ports.find(port.first);
                if (found==cell->ports.end() || found->second.net!=port.second.net || found->second.user_idx!=port.second.user_idx ||
                    found->second.type!=port.second.type || found->second.name!=port.second.name) return fail("stationary occupant port binding changed");
            }
            if (cell->type==id_MISTRAL_FF && (cell->ffInfo.ctrlset!=saved.architecture.ffInfo.ctrlset ||
                cell->ffInfo.datain!=saved.architecture.ffInfo.datain || cell->ffInfo.sdata!=saved.architecture.ffInfo.sdata))
                return fail("stationary FF control/data state changed");
            if (cell->type!=id_MISTRAL_FF) {
                const auto &x=cell->combInfo,&y=saved.architecture.combInfo;
                if (x.lut_in!=y.lut_in || x.comb_out!=y.comb_out || x.lut_input_count!=y.lut_input_count ||
                    x.used_lut_input_count!=y.used_lut_input_count || x.lut_bits_count!=y.lut_bits_count ||
                    x.chain_shared_input_count!=y.chain_shared_input_count || x.is_carry!=y.is_carry ||
                    x.is_shared!=y.is_shared || x.is_extended!=y.is_extended || x.carry_start!=y.carry_start ||
                    x.carry_end!=y.carry_end || x.mlab_group!=y.mlab_group || x.wclk!=y.wclk || x.we!=y.we)
                    return fail("stationary combinational architecture state changed");
            }
            auto where=alm_of(ctx,cell);
            if (view.alms.count(where) && cell->type!=id_MISTRAL_FF &&
                ((ctx->compute_lut_mask(where.first,where.second)^masks.at(where))&half_mask(ctx,cell)))
                return fail("stationary mate physical half mask changed");
        }
        if (!logical_function(view) || !physical_function(ctx,view.target)) return fail("rewritten logical/native LUT function mismatch");
        if (configured && static_config(ctx,view)!=config) return fail("configured stationary LAB/FF selector fields changed");
        return true;
    }
};
} // namespace

bool critical_routed_lut_permutation_trial(Context *ctx, CellInfo *target, IdString first, IdString second,
    int visit_budget, const std::function<bool()> &accept, std::string *reason,
    const std::function<void(unsigned)> &stage, bool keep_source_exits)
{
    View view; std::string why;
    auto fail=[&](const char *text) { if(reason)*reason=text; return false; };
    if (visit_budget<=0 || visit_budget>500000) return fail("local route node budget must be 1..500000");
    if (!inspect(ctx,target,view,why)) { if(reason)*reason=why; return false; }
    auto a=std::find(pins.begin(),pins.begin()+view.width,first);
    auto b=std::find(pins.begin(),pins.begin()+view.width,second);
    auto mask=routed_lut_permutation_policy::transpose(view.mask,view.width,unsigned(a-pins.begin()),unsigned(b-pins.begin()));
    if (!mask) return fail("invalid ordinary LUT transposition");
    Transaction tx(ctx,view);
    if (keep_source_exits) for (auto &saved : tx.nets) {
        if (saved.net==view.output) continue;
        if (!tx.retain_source_exits(saved,why)) { if(reason)*reason=why; return false; }
    }
    auto reject=[&](const std::string &text) { tx.restore(); if(reason)*reason=text; return false; };
    try {
        tx.dirty=true;
        target->params[id_LUT]=Property(int64_t(*mask),1u<<view.width);
        auto &pa=target->ports.at(first),&pb=target->ports.at(second);
        std::swap(pa.net,pb.net);std::swap(pa.user_idx,pb.user_idx);
        pa.net->users.at(pa.user_idx)={target,first};pb.net->users.at(pb.user_idx)={target,second};
        std::swap(target->pin_data.at(first).state,target->pin_data.at(second).state);
        ctx->assign_comb_info(target);
        if(stage)stage(1);
        const auto where=*view.alms.begin();
        ctx->update_alm_input_count(where.first,where.second);
        ctx->assign_alm_lut_inputs(where.first,where.second);
        if (!tx.valid(false,why)) return reject(why);
        if(stage)stage(2);
        size_t changed=0;
        for (auto &saved : tx.nets) {
            const auto root=ctx->getNetinfoSourceWire(saved.net);
            if (!saved.route.count(root) || saved.route.at(root).first!=PipId())
                return reject("source root identity changed");
            saved.retained.insert(root);
            for (const auto &slot : saved.sinks) {
                if (!saved.net->users.count(slot.first)) return reject("rewritten user-slot disappeared");
                auto sinks=sink_wires(ctx,saved.net,saved.net->users.at(slot.first));
                if (sinks.empty()) return reject("rewritten physical sink is unmapped");
                if (sinks!=slot.second) { saved.changed=true; continue; }
                for (auto sink : slot.second) for (auto cursor=sink;cursor!=root;) {
                    saved.retained.insert(cursor); auto at=saved.route.find(cursor);
                    if (at==saved.route.end() || at->second.first==PipId()) return reject("stationary source path disappeared");
                    cursor=ctx->getPipSrcWire(at->second.first);
                }
            }
            if (saved.changed) ++changed;
        }
        if (!changed || changed>view.width) return reject("no changed physical input sink or excessive footprint");
        for (const auto &saved : tx.nets) if (saved.changed && saved.net==view.output)
            return reject("target output physical sink changed");
        for (auto &saved : tx.nets) if (saved.changed) {
            ctx->ripupNet(saved.net->name);
            for (const auto &wire : saved.route) if (saved.retained.count(wire.first) && wire.second.first==PipId())
                ctx->bindWire(wire.first,saved.net,wire.second.second);
            for (const auto &wire : saved.route) if (saved.retained.count(wire.first) && wire.second.first!=PipId())
                ctx->bindPip(wire.second.first,saved.net,wire.second.second);
        }
        if(stage)stage(3);
        unsigned route_index=0;
        for (auto &saved : tx.nets) if (saved.changed) {
            if (!critical_local_route_net(ctx,saved.net,visit_budget)) return reject("local input-net routing failed or exhausted node budget: "+saved.net->name.str(ctx));
            if(stage)stage(10+route_index++);
        }
        ctx->configure_bitstream(); if(stage)stage(20);
        if (!tx.valid(true,why)) return reject(why);
        for (const auto &saved : tx.nets) {
            auto now=route_tree(saved.net);
            if (!saved.changed && now!=saved.route) return reject("unchanged LUT permutation route changed");
            for (auto wire : saved.retained) if (!now.count(wire) || now.at(wire)!=saved.route.at(wire))
                return reject("stationary physical path/strength changed");
            if (!valid_tree(ctx,saved.net,why)) return reject(why);
        }
        ctx->check(); if(stage)stage(21);
        if (!accept()) return reject(reason && !reason->empty()?*reason:"native inspection rejected");
        tx.dirty=false;
        return true;
    } catch (...) { tx.restore(); throw; }
}

void Arch::repair_critical_lut_permutations()
{
    if (!critical_lut_permute_budget) return;
    Context *ctx=getCtx();
    if (critical_lut_permute_budget<0 || critical_lut_permute_budget>32 || !ordinary_context(ctx))
        log_error("Critical LUT permutation requires an ordinary completed route and budget 1..32.\n");
    configure_bitstream();
    auto analyse=[&](bool skew) {auto t=std::make_unique<TimingAnalyser>(ctx);t->with_clock_skew=skew;t->setup(false,false,true);return t;};
    auto before=analyse(true),reference=analyse(false);
    if (before->have_loops || reference->have_loops) log_error("Critical LUT permutation refuses timing loops.\n");
    auto inventory=critical_swap_live_endpoint_snapshot(ctx,*before,*reference);
    size_t failing=0;
    for (const auto &endpoint : inventory["endpoints"].object_items()) for (const auto &row : endpoint.second.array_items())
        if (row["setup_timed"].bool_value() && row["setup"].is_number() && row["setup"].number_value()<0) ++failing;
    auto paths=before->get_report_setup_paths(int(std::max<size_t>(2,std::min<size_t>(16384,failing+1))));
    for (const auto &path : before->get_timing_result().clock_paths) paths.push_back(path.second);
    for (const auto &path : before->get_timing_result().xclock_paths) paths.push_back(path);
    struct Witness { CellInfo *cell=nullptr; int64_t slack=0; std::set<IdString> inputs; };
    std::map<std::string,Witness> witnessed;
    size_t negative_paths=0;
    for (const auto &path : paths) {
        if (path.segments.empty() || path.segments.back().type!=CriticalPath::Segment::Type::SETUP || path.max_delay<=0) continue;
        int64_t total=0;bool finite=true;
        for (const auto &segment : path.segments) {
            if(!std::isfinite(double(segment.delay))) {finite=false;break;}
            total+=int64_t(segment.delay);
        }
        const int64_t slack=int64_t(path.max_delay)-total;
        if (!finite || slack>=0) continue;
        ++negative_paths;
        for (const auto &segment : path.segments) {
            if (segment.type!=CriticalPath::Segment::Type::LOGIC || segment.from.first!=segment.to.first || segment.to.second!=id_Q) continue;
            auto found=ctx->cells.find(segment.from.first);
            if (found==ctx->cells.end() || !width_of(found->second.get())) continue;
            auto *cell=found->second.get();const auto width=width_of(cell);
            if(std::find(pins.begin(),pins.begin()+width,segment.from.second)==pins.begin()+width)continue;
            const auto name=cell->name.str(ctx);
            if (!witnessed.count(name) && witnessed.size()>=64) continue;
            auto &entry=witnessed[name];entry.cell=cell;entry.slack=std::min(entry.slack,slack);entry.inputs.insert(segment.from.second);
        }
    }
    struct Choice {CellInfo *cell;IdString first,second;delay_t arc_gain;int64_t slack;};
    struct Group {std::string name;int64_t slack;std::vector<Choice> choices;};
    std::vector<Group> groups;
    auto coverage=[&](const View &view,TimingAnalyser &timing,std::string &why) {
        if (!critical_lut_permutation_source_coverage(ctx,timing,view.target,&why)) return false;
        auto output_coverage = ctx->critical_unobserved_arith_outputs
            ? critical_output_coverage_with_unobserved_arithmetic : critical_lut6_swap_output_coverage;
        for (auto *net : view.signals) if (!output_coverage(ctx,timing,net,&why)) return false;
        return output_coverage(ctx,timing,view.output,&why);
    };
    for (const auto &entry : witnessed) {
        View view;std::string why;
        if (!inspect(ctx,entry.second.cell,view,why) || !coverage(view,*before,why) || !coverage(view,*reference,why)) {
            log_info("Critical LUT permutation filter: cell=%s: %s.\n",entry.first.c_str(),why.c_str());continue;
        }
        Group group{entry.first,entry.second.slack,{}};
        std::set<std::pair<IdString,IdString>> pairs;
        for(auto first:entry.second.inputs)for(unsigned i=0;i<view.width;++i) {
            const auto second=pins[i];
            if(first==second || !pairs.insert(std::minmax(first,second)).second)continue;
            DelayQuad old_arc,new_arc;
            if(!ctx->getCellDelay(view.target,first,id_Q,old_arc) || !ctx->getCellDelay(view.target,second,id_Q,new_arc) ||
               !std::isfinite(double(old_arc.maxDelay())) || !std::isfinite(double(new_arc.maxDelay())))continue;
            // Current native terminal delays rank proposals only. The allocator
            // and actual local routing determine every retained timing change.
            group.choices.push_back({view.target,first,second,old_arc.maxDelay()-new_arc.maxDelay(),entry.second.slack});
        }
        std::sort(group.choices.begin(),group.choices.end(),[](const Choice &a,const Choice &b) {
            if(a.arc_gain!=b.arc_gain)return a.arc_gain>b.arc_gain;
            return std::tie(a.first,a.second)<std::tie(b.first,b.second);
        });
        if(!group.choices.empty())groups.push_back(std::move(group));
    }
    std::sort(groups.begin(),groups.end(),[](const Group &a,const Group &b){return std::tie(a.slack,a.name)<std::tie(b.slack,b.name);});
    log_info("Critical LUT permutation discovery: negative rows=%zu paths=%zu bounded cell witnesses=%zu eligible groups=%zu; native path cap=16384, cell cap=64.\n",failing,negative_paths,witnessed.size(),groups.size());
    auto clock_deltas=[&](const char *kind,const TimingResult &old,const TimingResult &now) {
        for(const auto &clock:old.clock_setup_slack) {
            auto next=now.clock_setup_slack.find(clock.first);
            if(next==now.clock_setup_slack.end() || next->second!=clock.second)
                log_info("Critical LUT permutation %s setup: clock=%s before=%dps after=%s.\n",kind,clock.first.c_str(ctx),int(clock.second),next==now.clock_setup_slack.end()?"missing":(std::to_string(int(next->second))+"ps").c_str());
        }
        for(const auto &clock:old.clock_hold_slack) {
            auto next=now.clock_hold_slack.find(clock.first);
            if(next==now.clock_hold_slack.end() || next->second!=clock.second)
                log_info("Critical LUT permutation %s hold: clock=%s before=%dps after=%s.\n",kind,clock.first.c_str(ctx),int(clock.second),next==now.clock_hold_slack.end()?"missing":(std::to_string(int(next->second))+"ps").c_str());
        }
        for(const auto &clock:old.clock_fmax) {
            auto next=now.clock_fmax.find(clock.first);
            if(next==now.clock_fmax.end() || next->second.achieved!=clock.second.achieved || next->second.constraint!=clock.second.constraint)
                log_info("Critical LUT permutation %s Fmax: clock=%s before=%.6fMHz after=%.6fMHz constraint=%.6f/%.6fMHz.\n",kind,clock.first.c_str(ctx),clock.second.achieved,next==now.clock_fmax.end()?-1.0:next->second.achieved,clock.second.constraint,next==now.clock_fmax.end()?-1.0:next->second.constraint);
        }
    };
    int tried=0,kept=0;std::set<CellInfo *> changed;
    for(size_t rank=0;rank<10 && tried<critical_lut_permute_budget;++rank) for(const auto &group:groups) {
        if(rank>=group.choices.size() || changed.count(group.choices[rank].cell))continue;
        if(tried>=critical_lut_permute_budget)break;
        const auto &choice=group.choices[rank];View view;std::string reason;
        // Previous retention can change a source cone: revalidate the current
        // native source and every stationary output branch before mutation.
        if(!inspect(ctx,choice.cell,view,reason) || !coverage(view,*before,reason) || !coverage(view,*reference,reason)) {
            log_info("Critical LUT permutation current eligibility: cell=%s: %s.\n",group.name.c_str(),reason.c_str());continue;
        }
        ++tried;std::unique_ptr<TimingAnalyser> after,next_reference;json11::Json next_inventory;
        bool accepted=critical_routed_lut_permutation_trial(ctx,choice.cell,choice.first,choice.second,500000,[&]() {
            after=analyse(true);next_reference=analyse(false);
            if(after->have_loops || next_reference->have_loops) {reason="timing loop";return false;}
            clock_deltas("with-skew",before->get_timing_result(),after->get_timing_result());
            clock_deltas("reference",reference->get_timing_result(),next_reference->get_timing_result());
            if(!coverage(view,*after,reason) || !coverage(view,*next_reference,reason))return false;
            if(!private_pair_clock_safe(before->get_timing_result(),after->get_timing_result(),&reason) ||
               !private_pair_clock_safe(reference->get_timing_result(),next_reference->get_timing_result(),&reason))return false;
            next_inventory=critical_swap_live_endpoint_snapshot(ctx,*after,*next_reference);
            return critical_route_snapshot_safe(inventory,next_inventory,&reason);
        },&reason,{},critical_lut_permute_retain_source_exits);
        log_info("Critical LUT permutation trial %d: cell=%s pins=%s/%s prior-slack=%lldps native-arc-rank=%dps accepted=%d; %s.\n",tried,group.name.c_str(),choice.first.c_str(ctx),choice.second.c_str(ctx),static_cast<long long>(choice.slack),int(choice.arc_gain),int(accepted),reason.c_str());
        if(accepted){before=std::move(after);reference=std::move(next_reference);inventory=std::move(next_inventory);changed.insert(choice.cell);++kept;}
    }
    log_info("Critical LUT permutation: tried=%d/%d kept=%d; unchanged unsupported coverage is not qualification.\n",tried,critical_lut_permute_budget,kept);
}
NEXTPNR_NAMESPACE_END
