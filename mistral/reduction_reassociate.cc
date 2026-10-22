/* Opt-in, verified two-LUT reassociation on an unrouted placed checkpoint.
 * SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "log.h"
#include "reduction_reassociate.h"
#include <algorithm>
#include <array>
#include <functional>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

NEXTPNR_NAMESPACE_BEGIN
namespace {
const IdString inputs[] = {id_A, id_B, id_C, id_D, id_E, id_F};

int lut_width(IdString type)
{
    const IdString types[] = {id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                              id_MISTRAL_ALUT5, id_MISTRAL_ALUT6};
    for (int i = 0; i < 5; ++i)
        if (type == types[i]) return i + 2;
    return 0;
}
IdString lut_type(size_t width)
{
    const IdString types[] = {id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                              id_MISTRAL_ALUT5, id_MISTRAL_ALUT6};
    NPNR_ASSERT(width >= 2 && width <= 6);
    return types[width - 2];
}
bool protected_attrs(Context *ctx, const dict<IdString, Property> &attrs)
{
    // Merely inspecting an absent attribute must not intern a new IdString.
    for (const auto &attr : attrs) {
        auto name = attr.first.str(ctx);
        if (name == "keep" || name == "dont_touch" || name == "BEL" || name == "FES_SLOT") return true;
    }
    return false;
}
bool movable(Context *ctx, CellInfo *cell)
{
    return cell && cell->bel != BelId() && cell->belStrength <= STRENGTH_WEAK && !cell->isPseudo() &&
           cell->cluster == ClusterId() && cell->constr_children.empty() && cell->constr_x == 0 &&
           cell->constr_y == 0 && cell->constr_z == 0 && !cell->constr_abs_z && !cell->region &&
           !protected_attrs(ctx, cell->attrs);
}
using Alm = std::pair<uint32_t, uint8_t>;
Alm alm_of(Context *ctx, BelId bel)
{
    const auto &data = ctx->bel_data(bel).lab_data;
    return {data.lab, data.alm};
}
std::array<BelId, 6> alm_bels(Context *ctx, Alm where)
{
    const auto &alm = ctx->labs.at(where.first).alms.at(where.second);
    return {alm.lut_bels[0], alm.lut_bels[1], alm.ff_bels[0], alm.ff_bels[1], alm.ff_bels[2], alm.ff_bels[3]};
}
bool legal_labs(Context *ctx, const std::set<uint32_t> &labs)
{
    for (auto lab : labs)
        for (uint8_t alm = 0; alm < 10; ++alm)
            for (auto bel : alm_bels(ctx, {lab, alm}))
                if (ctx->getBoundBelCell(bel) && !ctx->isBelLocationValid(bel)) return false;
    return true;
}
struct InputSlot {
    NetInfo *net;
    store_index<PortRef> index;
};
struct SavedCell {
    CellInfo *cell;
    IdString type;
    decltype(CellInfo::ports) ports;
    decltype(CellInfo::params) params;
    BelId bel;
    PlaceStrength strength;
};
class ReassociationTransaction {
  public:
    ReassociationTransaction(Context *ctx, CellInfo *producer, CellInfo *consumer,
                             const std::set<NetInfo *> &nets, const std::set<Alm> &alms) : ctx(ctx), labs(ctx->labs)
    {
        for (auto *cell : {producer, consumer})
            cells.push_back({cell, cell->type, cell->ports, cell->params, cell->bel, cell->belStrength});
        for (auto *net : nets) users.emplace(net, net->users);
        // Pin reassignment can also touch the other LUT in an original ALM.
        // Relocation destinations are whole empty ALMs, so there are no new mates.
        for (auto alm : alms)
            for (auto bel : alm_bels(ctx, alm))
                if (auto *cell = ctx->getBoundBelCell(bel)) architecture.emplace(cell, *static_cast<ArchCellInfo *>(cell));
    }
    ~ReassociationTransaction()
    {
        if (kept) return;
        unbind();
        for (auto &saved : cells) {
            saved.cell->type = saved.type;
            saved.cell->ports = std::move(saved.ports);
            saved.cell->params = std::move(saved.params);
        }
        for (auto &entry : users) entry.first->users = std::move(entry.second);
        for (auto &entry : architecture) *static_cast<ArchCellInfo *>(entry.first) = std::move(entry.second);
        for (const auto &saved : cells) ctx->bindBel(saved.bel, saved.cell, saved.strength);
        ctx->labs = std::move(labs);
    }
    void unbind()
    {
        for (const auto &saved : cells)
            if (saved.cell->bel != BelId()) ctx->unbindBel(saved.cell->bel);
    }
    void commit() { kept = true; }
    Context *ctx;
    std::vector<SavedCell> cells;
    std::map<NetInfo *, indexed_store<PortRef>> users;
    std::map<CellInfo *, ArchCellInfo> architecture;
    std::vector<LABInfo> labs;
    bool kept = false;
};
} // namespace

// The callback inspects the complete legal rewritten placement only. It must
// not mutate the design. Rejection and exceptions restore exact original cell,
// input-user-slot, physical-pin and LAB state. Success proves Boolean equivalence
// at the public consumer Q; it is not a routed timing acceptance decision.
bool reassociate_producer_trial(Context *ctx, const std::string &producer_name, IdString promoted_input,
                                const std::function<bool()> &accept, std::string *reason)
{
    auto reject = [&](const char *why) { if (reason) *reason = why; return false; };
    if (reason) reason->clear();
    if (!ctx->attrs.count(id_step) || ctx->attrs.at(id_step).as_string() != "place")
        return reject("requires a placed checkpoint");
    if (ctx->fes_any_slot_region_active || ctx->fes_fence_active || ctx->fes_has_reserved_rect ||
        !ctx->fes_frozen_cells.empty() || !ctx->fes_frozen_pips.empty())
        return reject("requires ordinary placement without frozen regions");
    for (const auto &net : ctx->nets)
        if (!net.second->wires.empty()) return reject("requires a completely unrouted design");
    // Do not intern a nonexistent user-supplied cell name on a rejected trial.
    CellInfo *producer = nullptr;
    for (const auto &entry : ctx->cells)
        if (entry.first.str(ctx) == producer_name) { producer = entry.second.get(); break; }
    if (!producer || lut_width(producer->type) < 3 || !movable(ctx, producer))
        return reject("producer is not a movable ordinary 3..6-input LUT");
    int selected = -1;
    for (int pin = 0; pin < lut_width(producer->type); ++pin)
        if (inputs[pin] == promoted_input) selected = pin;
    if (selected < 0) return reject("selected input is outside the producer LUT");
    auto *link = producer->getPort(id_Q);
    if (!link || link->driver.cell != producer || link->driver.port != id_Q || link->users.entries() != 1)
        return reject("producer Q must have exactly one ordinary consumer");
    for (const auto &port : ctx->ports)
        if (port.second.net == link) return reject("producer Q is externally observable");
    const auto use = *link->users.begin();
    auto *consumer = use.cell;
    if (consumer == producer || !consumer || !lut_width(consumer->type) || !movable(ctx, consumer))
        return reject("consumer is not a movable ordinary LUT");
    const std::set<Alm> original_alms{alm_of(ctx, producer->bel), alm_of(ctx, consumer->bel)};
    std::set<uint32_t> affected_labs;
    for (auto alm : original_alms) {
        affected_labs.insert(alm.first);
        for (auto bel : alm_bels(ctx, alm))
            if (auto *cell = ctx->getBoundBelCell(bel))
                if (!movable(ctx, cell) || cell->type == id_MISTRAL_MLAB)
                    return reject("affected ALM contains a protected occupant");
    }
    if (!legal_labs(ctx, affected_labs)) return reject("original affected LAB is not legal");
    std::set<NetInfo *> touched;
    std::map<NetInfo *, int> signal_ids;
    std::vector<NetInfo *> signals;
    auto unsafe_net = [&](NetInfo *net) {
        if (!net || net->is_global || net->clkconstr || net->region || net->constant_value != IdString() ||
            protected_attrs(ctx, net->attrs)) return true;
        for (const auto &user : net->users) {
            if (!user.cell) return true;
            int clocks = 0;
            if (ctx->getPortTimingClass(user.cell, user.port, clocks) == TMG_CLOCK_INPUT) return true;
        }
        return false;
    };
    reduction_reassociate::Pair before;
    std::array<std::vector<InputSlot>, 2> slots;
    auto inspect = [&](CellInfo *cell, reduction_reassociate::Lut &lut, std::vector<InputSlot> &cell_slots) {
        const int width = lut_width(cell->type);
        if (cell->ports.size() != size_t(width + 1) || cell->params.size() != 1 || !cell->params.count(id_LUT) ||
            cell->get_pin_state(id_Q) != PIN_SIG || !cell->ports.count(id_Q) ||
            cell->ports.at(id_Q).type != PORT_OUT || unsafe_net(cell->getPort(id_Q)) ||
            cell->getPort(id_Q)->driver.cell != cell || cell->getPort(id_Q)->driver.port != id_Q) return false;
        const auto &table = cell->params.at(id_LUT);
        if (!table.is_fully_def() || table.size() != (1u << width)) return false;
        lut.mask = table.as_int64();
        for (int pin = 0; pin < width; ++pin) {
            auto found = cell->ports.find(inputs[pin]);
            if (found == cell->ports.end() || found->second.type != PORT_IN || unsafe_net(found->second.net)) return false;
            const auto &port = found->second;
            if (!port.net->users.count(port.user_idx) || port.net->users.at(port.user_idx).cell != cell ||
                port.net->users.at(port.user_idx).port != inputs[pin]) return false;
            auto state = cell->get_pin_state(inputs[pin]);
            if (state != PIN_SIG && state != PIN_INV) return false;
            if (port.net == consumer->getPort(id_Q) || (cell == producer && port.net == link)) return false;
            int signal = reduction_reassociate::INTERNAL;
            if (port.net != link) {
                if (!signal_ids.count(port.net)) {
                    signal_ids.emplace(port.net, int(signals.size()));
                    signals.push_back(port.net);
                }
                signal = signal_ids.at(port.net);
            } else if (cell != consumer || inputs[pin] != use.port) return false;
            lut.pins.push_back({signal, state == PIN_INV});
            cell_slots.push_back({port.net, port.user_idx});
            touched.insert(port.net);
        }
        return true;
    };
    if (!inspect(producer, before.producer, slots[0]) || !inspect(consumer, before.consumer, slots[1]))
        return reject("pair has unsupported ports, tables, pins, nets or connectivity");
    auto candidates = reduction_reassociate::reassociate(before);
    auto selected_plan = std::find_if(candidates.begin(), candidates.end(), [&](const auto &plan) {
        return plan.promoted_pin == size_t(selected);
    });
    if (selected_plan == candidates.end() || !reduction_reassociate::equivalent(before, selected_plan->network))
        return reject("selected input has no proven two-LUT factor reassociation");
    const auto &plan = *selected_plan;
    const auto producer_home = producer->bel, consumer_home = consumer->bel;
    const auto producer_strength = producer->belStrength, consumer_strength = consumer->belStrength;
    const auto promoted_slot = slots[0].at(selected);
    slots[0].erase(slots[0].begin() + selected);
    const bool reused_consumer_pin = plan.consumer_pin < slots[1].size();
    if (!reused_consumer_pin) slots[1].push_back(promoted_slot);
    // Slot transfer is intentionally narrower than an arbitrary equivalent
    // rewrite. Fail before mutation if the pure planner ever changes ordering
    // or support without updating this physical transaction's wiring rules.
    auto matches_slots = [&](const reduction_reassociate::Lut &lut, const std::vector<InputSlot> &mapped) {
        if (lut.pins.size() != mapped.size()) return false;
        for (size_t pin = 0; pin < lut.pins.size(); ++pin) {
            const int signal = lut.pins[pin].signal;
            NetInfo *expected = nullptr;
            if (signal == reduction_reassociate::INTERNAL) expected = link;
            else if (signal >= 0 && size_t(signal) < signals.size()) expected = signals[signal];
            if (!expected || expected != mapped[pin].net) return false;
        }
        return true;
    };
    if (!matches_slots(plan.network.producer, slots[0]) || !matches_slots(plan.network.consumer, slots[1]))
        return reject("proven plan does not match the supported input-slot transfer");
    // Build the deterministic relocation list before mutation. The original
    // consumer site wins if legal; otherwise use only a completely empty ALM
    // within two tile steps, without displacing another LUT or FF.
    std::vector<BelId> destinations{consumer_home};
    const auto origin = ctx->getBelLocation(consumer_home);
    for (auto bel : ctx->getBels()) {
        if (bel == consumer_home || !ctx->isValidBelForCellType(lut_type(plan.network.consumer.pins.size()), bel) ||
            !ctx->checkBelAvail(bel) || !ctx->fes_placement_allowed(bel, consumer)) continue;
        const auto at = ctx->getBelLocation(bel);
        if (std::abs(at.x - origin.x) + std::abs(at.y - origin.y) > 2) continue;
        bool empty = true;
        for (auto other : alm_bels(ctx, alm_of(ctx, bel))) empty &= ctx->checkBelAvail(other);
        if (empty) destinations.push_back(bel);
    }
    std::sort(destinations.begin() + 1, destinations.end(), [&](BelId a, BelId b) {
        auto x = ctx->getBelLocation(a), y = ctx->getBelLocation(b);
        return std::make_tuple(std::abs(x.x - origin.x) + std::abs(x.y - origin.y), x.x, x.y, x.z) <
               std::make_tuple(std::abs(y.x - origin.x) + std::abs(y.y - origin.y), y.x, y.y, y.z);
    });
    ReassociationTransaction transaction(ctx, producer, consumer, touched, original_alms);
    transaction.unbind();
    auto rewrite = [&](CellInfo *cell, const reduction_reassociate::Lut &lut, const std::vector<InputSlot> &new_slots) {
        for (int pin = 0; pin < lut_width(cell->type); ++pin) {
            cell->ports.erase(inputs[pin]);
            cell->pin_data.erase(inputs[pin]);
        }
        NPNR_ASSERT(new_slots.size() == lut.pins.size());
        cell->type = lut_type(lut.pins.size());
        cell->params[id_LUT] = Property(int64_t(lut.mask), 1u << lut.pins.size());
        for (size_t pin = 0; pin < lut.pins.size(); ++pin) {
            cell->addInput(inputs[pin]);
            auto &port = cell->ports.at(inputs[pin]);
            port.net = new_slots[pin].net;
            port.user_idx = new_slots[pin].index;
            port.net->users.at(port.user_idx) = {cell, inputs[pin]};
            cell->pin_data[inputs[pin]].state = lut.pins[pin].inverted ? PIN_INV : PIN_SIG;
        }
        ctx->assign_comb_info(cell);
        ctx->assign_default_pinmap(cell);
    };
    rewrite(producer, plan.network.producer, slots[0]);
    rewrite(consumer, plan.network.consumer, slots[1]);
    if (reused_consumer_pin) promoted_slot.net->users.remove(promoted_slot.index);
    ctx->bindBel(producer_home, producer, producer_strength);
    bool placed = false;
    for (auto target : destinations) {
        ctx->bindBel(target, consumer, consumer_strength);
        auto labs = affected_labs;
        labs.insert(alm_of(ctx, target).first);
        if (legal_labs(ctx, labs)) { placed = true; break; }
        ctx->unbindBel(target);
    }
    if (!placed) return reject("expanded consumer has no legal nearby placement");
    auto final_alms = original_alms;
    final_alms.insert(alm_of(ctx, consumer->bel));
    for (auto alm : final_alms) ctx->assign_alm_lut_inputs(alm.first, alm.second);
    for (auto *cell : {producer, consumer})
        for (int pin = 0; pin < lut_width(cell->type); ++pin)
            if (cell->pin_data.at(inputs[pin]).bel_pins.empty()) return reject("legal placement has an unmapped LUT input");
    ctx->check();
    if (!accept()) return reject("rewritten placement rejected by inspection");
    log_info("Reassociated producer %s input %s into consumer %s: LUT%d + LUT%d -> LUT%d + LUT%d; consumer %s -> %s. "
             "Boolean proof only; routed timing remains unqualified.\n",
             producer_name.c_str(), promoted_input.c_str(ctx), ctx->nameOf(consumer), int(before.producer.pins.size()),
             int(before.consumer.pins.size()), lut_width(producer->type), lut_width(consumer->type),
             ctx->getBelName(consumer_home).str(ctx).c_str(), ctx->getBelName(consumer->bel).str(ctx).c_str());
    transaction.commit();
    return true;
}

bool reassociate_producer(Context *ctx, const std::string &producer_name, IdString promoted_input, std::string *reason)
{
    return reassociate_producer_trial(ctx, producer_name, promoted_input, [] { return true; }, reason);
}
NEXTPNR_NAMESPACE_END
