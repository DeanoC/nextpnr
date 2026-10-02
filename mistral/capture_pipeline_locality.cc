/* Experimental guarded capture-pipeline placement. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "json11.hpp"
#include "log.h"
#include "lut_driver_copy.h"
#include "remap_clock_guard.h"
#include "remap_report.h"
#include "timing.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
using Lab = std::pair<int, int>;
namespace guard = mistral_remap_clock_guard;

bool protected_attrs(const dict<IdString, Property> &attrs, Context *ctx)
{
    for (const auto &entry : attrs) {
        auto name = entry.first.str(ctx);
        if (name == "keep" || name == "dont_touch" || name == "BEL" || name == "FES_SLOT") return true;
    }
    return false;
}

bool pins_equal(const dict<IdString, ArchPinInfo> &a, const dict<IdString, ArchPinInfo> &b)
{
    if (a.size() != b.size()) return false;
    for (const auto &pin : a)
        if (!b.count(pin.first) || pin.second.state != b.at(pin.first).state ||
            pin.second.bel_pins != b.at(pin.first).bel_pins) return false;
    return true;
}

bool info_equal(Context *ctx, const CellInfo *cell, const ArchCellInfo &saved)
{
    if (cell->constr_children != saved.constr_children || cell->constr_x != saved.constr_x ||
        cell->constr_y != saved.constr_y || cell->constr_z != saved.constr_z ||
        cell->constr_abs_z != saved.constr_abs_z || !pins_equal(cell->pin_data, saved.pin_data)) return false;
    if (ctx->is_comb_cell(cell->type) || cell->type.in(id_MISTRAL_BUF, id_MISTRAL_MLAB)) {
        const auto &a = cell->combInfo, &b = saved.combInfo;
        if (a.comb_out != b.comb_out || a.lut_input_count != b.lut_input_count ||
            a.used_lut_input_count != b.used_lut_input_count || a.lut_bits_count != b.lut_bits_count ||
            a.chain_shared_input_count != b.chain_shared_input_count || a.is_carry != b.is_carry ||
            a.is_shared != b.is_shared || a.is_extended != b.is_extended || a.carry_start != b.carry_start ||
            a.carry_end != b.carry_end || a.mlab_group != b.mlab_group) return false;
        for (int i = 0; i < a.lut_input_count; ++i) if (a.lut_in[i] != b.lut_in[i]) return false;
        if (cell->type == id_MISTRAL_MLAB && (!(a.wclk == b.wclk) || !(a.we == b.we))) return false;
    } else if (cell->type == id_MISTRAL_FF) {
        if (!(cell->ffInfo.ctrlset == saved.ffInfo.ctrlset) || cell->ffInfo.sdata != saved.ffInfo.sdata ||
            cell->ffInfo.datain != saved.ffInfo.datain) return false;
    }
    return true;
}

bool users_equal(const indexed_store<PortRef> &a, const indexed_store<PortRef> &b)
{
    if (a.entries() != b.entries() || a.capacity() != b.capacity()) return false;
    auto ac = a, bc = b;
    auto left = ac.enumerate(), right = bc.enumerate();
    auto x = left.begin(), y = right.begin();
    for (; x != left.end() && y != right.end(); ++x, ++y)
        if ((*x).index != (*y).index || (*x).value.cell != (*y).value.cell ||
            (*x).value.port != (*y).value.port) return false;
    if (x != left.end() || y != right.end()) return false;
    const size_t probes = size_t(a.capacity()) + 1;
    for (size_t i = 0; i < probes; ++i)
        if (ac.add(PortRef{}) != bc.add(PortRef{})) return false;
    return true;
}

bool labs_equal(const std::vector<LABInfo> &a, const std::vector<LABInfo> &b,
                const std::set<std::pair<uint32_t, uint8_t>> &changed_counts)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto &x = a[i], &y = b[i];
        if (x.is_mlab != y.is_mlab || x.clk_wires != y.clk_wires || x.ena_wires != y.ena_wires ||
            x.aclr_wires != y.aclr_wires || x.sclr_wire != y.sclr_wire || x.sload_wire != y.sload_wire ||
            x.aclr_used != y.aclr_used) return false;
        for (size_t j = 0; j < x.alms.size(); ++j) {
            const auto &p = x.alms[j], &q = y.alms[j];
            if (p.comb_out != q.comb_out || p.sel_clk != q.sel_clk || p.sel_ena != q.sel_ena ||
                p.sel_aclr != q.sel_aclr || p.sel_ef != q.sel_ef || p.ff_in != q.ff_in ||
                p.ff_out != q.ff_out || p.lut_bels != q.lut_bels || p.ff_bels != q.ff_bels ||
                p.carry_mode != q.carry_mode || p.clk_ena_idx != q.clk_ena_idx ||
                p.aclr_idx != q.aclr_idx || p.l6_mode != q.l6_mode ||
                (!changed_counts.count({uint32_t(i), uint8_t(j)}) &&
                 p.unique_input_count != q.unique_input_count)) return false;
        }
    }
    return true;
}

using ClockValues = std::tuple<delay_t, delay_t, delay_t, delay_t, delay_t, delay_t, IdString, delay_t>;
std::map<IdString, ClockValues> clock_values(Context *ctx)
{
    std::map<IdString, ClockValues> result;
    for (const auto &entry : ctx->nets) if (entry.second->clkconstr) {
        const auto &c = *entry.second->clkconstr;
        result.emplace(entry.first, std::make_tuple(c.period.minDelay(), c.period.maxDelay(), c.high.minDelay(),
                       c.high.maxDelay(), c.low.minDelay(), c.low.maxDelay(), c.phase_group, c.phase_shift));
    }
    return result;
}

// Binding and timing must not change graph owners, ordered ports, raw user
// storage, parameters, pin states or the caches of any unmoved cell.
struct Snapshot {
    struct Cell {
        CellInfo *owner;
        IdString name, type, hierpath;
        BelId bel;
        PlaceStrength strength;
        ClusterId cluster;
        Region *region;
        PseudoCell *pseudo;
        dict<IdString, Property> params, attrs;
        dict<IdString, PortInfo> ports;
        std::vector<IdString> port_order;
        ArchCellInfo info;
    };
    struct Net {
        NetInfo *owner;
        IdString name, hierpath, constant;
        Region *region;
        bool global;
        PortRef driver;
        dict<IdString, Property> attrs;
        std::vector<IdString> aliases;
        indexed_store<PortRef> users;
        const ClockConstraint *clock;
    };
    std::vector<Cell> cells;
    std::vector<Net> nets;
    std::vector<std::pair<IdString, IdString>> aliases;
    std::vector<LABInfo> labs;
    std::map<IdString, ClockValues> clocks;

    explicit Snapshot(Context *ctx) : labs(ctx->labs), clocks(clock_values(ctx))
    {
        for (const auto &entry : ctx->cells) {
            auto *c = entry.second.get();
            std::vector<IdString> order;
            for (const auto &port : c->ports) order.push_back(port.first);
            cells.push_back({c, c->name, c->type, c->hierpath, c->bel, c->belStrength, c->cluster, c->region,
                             c->pseudo_cell.get(), c->params, c->attrs, c->ports, std::move(order),
                             static_cast<const ArchCellInfo &>(*c)});
        }
        for (const auto &entry : ctx->nets) {
            auto *n = entry.second.get();
            nets.push_back({n, n->name, n->hierpath, n->constant_value, n->region, n->is_global, n->driver,
                            n->attrs, n->aliases, n->users, n->clkconstr.get()});
        }
        for (const auto &entry : ctx->net_aliases) aliases.emplace_back(entry.first, entry.second);
    }

    bool fixed(Context *ctx, CellInfo *first, BelId first_bel, CellInfo *second, BelId second_bel,
               const std::set<std::pair<uint32_t, uint8_t>> &changed_counts) const
    {
        if (ctx->cells.size() != cells.size() || ctx->nets.size() != nets.size() ||
            ctx->net_aliases.size() != aliases.size() || clock_values(ctx) != clocks ||
            !labs_equal(labs, ctx->labs, changed_counts)) return false;
        size_t index = 0;
        for (const auto &entry : ctx->cells) {
            const auto &saved = cells.at(index++);
            auto *c = entry.second.get();
            BelId expected = c == first ? first_bel : c == second ? second_bel : saved.bel;
            if (c != saved.owner || entry.first != saved.name || c->name != saved.name || c->type != saved.type ||
                c->hierpath != saved.hierpath || c->bel != expected || c->belStrength != saved.strength ||
                c->cluster != saved.cluster || c->region != saved.region || c->pseudo_cell.get() != saved.pseudo ||
                c->params != saved.params || c->attrs != saved.attrs || c->ports.size() != saved.ports.size() ||
                !info_equal(ctx, c, saved.info) ||
                (expected != BelId() && ctx->getBoundBelCell(expected) != c)) return false;
            size_t pin = 0;
            for (const auto &port : c->ports) if (port.first != saved.port_order.at(pin++)) return false;
            for (const auto &port : saved.ports) {
                auto now = c->ports.find(port.first);
                if (now == c->ports.end() || now->second.name != port.second.name ||
                    now->second.type != port.second.type || now->second.net != port.second.net ||
                    now->second.user_idx != port.second.user_idx) return false;
            }
        }
        index = 0;
        for (const auto &entry : ctx->nets) {
            const auto &saved = nets.at(index++);
            auto *n = entry.second.get();
            if (n != saved.owner || entry.first != saved.name || n->name != saved.name ||
                n->hierpath != saved.hierpath || n->constant_value != saved.constant ||
                n->region != saved.region || n->is_global != saved.global || n->attrs != saved.attrs ||
                n->aliases != saved.aliases || n->driver.cell != saved.driver.cell ||
                n->driver.port != saved.driver.port || n->clkconstr.get() != saved.clock ||
                !n->wires.empty() || !users_equal(n->users, saved.users)) return false;
        }
        index = 0;
        for (const auto &entry : ctx->net_aliases)
            if (entry.first != aliases.at(index).first || entry.second != aliases.at(index++).second) return false;
        return true;
    }

    void restore(Context *ctx, CellInfo *first, BelId first_bel, PlaceStrength first_strength,
                 CellInfo *second, BelId second_bel, PlaceStrength second_strength) const
    {
        if (first->bel != BelId()) ctx->unbindBel(first->bel);
        if (second->bel != BelId()) ctx->unbindBel(second->bel);
        ctx->bindBel(first_bel, first, first_strength);
        ctx->bindBel(second_bel, second, second_strength);
        for (const auto &saved : cells) static_cast<ArchCellInfo &>(*saved.owner) = saved.info;
        ctx->labs = labs;
        NPNR_ASSERT(fixed(ctx, first, first_bel, second, second_bel, {}));
    }

    // A hard-input transaction moves only its launch FF. Never unbind the
    // registered hard sink, including when restoring an exceptional trial.
    void restore_one(Context *ctx, CellInfo *cell, BelId bel, PlaceStrength strength) const
    {
        if (cell->bel != BelId()) ctx->unbindBel(cell->bel);
        ctx->bindBel(bel, cell, strength);
        for (const auto &saved : cells) static_cast<ArchCellInfo &>(*saved.owner) = saved.info;
        ctx->labs = labs;
        NPNR_ASSERT(fixed(ctx, cell, bel, nullptr, BelId(), {}));
    }
};

bool finite_delay(int64_t value)
{
    return value >= 0 && value < int64_t(std::numeric_limits<delay_t>::max());
}

bool finite_timing(delay_t value)
{
    // Native setup checks can be negative (the ordinary FF is -196ps).
    return std::isfinite(double(value)) && value != std::numeric_limits<delay_t>::max() &&
           value != std::numeric_limits<delay_t>::lowest();
}

bool internal_rows_safe(const guard::Rows &before, const guard::Rows &after, IdString clock, ClockEdge edge)
{
    if (!guard::rows_match(before, after) || before.size() != 1) return false;
    const auto &old = before.front(), &now = after.front();
    return old.launch == ClockDomainKey(clock, edge) && old.capture == ClockDomainKey(clock, edge) &&
           old.setup_timed && old.setup_window && old.setup_margin && *old.setup_margin >= 0 &&
           old.hold_related && old.hold_margin && now.setup_margin && *now.setup_margin >= 0 &&
           now.hold_margin && *now.hold_margin >= std::min(delay_t(0), *old.hold_margin);
}

struct Options { std::string report; int budget, radius; };
Options read_options(const char *spec)
{
    std::istringstream options(spec);
    std::string path, extra;
    int budget = 0, radius = 0;
    if (!(options >> path >> budget >> radius) || (options >> extra) ||
        budget < 1 || budget > 64 || radius < 1 || radius > 24 || path.find('\0') != std::string::npos)
        log_error("Capture pipeline locality requires exactly report, budget 1..64 and radius 1..24.\n");
    std::ifstream file(path);
    if (!file) log_error("Cannot read capture pipeline locality timing report.\n");
    std::string report((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()), error;
    auto json = json11::Json::parse(report, error);
    if (!error.empty() || !json.is_object() || !json["critical_paths"].is_array())
        log_error("Invalid capture pipeline locality timing report.\n");
    return {std::move(report), budget, radius};
}
} // namespace

int capture_pipeline_locality(Context *ctx, const std::string &report, int budget, int radius)
{
    if (budget < 1 || budget > 64 || radius < 1 || radius > 24)
        log_error("Capture pipeline locality budget must be 1..64 and radius 1..24.\n");
    if (ctx->fes_any_slot_region_active) log_error("Capture pipeline locality requires ordinary placement.\n");
    for (const auto &entry : ctx->nets) if (!entry.second->wires.empty())
        log_error("Capture pipeline locality requires an unrouted design.\n");
    std::string error;
    auto json = json11::Json::parse(report, error);
    if (!error.empty() || !json.is_object() || !json["critical_paths"].is_array())
        log_error("Invalid capture pipeline locality timing report.\n");
    // The native, current pre-route path validator runs before any binding.
    // Routed feed-through buffers are deliberately not normalised here.
    auto paths = mistral_remap_report::validate(ctx, json, true);
    auto lab = [&](BelId bel) { auto at = ctx->getBelLocation(bel); return Lab(at.x, at.y); };
    auto weak = [&](const CellInfo *cell) {
        return cell && cell->bel != BelId() && cell->belStrength <= STRENGTH_WEAK &&
               cell->cluster == ClusterId() && !cell->region && !cell->isPseudo() &&
               !protected_attrs(cell->attrs, ctx);
    };
    std::set<Lab> protected_labs;
    std::map<IdString, size_t> bel_counts;
    for (auto bel : ctx->getBels()) ++bel_counts[ctx->getBelType(bel)];
    for (const auto &entry : ctx->cells) {
        auto *cell = entry.second.get();
        if (cell->bel != BelId() && (!weak(cell) || cell->type.in(id_MISTRAL_MLAB, id_MISTRAL_ALUT_ARITH)))
            protected_labs.insert(lab(cell->bel));
    }
    std::set<const NetInfo *> boundary;
    for (const auto &entry : ctx->ports) if (entry.second.net) boundary.insert(entry.second.net);
    auto ordinary = [&](const NetInfo *net) {
        return net && net->driver.cell && !net->is_global && !net->clkconstr && !net->region &&
               net->wires.empty() && net->constant_value == IdString() && !boundary.count(net) &&
               !protected_attrs(net->attrs, ctx) && net->driver.cell->ports.count(net->driver.port) &&
               net->driver.cell->ports.at(net->driver.port).type == PORT_OUT &&
               net->driver.cell->getPort(net->driver.port) == net;
    };
    auto clocked = [&](CellInfo *cell, IdString port, int count) {
        if (count <= 0 || cell->bel == BelId() || ctx->getBelPinsForCellPin(cell, port).empty()) return false;
        for (int i = 0; i < count; ++i) {
            auto info = ctx->getPortClockingInfo(cell, port, i);
            auto *clock = cell->getPort(info.clock_port);
            if (!clock || !clock->clkconstr || clock->clkconstr->period.minDelay() <= 0 ||
                !finite_timing(info.setup.minDelay()) || !finite_timing(info.setup.maxDelay()) ||
                !finite_timing(info.hold.minDelay()) || !finite_timing(info.hold.maxDelay()) ||
                !finite_delay(info.clockToQ.maxDelay())) return false;
        }
        return true;
    };
    auto wire_at = [&](const CellInfo *cell, BelId bel, IdString pin) {
        if (!cell->pin_data.count(pin) || cell->pin_data.at(pin).bel_pins.size() != 1) return WireId();
        auto physical = cell->pin_data.at(pin).bel_pins.front();
        const auto &pins = ctx->bel_data(bel).pins;
        if (!pins.count(physical) || !cell->ports.count(pin) || pins.at(physical).dir != cell->ports.at(pin).type)
            return WireId();
        return ctx->getBelPinWire(bel, physical);
    };
    auto plain = [&](CellInfo *cell) {
        if (!weak(cell) || cell->type != id_MISTRAL_FF || protected_labs.count(lab(cell->bel)) ||
            cell->get_pin_state(id_DATAIN) != PIN_SIG || cell->get_pin_state(id_Q) != PIN_SIG ||
            (cell->get_pin_state(id_CLK) != PIN_SIG && cell->get_pin_state(id_CLK) != PIN_INV) ||
            !ordinary(cell->getPort(id_DATAIN)) || !ordinary(cell->getPort(id_Q)) ||
            wire_at(cell, cell->bel, id_DATAIN) == WireId() || wire_at(cell, cell->bel, id_Q) == WireId()) return false;
        for (IdString control : {id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA})
            if (cell->getPort(control)) return false;
        for (const auto &port : cell->ports)
            if (port.second.net && !port.first.in(id_DATAIN, id_Q, id_CLK)) return false;
        int inputs = 0, outputs = 0;
        return ctx->getPortTimingClass(cell, id_DATAIN, inputs) == TMG_REGISTER_INPUT && inputs == 1 &&
               ctx->getPortTimingClass(cell, id_Q, outputs) == TMG_REGISTER_OUTPUT && outputs == 1 &&
               clocked(cell, id_DATAIN, inputs) && clocked(cell, id_Q, outputs);
    };
    auto registered_source = [&](NetInfo *net, TimingClockingInfo &info) {
        if (!ordinary(net) || net->driver.cell->bel == BelId() || net->driver.cell->isPseudo()) return false;
        auto *source = net->driver.cell;
        if (bel_counts[ctx->getBelType(source->bel)] != 1 || source->type == id_MISTRAL_FF ||
            ctx->is_comb_cell(source->type) || source->type.in(id_MISTRAL_MLAB, id_MISTRAL_BUF, id_MISTRAL_NOT)) return false;
        int count = 0;
        if (ctx->getPortTimingClass(source, net->driver.port, count) != TMG_REGISTER_OUTPUT || count != 1 ||
            !clocked(source, net->driver.port, count) || ctx->getNetinfoSourceWire(net) == WireId()) return false;
        info = ctx->getPortClockingInfo(source, net->driver.port, 0);
        return true;
    };
    auto valid_user = [&](NetInfo *net, PortRef user) {
        if (!user.cell || !user.cell->ports.count(user.port) || user.cell->getPort(user.port) != net ||
            user.cell->ports.at(user.port).type != PORT_IN || !net->users.count(user.cell->ports.at(user.port).user_idx)) return false;
        auto stored = net->users.at(user.cell->ports.at(user.port).user_idx);
        return stored.cell == user.cell && stored.port == user.port;
    };
    auto pair = [&](CellInfo *first, CellInfo *&second, TimingClockingInfo &source_info) {
        if (!plain(first) || !registered_source(first->getPort(id_DATAIN), source_info)) return false;
        auto *middle = first->getPort(id_Q);
        if (middle->users.entries() != 1) return false;
        auto user = *middle->users.begin();
        second = user.cell;
        if (!valid_user(middle, user) || user.port != id_DATAIN || second == first || !plain(second) ||
            first->getPort(id_CLK) != second->getPort(id_CLK) ||
            first->get_pin_state(id_CLK) != second->get_pin_state(id_CLK) || second->getPort(id_Q)->users.empty()) return false;
        auto a = ctx->getPortClockingInfo(first, id_DATAIN, 0), b = ctx->getPortClockingInfo(second, id_DATAIN, 0);
        return a.edge == b.edge && source_info.edge == a.edge &&
               first->getPort(id_CLK) == first->getPort(id_DATAIN)->driver.cell->getPort(source_info.clock_port);
    };
    CellInfo *critical = nullptr;
    CellInfo *source = nullptr;
    IdString source_port;
    double excess = 0;
    for (const auto &path : paths) if (!path.edges.empty()) {
        auto edge = path.edges.back();
        if (edge.second.port != id_DATAIN) continue;
        CellInfo *second = nullptr;
        TimingClockingInfo info;
        if (!pair(edge.second.cell, second, info)) continue;
        auto *net = edge.second.cell->getPort(id_DATAIN);
        if (edge.first.cell != net->driver.cell || edge.first.port != net->driver.port) continue;
        if (!critical || path.excess > excess ||
            (path.excess == excess && edge.second.cell->name.str(ctx) < critical->name.str(ctx))) {
            critical = edge.second.cell; source = net->driver.cell; source_port = net->driver.port; excess = path.excess;
        }
    }
    if (!critical) { log_info("Capture pipeline locality: no eligible critical registered hard-IP source.\n"); return 0; }
    struct Chain { CellInfo *first, *second; IdString source_port; };
    std::vector<Chain> chains;
    for (const auto &entry : ctx->cells) {
        auto *first = entry.second.get();
        CellInfo *second = nullptr;
        TimingClockingInfo info;
        if (pair(first, second, info) && first->getPort(id_DATAIN)->driver.cell == source &&
            first->getPort(id_CLK) == critical->getPort(id_CLK))
            chains.push_back({first, second, first->getPort(id_DATAIN)->driver.port});
    }
    std::sort(chains.begin(), chains.end(), [&](const Chain &a, const Chain &b) {
        if (a.first == critical || b.first == critical) return a.first == critical && b.first != critical;
        auto delay = [&](CellInfo *c) {
            return ctx->estimateDelay(ctx->getNetinfoSourceWire(c->getPort(id_DATAIN)), wire_at(c, c->bel, id_DATAIN));
        };
        auto x = delay(a.first), y = delay(b.first);
        return x != y ? x > y : a.first->name.str(ctx) < b.first->name.str(ctx);
    });
    if (chains.size() > size_t(budget)) chains.resize(budget);
    int accepted = 0;
    for (const auto &chain : chains) {
        auto *first = chain.first, *second = chain.second;
        const CellPortKey first_key(first->name, id_DATAIN), second_key(second->name, id_DATAIN);
        auto *clock = first->getPort(id_CLK);
        auto edge = ctx->getPortClockingInfo(first, id_DATAIN, 0).edge;
        std::set<CellPortKey> endpoint_keys{first_key, second_key};
        std::set<NetInfo *> visiting;
        std::map<NetInfo *, int64_t> tails;
        size_t traversed_edges = 0;
        // Longest combinational suffix, excluding the source net's first wire.
        // The same walk discovers every affected registered endpoint.
        std::function<bool(NetInfo *, int64_t &)> follow = [&](NetInfo *net, int64_t &tail) {
            if (!ordinary(net) || net->users.empty()) return false;
            auto cached = tails.find(net);
            if (cached != tails.end()) { tail = cached->second; return true; }
            if (tails.size() + visiting.size() >= 256 || !visiting.insert(net).second) return false;
            int64_t worst = 0;
            for (auto user : net->users) {
                if (++traversed_edges > 1024 || !valid_user(net, user) || user.cell->bel == BelId() ||
                    user.cell->isPseudo() || protected_attrs(user.cell->attrs, ctx)) return false;
                int count = 0;
                auto cls = ctx->getPortTimingClass(user.cell, user.port, count);
                int64_t suffix = 0;
                if (cls == TMG_REGISTER_INPUT) {
                    if (!clocked(user.cell, user.port, count) || endpoint_keys.size() >= 128) return false;
                    endpoint_keys.insert(CellPortKey(user));
                    for (int i = 0; i < count; ++i)
                        suffix = std::max(suffix, int64_t(ctx->getPortClockingInfo(user.cell, user.port, i).setup.maxDelay()));
                } else {
                    if (cls != TMG_COMB_INPUT ||
                        !(mistral_remap_report::lut_width(user.cell->type) || user.cell->type.in(id_MISTRAL_BUF, id_MISTRAL_NOT))) return false;
                    bool arc = false;
                    for (const auto &port : user.cell->ports) if (port.second.type == PORT_OUT) {
                        int outputs = 0;
                        DelayQuad logic;
                        if (ctx->getPortTimingClass(user.cell, port.first, outputs) != TMG_COMB_OUTPUT ||
                            !ctx->getCellDelay(user.cell, user.port, port.first, logic)) continue;
                        int64_t next = 0;
                        if (!finite_delay(logic.maxDelay()) || !follow(port.second.net, next)) return false;
                        arc = true;
                        suffix = std::max(suffix, int64_t(logic.maxDelay()) + next);
                    }
                    if (!arc) return false;
                }
                auto wire = ctx->predictArcDelay(net, user);
                if (!finite_delay(wire) || !finite_delay(int64_t(wire) + suffix)) return false;
                worst = std::max(worst, int64_t(wire) + suffix);
            }
            visiting.erase(net); tails.emplace(net, worst); tail = worst; return true;
        };
        int64_t downstream = 0;
        if (!follow(second->getPort(id_Q), downstream)) {
            log_info("Capture pipeline locality rejected %s -> %s: downstream coverage unavailable.\n",
                     ctx->nameOf(first), ctx->nameOf(second)); continue;
        }
        TimingAnalyser before(ctx); before.with_clock_skew = true; before.setup(false, false, true);
        float old_slack = before.get_setup_slack(first_key);
        if (before.have_loops || before.get_timing_result().clock_fmax.empty() || !guard::timed(old_slack)) continue;
        std::map<CellPortKey, guard::Rows> rows, references;
        bool complete = true, unrelated = false;
        for (auto key : endpoint_keys) {
            guard::Rows current;
            if (!before.get_endpoint_clock_pair_timings(key, current) || current.empty()) { complete = false; break; }
            for (const auto &row : current) {
                unrelated |= !row.setup_timed;
                if (row.setup_timed && (!row.setup_window || !row.setup_margin || !row.hold_related || !row.hold_margin)) complete = false;
            }
            rows.emplace(key, std::move(current));
        }
        if (!complete || !internal_rows_safe(rows.at(second_key), rows.at(second_key), clock->name, edge)) continue;
        if (unrelated) {
            TimingAnalyser reference(ctx); reference.with_clock_skew = false; reference.setup(false, false, true);
            if (reference.have_loops) complete = false;
            for (const auto &entry : rows) {
                guard::Rows current;
                if (!reference.get_endpoint_clock_pair_timings(entry.first, current) || !guard::rows_match(entry.second, current)) complete = false;
                else references.emplace(entry.first, std::move(current));
            }
        }
        if (!complete) continue;
        auto old_holds = guard::holds(before);
        auto first_info = ctx->getPortClockingInfo(first, id_Q, 0);
        auto second_info = ctx->getPortClockingInfo(second, id_Q, 0);
        auto source_info = ctx->getPortClockingInfo(source, chain.source_port, 0);
        const BelId old_first = first->bel, old_second = second->bel;
        const PlaceStrength first_strength = first->belStrength, second_strength = second->belStrength;
        auto source_wire = ctx->getNetinfoSourceWire(first->getPort(id_DATAIN));
        auto incoming = [&](BelId target) -> int64_t {
            auto wire = wire_at(first, target, id_DATAIN);
            if (wire == WireId()) return -1;
            auto delay = ctx->estimateDelay(source_wire, wire);
            auto result = int64_t(source_info.clockToQ.maxDelay()) + int64_t(delay) +
                          ctx->getPortClockingInfo(first, id_DATAIN, 0).setup.maxDelay();
            return finite_delay(delay) && finite_delay(result) ? result : -1;
        };
        auto outgoing = [&](BelId target) -> int64_t {
            auto start = wire_at(second, target, id_Q);
            if (start == WireId()) return -1;
            int64_t result = 0;
            for (auto user : second->getPort(id_Q)->users) {
                auto sink = ctx->getNetinfoSinkWire(second->getPort(id_Q), user, 0);
                if (sink == WireId()) return -1;
                int64_t suffix = 0;
                int count = 0;
                auto cls = ctx->getPortTimingClass(user.cell, user.port, count);
                if (cls == TMG_REGISTER_INPUT) {
                    for (int i = 0; i < count; ++i)
                        suffix = std::max(suffix, int64_t(ctx->getPortClockingInfo(user.cell, user.port, i).setup.maxDelay()));
                } else {
                    for (const auto &port : user.cell->ports) if (port.second.type == PORT_OUT) {
                        DelayQuad logic;
                        auto tail = tails.find(port.second.net);
                        if (tail != tails.end() && ctx->getCellDelay(user.cell, user.port, port.first, logic))
                            suffix = std::max(suffix, int64_t(logic.maxDelay()) + tail->second);
                    }
                }
                auto wire = ctx->estimateDelay(start, sink);
                int64_t value = int64_t(second_info.clockToQ.maxDelay()) + int64_t(wire) + suffix;
                if (!finite_delay(wire) || !finite_delay(value)) return -1;
                result = std::max(result, value);
            }
            return result;
        };
        auto free_half = [&](CellInfo *cell, BelId bel) {
            if (ctx->getBelType(bel) != id_MISTRAL_FF || !ctx->isValidBelForCellType(cell->type, bel) ||
                protected_labs.count(lab(bel)) || (ctx->getBoundBelCell(bel) && ctx->getBoundBelCell(bel) != cell)) return false;
            auto at = ctx->getBelLocation(bel);
            if (at.z % 6 != 2 && at.z % 6 != 4) return false;
            auto comb = ctx->getBelByLocation(Loc(at.x, at.y, (at.z / 6) * 6 + (at.z % 6 == 4 ? 1 : 0)));
            auto partner = ctx->getBelByLocation(Loc(at.x, at.y, at.z + 1));
            if (comb == BelId() || partner == BelId() || ctx->getBoundBelCell(comb) || ctx->getBoundBelCell(partner)) return false;
            const auto &d = ctx->bel_data(bel).lab_data;
            const auto &alm = ctx->labs.at(d.lab).alms.at(d.alm);
            return !alm.carry_mode && wire_at(cell, bel, id_DATAIN) != WireId() && wire_at(cell, bel, id_Q) != WireId();
        };
        struct Site { BelId bel; int64_t score; };
        auto sites = [&](CellInfo *cell, bool is_first) {
            std::vector<Site> all, result;
            auto center = ctx->getBelLocation(cell->bel);
            for (int x = center.x - radius; x <= center.x + radius; ++x)
                for (int y = center.y - radius; y <= center.y + radius; ++y) {
                    if (std::abs(x - center.x) + std::abs(y - center.y) > radius) continue;
                    for (auto bel : ctx->getBelsByTile(x, y)) {
                        if (bel == cell->bel || !free_half(cell, bel)) continue;
                        int64_t score = is_first ? incoming(bel) : outgoing(bel);
                        if (score >= 0) all.push_back({bel, score});
                    }
                }
            std::sort(all.begin(), all.end(), [&](const Site &a, const Site &b) {
                auto x = ctx->getBelLocation(a.bel), y = ctx->getBelLocation(b.bel);
                return std::make_tuple(a.score, x.x, x.y, x.z) < std::make_tuple(b.score, y.x, y.y, y.z);
            });
            std::set<Lab> tiles;
            for (auto candidate : all) if (tiles.insert(lab(candidate.bel)).second) {
                result.push_back(candidate); if (result.size() == 32) break;
            }
            return result;
        };
        auto first_sites = sites(first, true), second_sites = sites(second, false);
        struct Placement { BelId first, second; int64_t incoming, middle, outgoing, peak, sum; };
        std::vector<Placement> candidates;
        for (auto a : first_sites) for (auto b : second_sites) {
            if (a.bel == b.bel || (a.bel == old_first && b.bel == old_second)) continue;
            auto aw = wire_at(first, a.bel, id_Q), bw = wire_at(second, b.bel, id_DATAIN);
            if (aw == WireId() || bw == WireId()) continue;
            auto wire = ctx->estimateDelay(aw, bw);
            int64_t middle = int64_t(first_info.clockToQ.maxDelay()) + int64_t(wire) +
                             ctx->getPortClockingInfo(second, id_DATAIN, 0).setup.maxDelay();
            if (!finite_delay(wire) || !finite_delay(middle) || middle >= clock->clkconstr->period.minDelay()) continue;
            auto ad = ctx->bel_data(a.bel).lab_data, bd = ctx->bel_data(b.bel).lab_data;
            // Two plain feed-through halves cannot occupy the same half.
            if (ad.lab == bd.lab && ad.alm == bd.alm && ad.idx / 2 == bd.idx / 2) continue;
            candidates.push_back({a.bel, b.bel, a.score, middle, b.score,
                                  std::max({a.score, middle, b.score}), a.score + middle + b.score});
        }
        std::sort(candidates.begin(), candidates.end(), [&](const Placement &a, const Placement &b) {
            auto af = ctx->getBelLocation(a.first), as = ctx->getBelLocation(a.second);
            auto bf = ctx->getBelLocation(b.first), bs = ctx->getBelLocation(b.second);
            return std::make_tuple(a.peak, a.sum, a.incoming, a.outgoing, af.x, af.y, as.x, as.y, af.z, as.z) <
                   std::make_tuple(b.peak, b.sum, b.incoming, b.outgoing, bf.x, bf.y, bs.x, bs.y, bf.z, bs.z);
        });
        std::set<std::pair<Lab, Lab>> geometries;
        std::vector<Placement> diverse;
        for (const auto &candidate : candidates) if (geometries.insert({lab(candidate.first), lab(candidate.second)}).second) {
            diverse.push_back(candidate); if (diverse.size() == 16) break;
        }
        log_info("Capture pipeline locality chain first=%s second=%s endpoints=%zu pair_trials=%zu.\n",
                 ctx->nameOf(first), ctx->nameOf(second), endpoint_keys.size(), diverse.size());
        for (const auto &candidate : diverse) {
            Snapshot saved(ctx);
            bool live = true;
            auto restore = [&]() {
                if (!live) return;
                saved.restore(ctx, first, old_first, first_strength, second, old_second, second_strength);
                live = false;
            };
            try {
                ctx->unbindBel(old_first); ctx->unbindBel(old_second);
                ctx->bindBel(candidate.first, first, first_strength);
                ctx->bindBel(candidate.second, second, second_strength);
                std::set<uint32_t> changed_labs;
                std::set<std::pair<uint32_t, uint8_t>> changed_counts;
                for (auto bel : {old_first, old_second, candidate.first, candidate.second}) {
                    const auto &d = ctx->bel_data(bel).lab_data;
                    changed_labs.insert(d.lab); changed_counts.insert({d.lab, d.alm});
                }
                auto legal = [&]() {
                    for (auto index : changed_labs) for (const auto &alm : ctx->labs.at(index).alms)
                        for (auto bel : {alm.lut_bels[0], alm.lut_bels[1], alm.ff_bels[0], alm.ff_bels[1], alm.ff_bels[2], alm.ff_bels[3]})
                            if (ctx->getBoundBelCell(bel) && !ctx->isBelLocationValid(bel)) return false;
                    return true;
                };
                if (!legal() || !saved.fixed(ctx, first, candidate.first, second, candidate.second, changed_counts)) {
                    restore(); continue;
                }
                TimingAnalyser after(ctx); after.with_clock_skew = true; after.setup(false, false, true);
                float slack = after.get_setup_slack(first_key);
                bool improve = !after.have_loops && guard::timed(slack) && slack >= old_slack + 250;
                bool endpoints_ok = !after.have_loops;
                for (const auto &entry : rows) {
                    guard::Rows current;
                    bool have = after.get_endpoint_clock_pair_timings(entry.first, current);
                    endpoints_ok &= have && (entry.first == second_key
                        ? internal_rows_safe(entry.second, current, clock->name, edge)
                        : guard::rows_nonregressing(entry.second, current, false));
                }
                if (!references.empty()) {
                    TimingAnalyser reference(ctx); reference.with_clock_skew = false; reference.setup(false, false, true);
                    endpoints_ok &= !reference.have_loops;
                    for (const auto &entry : references) {
                        guard::Rows current;
                        endpoints_ok &= reference.get_endpoint_clock_pair_timings(entry.first, current) &&
                                        guard::rows_nonregressing(entry.second, current, true);
                    }
                }
                bool clocks_ok = guard::clocks_nonregressing(before, after);
                bool holds_ok = guard::holds_nonregressing(old_holds, guard::holds(after));
                bool fixed = legal() && saved.fixed(ctx, first, candidate.first, second, candidate.second, changed_counts);
                log_info("Capture pipeline locality trial first=%s second=%s first_bel=%s second_bel=%s gain=%.0fps improve=%d endpoints=%d clocks=%d holds=%d fixed=%d stages=%lld/%lld/%lldps.\n",
                         ctx->nameOf(first), ctx->nameOf(second), ctx->getBelName(candidate.first).str(ctx).c_str(),
                         ctx->getBelName(candidate.second).str(ctx).c_str(), slack - old_slack, int(improve),
                         int(endpoints_ok), int(clocks_ok), int(holds_ok), int(fixed),
                         (long long)candidate.incoming, (long long)candidate.middle, (long long)candidate.outgoing);
                if (improve && endpoints_ok && clocks_ok && holds_ok && fixed) {
                    ctx->check(); live = false; ++accepted;
                    log_info("Capture pipeline locality retained first=%s second=%s; routing and signoff still required.\n",
                             ctx->nameOf(first), ctx->nameOf(second));
                    break;
                }
                restore();
            } catch (...) { restore(); throw; }
        }
    }
    ctx->check();
    log_info("Capture pipeline locality: source %s.%s, eligible=%zu retained_pairs=%d (route signoff still required).\n",
             ctx->nameOf(source), source_port.c_str(ctx), chains.size(), accepted);
    return accepted;
}

void preload_capture_pipeline_locality(Context *ctx, const char *spec)
{
    ctx->capture_pipeline_report.clear();
    ctx->capture_pipeline_budget = 0;
    ctx->capture_pipeline_radius = 24;
    if (!spec) return;
    auto options = read_options(spec);
    ctx->capture_pipeline_report = std::move(options.report);
    ctx->capture_pipeline_budget = options.budget;
    ctx->capture_pipeline_radius = options.radius;
}

void diagnostic_capture_pipeline_locality(Context *ctx, const char *spec)
{
    if (!spec) return;
    auto options = read_options(spec);
    capture_pipeline_locality(ctx, options.report, options.budget, options.radius);
}

namespace {
// Unlike the capture-pair report reader, this validates all exported paths,
// including passing paths and registered hard inputs. No routed BUF aliases
// or missing owners are normalised into the current placement graph.
struct HardPath {
    std::vector<std::pair<PortRef, PortRef>> edges;
    PortRef launch, capture;
    double excess = 0;
    bool registered_launch = false, registered_capture = false;
};

std::vector<HardPath> hard_paths(Context *ctx, const json11::Json &json)
{
    auto same = [](PortRef a, PortRef b) { return a.cell == b.cell && a.port == b.port; };
    auto endpoint = [&](const json11::Json &value) -> PortRef {
        const auto &loc = value["loc"].array_items();
        if (!value["cell"].is_string() || !value["port"].is_string() || loc.size() != 2 ||
            !loc[0].is_number() || !loc[1].is_number())
            log_error("Malformed hard input path endpoint.\n");
        auto found = ctx->cells.find(ctx->id(value["cell"].string_value()));
        if (found == ctx->cells.end()) log_error("Stale hard input report cell.\n");
        auto *cell = found->second.get(); auto pin = ctx->id(value["port"].string_value());
        if (!cell->ports.count(pin) || cell->bel == BelId())
            log_error("Stale hard input report port or placement.\n");
        auto at = ctx->getBelLocation(cell->bel);
        if (loc[0].number_value() != at.x || loc[1].number_value() != at.y)
            log_error("Stale hard input report placement.\n");
        return {cell, pin};
    };
    auto clock_event = [&](PortRef port, const std::string &event, bool output) {
        int count = 0;
        if (ctx->getPortTimingClass(port.cell, port.port, count) !=
            (output ? TMG_REGISTER_OUTPUT : TMG_REGISTER_INPUT) || count <= 0) return false;
        for (int i = 0; i < count; ++i) {
            auto info = ctx->getPortClockingInfo(port.cell, port.port, i);
            auto *clock = port.cell->getPort(info.clock_port);
            if (clock && event == (info.edge == FALLING_EDGE ? "negedge " : "posedge ") + clock->name.str(ctx))
                return true;
        }
        return false;
    };
    std::vector<HardPath> result;
    for (const auto &path : json["critical_paths"].array_items()) {
        if (!path.is_object() || !path["path"].is_array() || path["path"].array_items().empty() ||
            !path["from"].is_string() || !path["to"].is_string() ||
            !path["max_delay"].is_number() || !std::isfinite(path["max_delay"].number_value()) ||
            path["max_delay"].number_value() <= 0)
            log_error("Malformed hard input path constraint.\n");
        HardPath checked; PortRef previous;
        std::vector<std::pair<PortRef, PortRef>> clock_prefix;
        bool started = false, finished = false; double total = 0;
        for (const auto &segment : path["path"].array_items()) {
            if (!segment["type"].is_string() || !segment["delay"].is_number() ||
                !std::isfinite(segment["delay"].number_value()))
                log_error("Malformed hard input path delay.\n");
            total += segment["delay"].number_value();
            auto from = endpoint(segment["from"]), to = endpoint(segment["to"]);
            const auto &type = segment["type"].string_value();
            int a = 0, b = 0;
            if (type == "clk-skew" || type == "clk-to-clk") {
                if (started || finished || clock_prefix.size() >= 2 ||
                    ctx->getPortTimingClass(from.cell, from.port, a) != TMG_CLOCK_INPUT ||
                    ctx->getPortTimingClass(to.cell, to.port, b) != TMG_CLOCK_INPUT)
                    log_error("Malformed hard input clock prefix.\n");
                clock_prefix.emplace_back(from, to); continue;
            }
            if (finished || (started && !same(previous, from)))
                log_error("Disconnected hard input report path.\n");
            if (type == "clk-to-q" || type == "source") {
                if (started || !same(from, to) || from.cell->ports.at(from.port).type != PORT_OUT)
                    log_error("Malformed hard input launch segment.\n");
                checked.launch = from;
                checked.registered_launch = type == "clk-to-q";
                if (checked.registered_launch && !clock_event(from, path["from"].string_value(), true))
                    log_error("Stale hard input launch clock or edge.\n");
                if (!checked.registered_launch && path["from"].string_value() != "<async>")
                    log_error("Malformed hard input asynchronous launch.\n");
            } else if (type == "routing") {
                auto *net = from.cell->getPort(from.port);
                if (!started || !net || from.cell->ports.at(from.port).type != PORT_OUT ||
                    to.cell->ports.at(to.port).type != PORT_IN || net != to.cell->getPort(to.port) ||
                    net->driver.cell != from.cell || net->driver.port != from.port ||
                    segment["net"].string_value() != net->name.str(ctx) ||
                    !net->users.count(to.cell->ports.at(to.port).user_idx))
                    log_error("Stale hard input report edge.\n");
                auto user = net->users.at(to.cell->ports.at(to.port).user_idx);
                if (!same(user, to)) log_error("Stale hard input report raw user.\n");
                checked.edges.emplace_back(from, to);
            } else if (type == "logic") {
                DelayQuad delay;
                if (!started || from.cell != to.cell ||
                    ctx->getPortTimingClass(from.cell, from.port, a) != TMG_COMB_INPUT ||
                    ctx->getPortTimingClass(to.cell, to.port, b) != TMG_COMB_OUTPUT ||
                    !ctx->getCellDelay(from.cell, from.port, to.port, delay))
                    log_error("Malformed hard input logic segment.\n");
            } else if (type == "setup") {
                if (!started || checked.edges.empty() || !same(from, to) ||
                    !same(checked.edges.back().second, to) || !clock_event(to, path["to"].string_value(), false))
                    log_error("Stale hard input setup clock or endpoint.\n");
                checked.capture = to; checked.registered_capture = true; finished = true;
            } else {
                log_error("Unsupported hard input path segment.\n");
            }
            previous = to; started = true;
        }
        if (!started || checked.edges.empty() || !std::isfinite(total))
            log_error("Malformed hard input report path.\n");
        if (!checked.registered_capture) {
            int count = 0;
            auto cls = ctx->getPortTimingClass(previous.cell, previous.port, count);
            if (cls == TMG_REGISTER_INPUT || path["to"].string_value() != "<async>")
                log_error("Missing hard input report setup endpoint.\n");
        }
        for (const auto &prefix : clock_prefix) {
            auto matches = [&](PortRef data, PortRef clock, bool output) {
                if (!data.cell || data.cell != clock.cell) return false;
                int count = 0;
                if (ctx->getPortTimingClass(data.cell, data.port, count) !=
                    (output ? TMG_REGISTER_OUTPUT : TMG_REGISTER_INPUT)) return false;
                for (int i = 0; i < count; ++i)
                    if (ctx->getPortClockingInfo(data.cell, data.port, i).clock_port == clock.port) return true;
                return false;
            };
            if (!checked.registered_launch || !checked.registered_capture ||
                !matches(checked.launch, prefix.first, true) || !matches(checked.capture, prefix.second, false))
                log_error("Hard input clock prefix does not match its registers.\n");
        }
        checked.excess = total - path["max_delay"].number_value();
        result.push_back(std::move(checked));
    }
    return result;
}

Options read_hard_input_options(const char *spec)
{
    std::istringstream options(spec); std::string path, extra;
    int budget = 0, radius = 0;
    if (!(options >> path >> budget >> radius) || (options >> extra) ||
        budget < 1 || budget > 64 || radius < 1 || radius > 24)
        log_error("Hard input locality requires exactly report, budget 1..64 and radius 1..24.\n");
    std::ifstream file(path);
    if (!file) log_error("Cannot read hard input locality timing report.\n");
    std::string report((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()), error;
    auto json = json11::Json::parse(report, error);
    if (!error.empty() || !json.is_object() || !json["critical_paths"].is_array())
        log_error("Invalid hard input locality timing report.\n");
    return {std::move(report), budget, radius};
}

bool hard_rows_complete(const guard::Rows &rows)
{
    if (rows.empty()) return false;
    for (const auto &row : rows) {
        if (!std::isfinite(double(row.max_path_delay)) || !std::isfinite(double(row.min_path_delay)) ||
            row.max_path_delay >= std::numeric_limits<delay_t>::max() ||
            row.max_path_delay <= std::numeric_limits<delay_t>::lowest() ||
            row.min_path_delay >= std::numeric_limits<delay_t>::max() ||
            row.min_path_delay <= std::numeric_limits<delay_t>::lowest() ||
            (row.setup_timed && (!row.setup_window || !row.setup_margin || !row.hold_related || !row.hold_margin)) ||
            (row.setup_window && !finite_timing(*row.setup_window)) ||
            (row.setup_margin && !finite_timing(*row.setup_margin)) ||
            (row.hold_margin && !finite_timing(*row.hold_margin))) return false;
    }
    return true;
}
} // namespace

void prevalidate_hard_input_locality_prefix(Context *ctx)
{
    // Reuse the existing read-only ordering checks, without executing a pass.
    prevalidate_lut_driver_copy_prefix(ctx);
    if (!ctx->lut_driver_copy_report.empty() && ctx->lut_driver_copy_selection < 0)
        log_error("A LUT driver copy listing must be final; it cannot precede hard input locality.\n");
}

int hard_input_locality(Context *ctx, const std::string &report, int budget, int radius)
{
    if (budget < 1 || budget > 64 || radius < 1 || radius > 24)
        log_error("Hard input locality budget must be 1..64 and radius 1..24.\n");
    if (ctx->fes_any_slot_region_active ||
        (ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() != "") ||
        (ctx->settings.count(id_placer) && ctx->settings.at(id_placer).as_string() != "heap"))
        log_error("Hard input locality requires ordinary fresh HeAP placement.\n");
    prevalidate_hard_input_locality_prefix(ctx);
    for (const auto &entry : ctx->nets) if (!entry.second->wires.empty())
        log_error("Hard input locality requires an unrouted design.\n");
    std::string error; auto json = json11::Json::parse(report, error);
    if (!error.empty() || !json.is_object() || !json["critical_paths"].is_array())
        log_error("Invalid hard input locality timing report.\n");
    const auto paths = hard_paths(ctx, json); // Whole report validated before binding.
    auto lab = [&](BelId bel) { auto at = ctx->getBelLocation(bel); return Lab(at.x, at.y); };
    auto weak = [&](const CellInfo *cell) {
        return cell && cell->bel != BelId() && cell->belStrength <= STRENGTH_WEAK &&
               cell->cluster == ClusterId() && !cell->region && !cell->isPseudo() &&
               !protected_attrs(cell->attrs, ctx);
    };
    std::set<Lab> protected_labs;
    std::map<IdString, size_t> bel_counts;
    for (auto bel : ctx->getBels()) ++bel_counts[ctx->getBelType(bel)];
    for (const auto &entry : ctx->cells) {
        auto *cell = entry.second.get();
        if (cell->bel != BelId() && (!weak(cell) || cell->type.in(id_MISTRAL_MLAB, id_MISTRAL_ALUT_ARITH)))
            protected_labs.insert(lab(cell->bel));
    }
    for (const auto &native_lab : ctx->labs) {
        bool protected_mode = native_lab.is_mlab;
        for (const auto &alm : native_lab.alms) protected_mode |= alm.carry_mode;
        if (protected_mode && !native_lab.alms.empty())
            protected_labs.insert(lab(native_lab.alms.front().lut_bels[0]));
    }
    std::set<const NetInfo *> boundary;
    for (const auto &entry : ctx->ports) if (entry.second.net) boundary.insert(entry.second.net);
    auto ordinary = [&](const NetInfo *net) {
        return net && net->driver.cell && net->driver.cell->bel != BelId() && !net->driver.cell->isPseudo() &&
               !net->is_global && !net->clkconstr && !net->region && net->wires.empty() &&
               net->constant_value == IdString() && !boundary.count(net) && !protected_attrs(net->attrs, ctx) &&
               net->driver.cell->ports.count(net->driver.port) &&
               net->driver.cell->ports.at(net->driver.port).type == PORT_OUT &&
               net->driver.cell->getPort(net->driver.port) == net;
    };
    auto valid_user = [&](NetInfo *net, PortRef user) {
        if (!user.cell || !user.cell->ports.count(user.port) || user.cell->getPort(user.port) != net ||
            user.cell->ports.at(user.port).type != PORT_IN ||
            !net->users.count(user.cell->ports.at(user.port).user_idx)) return false;
        auto actual = net->users.at(user.cell->ports.at(user.port).user_idx);
        return actual.cell == user.cell && actual.port == user.port;
    };
    auto wire_at = [&](CellInfo *cell, BelId bel, IdString pin) {
        if (!cell->pin_data.count(pin) || cell->pin_data.at(pin).bel_pins.size() != 1) return WireId();
        auto physical = cell->pin_data.at(pin).bel_pins.front();
        const auto &pins = ctx->bel_data(bel).pins;
        if (!cell->ports.count(pin) || !pins.count(physical) || pins.at(physical).dir != cell->ports.at(pin).type)
            return WireId();
        return ctx->getBelPinWire(bel, physical);
    };
    auto clocked = [&](CellInfo *cell, IdString pin, int count) {
        if (count != 1 || cell->bel == BelId() || wire_at(cell, cell->bel, pin) == WireId()) return false;
        auto info = ctx->getPortClockingInfo(cell, pin, 0); auto *clock = cell->getPort(info.clock_port);
        int clock_count = 0;
        if (!clock || !clock->clkconstr || !clock->wires.empty() || clock->region ||
            ctx->getPortTimingClass(cell, info.clock_port, clock_count) != TMG_CLOCK_INPUT ||
            !valid_user(clock, {cell, info.clock_port}) || wire_at(cell, cell->bel, info.clock_port) == WireId() ||
            clock->clkconstr->period.minDelay() <= 0 || clock->clkconstr->high.minDelay() <= 0 ||
            clock->clkconstr->low.minDelay() <= 0 || !finite_timing(clock->clkconstr->period.maxDelay()) ||
            !finite_timing(clock->clkconstr->high.maxDelay()) || !finite_timing(clock->clkconstr->low.maxDelay()) ||
            clock->clkconstr->period.minDelay() > clock->clkconstr->period.maxDelay() ||
            clock->clkconstr->high.minDelay() > clock->clkconstr->high.maxDelay() ||
            clock->clkconstr->low.minDelay() > clock->clkconstr->low.maxDelay() ||
            !finite_timing(clock->clkconstr->phase_shift) ||
            !finite_timing(info.setup.minDelay()) || !finite_timing(info.setup.maxDelay()) ||
            !finite_timing(info.hold.minDelay()) || !finite_timing(info.hold.maxDelay()) ||
            !finite_delay(info.clockToQ.minDelay()) || !finite_delay(info.clockToQ.maxDelay())) return false;
        return info.edge == RISING_EDGE || info.edge == FALLING_EDGE;
    };
    const std::array<IdString, 6> inputs{{id_DATAIN, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA}};
    auto movable = [&](CellInfo *cell) {
        if (!weak(cell) || cell->type != id_MISTRAL_FF || cell->ports.size() != 8 ||
            protected_labs.count(lab(cell->bel)) || !ordinary(cell->getPort(id_Q)) ||
            !ordinary(cell->getPort(id_DATAIN)) || cell->get_pin_state(id_Q) != PIN_SIG ||
            !cell->ports.count(id_Q) || cell->ports.at(id_Q).type != PORT_OUT ||
            !cell->ports.count(id_CLK) || cell->ports.at(id_CLK).type != PORT_IN ||
            (cell->get_pin_state(id_CLK) != PIN_SIG && cell->get_pin_state(id_CLK) != PIN_INV)) return false;
        int count = 0;
        if (ctx->getPortTimingClass(cell, id_Q, count) != TMG_REGISTER_OUTPUT || !clocked(cell, id_Q, count)) return false;
        for (auto pin : inputs) {
            if (!cell->ports.count(pin) || cell->ports.at(pin).type != PORT_IN) return false;
            if (!cell->getPort(pin)) continue;
            if (!ordinary(cell->getPort(pin)) ||
                (cell->get_pin_state(pin) != PIN_SIG && cell->get_pin_state(pin) != PIN_INV) ||
                ctx->getPortTimingClass(cell, pin, count) != TMG_REGISTER_INPUT || !clocked(cell, pin, count) ||
                !valid_user(cell->getPort(pin), {cell, pin})) return false;
        }
        return true;
    };
    auto hard_sink = [&](PortRef sink) {
        auto *cell = sink.cell; int count = 0;
        return cell && cell->bel != BelId() && !cell->region && !cell->isPseudo() &&
               cell->type != id_MISTRAL_FF && !ctx->is_comb_cell(cell->type) &&
               !cell->type.in(id_MISTRAL_BUF, id_MISTRAL_NOT, id_MISTRAL_MLAB) &&
               (bel_counts[ctx->getBelType(cell->bel)] == 1 || cell->belStrength >= STRENGTH_LOCKED) &&
               ctx->getPortTimingClass(cell, sink.port, count) == TMG_REGISTER_INPUT && clocked(cell, sink.port, count);
    };
    struct Attempt { CellInfo *ff; PortRef sink; WireId wire; double excess; int64_t delay; };
    std::map<IdString, Attempt> unique;
    for (const auto &path : paths) {
        if (!path.registered_launch || !path.registered_capture || path.edges.size() != 1) continue;
        auto edge = path.edges.front(); auto *ff = edge.first.cell;
        if (edge.first.port != id_Q || edge.first.cell != path.launch.cell || edge.first.port != path.launch.port ||
            edge.second.cell != path.capture.cell || edge.second.port != path.capture.port ||
            !movable(ff) || !hard_sink(edge.second) || !valid_user(ff->getPort(id_Q), edge.second)) continue;
        auto wire = ctx->getNetinfoSinkWire(ff->getPort(id_Q), edge.second, 0);
        auto source = wire_at(ff, ff->bel, id_Q);
        if (wire == WireId() || source == WireId()) continue;
        auto delay = ctx->estimateDelay(source, wire); if (!finite_delay(delay)) continue;
        Attempt next{ff, edge.second, wire, path.excess, delay};
        auto old = unique.find(ff->name);
        if (old == unique.end() || std::make_tuple(next.excess, next.delay) >
                                  std::make_tuple(old->second.excess, old->second.delay)) unique[ff->name] = next;
    }
    std::vector<Attempt> attempts;
    for (const auto &entry : unique) attempts.push_back(entry.second);
    std::sort(attempts.begin(), attempts.end(), [&](const Attempt &a, const Attempt &b) {
        if (a.excess != b.excess) return a.excess > b.excess;
        if (a.delay != b.delay) return a.delay > b.delay;
        return a.ff->name.str(ctx) < b.ff->name.str(ctx);
    });
    if (attempts.size() > size_t(budget)) attempts.resize(budget);
    int accepted = 0;
    for (const auto &attempt : attempts) {
        auto *ff = attempt.ff; const CellPortKey target(attempt.sink);
        std::set<CellPortKey> endpoints{target}; std::set<NetInfo *> visiting, done;
        size_t edges = 0;
        std::function<bool(NetInfo *)> follow = [&](NetInfo *net) {
            if (!ordinary(net) || net->users.empty() || ctx->getNetinfoSourceWire(net) == WireId()) return false;
            if (done.count(net)) return true;
            if (done.size() + visiting.size() >= 1024 || !visiting.insert(net).second) return false;
            for (auto user : net->users) {
                if (++edges > 4096 || !valid_user(net, user) || user.cell->bel == BelId() ||
                    user.cell->region || user.cell->isPseudo() || protected_attrs(user.cell->attrs, ctx) ||
                    ctx->getNetinfoSinkWire(net, user, 0) == WireId()) return false;
                int count = 0; auto cls = ctx->getPortTimingClass(user.cell, user.port, count);
                if (cls == TMG_REGISTER_INPUT) {
                    if (!clocked(user.cell, user.port, count) || endpoints.size() >= 2048) return false;
                    endpoints.insert(CellPortKey(user));
                } else {
                    if (cls != TMG_COMB_INPUT || !(mistral_remap_report::lut_width(user.cell->type) ||
                        user.cell->type.in(id_MISTRAL_BUF, id_MISTRAL_NOT, id_MISTRAL_ALUT_ARITH))) return false;
                    bool arc = false;
                    for (const auto &port : user.cell->ports) if (port.second.type == PORT_OUT && port.second.net) {
                        DelayQuad delay; int outputs = 0;
                        if (ctx->getPortTimingClass(user.cell, port.first, outputs) != TMG_COMB_OUTPUT ||
                            !ctx->getCellDelay(user.cell, user.port, port.first, delay)) continue;
                        if (!finite_delay(delay.minDelay()) || !finite_delay(delay.maxDelay()) || !follow(port.second.net)) return false;
                        arc = true;
                    }
                    if (!arc) return false;
                }
            }
            visiting.erase(net); done.insert(net); return true;
        };
        bool covered = follow(ff->getPort(id_Q));
        for (auto pin : inputs) if (ff->getPort(pin)) {
            endpoints.insert(CellPortKey(ff->name, pin)); covered &= follow(ff->getPort(pin));
        }
        if (!covered) {
            log_info("Hard input locality rejected %s: complete input/control/feedback/branch coverage unavailable.\n", ctx->nameOf(ff));
            continue;
        }
        TimingAnalyser before(ctx); before.with_clock_skew = true; before.setup(false, false, true);
        float old_slack = before.get_setup_slack(target);
        if (before.have_loops || !guard::timed(old_slack) || before.get_timing_result().clock_fmax.empty()) continue;
        std::map<CellPortKey, guard::Rows> rows, references; bool complete = true, unrelated = false;
        for (auto key : endpoints) {
            guard::Rows current;
            if (!before.get_endpoint_clock_pair_timings(key, current) || !hard_rows_complete(current)) { complete = false; break; }
            for (const auto &row : current) unrelated |= !row.setup_timed;
            rows.emplace(key, std::move(current));
        }
        if (unrelated && complete) {
            TimingAnalyser reference(ctx); reference.with_clock_skew = false; reference.setup(false, false, true);
            complete &= !reference.have_loops;
            for (const auto &entry : rows) {
                guard::Rows current;
                if (!reference.get_endpoint_clock_pair_timings(entry.first, current) || !hard_rows_complete(current) ||
                    !guard::rows_match(entry.second, current)) complete = false;
                else references.emplace(entry.first, std::move(current));
            }
        }
        if (!complete) continue;
        const auto old_holds = guard::holds(before); const auto old_bel = ff->bel; const auto strength = ff->belStrength;
        const auto old_wire_delay = ctx->estimateDelay(wire_at(ff, old_bel, id_Q), attempt.wire);
        auto free_half = [&](BelId bel) {
            if (bel == old_bel || ctx->getBelType(bel) != id_MISTRAL_FF || !ctx->isValidBelForCellType(ff->type, bel) ||
                ctx->getBoundBelCell(bel) || protected_labs.count(lab(bel))) return false;
            auto at = ctx->getBelLocation(bel); if (at.z % 6 != 2 && at.z % 6 != 4) return false;
            auto comb = ctx->getBelByLocation(Loc(at.x, at.y, (at.z / 6) * 6 + (at.z % 6 == 4 ? 1 : 0)));
            auto partner = ctx->getBelByLocation(Loc(at.x, at.y, at.z + 1));
            if (comb == BelId() || partner == BelId() || ctx->getBoundBelCell(comb) || ctx->getBoundBelCell(partner)) return false;
            const auto &d = ctx->bel_data(bel).lab_data;
            if (ctx->labs.at(d.lab).is_mlab || ctx->labs.at(d.lab).alms.at(d.alm).carry_mode ||
                wire_at(ff, bel, id_Q) == WireId()) return false;
            for (auto pin : inputs) if (ff->getPort(pin) && wire_at(ff, bel, pin) == WireId()) return false;
            return true;
        };
        struct Site { BelId bel; int64_t delay; };
        std::vector<Site> all, candidates; const auto center = ctx->getBelLocation(old_bel);
        for (int x = center.x - radius; x <= center.x + radius; ++x)
            for (int y = center.y - radius; y <= center.y + radius; ++y) {
                if (std::abs(x - center.x) + std::abs(y - center.y) > radius) continue;
                for (auto bel : ctx->getBelsByTile(x, y)) if (free_half(bel)) {
                    auto delay = ctx->estimateDelay(wire_at(ff, bel, id_Q), attempt.wire);
                    if (finite_delay(delay) && old_wire_delay - int64_t(delay) >= 250) all.push_back({bel, delay});
                }
            }
        std::sort(all.begin(), all.end(), [&](const Site &a, const Site &b) {
            auto x = ctx->getBelLocation(a.bel), y = ctx->getBelLocation(b.bel);
            return std::make_tuple(a.delay, x.x, x.y, x.z) < std::make_tuple(b.delay, y.x, y.y, y.z);
        });
        std::set<Lab> tiles; std::set<std::tuple<int, int, int>> halves;
        for (auto site : all) {
            auto at = ctx->getBelLocation(site.bel); auto tile = lab(site.bel);
            if (!tiles.count(tile) && tiles.size() >= 32) continue;
            if (!halves.insert({at.x, at.y, (at.z % 6) / 2}).second) continue;
            tiles.insert(tile); candidates.push_back(site); if (candidates.size() == 64) break;
        }
        Snapshot saved(ctx); int geometry = 0, legal_rejects = 0, fixed_rejects = 0, timed = 0; bool retained = false;
        log_info("Hard input locality launch=%s sink=%s.%s wire=%s sites=%zu endpoints=%zu.\n",
                 ctx->nameOf(ff), ctx->nameOf(attempt.sink.cell), attempt.sink.port.c_str(ctx),
                 ctx->getWireName(attempt.wire).str(ctx).c_str(), candidates.size(), endpoints.size());
        for (auto candidate : candidates) {
            if (timed >= 16) break;
            ++geometry; bool live = true;
            auto restore = [&]() { if (live) { saved.restore_one(ctx, ff, old_bel, strength); live = false; } };
            try {
                ctx->unbindBel(old_bel); ctx->bindBel(candidate.bel, ff, strength);
                std::set<uint32_t> changed_labs; std::set<std::pair<uint32_t, uint8_t>> changed_counts;
                for (auto bel : {old_bel, candidate.bel}) {
                    const auto &d = ctx->bel_data(bel).lab_data;
                    changed_labs.insert(d.lab); changed_counts.insert({d.lab, d.alm});
                }
                auto legal = [&]() {
                    for (auto index : changed_labs) for (const auto &alm : ctx->labs.at(index).alms)
                        for (auto bel : {alm.lut_bels[0], alm.lut_bels[1], alm.ff_bels[0], alm.ff_bels[1], alm.ff_bels[2], alm.ff_bels[3]})
                            if (ctx->getBoundBelCell(bel) && !ctx->isBelLocationValid(bel)) return false;
                    return true;
                };
                bool legal_now = legal(); bool fixed_now = saved.fixed(ctx, ff, candidate.bel, nullptr, BelId(), changed_counts);
                if (!legal_now || !fixed_now) {
                    legal_rejects += !legal_now; fixed_rejects += !fixed_now;
                    if (legal_rejects + fixed_rejects <= 4)
                        log_info("Hard input locality preflight launch=%s bel=%s legal=%d fixed=%d.\n", ctx->nameOf(ff),
                                 ctx->getBelName(candidate.bel).str(ctx).c_str(), int(legal_now), int(fixed_now));
                    restore(); continue;
                }
                ++timed;
                TimingAnalyser after(ctx); after.with_clock_skew = true; after.setup(false, false, true);
                float slack = after.get_setup_slack(target);
                bool improve = !after.have_loops && guard::timed(slack) && slack >= old_slack + 250;
                bool endpoints_ok = !after.have_loops;
                for (const auto &entry : rows) {
                    guard::Rows current;
                    endpoints_ok &= after.get_endpoint_clock_pair_timings(entry.first, current) && hard_rows_complete(current) &&
                                    guard::rows_nonregressing(entry.second, current, false);
                }
                if (!references.empty()) {
                    TimingAnalyser reference(ctx); reference.with_clock_skew = false; reference.setup(false, false, true);
                    endpoints_ok &= !reference.have_loops;
                    for (const auto &entry : references) {
                        guard::Rows current;
                        endpoints_ok &= reference.get_endpoint_clock_pair_timings(entry.first, current) && hard_rows_complete(current) &&
                                        guard::rows_nonregressing(entry.second, current, true);
                    }
                }
                bool clocks_ok = guard::clocks_nonregressing(before, after);
                bool holds_ok = guard::holds_nonregressing(old_holds, guard::holds(after));
                bool fixed = legal() && saved.fixed(ctx, ff, candidate.bel, nullptr, BelId(), changed_counts);
                auto current_wire = ctx->getNetinfoSinkWire(ff->getPort(id_Q), attempt.sink, 0);
                auto wire_gain = old_wire_delay - int64_t(ctx->estimateDelay(wire_at(ff, candidate.bel, id_Q), attempt.wire));
                bool wire_ok = current_wire == attempt.wire && wire_gain >= 250;
                log_info("Hard input locality trial launch=%s bel=%s gain=%.0fps wire_gain=%lldps improve=%d wire=%d endpoints=%d clocks=%d holds=%d fixed=%d.\n",
                         ctx->nameOf(ff), ctx->getBelName(candidate.bel).str(ctx).c_str(), slack - old_slack,
                         (long long)wire_gain, int(improve), int(wire_ok), int(endpoints_ok), int(clocks_ok), int(holds_ok), int(fixed));
                if (improve && wire_ok && endpoints_ok && clocks_ok && holds_ok && fixed) {
                    ctx->check(); live = false; retained = true; ++accepted;
                    log_info("Hard input locality retained launch=%s; routing and signoff still required.\n", ctx->nameOf(ff));
                    break;
                }
                restore();
            } catch (...) { restore(); throw; }
        }
        log_info("Hard input locality summary launch=%s attempted_geometry=%d legality_rejects=%d preservation_rejects=%d timed_trials=%d retained=%d.\n",
                 ctx->nameOf(ff), geometry, legal_rejects, fixed_rejects, timed, int(retained));
    }
    ctx->check();
    log_info("Hard input locality: eligible=%zu attempted=%zu retained=%d (route signoff still required).\n", unique.size(), attempts.size(), accepted);
    return accepted;
}

void preload_hard_input_locality(Context *ctx, const char *spec)
{
    ctx->hard_input_report.clear(); ctx->hard_input_budget = 0; ctx->hard_input_radius = 24;
    if (!spec) return;
    auto options = read_hard_input_options(spec);
    ctx->hard_input_report = std::move(options.report); ctx->hard_input_budget = options.budget;
    ctx->hard_input_radius = options.radius;
}
NEXTPNR_NAMESPACE_END
