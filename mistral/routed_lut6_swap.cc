/* Bounded symmetric LUT6 input swaps after native routing. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "analogue_repair.h"
#include "critical_cohort.h"
#include "private_pair_decomposition_native.h"
#include "routed_lut6_swap_policy.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
const std::array<IdString, 6> pins{id_A,id_B,id_C,id_D,id_E,id_F};
bool protected_attrs(Context *ctx, const dict<IdString,Property> &attrs)
{
    for (const auto &entry : attrs) {
        const auto name = entry.first.str(ctx);
        if (name == "keep" || name == "dont_touch" || name == "BEL" || name == "FES_SLOT") return true;
    }
    return false;
}
bool ordinary_cell(Context *ctx, CellInfo *cell)
{
    return cell && cell->bel != BelId() && cell->belStrength <= STRENGTH_WEAK && !cell->isPseudo() &&
           !cell->region && cell->cluster == ClusterId() && cell->constr_children.empty() &&
           !cell->constr_x && !cell->constr_y && !cell->constr_z && !cell->constr_abs_z &&
           !protected_attrs(ctx,cell->attrs);
}
bool ordinary_context(Context *ctx)
{
    return !ctx->fes_any_slot_region_active && !ctx->fes_fence_active && !ctx->fes_has_reserved_rect &&
           ctx->fes_frozen_cells.empty() && ctx->fes_frozen_pips.empty() &&
           ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() == "route";
}
bool same_pins(const CellInfo *cell, const ArchCellInfo &old)
{
    if (cell->pin_data.size() != old.pin_data.size()) return false;
    for (const auto &pin : old.pin_data) {
        auto found=cell->pin_data.find(pin.first);
        if(found==cell->pin_data.end() || pin.second.state!=found->second.state ||
           pin.second.bel_pins!=found->second.bel_pins) return false;
    }
    return true;
}
bool same_lab(const LABInfo &a,const LABInfo &b)
{
    if(a.is_mlab!=b.is_mlab || a.clk_wires!=b.clk_wires || a.ena_wires!=b.ena_wires ||
       a.aclr_wires!=b.aclr_wires || a.sclr_wire!=b.sclr_wire || a.sload_wire!=b.sload_wire ||
       a.aclr_used!=b.aclr_used) return false;
    for(size_t i=0;i<a.alms.size();++i) {
        const auto &x=a.alms[i],&y=b.alms[i];
        if(x.comb_out!=y.comb_out || x.sel_clk!=y.sel_clk || x.sel_ena!=y.sel_ena ||
           x.sel_aclr!=y.sel_aclr || x.sel_ef!=y.sel_ef || x.ff_in!=y.ff_in || x.ff_out!=y.ff_out ||
           x.lut_bels!=y.lut_bels || x.ff_bels!=y.ff_bels || x.l6_mode!=y.l6_mode ||
           x.carry_mode!=y.carry_mode || x.clk_ena_idx!=y.clk_ena_idx || x.aclr_idx!=y.aclr_idx ||
           x.unique_input_count!=y.unique_input_count) return false;
    }
    return true;
}
bool legal_lab(Context *ctx,uint32_t lab)
{
    for(const auto &alm:ctx->labs.at(lab).alms) {
        for(auto bel:alm.lut_bels) if(ctx->getBoundBelCell(bel) && !ctx->isBelLocationValid(bel)) return false;
        for(auto bel:alm.ff_bels) if(ctx->getBoundBelCell(bel) && !ctx->isBelLocationValid(bel)) return false;
    }
    return true;
}
bool ordinary_net(Context *ctx,NetInfo *net)
{
    if(!net || !net->driver.cell || net->region || net->is_global || net->clkconstr ||
       net->constant_value!=IdString() || net->wires.empty() || protected_attrs(ctx,net->attrs)) return false;
    for(const auto &port:ctx->ports) if(port.second.net==net) return false;
    return true;
}
bool inspect(Context *ctx,CellInfo *cell,IdString first,IdString second,std::string &reason)
{
    auto fail=[&](const char *why) {reason=why;return false;};
    if(!ordinary_context(ctx) || !ordinary_cell(ctx,cell) || cell->type!=id_MISTRAL_ALUT6)
        return fail("requires an ordinary weak unprotected routed LUT6");
    auto a=std::find(pins.begin(),pins.end(),first),b=std::find(pins.begin(),pins.end(),second);
    if(a==pins.end() || b==pins.end() || first==second) return fail("invalid distinct logical input pair");
    if(cell->ports.size()!=7 || cell->params.size()!=1 || !cell->params.count(id_LUT) ||
       cell->params.at(id_LUT).is_string || cell->params.at(id_LUT).size()!=64 ||
       !cell->params.at(id_LUT).is_fully_def()) return fail("requires an exact fully defined LUT6 mask");
    if(!routed_lut6_swap_policy::symmetric(uint64_t(cell->params.at(id_LUT).as_int64()),a-pins.begin(),b-pins.begin()))
        return fail("LUT mask is not symmetric under this transposition");
    const auto &where=ctx->bel_data(cell->bel).lab_data;
    const auto &alm=ctx->labs.at(where.lab).alms.at(where.alm);
    if(!alm.l6_mode || alm.carry_mode || cell->combInfo.is_carry || cell->combInfo.lut_input_count!=6 ||
       cell->combInfo.used_lut_input_count!=6 || cell->combInfo.lut_bits_count!=64 ||
       ctx->getBoundBelCell(alm.lut_bels.at(1-where.idx)))
        return fail("requires unchanged solitary LUT6 ALM mode");
    for(auto bel:alm.ff_bels) if(auto *ff=ctx->getBoundBelCell(bel))
        if(ff->type!=id_MISTRAL_FF || !ordinary_cell(ctx,ff)) return fail("protected or unsupported cohabiting register");
    if(!legal_lab(ctx,where.lab)) return fail("original LAB is illegal");
    const std::array<IdString,6> physical{id_A,id_B,id_C,id_D,where.idx?id_E1:id_E0,where.idx?id_F1:id_F0};
    std::set<NetInfo *> distinct;
    for(size_t p=0;p<pins.size();++p) {
        auto port=cell->ports.find(pins[p]);
        if(port==cell->ports.end() || port->second.name!=pins[p] || port->second.type!=PORT_IN ||
           !ordinary_net(ctx,port->second.net) || !distinct.insert(port->second.net).second ||
           cell->get_pin_state(pins[p])!=PIN_SIG) return fail("requires six distinct live ordinary PIN_SIG inputs");
        auto pin=cell->pin_data.find(pins[p]);
        if(pin==cell->pin_data.end() || pin->second.bel_pins!=std::vector<IdString>{physical[p]} ||
           cell->combInfo.lut_in[p]!=port->second.net)
            return fail("requires fixed full LUT6 physical pin occupancy");
        auto &users=port->second.net->users;
        if(!users.count(port->second.user_idx) || users.at(port->second.user_idx).cell!=cell ||
           users.at(port->second.user_idx).port!=pins[p]) return fail("input user-slot identity mismatch");
    }
    auto output=cell->ports.find(id_Q);
    if(output==cell->ports.end() || output->second.type!=PORT_OUT || !ordinary_net(ctx,output->second.net) ||
       output->second.net->driver.cell!=cell || output->second.net->driver.port!=id_Q ||
       cell->get_pin_state(id_Q)!=PIN_SIG || !cell->pin_data.count(id_Q) ||
       cell->pin_data.at(id_Q).bel_pins!=std::vector<IdString>{id_COMBOUT} ||
       cell->combInfo.comb_out!=output->second.net || distinct.count(output->second.net)) return fail("invalid ordinary LUT6 output");
    for(auto pin:{first,second}) {
        auto *net=cell->getPort(pin);auto *driver=net->driver.cell;
        if(net->users.entries()!=1) return fail("swapped input net is not single-sink");
        if(driver==cell || net->driver.port!=id_Q || driver->getPort(net->driver.port)!=net || !ordinary_cell(ctx,driver) ||
           !driver->type.in(id_MISTRAL_FF,id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,id_MISTRAL_ALUT4,id_MISTRAL_ALUT5,id_MISTRAL_ALUT6))
            return fail("protected or unsupported swapped-net driver");
        for(const auto &wire:net->wires) {
            if(wire.second.strength>STRENGTH_STRONG) return fail("protected swapped-net route");
            if(ctx->getBoundWireNet(wire.first)!=net ||
               (wire.second.pip!=PipId() && ctx->getBoundPipNet(wire.second.pip)!=net))
                return fail("swapped-net physical binding ownership mismatch");
        }
        auto root=ctx->getNetinfoSourceWire(net);
        if(!net->wires.count(root) || net->wires.at(root).pip!=PipId()) return fail("malformed swapped-net root");
        for(const auto &entry:net->wires) {
            std::set<WireId> seen;auto wire=entry.first;
            while(wire!=root) {
                auto at=net->wires.find(wire);
                if(!seen.insert(wire).second || at==net->wires.end() || at->second.pip==PipId() ||
                   ctx->getPipDstWire(at->second.pip)!=wire) return fail("malformed swapped-net tree");
                wire=ctx->getPipSrcWire(at->second.pip);
            }
        }
        for(auto sink:ctx->getNetinfoSinkWires(net,*net->users.begin()))
            if(!net->wires.count(sink)) return fail("unrouted swapped input sink");
    }
    return true;
}
struct SavedNet {
    NetInfo *net;
    indexed_store<PortRef> users;
    std::vector<std::tuple<WireId,PipId,PlaceStrength>> wires;
};
}

static bool lut_source_coverage(Context *ctx, TimingAnalyser &timing, CellInfo *target,
                                bool supported_target, const char *requirement, std::string *reason)
{
    using Launch = std::pair<IdString, int>;
    using Launches = std::set<Launch>;
    using Relationship = std::tuple<IdString, int, IdString, int>;
    std::map<NetInfo *, Launches> done;
    std::set<NetInfo *> visiting;
    size_t work = 0;
    std::string failure;
    auto fail = [&](const char *text) { failure = text; return false; };
    auto finite = [](delay_t value) {
        return std::isfinite(double(value)) && value != std::numeric_limits<delay_t>::lowest() &&
               value != std::numeric_limits<delay_t>::max();
    };
    auto hints_match = [&](CellPortKey port, const Launches &expected) {
        std::vector<TimingAnalyser::SetupWindowHint> rows;
        if (expected.empty() || !timing.get_setup_window_hints(port, rows) || rows.empty())
            return fail("source cone lacks complete native launch relationships");
        Launches actual;
        std::set<Relationship> pairs;
        for (const auto &row : rows) {
            if (row.cut || !row.window || !finite(*row.window) || *row.window <= 0 ||
                !pairs.emplace(row.launch.clock, int(row.launch.edge), row.capture.clock, int(row.capture.edge)).second)
                return fail("source cone has cut, untimed or duplicate native relationships");
            actual.emplace(row.launch.clock, int(row.launch.edge));
        }
        return actual == expected || fail("structural and native source launch inventories differ");
    };
    auto constant = [&](NetInfo *net, int *value) {
        auto *cell = net->driver.cell;
        if (!cell || cell->type != id_MISTRAL_CONST || net->driver.port != id_Q ||
            cell->ports.size() != 1 || cell->getPort(id_Q) != net || !cell->params.count(id_LUT)) return false;
        const auto &mask = cell->params.at(id_LUT);
        if (mask.is_string || !mask.is_fully_def() || mask.size() == 0 || mask.size() > 64 ||
            (mask.as_int64() != 0 && mask.as_int64() != 1)) return false;
        *value = int(mask.as_int64());
        return true;
    };
    auto cone = [&](auto &&self, NetInfo *net, unsigned depth, Launches &result) -> bool {
        if (!net || depth > 64 || ++work > 4096) return fail("source cone exceeds depth/work bound");
        if (visiting.count(net)) return fail("source cone contains a cycle");
        if (done.count(net)) { result = done.at(net); return true; }
        if (net->is_global || net->clkconstr || !net->driver.cell)
            return fail("source cone has a global, clock or undriven data net");
        auto stored = ctx->nets.find(net->name);
        if (stored == ctx->nets.end() || stored->second.get() != net)
            return fail("source net identity is not in the live context");
        for (const auto &port : ctx->ports) if (port.second.net == net)
            return fail("source cone reaches a public boundary");
        auto *cell = net->driver.cell;
        auto owner = ctx->cells.find(cell->name);
        auto output = cell->ports.find(net->driver.port);
        if (owner == ctx->cells.end() || owner->second.get() != cell || output == cell->ports.end() ||
            output->second.name != net->driver.port || output->second.type != PORT_OUT || output->second.net != net ||
            cell->get_pin_state(net->driver.port) != PIN_SIG)
            return fail("source cone has a malformed output identity or state");
        for (const auto &user : net->users) {
            if (++work > 4096) return fail("source cone exceeds depth/work bound");
            if (!user.cell) return fail("source cone has a missing user");
            auto owner = ctx->cells.find(user.cell->name);
            auto pin = user.cell->ports.find(user.port);
            if (owner == ctx->cells.end() || owner->second.get() != user.cell || pin == user.cell->ports.end() ||
                pin->second.name != user.port || pin->second.type != PORT_IN || pin->second.net != net ||
                !net->users.count(pin->second.user_idx) || &net->users.at(pin->second.user_idx) != &user)
                return fail("source cone has a malformed indexed user identity");
        }
        int value = 0;
        if (constant(net, &value)) { done.emplace(net, Launches{}); result.clear(); return true; }
        visiting.insert(net);
        int clocks = 0;
        const auto kind = ctx->getPortTimingClass(cell, net->driver.port, clocks);
        Launches launches;
        if (cell->type.in(id_MISTRAL_FF, id_MISTRAL_M10K) && kind == TMG_REGISTER_OUTPUT && clocks > 0) {
            for (int i = 0; i < clocks; ++i) {
                const auto info = ctx->getPortClockingInfo(cell, net->driver.port, i);
                auto clock_port = cell->ports.find(info.clock_port);
                auto *clock = cell->getPort(info.clock_port);
                const auto clock_state = cell->get_pin_state(info.clock_port);
                if (!clock || (clock_state != PIN_SIG && clock_state != PIN_INV) ||
                    clock_port == cell->ports.end() || clock_port->second.name != info.clock_port ||
                    clock_port->second.type != PORT_IN || !clock->users.count(clock_port->second.user_idx) ||
                    clock->users.at(clock_port->second.user_idx).cell != cell ||
                    clock->users.at(clock_port->second.user_idx).port != info.clock_port ||
                    !ctx->nets.count(clock->name) || ctx->nets.at(clock->name).get() != clock || !clock->clkconstr ||
                    (info.edge != RISING_EDGE && info.edge != FALLING_EDGE) ||
                    !finite(info.clockToQ.minDelay()) || !finite(info.clockToQ.maxDelay()) ||
                    info.clockToQ.minDelay() > info.clockToQ.maxDelay() ||
                    !launches.emplace(clock->name, int(info.edge)).second)
                    return fail("source register has a missing or uncharacterized native clock");
            }
            for (const auto &pin : cell->ports) if (pin.second.type == PORT_IN) {
                DelayQuad delay;
                if (ctx->getCellDelay(cell, pin.first, net->driver.port, delay))
                    return fail("source register has a hybrid combinational output");
            }
        } else {
            const int width = cell->type == id_MISTRAL_ALUT6 ? 6 : cell->type == id_MISTRAL_ALUT5 ? 5 :
                              cell->type == id_MISTRAL_ALUT4 ? 4 : cell->type == id_MISTRAL_ALUT3 ? 3 :
                              cell->type == id_MISTRAL_ALUT2 ? 2 : cell->type == id_MISTRAL_BUF ? 1 : 0;
            const bool arithmetic = cell->type == id_MISTRAL_ALUT_ARITH;
            if ((!width && !arithmetic) || kind != TMG_COMB_OUTPUT ||
                (arithmetic ? !net->driver.port.in(id_SO, id_CO) : net->driver.port != id_Q))
                return fail("source cone reaches an unsupported or asynchronous origin");
            const std::vector<IdString> inputs = arithmetic ? std::vector<IdString>{id_A,id_B,id_C,id_D0,id_D1,id_CI} :
                                                             std::vector<IdString>(pins.begin(), pins.begin() + width);
            for (const auto &pin : cell->ports) {
                if (pin.second.type == PORT_IN && std::find(inputs.begin(), inputs.end(), pin.first) == inputs.end())
                    return fail("source combinational cell has an unknown input");
            }
            for (auto input : inputs) {
                DelayQuad delay;
                if (!ctx->getCellDelay(cell, input, net->driver.port, delay) ||
                    !finite(delay.minDelay()) || !finite(delay.maxDelay()) || delay.minDelay() > delay.maxDelay())
                    return fail("source cone has an uncharacterized combinational arc");
                const auto state = cell->get_pin_state(input);
                auto pin = cell->ports.find(input);
                auto *source = cell->getPort(input);
                if (pin != cell->ports.end() && (pin->second.name != input || pin->second.type != PORT_IN))
                    return fail("source combinational input direction is invalid");
                if (state == PIN_0 || state == PIN_1) {
                    if (source && (!constant(source, &value) || value != int(state == PIN_1)))
                        return fail("explicit source constant has a conflicting live net");
                    if (!source) continue;
                } else if (state != PIN_SIG && state != PIN_INV) {
                    return fail("source combinational input state is unsupported");
                }
                if (!source || pin == cell->ports.end() || !source->users.count(pin->second.user_idx) ||
                    source->users.at(pin->second.user_idx).cell != cell || source->users.at(pin->second.user_idx).port != input)
                    return fail("source combinational input is missing or malformed");
                Launches incoming;
                if (!self(self, source, depth + 1, incoming)) return false;
                if (!incoming.empty() && !hints_match(CellPortKey(cell->name, input), incoming)) return false;
                launches.insert(incoming.begin(), incoming.end());
            }
        }
        if (!hints_match(CellPortKey(net->driver), launches)) return false;
        visiting.erase(net);
        done.emplace(net, launches);
        result = std::move(launches);
        return true;
    };
    Launches launches;
    const bool complete = target && supported_target && !timing.setup_only && !timing.have_loops &&
                          cone(cone, target->getPort(id_Q), 0, launches);
    if (reason) *reason = complete ? "complete bounded structural/native source launch coverage" :
                                   (failure.empty() ? requirement : failure);
    return complete;
}

// Separate target scopes retain the original LUT6 admission boundary.
bool critical_lut6_swap_source_coverage(Context *ctx, TimingAnalyser &timing, CellInfo *target,
                                       std::string *reason)
{
    return lut_source_coverage(ctx,timing,target,target && target->type==id_MISTRAL_ALUT6,
                               "requires current full native LUT6 timing",reason);
}
bool critical_lut_permutation_source_coverage(Context *ctx, TimingAnalyser &timing, CellInfo *target,
                                             std::string *reason)
{
    return lut_source_coverage(ctx,timing,target,target && target->type.in(id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,
                               id_MISTRAL_ALUT4,id_MISTRAL_ALUT5),
                               "requires current full native ALUT2..5 timing",reason);
}

namespace {
bool output_coverage(Context *ctx, TimingAnalyser &timing, NetInfo *output, bool allow_unobserved_arithmetic,
                     std::string *reason)
{
    using ClockPairIdentity=std::tuple<IdString,int,IdString,int>;
    auto identity=[](const auto &row) {return ClockPairIdentity(row.launch.clock,int(row.launch.edge),row.capture.clock,int(row.capture.edge));};
    auto finite=[](delay_t value) {return std::isfinite(double(value)) && value!=std::numeric_limits<delay_t>::lowest() && value!=std::numeric_limits<delay_t>::max();};
    std::set<NetInfo *> visiting,done;size_t work=0;
    std::string failure;
    auto fail=[&](const char *text) {failure=text;return false;};
    // Only the optional path scans reverse references, lazily and once per walk.
    // Its independent bound admits large designs without spending the cone budget.
    bool references_scanned=false;
    std::map<NetInfo *,size_t> references;
    size_t captures=0;
    auto unobserved_arithmetic=[&](NetInfo *net,bool validated_arithmetic_arc) {
        if(!validated_arithmetic_arc || net->users.entries() || !net->wires.empty() ||
           net->is_global || net->clkconstr || net->region || net->constant_value!=IdString() ||
           protected_attrs(ctx,net->attrs))
            return fail("unused arithmetic output has an observed or protected boundary");
        auto stored=ctx->nets.find(net->name);
        auto *cell=net->driver.cell;
        if(stored==ctx->nets.end() || stored->second.get()!=net || !cell)
            return fail("unused arithmetic output is not a live net identity");
        auto owner=ctx->cells.find(cell->name);
        auto port=cell->ports.find(net->driver.port);
        if(owner==ctx->cells.end() || owner->second.get()!=cell || cell->type!=id_MISTRAL_ALUT_ARITH ||
           cell->isPseudo() || cell->region || protected_attrs(ctx,cell->attrs) ||
           !net->driver.port.in(id_SO,id_CO) || port==cell->ports.end() ||
           port->second.name!=net->driver.port || port->second.type!=PORT_OUT || port->second.net!=net ||
           cell->get_pin_state(net->driver.port)!=PIN_SIG)
            return fail("unused arithmetic output has a malformed native driver identity");
        int clocks=0;
        if(ctx->getPortTimingClass(cell,net->driver.port,clocks)!=TMG_COMB_OUTPUT || clocks)
            return fail("unused arithmetic output is not a native combinational output");
        for(const auto &entry:ctx->ports) if(entry.second.net==net)
            return fail("unused arithmetic output reaches a public port");
        if(!references_scanned) {
            size_t scanned=0;
            for(const auto &entry:ctx->cells) for(const auto &port:entry.second->ports) {
                if(++scanned>(1u<<20)) return fail("unused arithmetic reverse-reference scan exceeds bound");
                if(port.second.net) ++references[port.second.net];
            }
            references_scanned=true;
        }
        if(references[net]!=1)
            return fail("unused arithmetic output has an omitted or duplicate live port reference");
        return true;
    };
    auto cone=[&](auto &&self,NetInfo *net,unsigned depth,bool validated_arithmetic_arc)->bool {
        if(!net || depth>64 || ++work>4096) return fail("output cone exceeds depth/work bound");
        if(visiting.count(net))return fail("output cone contains a cycle");
        if(done.count(net))return true;
        if(allow_unobserved_arithmetic && !net->users.entries()) {
            if(!unobserved_arithmetic(net,validated_arithmetic_arc)) return false;
            done.insert(net);return true;
        }
        if(net->is_global || net->clkconstr || !net->driver.cell || !net->users.entries())
            return fail("output cone has an untimed or unused boundary");
        for(const auto &port:ctx->ports) if(port.second.net==net) return fail("output cone reaches a public port");
        visiting.insert(net);
        for(const auto &user:net->users) {
            auto *cell=user.cell;
            if(!cell || cell->getPort(user.port)!=net || !cell->ports.count(user.port) || cell->ports.at(user.port).type!=PORT_IN)
                return fail("output cone has a malformed user identity");
            int clocks=0;const auto kind=ctx->getPortTimingClass(cell,user.port,clocks);
            int width=cell->type==id_MISTRAL_ALUT6?6:cell->type==id_MISTRAL_ALUT5?5:
                      cell->type==id_MISTRAL_ALUT4?4:cell->type==id_MISTRAL_ALUT3?3:
                      cell->type==id_MISTRAL_ALUT2?2:cell->type==id_MISTRAL_BUF?1:0;
            bool arithmetic=cell->type==id_MISTRAL_ALUT_ARITH;
            if((width || arithmetic) && kind==TMG_COMB_INPUT) {
                if(width && std::find(pins.begin(),pins.begin()+width,user.port)==pins.begin()+width)
                    return fail("output cone has an unknown LUT/buffer input");
                if(arithmetic && !user.port.in(id_A,id_B,id_C,id_D0,id_D1,id_CI))
                    return fail("output cone has an unknown carry input");
                if(allow_unobserved_arithmetic && arithmetic) {
                    // A malformed output direction must not hide a connected sibling
                    // before the optional leaf predicate can inspect its identity.
                    for(const auto &port:cell->ports) if(port.first.in(id_SO,id_CO) && port.second.net &&
                       (port.second.name!=port.first || port.second.type!=PORT_OUT ||
                        port.second.net->driver.cell!=cell || port.second.net->driver.port!=port.first))
                        return fail("arithmetic output cone has a malformed connected sibling");
                }
                bool followed=false;
                for(const auto &port:cell->ports) if(port.second.type==PORT_OUT && port.second.net) {
                    if(arithmetic?!port.first.in(id_SO,id_CO):port.first!=id_Q)
                        return fail("output cone has an unknown combinational output");
                    DelayQuad delay;
                    if(!ctx->getCellDelay(cell,user.port,port.first,delay) ||
                       !finite(delay.minDelay()) || !finite(delay.maxDelay()) || delay.minDelay()>delay.maxDelay())
                        return fail("output cone has an uncharacterized combinational arc");
                    if(port.second.net->driver.cell!=cell || port.second.net->driver.port!=port.first)
                        return fail("output cone has a malformed output driver");
                    if(!self(self,port.second.net,depth+1,arithmetic))return false;
                    followed=true;
                }
                if(!followed)return fail("output cone has no modeled capture branch");
            } else if(cell->type.in(id_MISTRAL_FF,id_MISTRAL_M10K) && kind==TMG_REGISTER_INPUT && clocks>0) {
                if((cell->type==id_MISTRAL_FF && !user.port.in(id_DATAIN,id_ENA,id_SCLR,id_SLOAD,id_SDATA)) ||
                   user.port.in(id_ACLR0,id_ACLR1))
                    return fail("output cone reaches an asynchronous or unsupported capture control");
                std::vector<TimingAnalyser::SetupWindowHint> hints;std::vector<EndpointClockPairTiming> rows;
                if(!timing.get_setup_window_hints(CellPortKey(user),hints) || hints.empty() ||
                   !timing.get_endpoint_clock_pair_timings(CellPortKey(user),rows) || rows.empty())
                    return fail("output capture lacks complete native setup/hold coverage");
                std::set<ClockPairIdentity> expected,actual;
                for(const auto &hint:hints) {
                    if(hint.cut || !hint.window || !finite(*hint.window) || *hint.window<=0 || !expected.insert(identity(hint)).second)
                        return fail("output capture has a cut, untimed or malformed clock relationship");
                }
                for(const auto &row:rows) {
                    if(!row.setup_timed || !row.hold_related || !row.setup_window || *row.setup_window<=0 ||
                       !row.setup_margin || !row.hold_margin || !finite(*row.setup_window) ||
                       !finite(*row.setup_margin) || !finite(*row.hold_margin) ||
                       !finite(row.min_path_delay) || !finite(row.max_path_delay) ||
                       !actual.insert(identity(row)).second)
                        return fail("output capture has an untimed, unrelated or invalid native row");
                }
                if(expected!=actual)return fail("output capture clock-pair inventory is incomplete");
                for(const auto &port:cell->ports) if(port.second.type==PORT_OUT && port.second.net) {
                    DelayQuad delay;
                    if(ctx->getCellDelay(cell,user.port,port.first,delay))return fail("output capture has a hybrid combinational branch");
                }
                ++captures;
            } else return fail("output cone reaches an unsupported primitive or capture");
        }
        visiting.erase(net);done.insert(net);return true;
    };
    bool complete=cone(cone,output,0,false);
    if(complete && allow_unobserved_arithmetic && !captures)
        complete=fail("output cone has no actual fully timed register capture");
    if(reason)*reason=complete?"complete bounded native output setup/hold cone":failure;
    return complete;
}
} // namespace

bool critical_lut6_swap_output_coverage(Context *ctx,TimingAnalyser &timing,NetInfo *output,std::string *reason)
{
    return output_coverage(ctx,timing,output,false,reason);
}

bool critical_output_coverage_with_unobserved_arithmetic(Context *ctx,TimingAnalyser &timing,NetInfo *output,
                                                        std::string *reason)
{
    return output_coverage(ctx,timing,output,true,reason);
}

bool critical_routed_lut6_swap_trial(Context *ctx,CellInfo *cell,IdString first,IdString second,int visit_budget,
                                    const std::function<bool()> &accept,std::string *reason,
                                    const std::function<void(unsigned)> &stage)
{
    std::string why;
    auto fail=[&](const char *text) {if(reason)*reason=text;return false;};
    if(visit_budget<=0 || visit_budget>500000) return fail("local route node budget must be 1..500000");
    if(!inspect(ctx,cell,first,second,why)) {if(reason)*reason=why;return false;}
    auto ports=cell->ports;auto params=cell->params;
    ArchCellInfo architecture=*static_cast<ArchCellInfo *>(cell);
    const auto where=ctx->bel_data(cell->bel).lab_data;
    auto labs=ctx->labs;const auto bel=cell->bel;const auto strength=cell->belStrength;
    const auto mask=ctx->compute_lut_mask(where.lab,where.alm);
    std::array<SavedNet,2> saved{{{cell->getPort(first),cell->getPort(first)->users,{}},
                                  {cell->getPort(second),cell->getPort(second)->users,{}}}};
    for(auto &net:saved) {
        for(const auto &wire:net.net->wires) net.wires.emplace_back(wire.first,wire.second.pip,wire.second.strength);
    }
    auto restore=[&]() {
        for(auto &net:saved) ctx->ripupNet(net.net->name);
        cell->ports=ports;cell->params=params;*static_cast<ArchCellInfo *>(cell)=architecture;ctx->labs=labs;
        for(auto &net:saved) net.net->users=indexed_store<PortRef>(net.users);
        for(const auto &net:saved) {
            for(const auto &wire:net.wires) if(std::get<1>(wire)==PipId())
                ctx->bindWire(std::get<0>(wire),net.net,std::get<2>(wire));
            for(const auto &wire:net.wires) if(std::get<1>(wire)!=PipId())
                ctx->bindPip(std::get<1>(wire),net.net,std::get<2>(wire));
        }
        ctx->configure_bitstream();
    };
    auto valid=[&]() {
        if(cell->bel!=bel || cell->belStrength!=strength || cell->type!=id_MISTRAL_ALUT6 ||
           cell->params!=params || !same_pins(cell,architecture) || ctx->labs.size()!=labs.size() ||
           !legal_lab(ctx,where.lab) || ctx->compute_lut_mask(where.lab,where.alm)!=mask) return false;
        for(size_t l=0;l<labs.size();++l) if(!same_lab(labs[l],ctx->labs[l])) return false;
        return true;
    };
    try {
        for(auto &net:saved) ctx->ripupNet(net.net->name);
        if(stage)stage(1);
        // Port names and the indexed stores themselves stay fixed. The two
        // original storage slots follow their nets, including all unused holes.
        auto &a=cell->ports.at(first),&b=cell->ports.at(second);
        std::swap(a.net,b.net);std::swap(a.user_idx,b.user_idx);
        a.net->users.at(a.user_idx)={cell,first};b.net->users.at(b.user_idx)={cell,second};
        ctx->assign_comb_info(cell);
        if(stage)stage(2);
        if(!valid()) {restore();return fail("LUT6 mode, mask, pin or LAB legality changed");}
        for(const auto &net:saved) for(const auto &wire:net.wires) if(std::get<1>(wire)==PipId()) {
            if(!ctx->checkWireAvail(std::get<0>(wire))) {restore();return fail("input root became obstructed");}
            ctx->bindWire(std::get<0>(wire),net.net,std::get<2>(wire));
        }
        if(!critical_local_route_net(ctx,saved[0].net,visit_budget)) {restore();return fail("first input-net routing failed or exhausted node budget");}
        if(stage)stage(3);
        if(!critical_local_route_net(ctx,saved[1].net,visit_budget)) {restore();return fail("second input-net routing failed or exhausted node budget");}
        if(stage)stage(4);
        ctx->configure_bitstream();
        if(stage)stage(5);
        if(!valid()) {restore();return fail("configured LUT6 mask, pins or native LAB selectors changed");}
        ctx->check();
        if(accept()) return true;
    } catch(...) {restore();throw;}
    restore();return false;
}

void Arch::repair_critical_lut6_swaps()
{
    if(!critical_lut6_swap_budget) return;
    Context *ctx=getCtx();
    if(critical_lut6_swap_budget<0 || critical_lut6_swap_budget>16 || !ordinary_context(ctx))
        log_error("Critical LUT6 swap requires an ordinary completed route and budget 1..16.\n");
    configure_bitstream();
    auto analyse=[&](bool skew) {auto t=std::make_unique<TimingAnalyser>(ctx);t->with_clock_skew=skew;t->setup(false,false,true);return t;};
    auto before=analyse(true),reference=analyse(false);
    if(before->have_loops || reference->have_loops) log_error("Critical LUT6 swap refuses timing loops.\n");
    auto inventory=critical_swap_live_endpoint_snapshot(ctx,*before,*reference);
    size_t failing_rows=0;
    for(const auto &endpoint:inventory["endpoints"].object_items()) for(const auto &row:endpoint.second.array_items())
        if(row["setup_timed"].bool_value() && row["setup"].is_number() && row["setup"].number_value()<0) ++failing_rows;
    // Every domain pair contributes at most the total failing row count. One
    // additional slot includes its ordinary worst-path report without dropping
    // another failing endpoint; the public native report API caps at 16384.
    int count=int(std::max<size_t>(2,std::min<size_t>(16384,failing_rows+1)));
    auto paths=before->get_report_setup_paths(count);
    for(const auto &path:before->get_timing_result().clock_paths) paths.push_back(path.second);
    for(const auto &path:before->get_timing_result().xclock_paths) paths.push_back(path);
    if(failing_rows>=16384) log_info("Critical LUT6 discovery: native path bound truncates at 16384 per clock pair.\n");
    struct Choice {CellInfo *cell;IdString first,second;int64_t slack;delay_t gain;};
    std::map<std::tuple<std::string,int,int>,Choice> choices;
    std::set<std::string> witnesses;
    std::set<std::tuple<std::string,int,int>> considered;
    std::map<CellInfo *,std::pair<bool,std::string>> coverage;
    size_t lut6_arcs=0,filtered=0;
    for(const auto &path:paths) {
        if(path.segments.empty() || path.segments.back().type!=CriticalPath::Segment::Type::SETUP || path.max_delay<=0) continue;
        int64_t total=0;bool finite=true;
        for(const auto &segment:path.segments) {
            if(!std::isfinite(double(segment.delay))) {finite=false;break;}
            total+=int64_t(segment.delay);
        }
        const int64_t slack=int64_t(path.max_delay)-total;
        if(!finite || slack>=0) continue;
        const auto &endpoint=path.segments.back().to;
        witnesses.insert(json11::Json(json11::Json::array{endpoint.first.str(ctx),endpoint.second.str(ctx),
            path.clock_pair.start.clock.str(ctx),int(path.clock_pair.start.edge),path.clock_pair.end.clock.str(ctx),int(path.clock_pair.end.edge)}).dump());
        for(const auto &segment:path.segments) {
            if(segment.type!=CriticalPath::Segment::Type::LOGIC || segment.from.first!=segment.to.first || segment.to.second!=id_Q) continue;
            auto found=ctx->cells.find(segment.from.first);
            if(found==ctx->cells.end() || found->second->type!=id_MISTRAL_ALUT6) continue;
            auto *cell=found->second.get();auto first=segment.from.second;
            if(std::find(pins.begin(),pins.end(),first)==pins.end()) continue;
            ++lut6_arcs;
            for(auto second:pins) {
                if(first==second) continue;
                auto key=std::make_tuple(cell->name.str(ctx),std::min(first.index,second.index),std::max(first.index,second.index));
                if(choices.count(key)) {choices.at(key).slack=std::min(choices.at(key).slack,slack);continue;}
                if(!considered.insert(key).second)continue;
                std::string reason;
                if(!inspect(ctx,cell,first,second,reason)) {
                    ++filtered;log_info("Critical LUT6 filter: cell=%s pins=%s/%s: %s.\n",ctx->nameOf(cell),first.c_str(ctx),second.c_str(ctx),reason.c_str());continue;
                }
                // Native terminal-input delay ranks partners only. The new
                // route changes slew/loading and must pass full fresh STA.
                if(!coverage.count(cell)) {
                    bool complete=critical_lut6_swap_source_coverage(ctx,*before,cell,&reason) &&
                                  critical_lut6_swap_source_coverage(ctx,*reference,cell,&reason) &&
                                  critical_lut6_swap_output_coverage(ctx,*before,cell->getPort(id_Q),&reason) &&
                                  critical_lut6_swap_output_coverage(ctx,*reference,cell->getPort(id_Q),&reason);
                    coverage.emplace(cell,std::make_pair(complete,reason));
                }
                if(!coverage.at(cell).first) {
                    ++filtered;log_info("Critical LUT6 filter: cell=%s pins=%s/%s: %s.\n",ctx->nameOf(cell),first.c_str(ctx),second.c_str(ctx),coverage.at(cell).second.c_str());continue;
                }
                delay_t gain=get_lut_input_delay(cell,first).maxDelay()-get_lut_input_delay(cell,second).maxDelay();
                choices.emplace(key,Choice{cell,first,second,slack,gain});
            }
        }
    }
    std::map<std::string,std::vector<Choice>> grouped;
    for(const auto &entry:choices)grouped[entry.second.cell->name.str(ctx)].push_back(entry.second);
    struct CellChoices {std::string name;int64_t slack;std::vector<Choice> pairs;};
    std::vector<CellChoices> ranked;
    for(auto &group:grouped) {
        int64_t slack=std::numeric_limits<int64_t>::max();
        for(const auto &choice:group.second)slack=std::min(slack,choice.slack);
        std::sort(group.second.begin(),group.second.end(),[&](const Choice &a,const Choice &b) {
            if(a.gain!=b.gain)return a.gain>b.gain;
            return std::make_pair(a.first.index,a.second.index)<std::make_pair(b.first.index,b.second.index);
        });
        ranked.push_back({group.first,slack,std::move(group.second)});
    }
    std::sort(ranked.begin(),ranked.end(),[](const CellChoices &a,const CellChoices &b) {
        return std::tie(a.slack,a.name)<std::tie(b.slack,b.name);
    });
    log_info("Critical LUT6 discovery: negative endpoint rows=%zu native failing path witnesses=%zu LUT6 arcs=%zu filtered pairs=%zu eligible pairs=%zu cells=%zu.\n",
             failing_rows,witnesses.size(),lut6_arcs,filtered,choices.size(),ranked.size());
    int tried=0,kept=0;std::set<CellInfo *> changed;
    // Give every discovered cell a first trial before its second partner.
    for(size_t rank=0;rank<15 && tried<critical_lut6_swap_budget;++rank) for(const auto &group:ranked) {
        if(rank>=group.pairs.size())continue;
        const auto &choice=group.pairs[rank];
        if(tried>=critical_lut6_swap_budget)break;
        if(changed.count(choice.cell))continue;
        std::string reason;
        // Discovery is only a proposal. Recheck all origins and captures on
        // the currently accepted native state before changing either tree.
        if(!critical_lut6_swap_source_coverage(ctx,*before,choice.cell,&reason) ||
           !critical_lut6_swap_source_coverage(ctx,*reference,choice.cell,&reason) ||
           !critical_lut6_swap_output_coverage(ctx,*before,choice.cell->getPort(id_Q),&reason) ||
           !critical_lut6_swap_output_coverage(ctx,*reference,choice.cell->getPort(id_Q),&reason)) {
            log_info("Critical LUT6 current coverage: cell=%s: %s.\n",ctx->nameOf(choice.cell),reason.c_str());
            continue;
        }
        ++tried;reason="native timing rejected";
        std::unique_ptr<TimingAnalyser> after,next_reference;json11::Json next_inventory;
        bool accepted=critical_routed_lut6_swap_trial(ctx,choice.cell,choice.first,choice.second,500000,[&]() {
            after=analyse(true);next_reference=analyse(false);
            if(after->have_loops || next_reference->have_loops) {reason="timing loop";return false;}
            if(!critical_lut6_swap_source_coverage(ctx,*after,choice.cell,&reason) ||
               !critical_lut6_swap_source_coverage(ctx,*next_reference,choice.cell,&reason) ||
               !critical_lut6_swap_output_coverage(ctx,*after,choice.cell->getPort(id_Q),&reason) ||
               !critical_lut6_swap_output_coverage(ctx,*next_reference,choice.cell->getPort(id_Q),&reason)) return false;
            if(!private_pair_clock_safe(before->get_timing_result(),after->get_timing_result(),&reason) ||
               !private_pair_clock_safe(reference->get_timing_result(),next_reference->get_timing_result(),&reason)) return false;
            next_inventory=critical_swap_live_endpoint_snapshot(ctx,*after,*next_reference);
            return critical_route_snapshot_safe(inventory,next_inventory,&reason);
        },&reason,{});
        log_info("Critical LUT6 trial %d: cell=%s pins=%s/%s prior-path-slack=%lldps native-pin-rank=%dps accepted=%d; %s.\n",
                 tried,ctx->nameOf(choice.cell),choice.first.c_str(ctx),choice.second.c_str(ctx),
                 static_cast<long long>(choice.slack),int(choice.gain),int(accepted),reason.c_str());
        if(accepted) {before=std::move(after);reference=std::move(next_reference);inventory=std::move(next_inventory);changed.insert(choice.cell);++kept;}
    }
    log_info("Critical LUT6 swap: tried=%d/%d kept=%d eligible=%zu; unchanged unsupported coverage is not qualification.\n",
             tried,critical_lut6_swap_budget,kept,choices.size());
}
NEXTPNR_NAMESPACE_END
