/* Opt-in pre-placement balancing of a verified wide conjunction. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "log.h"
#include "reduction_balance_policy.h"
#include "reduction_balance_plan.h"
#include <algorithm>
#include <map>
#include <set>

NEXTPNR_NAMESPACE_BEGIN
namespace {
const IdString pin_names[] = {id_A, id_B, id_C, id_D, id_E, id_F};
int lut_width(IdString type)
{
    if (type == id_MISTRAL_ALUT2) return 2;
    if (type == id_MISTRAL_ALUT3) return 3;
    if (type == id_MISTRAL_ALUT4) return 4;
    if (type == id_MISTRAL_ALUT5) return 5;
    if (type == id_MISTRAL_ALUT6) return 6;
    return 0;
}
IdString lut_type(int width)
{
    const IdString types[] = {id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5,
                              id_MISTRAL_ALUT6};
    return types[width - 2];
}
std::pair<std::string, int> bus_index(const std::string &name)
{
    auto open = name.rfind('[');
    if (open == std::string::npos || name.empty() || name.back() != ']') return {name, -1};
    try {
        auto number = name.substr(open + 1, name.size() - open - 2);
        size_t end = 0;
        int index = std::stoi(number, &end);
        if (end == number.size() && index >= 0) return {name.substr(0, open), index};
    } catch (...) {}
    return {name, -1};
}
}

bool plan_reduction(Context *ctx, const std::string &root_name, bool placed, ReductionBalancePlan &plan)
{
    auto root_it = ctx->cells.find(ctx->id(root_name));
    if (root_it == ctx->cells.end()) return false;
    CellInfo *root = root_it->second.get();
    std::vector<CellInfo *> cone;
    std::set<CellInfo *> seen;
    // Do not intern an otherwise absent attribute before placement: even an
    // unrelated new IdString can perturb the placer's iteration order.
    auto protected_attrs = [&](const auto &attrs) {
        for (const auto &attr : attrs) {
            auto name = attr.first.str(ctx);
            if (name == "keep" || name == "dont_touch") return true;
        }
        return false;
    };
    auto unsafe_net = [&](NetInfo *net) {
        if (!net || net->is_global || net->clkconstr || net->region || !net->wires.empty() ||
            net->constant_value != IdString() || protected_attrs(net->attrs)) return true;
        // Packing has not marked globals yet, and clocks need not have an SDC
        // constraint. Exclude clock consumers using the architecture's port model.
        for (const auto &user : net->users) {
            int clock_count = 0;
            if (ctx->getPortTimingClass(user.cell, user.port, clock_count) == TMG_CLOCK_INPUT) return true;
        }
        return false;
    };
    auto visit = [&](auto &&self, CellInfo *cell) -> bool {
        if (!cell || !lut_width(cell->type) || !seen.insert(cell).second || seen.size() > 7 ||
            (placed ? (cell->bel == BelId() || cell->belStrength > STRENGTH_WEAK) : cell->bel != BelId()) ||
            cell->cluster != ClusterId() || cell->region || cell->isPseudo() ||
            protected_attrs(cell->attrs) || (placed && cell->get_pin_state(id_Q) != PIN_SIG) ||
            cell->params.size() != 1 || !cell->params.count(id_LUT)) return false;
        const auto &table = cell->params.at(id_LUT);
        if (!table.is_fully_def() || table.size() != (1u << lut_width(cell->type))) return false;
        cone.push_back(cell);
        for (int i = 0; i < lut_width(cell->type); ++i) {
            auto net = cell->getPort(pin_names[i]);
            if (unsafe_net(net)) return false;
            auto driver = net->driver.cell;
            if (driver && lut_width(driver->type) && !self(self, driver)) return false;
        }
        return true;
    };
    if (!visit(visit, root) || (cone.size() != 3 && cone.size() != 4 && cone.size() != 7)) return false;
    std::sort(cone.begin(), cone.end(), [&](CellInfo *a, CellInfo *b) { return a->name.str(ctx) < b->name.str(ctx); });
    std::map<NetInfo *, int> signals;
    std::vector<NetInfo *> by_signal;
    std::set<NetInfo *> boundary;
    for (const auto &port : ctx->ports) boundary.insert(port.second.net);
    for (CellInfo *cell : cone) {
        auto net = cell->getPort(id_Q);
        if (unsafe_net(net) || signals.count(net) || cell->ports.at(id_Q).type != PORT_OUT ||
            net->driver.cell != cell || net->driver.port != id_Q) return false;
        int signal = int(by_signal.size());
        signals.emplace(net, signal);
        by_signal.push_back(net);
        if (cell != root) {
            // Top-level ports do not occupy an ordinary cell consumer slot.
            // Only the root function is preserved by this rewrite.
            if (boundary.count(net) || net->users.entries() != 1) return false;
            for (auto user : net->users) if (!seen.count(user.cell)) return false;
        }
    }
    // External input IDs are assigned deterministically, independent of map iteration.
    for (CellInfo *cell : cone)
        for (int i = 0; i < lut_width(cell->type); ++i) {
            NetInfo *net = cell->getPort(pin_names[i]);
            if (signals.count(net)) continue;
            signals.emplace(net, int(by_signal.size()));
            by_signal.push_back(net);
        }
    std::vector<reduction_balance_policy::Node> network;
    for (CellInfo *cell : cone) {
        reduction_balance_policy::Node node;
        node.output = signals.at(cell->getPort(id_Q));
        node.mask = cell->params.at(id_LUT).as_int64();
        for (int i = 0; i < lut_width(cell->type); ++i) {
            auto state = cell->get_pin_state(pin_names[i]);
            if (state != PIN_SIG && state != PIN_INV) return false;
            node.inputs.push_back({signals.at(cell->getPort(pin_names[i])), state == PIN_INV});
        }
        network.push_back(node);
    }
    auto reduction = reduction_balance_policy::recognize(network, signals.at(root->getPort(id_Q)));
    if (!reduction.valid) return false;
    std::map<NetInfo *, store_index<PortRef>> old_slots;
    for (CellInfo *cell : cone)
        for (int i = 0; i < lut_width(cell->type); ++i) {
            auto pin = pin_names[i];
            auto net = cell->getPort(pin);
            auto slot = cell->ports.at(pin).user_idx;
            if (!net->users.count(slot) || net->users.at(slot).cell != cell || net->users.at(slot).port != pin ||
                !old_slots.emplace(net, slot).second) return false;
        }
    std::vector<std::pair<NetInfo *, bool>> literals;
    for (const auto &lit : reduction.literals) literals.emplace_back(by_signal.at(lit.signal), lit.required);
    std::sort(literals.begin(), literals.end(), [&](const auto &a, const auto &b) {
        auto an = a.first->name.str(ctx), bn = b.first->name.str(ctx);
        auto ai = bus_index(an), bi = bus_index(bn);
        if (ai.first != bi.first) return ai.first < bi.first;
        if (ai.second != bi.second) return ai.second < bi.second;
        return an < bn;
    });
    std::vector<CellInfo *> leaves, retired;
    std::vector<NetInfo *> retired_nets;
    const size_t leaf_count = cone.size() == 7 ? 4 : cone.size() - 1;
    for (CellInfo *cell : cone) {
        if (cell == root) continue;
        if (leaves.size() < leaf_count) leaves.push_back(cell);
        else {
            retired.push_back(cell);
            retired_nets.push_back(cell->getPort(id_Q));
        }
    }
    plan.root = root;
    plan.cells = std::move(cone);
    plan.leaves = std::move(leaves);
    plan.retired = std::move(retired);
    plan.retired_nets = std::move(retired_nets);
    plan.literals = std::move(literals);
    plan.slots = std::move(old_slots);
    return true;
}

void rewrite_reduction(Context *ctx, const ReductionBalancePlan &plan)
{
    CellInfo *root = plan.root;
    const auto &cone = plan.cells;
    const auto &literals = plan.literals;
    const auto &old_slots = plan.slots;
    for (CellInfo *cell : cone) NPNR_ASSERT(cell->bel == BelId());
    const auto &children = plan.leaves;
    const std::vector<int> sizes = cone.size() == 3 ? std::vector<int>{int((literals.size() + 1) / 2), int(literals.size() / 2)} :
                                  cone.size() == 4 ? std::vector<int>{6, 6, 4} : std::vector<int>{6, 6, 6, 6};
    NPNR_ASSERT(children.size() == sizes.size());
    NPNR_ASSERT(plan.retired.size() == (cone.size() == 7 ? 2 : 0));
    NPNR_ASSERT(plan.retired_nets.size() == plan.retired.size());
    if (!plan.retired.empty()) {
        // The seven-cell tree becomes five cells. Detach every old input port
        // without freeing the original slots: all literal and surviving Q-net
        // slots are reassigned below, preserving unrelated indexed-store state.
        for (CellInfo *cell : cone)
            for (int i = 0; i < lut_width(cell->type); ++i) {
                cell->ports.erase(pin_names[i]);
                cell->pin_data.erase(pin_names[i]);
            }
    }
    int offset = 0;
    auto rewrite = [&](CellInfo *cell, int width, uint64_t mask, const std::vector<NetInfo *> &inputs) {
        for (int i = 0; i < lut_width(cell->type); ++i) {
            // Every input has exactly one consumer in this cone before and after
            // the rewrite. Keep its live user slot and replace that PortRef below.
            // Disconnect/reconnect can insert before the previous consumer is
            // removed, changing the traversal order used by analytical placement.
            cell->ports.erase(pin_names[i]);
            cell->pin_data.erase(pin_names[i]);
        }
        cell->type = lut_type(width);
        cell->params[id_LUT] = Property(int64_t(mask), 1 << width);
        for (int i = 0; i < width; ++i) {
            cell->addInput(pin_names[i]);
            auto &port = cell->ports.at(pin_names[i]);
            port.net = inputs[i];
            port.user_idx = old_slots.at(inputs[i]);
            inputs[i]->users.at(port.user_idx) = {cell, pin_names[i]};
            cell->pin_data[pin_names[i]].state = PIN_SIG;
        }
    };
    for (size_t group = 0; group < sizes.size(); ++group) {
        std::vector<NetInfo *> inputs;
        unsigned row = 0;
        for (int i = 0; i < sizes[group]; ++i) {
            const auto &literal = literals.at(offset + i);
            inputs.push_back(literal.first);
            if (literal.second) row |= 1u << i;
        }
        rewrite(children[group], sizes[group], uint64_t(1) << row, inputs);
        offset += sizes[group];
    }
    std::vector<NetInfo *> root_inputs;
    for (CellInfo *child : children) root_inputs.push_back(child->getPort(id_Q));
    rewrite(root, int(children.size()), uint64_t(1) << ((1u << children.size()) - 1), root_inputs);
    for (size_t i = 0; i < plan.retired.size(); ++i) {
        auto *cell = plan.retired[i];
        auto *net = plan.retired_nets[i];
        NPNR_ASSERT(cell->getPort(id_Q) == net && net->driver.cell == cell && net->driver.port == id_Q);
        net->users.remove(old_slots.at(net));
        cell->disconnectPort(id_Q);
        NPNR_ASSERT(net->users.empty() && net->driver.cell == nullptr);
    }
    if (cone.size() == 3)
        log_info("Balanced three-LUT reduction at '%s' into %d+%d inputs.\n", root->name.c_str(ctx), sizes[0], sizes[1]);
    else if (cone.size() == 4)
        log_info("Balanced four-LUT reduction at '%s' into 6+6+4 inputs.\n", root->name.c_str(ctx));
    else
        log_info("Balanced seven-LUT reduction at '%s' into 6+6+6+6 inputs; retired two private LUTs.\n",
                 root->name.c_str(ctx));
}

bool Arch::balance_reduction(const std::string &root_name)
{
    ReductionBalancePlan plan;
    if (!plan_reduction(getCtx(), root_name, false, plan)) return false;
    rewrite_reduction(getCtx(), plan);
    // The unplaced path commits immediately; no detached object may reach pack.
    for (auto *cell : plan.retired) {
        const auto name = cell->name;
        getCtx()->cells.erase(name);
    }
    for (auto *net : plan.retired_nets) {
        const auto name = net->name;
        getCtx()->nets.erase(name);
    }
    return true;
}
NEXTPNR_NAMESPACE_END
