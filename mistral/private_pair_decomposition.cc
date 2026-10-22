/* Bounded private ALUT3 -> ALUT5 rewrite at unchanged placement. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "critical_cohort.h"
#include "private_pair_decomposition_native.h"
#include "analogue_repair.h"
#include "cyclonev.h"
#include "remap_report.h"
#include "log.h"
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <limits>
#include <set>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
namespace policy = private_pair_decomposition;
const std::array<IdString, 5> pins{id_A, id_B, id_C, id_D, id_E};
using Alm = std::pair<uint32_t, uint8_t>;
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
bool ordinary_context(Context *ctx)
{
    if (ctx->fes_any_slot_region_active || ctx->fes_fence_active || ctx->fes_has_reserved_rect ||
        !ctx->fes_frozen_cells.empty() || !ctx->fes_frozen_pips.empty()) return false;
    for (const auto &net : ctx->nets) if (!net.second->wires.empty()) return false;
    return true;
}
struct Slot { NetInfo *net; store_index<PortRef> index; };
struct PairView {
    CellInfo *producer = nullptr, *consumer = nullptr;
    NetInfo *link = nullptr;
    policy::Pair function;
    std::vector<NetInfo *> signals;
    std::map<int, Slot> slots;
    std::set<Alm> alms;
    std::set<uint32_t> labs;
};
bool inspect_pair_shape(Context *ctx, CellInfo *producer, PairView &view, std::string &reason)
{
    auto fail = [&](const char *why) { reason = why; return false; };
    if (!movable(ctx, producer) || producer->type != id_MISTRAL_ALUT3)
        return fail("requires an ordinary movable ALUT3");
    auto *link = producer->getPort(id_Q);
    if (!link || link->driver.cell != producer || link->driver.port != id_Q || link->users.entries() != 1)
        return fail("producer must have exactly one consumer");
    auto *consumer = (*link->users.begin()).cell;
    if (consumer == producer || !movable(ctx, consumer) || consumer->type != id_MISTRAL_ALUT5)
        return fail("consumer must be an ordinary movable ALUT5");
    auto unsafe = [&](NetInfo *net) {
        if (!net || !net->driver.cell || net->is_global || net->clkconstr || net->region ||
            net->constant_value != IdString() || protected_attrs(ctx, net->attrs)) return true;
        for (const auto &port : ctx->ports) if (port.second.net == net) return true;
        for (auto user : net->users) {
            if (!user.cell) return true;
            int clocks = 0;
            if (ctx->getPortTimingClass(user.cell, user.port, clocks) == TMG_CLOCK_INPUT) return true;
        }
        return false;
    };
    view.producer = producer; view.consumer = consumer; view.link = link;
    view.alms = {alm_of(ctx, producer), alm_of(ctx, consumer)};
    for (auto where : view.alms) {
        view.labs.insert(where.first);
        const auto &alm = ctx->labs.at(where.first).alms.at(where.second);
        for (auto bel : {alm.lut_bels[0], alm.lut_bels[1], alm.ff_bels[0], alm.ff_bels[1], alm.ff_bels[2], alm.ff_bels[3]})
            if (auto *cell = ctx->getBoundBelCell(bel))
                if (!movable(ctx, cell) || cell->type.in(id_MISTRAL_MLAB,id_MISTRAL_ALUT6,id_MISTRAL_ALUT_ARITH)) return fail("protected affected ALM occupant");
        if (alm.l6_mode || alm.carry_mode) return fail("requires ordinary non-LUT6 non-carry ALMs");
    }
    if (!legal_labs(ctx, view.labs)) return fail("original LAB is illegal");
    for (auto *cell : {producer, consumer}) {
        const size_t width = cell == producer ? 3 : 5;
        if (cell->ports.size() != width + 1 || cell->params.size() != 1 || !cell->params.count(id_LUT) ||
            !cell->params.at(id_LUT).is_fully_def() || cell->params.at(id_LUT).size() != (1u << width) ||
            !cell->ports.count(id_Q) || cell->ports.at(id_Q).type != PORT_OUT || cell->get_pin_state(id_Q) != PIN_SIG ||
            unsafe(cell->getPort(id_Q)) || cell->getPort(id_Q)->driver.cell != cell ||
            cell->getPort(id_Q)->driver.port != id_Q) return fail("unsupported LUT table/output");
        for (size_t p = 0; p < width; ++p) {
            auto port = cell->ports.find(pins[p]);
            if (port == cell->ports.end() || port->second.type != PORT_IN || unsafe(port->second.net))
                return fail("unsupported input net");
            auto *net = port->second.net;
            auto state = cell->get_pin_state(pins[p]);
            if ((state != PIN_SIG && state != PIN_INV) || !net->users.count(port->second.user_idx) ||
                net->users.at(port->second.user_idx).cell != cell || net->users.at(port->second.user_idx).port != pins[p])
                return fail("unsupported input state or user slot");
            if (net == consumer->getPort(id_Q) || (cell == producer && net == link)) return fail("pair cycle");
            if (net != link) view.signals.push_back(net);
        }
    }
    std::sort(view.signals.begin(), view.signals.end(), [&](auto *a, auto *b) { return a->name.str(ctx) < b->name.str(ctx); });
    if (view.signals.size() != 7 || std::adjacent_find(view.signals.begin(), view.signals.end()) != view.signals.end())
        return fail("requires seven external signals each used once");
    for (auto *cell : {producer, consumer}) {
        auto &lut = cell == producer ? view.function.producer : view.function.consumer;
        lut.mask = uint64_t(cell->params.at(id_LUT).as_int64());
        const size_t width = cell == producer ? 3 : 5;
        for (size_t p = 0; p < width; ++p) {
            const auto &port = cell->ports.at(pins[p]);
            int signal = port.net == link ? policy::INTERNAL : int(std::find(view.signals.begin(), view.signals.end(), port.net) - view.signals.begin());
            if (!view.slots.emplace(signal, Slot{port.net, port.user_idx}).second) return fail("duplicate source slot");
            lut.pins.push_back({signal, cell->get_pin_state(pins[p]) == PIN_INV});
        }
    }
    return !policy::decompose(view.function).empty() || fail("function is outside the essential seven-input scope");
}
bool inspect_pair(Context *ctx, CellInfo *producer, PairView &view, std::string &reason)
{
    // The original placed pass remains strictly unrouted.
    if (!ordinary_context(ctx)) {
        reason = "requires an ordinary movable ALUT3 in an unrouted full placement";
        return false;
    }
    return inspect_pair_shape(ctx, producer, view, reason);
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
    const unsigned width = cell->type == id_MISTRAL_ALUT3 ? 3 : 5;
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
struct Transaction {
    struct Cell { CellInfo *cell; decltype(CellInfo::ports) ports; decltype(CellInfo::params) params; BelId bel; PlaceStrength strength; };
    Context *ctx;
    std::vector<Cell> cells;
    std::map<NetInfo *, indexed_store<PortRef>> users;
    std::map<CellInfo *, ArchCellInfo> architecture;
    std::vector<LABInfo> labs;
    bool kept = false;
    Transaction(Context *ctx, const PairView &view) : ctx(ctx), labs(ctx->labs)
    {
        for (auto *cell : {view.producer, view.consumer})
            cells.push_back({cell, cell->ports, cell->params, cell->bel, cell->belStrength});
        for (const auto &slot : view.slots) users.emplace(slot.second.net, slot.second.net->users);
        for (const auto &entry : ctx->cells) if (entry.second->bel != BelId()) {
            auto *cell = entry.second.get();
            if ((ctx->is_comb_cell(cell->type) || cell->type == id_MISTRAL_FF) && view.labs.count(alm_of(ctx, cell).first))
                architecture.emplace(cell, *static_cast<ArchCellInfo *>(cell));
        }
    }
    void unbind() { for (auto &saved : cells) if (saved.cell->bel != BelId()) ctx->unbindBel(saved.cell->bel); }
    ~Transaction()
    {
        if (kept) return;
        unbind();
        for (auto &saved : cells) { saved.cell->ports = std::move(saved.ports); saved.cell->params = std::move(saved.params); }
        for (auto &entry : users) entry.first->users = std::move(entry.second);
        for (auto &entry : architecture) *static_cast<ArchCellInfo *>(entry.first) = std::move(entry.second);
        for (auto &saved : cells) ctx->bindBel(saved.bel, saved.cell, saved.strength);
        ctx->labs = std::move(labs);
    }
};
}

// Inspection is read-only. The optional stage callback is for native fault
// injection; exceptions at every mutation stage must restore the exact state.
bool private_pair_trial(Context *ctx, CellInfo *producer, const policy::Candidate &candidate,
                        const std::function<bool()> &accept, std::string *reason,
                        const std::function<void(unsigned)> &stage)
{
    PairView view; std::string why;
    auto fail = [&](const char *text) { if (reason) *reason = text; return false; };
    if (!inspect_pair(ctx, producer, view, why)) { if (reason) *reason = why; return false; }
    if (candidate.network.producer.pins.size() != 3 || candidate.network.consumer.pins.size() != 5 ||
        !reduction_reassociate::equivalent(view.function, candidate.network)) return fail("candidate is not equivalent");
    std::set<int> used;
    for (auto *lut : {&candidate.network.producer, &candidate.network.consumer}) for (auto pin : lut->pins)
        if (!view.slots.count(pin.signal) || !used.insert(pin.signal).second) return fail("candidate changes input-slot multiplicity");
    if (used.size() != 8) return fail("candidate input-slot coverage differs");
    Transaction tx(ctx, view);
    // Compare against the native pre-route allocation, not placement scaffolds.
    // Transaction construction precedes this preview so listing restores both.
    for (auto where : view.alms) ctx->assign_alm_lut_inputs(where.first, where.second);
    if (!physical_function(ctx,view.producer) || !physical_function(ctx,view.consumer))
        return fail("original physical function is invalid");
    std::map<Alm,uint64_t> original_masks;
    std::map<CellInfo *,decltype(CellInfo::pin_data)> stationary_pins;
    for (auto where : view.alms) {
        original_masks.emplace(where,ctx->compute_lut_mask(where.first,where.second));
        for (auto bel : ctx->labs.at(where.first).alms.at(where.second).lut_bels)
            if (auto *cell = ctx->getBoundBelCell(bel))
                if (cell != view.producer && cell != view.consumer) stationary_pins.emplace(cell,cell->pin_data);
    }
    tx.unbind(); if (stage) stage(1);
    size_t ci = 0;
    for (auto *lut : {&candidate.network.producer, &candidate.network.consumer}) {
        auto *cell = tx.cells.at(ci++).cell;
        cell->params[id_LUT] = Property(int64_t(lut->mask), 1u << lut->pins.size());
        for (size_t p = 0; p < lut->pins.size(); ++p) {
            const auto &slot = view.slots.at(lut->pins[p].signal);
            auto &port = cell->ports.at(pins[p]); port.net = slot.net; port.user_idx = slot.index;
            slot.net->users.at(slot.index) = {cell, pins[p]};
            cell->pin_data[pins[p]].state = lut->pins[p].inverted ? PIN_INV : PIN_SIG;
        }
        ctx->assign_comb_info(cell); ctx->assign_default_pinmap(cell);
    }
    if (stage) stage(2);
    for (const auto &saved : tx.cells) ctx->bindBel(saved.bel, saved.cell, saved.strength);
    if (stage) stage(3);
    if (!legal_labs(ctx, view.labs)) return fail("rewritten LAB is illegal");
    for (auto where : view.alms) ctx->assign_alm_lut_inputs(where.first, where.second);
    if (stage) stage(4);
    if (!legal_labs(ctx, view.labs)) return fail("physical input assignment is illegal");
    for (auto where : view.alms) {
        const auto &before = tx.labs.at(where.first).alms.at(where.second);
        const auto &after = ctx->labs.at(where.first).alms.at(where.second);
        if (after.l6_mode != before.l6_mode || after.carry_mode != before.carry_mode) return fail("ALM mode changed");
    }
    for (const auto &saved : tx.cells) for (size_t p = 0; p < (saved.cell == producer ? 3u : 5u); ++p)
        if (saved.cell->pin_data.at(pins[p]).bel_pins.size() != 1) return fail("physical input is unmapped");
    if (!physical_function(ctx,view.producer) || !physical_function(ctx,view.consumer))
        return fail("candidate physical function is invalid");
    for (const auto &stationary : stationary_pins) {
        auto *cell = stationary.first; auto where = alm_of(ctx,cell);
        if (!same_pins(cell,stationary.second) ||
            ((ctx->compute_lut_mask(where.first,where.second) ^ original_masks.at(where)) & half_mask(ctx,cell)))
            return fail("stationary ALM mate physical mapping or mask changed");
    }
    ctx->check(); if (stage) stage(5);
    if (!accept()) return fail("complete inspection rejected");
    tx.kept = true; if (reason) *reason = "equivalent private pair at unchanged BELs";
    return true;
}

bool private_pair_common_arrivals(Context *ctx, TimingAnalyser &timing, const std::vector<NetInfo *> &signals,
                                  std::array<delay_t, 7> &arrivals, std::string *reason)
{
    auto fail = [&](const char *why) { if (reason) *reason = why; return false; };
    if (signals.size() != 7) return fail("requires seven native arrivals");
    std::optional<ClockDomainKey> launch;
    std::set<NetInfo *> distinct;
    for (size_t i = 0; i < signals.size(); ++i) {
        if (!signals[i] || !distinct.insert(signals[i]).second) return fail("null or duplicate external signal");
        auto driver = signals[i]->driver; int clocks = 0;
        if (!driver.cell || driver.cell->getPort(driver.port) != signals[i] ||
            !driver.cell->type.in(id_MISTRAL_FF, id_MISTRAL_M10K) ||
            ctx->getPortTimingClass(driver.cell, driver.port, clocks) != TMG_REGISTER_OUTPUT || clocks <= 0)
            return fail("external source is not a characterized direct FF/M10K registered output");
        std::vector<TimingAnalyser::SetupWindowHint> rows;
        if (!timing.get_setup_window_hints(CellPortKey(driver), rows) || !timing.get_max_arrival(CellPortKey(driver), arrivals[i]))
            return fail("incomplete native arrival/window coverage");
        for (const auto &row : rows) {
            if (row.cut || !row.window || *row.window <= 0) return fail("cut or untimed native relationship");
            if (!launch) launch = row.launch;
            if (launch->clock != row.launch.clock || launch->edge != row.launch.edge) return fail("mixed launch frames");
        }
    }
    return bool(launch);
}

// Full fanout walk prevents a known capture tag from hiding an unsupported
// parallel branch. First-stage scope deliberately excludes hybrid primitives.
bool private_pair_output_coverage(Context *ctx, TimingAnalyser &timing, NetInfo *output, std::string *reason)
{
    std::set<NetInfo *> visiting, done; int work = 0;
    auto cone = [&](auto &&self, NetInfo *net, int depth) -> bool {
        if (!net || depth > 64 || ++work > 4096 || visiting.count(net)) return false;
        if (done.count(net)) return true;
        for (const auto &port : ctx->ports) if (port.second.net == net) return false;
        if (net->is_global || net->clkconstr || net->users.entries() == 0) return false;
        visiting.insert(net);
        for (auto user : net->users) {
            if (!user.cell) return false;
            int clocks = 0; auto kind = ctx->getPortTimingClass(user.cell, user.port, clocks);
            int width = mistral_remap_report::lut_width(user.cell->type);
            if (width && kind == TMG_COMB_INPUT) {
                if (!self(self, user.cell->getPort(id_Q), depth + 1)) return false;
            } else if (user.cell->type.in(id_MISTRAL_FF, id_MISTRAL_M10K) && kind == TMG_REGISTER_INPUT && clocks > 0) {
                // The legacy FF class approximates ACLR synchronously; it is
                // not a characterized asynchronous recovery/removal boundary.
                if (user.cell->type == id_MISTRAL_FF && user.port == id_ACLR) return false;
                std::vector<TimingAnalyser::SetupWindowHint> hints;
                std::vector<EndpointClockPairTiming> rows;
                if (!timing.get_setup_window_hints(CellPortKey(user), hints) ||
                    !timing.get_endpoint_clock_pair_timings(CellPortKey(user), rows) || rows.empty()) return false;
                for (const auto &hint : hints) if (hint.cut || !hint.window || *hint.window <= 0) return false;
                for (const auto &row : rows) {
                    if (!row.setup_timed || !row.hold_related || !row.setup_window || *row.setup_window <= 0 ||
                        !row.setup_margin || !row.hold_margin || !std::isfinite(double(*row.setup_window)) ||
                        !std::isfinite(double(*row.setup_margin)) || !std::isfinite(double(*row.hold_margin))) return false;
                }
                // These are ordinary register boundaries, not hybrid DSP arcs.
                for (const auto &port : user.cell->ports) {
                    DelayQuad delay;
                    if (port.second.type == PORT_OUT && port.second.net &&
                        ctx->getCellDelay(user.cell, user.port, port.first, delay)) return false;
                }
            } else return false;
        }
        visiting.erase(net); done.insert(net); return true;
    };
    bool complete = cone(cone, output, 0);
    if (reason) *reason = complete ? "complete bounded native output cone" : "incomplete native output cone";
    return complete;
}

bool private_pair_clock_safe(const TimingResult &before, const TimingResult &after, std::string *reason)
{
    auto fail = [&](const char *why) { if (reason) *reason = why; return false; };
    auto nonregressing = [](const auto &old, const auto &now) {
        if (old.size() != now.size()) return false;
        for (const auto &clock : old) {
            auto found = now.find(clock.first);
            if (found == now.end() || !std::isfinite(double(clock.second)) ||
                !std::isfinite(double(found->second)) || found->second < clock.second) return false;
        }
        return true;
    };
    if (before.clock_setup_slack.empty() || before.clock_fmax.empty() ||
        !nonregressing(before.clock_setup_slack,after.clock_setup_slack) ||
        !nonregressing(before.clock_hold_slack,after.clock_hold_slack) ||
        before.clock_fmax.size() != after.clock_fmax.size())
        return fail("native clock setup/hold coverage or margin regression");
    for (const auto &clock : before.clock_fmax) {
        auto found = after.clock_fmax.find(clock.first);
        if (found == after.clock_fmax.end() || !std::isfinite(clock.second.achieved) ||
            !std::isfinite(clock.second.constraint) || clock.second.achieved <= 0 || clock.second.constraint <= 0 ||
            !std::isfinite(found->second.achieved) || !std::isfinite(found->second.constraint) ||
            found->second.achieved < clock.second.achieved || found->second.constraint != clock.second.constraint)
            return fail("native clock Fmax coverage, constraint or margin regression");
    }
    return true;
}

bool Arch::remap_private_pair_critical(const std::string &report, int selection)
{
    Context *ctx = getCtx();
    if (selection < -1 || !ordinary_context(ctx)) log_error("Private pair decomposition requires ordinary unrouted placement.\n");
    std::string parse_error; auto json = json11::Json::parse(report, parse_error);
    if (!parse_error.empty()) log_error("Invalid private pair timing report.\n");
    std::map<std::string, CellInfo *> discovered;
    for (const auto &path : mistral_remap_report::validate(ctx, json, true)) for (const auto &edge : path.edges)
        if (edge.first.port == id_Q && edge.first.cell->type == id_MISTRAL_ALUT3 && edge.second.cell->type == id_MISTRAL_ALUT5)
            discovered.emplace(edge.first.cell->name.str(ctx), edge.first.cell);
    log_info("Private pair discovery: %zu report-guided pairs.\n", discovered.size());
    if (discovered.size() > 16) log_error("Private pair discovery exceeds the 16-pair bound.\n");
    json11::Json baseline;
    TimingResult baseline_metrics, baseline_unskewed;
    std::map<std::string, std::array<delay_t, 7>> arrivals;
    critical_cohort_pin_preview(ctx, [&]() {
        TimingAnalyser before(ctx), unskewed(ctx); before.with_clock_skew = true;
        critical_cohort_setup_timing(ctx, before); critical_cohort_setup_timing(ctx, unskewed);
        if (before.have_loops || unskewed.have_loops) log_error("Private pair decomposition refuses timing loops.\n");
        baseline = critical_swap_live_endpoint_snapshot(ctx, before, unskewed);
        baseline_metrics = before.get_timing_result(); baseline_unskewed = unskewed.get_timing_result();
        for (const auto &entry : discovered) {
            PairView view; std::string why; std::array<delay_t, 7> values;
            if (!inspect_pair(ctx, entry.second, view, why) ||
                !private_pair_common_arrivals(ctx, unskewed, view.signals, values, &why) ||
                !private_pair_output_coverage(ctx, before, view.consumer->getPort(id_Q), &why))
                log_info("Private pair ineligible: %s: %s.\n", entry.first.c_str(), why.c_str());
            else arrivals.emplace(entry.first, values);
        }
    });
    int qualified = 0;
    for (const auto &entry : discovered) {
        if (!arrivals.count(entry.first)) continue;
        PairView view; std::string why;
        if (!inspect_pair(ctx, entry.second, view, why)) continue;
        auto candidates = policy::decompose(view.function);
        struct Ranked { int64_t score; size_t index; };
        std::vector<Ranked> ranked;
        for (size_t index = 0; index < candidates.size(); ++index) {
            const auto &candidate = candidates[index]; int64_t encoded = 0, output = 0; bool valid = true;
            for (size_t node = 0; node < 2; ++node) {
                const auto &lut = node ? candidate.network.consumer : candidate.network.producer;
                auto *cell = node ? view.consumer : view.producer; int64_t latest = 0;
                for (size_t p = 0; p < lut.pins.size(); ++p) {
                    int signal = lut.pins[p].signal; DelayQuad logic;
                    NetInfo *net = signal == policy::INTERNAL ? view.link : view.signals.at(signal);
                    if (!getCellDelay(cell, pins[p], id_Q, logic)) { valid = false; break; }
                    int64_t time = signal == policy::INTERNAL ? encoded : arrivals.at(entry.first).at(signal);
                    time += int64_t(ctx->predictArcDelay(net, {cell, pins[p]})) + logic.maxDelay();
                    if (time < 0 || time > std::numeric_limits<delay_t>::max()) { valid = false; break; }
                    latest = std::max(latest, time);
                }
                if (node) output = latest; else encoded = latest;
            }
            if (valid) ranked.push_back({output, index});
        }
        std::sort(ranked.begin(), ranked.end(), [](const Ranked &a, const Ranked &b) { return std::tie(a.score, a.index) < std::tie(b.score, b.index); });
        std::set<std::array<int, 3>> supports;
        int tried = 0;
        for (const auto &rank : ranked) {
            const auto &candidate = candidates[rank.index];
            if (!supports.insert(candidate.bound).second) continue;
            if (tried++ == 4) break;
            bool safe = false; std::string decision;
            bool kept = private_pair_trial(ctx, view.producer, candidate, [&]() {
                critical_cohort_pin_preview(ctx, [&]() {
                    TimingAnalyser after(ctx), unskewed(ctx); after.with_clock_skew = true;
                    critical_cohort_setup_timing(ctx, after); critical_cohort_setup_timing(ctx, unskewed);
                    if (after.have_loops || unskewed.have_loops) { decision = "timing loop"; return; }
                    if (!private_pair_clock_safe(baseline_metrics,after.get_timing_result(),&decision) ||
                        !private_pair_clock_safe(baseline_unskewed,unskewed.get_timing_result(),&decision)) return;
                    auto snapshot = critical_swap_live_endpoint_snapshot(ctx, after, unskewed);
                    safe = critical_route_snapshot_safe(baseline, snapshot, &decision);
                });
                if (safe) log_info("Private pair candidate %d: producer=%s consumer=%s bound=%d,%d,%d score=%lldps.\n", qualified,
                        ctx->nameOf(view.producer), ctx->nameOf(view.consumer), candidate.bound[0], candidate.bound[1], candidate.bound[2], static_cast<long long>(rank.score));
                return safe && selection == qualified;
            }, &why, {});
            log_info("Private pair trial: producer=%s definition=%zu safe=%d kept=%d: %s.\n", ctx->nameOf(view.producer), rank.index, int(safe), int(kept), decision.empty() ? why.c_str() : decision.c_str());
            if (kept) return true;
            if (safe) ++qualified;
        }
    }
    log_info("Private pair decomposition: %d qualified candidates; predicted diagnostics only.\n", qualified);
    return false;
}
namespace {
bool routed_pair_context(Context *ctx)
{
    return !ctx->fes_any_slot_region_active && !ctx->fes_fence_active && !ctx->fes_has_reserved_rect &&
           !ctx->fes_has_cram_region && ctx->fes_frozen_cells.empty() && ctx->fes_frozen_pips.empty() &&
           ctx->attrs.count(id_step) && ctx->attrs.at(id_step).as_string() == "route";
}
using PairRoute = std::map<WireId, std::pair<PipId, PlaceStrength>>;
std::vector<WireId> pair_sinks(Context *ctx, NetInfo *net, const PortRef &user)
{
    auto physical=ctx->getNetinfoSinkWires(net,user);
    std::vector<WireId> result(physical.begin(),physical.end());
    std::sort(result.begin(),result.end());
    return result;
}
PairRoute pair_route(NetInfo *net)
{
    PairRoute result;
    for (const auto &wire : net->wires) result.emplace(wire.first, std::make_pair(wire.second.pip, wire.second.strength));
    return result;
}
bool routed_pair_tree(Context *ctx, NetInfo *net, std::string &reason)
{
    auto fail = [&](const char *text) { reason = text; return false; };
    if (net->wires.empty() || net->wires.size() > 4096 || net->users.entries() > 64)
        return fail("routed pair net exceeds tree/user bound or is unrouted");
    const auto root = ctx->getNetinfoSourceWire(net);
    if (!net->wires.count(root) || net->wires.at(root).pip != PipId()) return fail("invalid routed pair root");
    for (const auto &wire : net->wires) {
        if (wire.second.strength > STRENGTH_STRONG || ctx->getBoundWireNet(wire.first) != net ||
            (wire.second.pip != PipId() && ctx->getBoundPipNet(wire.second.pip) != net))
            return fail("protected or foreign routed pair wire/pip");
        std::set<WireId> seen;
        for (auto cursor = wire.first; cursor != root;) {
            auto at = net->wires.find(cursor);
            if (!seen.insert(cursor).second || at == net->wires.end() || at->second.pip == PipId() ||
                ctx->getPipDstWire(at->second.pip) != cursor) return fail("malformed routed pair tree");
            cursor = ctx->getPipSrcWire(at->second.pip);
        }
    }
    for (const auto &user : net->users) {
        if (!user.cell || !user.cell->ports.count(user.port)) return fail("malformed routed pair user");
        const auto &port = user.cell->ports.at(user.port);
        if (port.net != net || port.type != PORT_IN || !net->users.count(port.user_idx) ||
            net->users.at(port.user_idx).cell != user.cell || net->users.at(port.user_idx).port != user.port)
            return fail("routed pair user-slot mismatch");
        auto sinks = pair_sinks(ctx,net,user);
        if (sinks.empty()) return fail("unmapped routed pair sink");
        for (auto sink : sinks) if (!net->wires.count(sink)) return fail("unrouted pair sink");
    }
    return true;
}
bool inspect_routed_pair(Context *ctx, CellInfo *producer, PairView &view, std::string &reason)
{
    auto fail = [&](const char *text) { reason = text; return false; };
    if (!routed_pair_context(ctx)) return fail("requires an ordinary completed route");
    if (!inspect_pair_shape(ctx, producer, view, reason)) return false;
    if (view.alms.size() != 2) return fail("requires two distinct ordinary occupied ALMs");
    for (auto where : view.alms) {
        const auto &alm = ctx->labs.at(where.first).alms.at(where.second);
        for (auto bel : alm.lut_bels) {
            auto *cell = ctx->getBoundBelCell(bel);
            if (!cell || !cell->type.in(id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,id_MISTRAL_ALUT4,id_MISTRAL_ALUT5))
                return fail("routed pair requires ordinary occupied LUT mates");
        }
        for (size_t i = 0; i < alm.ff_bels.size(); ++i) if (auto *ff = ctx->getBoundBelCell(alm.ff_bels[i])) {
            auto *lut = ctx->getBoundBelCell(alm.lut_bels.at(i / 2));
            if (ff->type != id_MISTRAL_FF || !movable(ctx,ff) || ff->getPort(id_SDATA) ||
                !ff->getPort(id_DATAIN) || ff->getPort(id_DATAIN) != lut->getPort(id_Q))
                return fail("cohabiting FF requires local COMBOUT DATAIN and no SDATA");
        }
    }
    for (auto *net : view.signals) {
        const auto &driver = net->driver;
        if (!driver.cell || driver.cell->bel == BelId() || driver.cell->getPort(driver.port) != net ||
            !driver.cell->type.in(id_MISTRAL_FF,id_MISTRAL_M10K) || protected_attrs(ctx,driver.cell->attrs))
            return fail("routed pair requires unprotected direct FF/M10K sources");
        int clocks = 0;
        if (ctx->getPortTimingClass(driver.cell,driver.port,clocks) != TMG_REGISTER_OUTPUT || clocks != 1)
            return fail("routed pair source lacks one native registered output clock");
    }
    auto nets = view.signals; nets.push_back(view.link); nets.push_back(view.consumer->getPort(id_Q));
    for (auto *net : nets) if (!routed_pair_tree(ctx,net,reason)) return false;
    return (physical_function(ctx,view.producer) && physical_function(ctx,view.consumer)) ||
           fail("original routed physical function is invalid");
}
bool same_pair_lab(const LABInfo &a, const LABInfo &b)
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
// All actual configuration fields of the affected LABs, except the two masks
// intentionally rewritten. This includes FF input/control selectors and modes.
using PairConfig = std::map<std::tuple<int,int,int,int>, std::tuple<int,bool,uint32_t,std::vector<uint8_t>>>;
PairConfig pair_static_config(Context *ctx, const PairView &view)
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
    PairConfig result;
    for (const auto &value : ctx->cyclonev->bmux_get()) {
        if (!tiles.count({int(value.btype),value.pos.v})) continue;
        if (value.mux == CV::LUT_MASK && masks.count({int(value.btype),value.pos.v,value.midx})) continue;
        result.emplace(std::make_tuple(int(value.btype),value.pos.v,int(value.mux),value.midx),
                       std::make_tuple(value.type,value.def,value.s,value.r));
    }
    return result;
}
struct RoutedPairTransaction {
    struct Cell {
        CellInfo *cell; IdString type; BelId bel; PlaceStrength strength;
        decltype(CellInfo::ports) ports; decltype(CellInfo::params) params; decltype(CellInfo::attrs) attrs;
        ArchCellInfo architecture;
    };
    struct Net {
        NetInfo *net; indexed_store<PortRef> users; PairRoute route;
        std::map<store_index<PortRef>,std::vector<WireId>> sinks;
        std::set<WireId> retained; bool changed = false;
    };
    Context *ctx; const PairView &view;
    std::vector<Cell> cells;
    std::vector<Net> nets;
    std::vector<LABInfo> labs;
    std::map<Alm,uint64_t> masks;
    PairConfig config;
    bool dirty = false;
    RoutedPairTransaction(Context *ctx, const PairView &view) : ctx(ctx), view(view), labs(ctx->labs), config(pair_static_config(ctx,view))
    {
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            if (cell->bel == BelId() || !(ctx->is_comb_cell(cell->type) || cell->type == id_MISTRAL_FF) ||
                !view.labs.count(alm_of(ctx,cell).first)) continue;
            cells.push_back({cell,cell->type,cell->bel,cell->belStrength,cell->ports,cell->params,cell->attrs,*static_cast<ArchCellInfo *>(cell)});
        }
        for (auto where : view.alms) masks.emplace(where,ctx->compute_lut_mask(where.first,where.second));
        auto signals = view.signals; signals.push_back(view.link); signals.push_back(view.consumer->getPort(id_Q));
        std::sort(signals.begin(),signals.end(),[&](auto *a,auto *b) { return a->name.str(ctx)<b->name.str(ctx); });
        for (auto *net : signals) {
            Net saved{net,net->users,pair_route(net),{}, {}, false};
            for (const auto &user : net->users) {
                auto sinks = pair_sinks(ctx,net,user);
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
    bool valid(bool configured, std::string &reason)
    {
        auto fail=[&](const char *text) { reason=text; return false; };
        if (ctx->labs.size()!=labs.size() || !legal_labs(ctx,view.labs)) return fail("routed pair LAB legality changed");
        for (size_t i=0;i<labs.size();++i) if (!same_pair_lab(labs[i],ctx->labs[i])) return fail("routed pair LAB modes/counts/control selectors changed");
        for (const auto &saved : cells) {
            auto *cell=saved.cell;
            if (cell->bel!=saved.bel || cell->belStrength!=saved.strength || cell->type!=saved.type || cell->attrs!=saved.attrs)
                return fail("routed pair occupant placement/type/attributes changed");
            if (cell==view.producer || cell==view.consumer) continue;
            if (cell->params!=saved.params || !same_pins(cell,saved.architecture.pin_data)) return fail("stationary occupant mask or native pins changed");
            if (cell->ports.size()!=saved.ports.size()) return fail("stationary occupant ports changed");
            for (const auto &port : saved.ports) {
                auto found=cell->ports.find(port.first);
                if (found==cell->ports.end() || found->second.net!=port.second.net || found->second.user_idx!=port.second.user_idx ||
                    found->second.type!=port.second.type) return fail("stationary occupant port binding changed");
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
        if (!physical_function(ctx,view.producer) || !physical_function(ctx,view.consumer)) return fail("rewritten physical LUT function mismatch");
        if (configured && pair_static_config(ctx,view)!=config) return fail("configured stationary LAB/FF selector fields changed");
        return true;
    }
};
} // namespace

bool critical_routed_private_pair_candidates(Context *ctx, CellInfo *producer,
    std::vector<policy::Candidate> &candidates, std::vector<NetInfo *> &signals, std::string *reason)
{
    PairView view; std::string why;
    candidates.clear(); signals.clear();
    if (!inspect_routed_pair(ctx,producer,view,why)) { if(reason)*reason=why; return false; }
    candidates=policy::decompose(view.function); signals=view.signals;
    return true;
}

bool critical_routed_private_pair_common_launch(Context *ctx, TimingAnalyser &timing,
    const std::vector<NetInfo *> &signals, std::string *reason)
{
    std::array<delay_t,7> arrivals{};
    if (!private_pair_common_arrivals(ctx,timing,signals,arrivals,reason)) return false;
    NetInfo *clock=nullptr; ClockEdge edge=RISING_EDGE;
    auto finite=[](delay_t value) { return std::isfinite(double(value)) && value!=std::numeric_limits<delay_t>::lowest() && value!=std::numeric_limits<delay_t>::max(); };
    for (size_t i=0;i<signals.size();++i) {
        const auto &driver=signals[i]->driver; int count=0;
        if (ctx->getPortTimingClass(driver.cell,driver.port,count)!=TMG_REGISTER_OUTPUT || count!=1) { if(reason)*reason="source clock inventory differs"; return false; }
        const auto info=ctx->getPortClockingInfo(driver.cell,driver.port,0);
        auto *source_clock=driver.cell->getPort(info.clock_port);
        if (!source_clock || !finite(arrivals[i]) || !finite(info.clockToQ.minDelay()) || !finite(info.clockToQ.maxDelay()) ||
            info.clockToQ.minDelay()>info.clockToQ.maxDelay() || (clock && (clock!=source_clock || edge!=info.edge))) {
            if(reason)*reason="missing, mixed or uncharacterized direct source clock";
            return false;
        }
        clock=source_clock; edge=info.edge;
        std::vector<TimingAnalyser::SetupWindowHint> rows;
        if (!timing.get_setup_window_hints(CellPortKey(driver),rows) || rows.empty()) { if(reason)*reason="source has no native clock relationship"; return false; }
        for (const auto &row : rows) if (row.cut || !row.window || !finite(*row.window) || *row.window<=0) {
            if(reason)*reason="source native window is cut or invalid";
            return false;
        }
    }
    return true;
}

bool critical_routed_private_pair_trial(Context *ctx, CellInfo *producer, const policy::Candidate &candidate,
    int visit_budget, const std::function<bool()> &accept, std::string *reason,
    const std::function<void(unsigned)> &stage)
{
    PairView view; std::string why;
    auto fail=[&](const char *text) { if(reason)*reason=text; return false; };
    if (visit_budget<=0 || visit_budget>500000) return fail("local route node budget must be 1..500000");
    if (!inspect_routed_pair(ctx,producer,view,why)) { if(reason)*reason=why; return false; }
    if (candidate.network.producer.pins.size()!=3 || candidate.network.consumer.pins.size()!=5 ||
        !reduction_reassociate::equivalent(view.function,candidate.network)) return fail("candidate is not equivalent");
    std::set<int> used;
    for (auto *lut : {&candidate.network.producer,&candidate.network.consumer}) for (auto pin : lut->pins)
        if (!view.slots.count(pin.signal) || !used.insert(pin.signal).second) return fail("candidate changes source-slot multiplicity");
    if (used.size()!=8) return fail("candidate source-slot inventory differs");
    RoutedPairTransaction tx(ctx,view);
    auto reject=[&](const std::string &text) { tx.restore(); if(reason)*reason=text; return false; };
    try {
        tx.dirty=true;
        size_t index=0;
        for (auto *lut : {&candidate.network.producer,&candidate.network.consumer}) {
            auto *cell=index++?view.consumer:view.producer;
            cell->params[id_LUT]=Property(int64_t(lut->mask),1u<<lut->pins.size());
            for (size_t p=0;p<lut->pins.size();++p) {
                const auto &slot=view.slots.at(lut->pins[p].signal);
                auto &port=cell->ports.at(pins[p]); port.net=slot.net; port.user_idx=slot.index;
                slot.net->users.at(slot.index)={cell,pins[p]};
                cell->pin_data.at(pins[p]).state=lut->pins[p].inverted?PIN_INV:PIN_SIG;
            }
            ctx->assign_comb_info(cell);
        }
        if(stage)stage(1);
        for (auto where : view.alms) {
            ctx->update_alm_input_count(where.first,where.second);
            ctx->assign_alm_lut_inputs(where.first,where.second);
        }
        if (!tx.valid(false,why)) return reject(why);
        if(stage)stage(2);
        size_t changed=0;
        for (auto &saved : tx.nets) {
            const auto root=ctx->getNetinfoSourceWire(saved.net); saved.retained.insert(root);
            for (const auto &slot : saved.sinks) {
                if (!saved.net->users.count(slot.first)) return reject("rewritten user-slot disappeared");
                auto sinks=pair_sinks(ctx,saved.net,saved.net->users.at(slot.first));
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
        if (!changed || changed>7) return reject("no changed external physical sink or excessive footprint");
        for (const auto &saved : tx.nets) if (saved.changed && (saved.net==view.link || saved.net==view.consumer->getPort(id_Q)))
            return reject("private link or consumer output physical sink changed");
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
            auto now=pair_route(saved.net);
            if (!saved.changed && now!=saved.route) return reject("unchanged pair route changed");
            for (auto wire : saved.retained) if (!now.count(wire) || now.at(wire)!=saved.route.at(wire))
                return reject("stationary physical path/strength changed");
            if (!routed_pair_tree(ctx,saved.net,why)) return reject(why);
        }
        ctx->check(); if(stage)stage(21);
        if (!accept()) return reject(reason && !reason->empty()?*reason:"native inspection rejected");
        tx.dirty=false;
        return true;
    } catch (...) { tx.restore(); throw; }
}
void Arch::repair_critical_private_pairs()
{
    if (!critical_private_pair_budget) return;
    Context *ctx=getCtx();
    if (critical_private_pair_budget<0 || critical_private_pair_budget>4 || !routed_pair_context(ctx))
        log_error("Critical private pair requires an ordinary completed route and budget 1..4.\n");
    configure_bitstream();
    auto analyse=[&](bool skew) {auto t=std::make_unique<TimingAnalyser>(ctx);t->with_clock_skew=skew;t->setup(false,false,true);return t;};
    auto before=analyse(true),reference=analyse(false);
    if (before->have_loops || reference->have_loops) log_error("Critical private pair refuses timing loops.\n");
    auto inventory=critical_swap_live_endpoint_snapshot(ctx,*before,*reference);
    size_t failing=0;
    for (const auto &endpoint : inventory["endpoints"].object_items()) for (const auto &row : endpoint.second.array_items())
        if (row["setup_timed"].bool_value() && row["setup"].is_number() && row["setup"].number_value()<0) ++failing;
    auto paths=before->get_report_setup_paths(int(std::max<size_t>(2,std::min<size_t>(16384,failing+1))));
    for (const auto &path : before->get_timing_result().clock_paths) paths.push_back(path.second);
    for (const auto &path : before->get_timing_result().xclock_paths) paths.push_back(path);
    struct Witness { CellInfo *producer=nullptr; int64_t slack=0; std::set<NetInfo *> late_producer_inputs; };
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
            if (found==ctx->cells.end()) continue;
            auto *cell=found->second.get();
            std::vector<CellInfo *> producers;
            if (cell->type==id_MISTRAL_ALUT3) producers.push_back(cell);
            else if (cell->type==id_MISTRAL_ALUT5) for (const auto &port : cell->ports) {
                auto *net=port.second.net;
                if (port.second.type==PORT_IN && net && net->driver.cell && net->driver.cell->type==id_MISTRAL_ALUT3 &&
                    net->driver.port==id_Q && net->users.entries()==1) producers.push_back(net->driver.cell);
            }
            for (auto *producer : producers) {
                const auto name=producer->name.str(ctx);
                if (!witnessed.count(name) && witnessed.size()>=64) continue;
                auto &entry=witnessed[name];entry.producer=producer;entry.slack=std::min(entry.slack,slack);
                if (cell==producer && cell->getPort(segment.from.second)) entry.late_producer_inputs.insert(cell->getPort(segment.from.second));
            }
        }
    }
    struct Choice {CellInfo *producer;policy::Candidate candidate;size_t definition;int promoted;delay_t encoded;int64_t slack;};
    struct Group {std::string name;int64_t slack;std::vector<Choice> choices;};
    std::vector<Group> groups;
    auto coverage=[&](const PairView &view,TimingAnalyser &timing,std::string &why) {
        if (!critical_routed_private_pair_common_launch(ctx,timing,view.signals,&why)) return false;
        for (auto *net : view.signals) if (!critical_lut6_swap_output_coverage(ctx,timing,net,&why)) return false;
        return critical_lut6_swap_output_coverage(ctx,timing,view.consumer->getPort(id_Q),&why);
    };
    for (const auto &entry : witnessed) {
        PairView view;std::string why;
        if (!inspect_routed_pair(ctx,entry.second.producer,view,why) || !coverage(view,*before,why) || !coverage(view,*reference,why)) {
            log_info("Critical private pair filter: producer=%s: %s.\n",entry.first.c_str(),why.c_str());continue;
        }
        std::array<delay_t,7> arrivals{};
        if (!private_pair_common_arrivals(ctx,*before,view.signals,arrivals,&why)) {
            log_info("Critical private pair filter: producer=%s: %s.\n",entry.first.c_str(),why.c_str());continue;
        }
        std::set<int> old_support;for(auto pin:view.function.producer.pins)old_support.insert(pin.signal);
        auto candidates=policy::decompose(view.function);Group group{entry.first,entry.second.slack,{}};
        std::set<std::array<int,3>> partitions;
        for (size_t i=0;i<candidates.size();++i) {
            const auto &candidate=candidates[i];
            std::set<int> support;for(auto pin:candidate.network.producer.pins)support.insert(pin.signal);
            // Complementary encodings of the same partition do not consume
            // the small budget before a different two-level partition.
            if (support==old_support || !partitions.insert(candidate.bound).second) continue;
            int promoted=0;delay_t encoded=std::numeric_limits<delay_t>::lowest();
            for (auto *net : entry.second.late_producer_inputs) {
                auto at=std::find(view.signals.begin(),view.signals.end(),net);
                if (at!=view.signals.end() && !support.count(int(at-view.signals.begin()))) ++promoted;
            }
            for (int signal : support) encoded=std::max(encoded,arrivals.at(signal));
            group.choices.push_back({view.producer,candidate,i,promoted,encoded,entry.second.slack});
        }
        std::sort(group.choices.begin(),group.choices.end(),[](const Choice &a,const Choice &b) {
            if(a.promoted!=b.promoted)return a.promoted>b.promoted;
            return std::tie(a.encoded,a.candidate.bound,a.definition)<std::tie(b.encoded,b.candidate.bound,b.definition);
        });
        if(!group.choices.empty())groups.push_back(std::move(group));
    }
    std::sort(groups.begin(),groups.end(),[](const Group &a,const Group &b){return std::tie(a.slack,a.name)<std::tie(b.slack,b.name);});
    log_info("Critical private pair discovery: negative rows=%zu paths=%zu bounded producer witnesses=%zu eligible groups=%zu; native path cap=16384, producer cap=64.\n",failing,negative_paths,witnessed.size(),groups.size());
    auto clock_deltas=[&](const char *kind,const TimingResult &old,const TimingResult &now) {
        for(const auto &clock:old.clock_setup_slack) {
            auto next=now.clock_setup_slack.find(clock.first);
            if(next==now.clock_setup_slack.end() || next->second!=clock.second)
                log_info("Critical private pair %s setup: clock=%s before=%dps after=%s.\n",kind,clock.first.c_str(ctx),int(clock.second),next==now.clock_setup_slack.end()?"missing":(std::to_string(int(next->second))+"ps").c_str());
        }
        for(const auto &clock:old.clock_hold_slack) {
            auto next=now.clock_hold_slack.find(clock.first);
            if(next==now.clock_hold_slack.end() || next->second!=clock.second)
                log_info("Critical private pair %s hold: clock=%s before=%dps after=%s.\n",kind,clock.first.c_str(ctx),int(clock.second),next==now.clock_hold_slack.end()?"missing":(std::to_string(int(next->second))+"ps").c_str());
        }
        for(const auto &clock:old.clock_fmax) {
            auto next=now.clock_fmax.find(clock.first);
            if(next==now.clock_fmax.end() || next->second.achieved!=clock.second.achieved || next->second.constraint!=clock.second.constraint)
                log_info("Critical private pair %s Fmax: clock=%s before=%.6fMHz after=%.6fMHz constraint=%.6f/%.6fMHz.\n",kind,clock.first.c_str(ctx),clock.second.achieved,next==now.clock_fmax.end()?-1.0:next->second.achieved,clock.second.constraint,next==now.clock_fmax.end()?-1.0:next->second.constraint);
        }
    };
    int tried=0,kept=0;std::set<CellInfo *> changed;
    for(size_t rank=0;rank<policy::MAX_CANDIDATES && tried<critical_private_pair_budget;++rank) for(const auto &group:groups) {
        if(rank>=group.choices.size() || changed.count(group.choices[rank].producer))continue;
        if(tried>=critical_private_pair_budget)break;
        const auto &choice=group.choices[rank];PairView view;std::string reason;
        // Earlier accepted candidates may change this cone or its clock frames.
        // Ranking is only a proposal; recheck current native coverage before
        // charging a trial or mutating any live route.
        if(!inspect_routed_pair(ctx,choice.producer,view,reason) ||
           !coverage(view,*before,reason) || !coverage(view,*reference,reason)) {
            log_info("Critical private pair current eligibility: producer=%s: %s.\n",group.name.c_str(),reason.c_str());continue;
        }
        ++tried;std::unique_ptr<TimingAnalyser> after,next_reference;json11::Json next_inventory;
        bool accepted=critical_routed_private_pair_trial(ctx,choice.producer,choice.candidate,500000,[&]() {
            after=analyse(true);next_reference=analyse(false);
            if(after->have_loops || next_reference->have_loops) {reason="timing loop";return false;}
            clock_deltas("with-skew",before->get_timing_result(),after->get_timing_result());
            clock_deltas("reference",reference->get_timing_result(),next_reference->get_timing_result());
            if(!coverage(view,*after,reason) || !coverage(view,*next_reference,reason))return false;
            if(!private_pair_clock_safe(before->get_timing_result(),after->get_timing_result(),&reason) ||
               !private_pair_clock_safe(reference->get_timing_result(),next_reference->get_timing_result(),&reason))return false;
            next_inventory=critical_swap_live_endpoint_snapshot(ctx,*after,*next_reference);
            return critical_route_snapshot_safe(inventory,next_inventory,&reason);
        },&reason,{});
        log_info("Critical private pair trial %d: producer=%s definition=%zu bound=%d,%d,%d prior-slack=%lldps promoted-path-inputs=%d native-source-rank=%dps accepted=%d; %s.\n",tried,group.name.c_str(),choice.definition,choice.candidate.bound[0],choice.candidate.bound[1],choice.candidate.bound[2],static_cast<long long>(choice.slack),choice.promoted,int(choice.encoded),int(accepted),reason.c_str());
        if(accepted){before=std::move(after);reference=std::move(next_reference);inventory=std::move(next_inventory);changed.insert(choice.producer);++kept;}
    }
    log_info("Critical private pair: tried=%d/%d kept=%d; unchanged unsupported coverage is not qualification.\n",tried,critical_private_pair_budget,kept);
}
NEXTPNR_NAMESPACE_END
