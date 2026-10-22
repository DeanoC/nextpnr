/* Bounded private ALUT3 -> ALUT5 rewrite at unchanged placement. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "critical_cohort.h"
#include "private_pair_decomposition_native.h"
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
bool inspect_pair(Context *ctx, CellInfo *producer, PairView &view, std::string &reason)
{
    auto fail = [&](const char *why) { reason = why; return false; };
    if (!ordinary_context(ctx) || !movable(ctx, producer) || producer->type != id_MISTRAL_ALUT3)
        return fail("requires an ordinary movable ALUT3 in an unrouted full placement");
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
                std::vector<TimingAnalyser::SetupWindowHint> hints;
                std::vector<EndpointClockPairTiming> rows;
                if (!timing.get_setup_window_hints(CellPortKey(user), hints) ||
                    !timing.get_endpoint_clock_pair_timings(CellPortKey(user), rows) || rows.empty()) return false;
                for (const auto &hint : hints) if (hint.cut || !hint.window || *hint.window <= 0) return false;
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
NEXTPNR_NAMESPACE_END
