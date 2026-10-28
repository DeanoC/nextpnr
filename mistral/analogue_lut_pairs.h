/* Native FF-free L5 co-packing trials. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_ANALOGUE_LUT_PAIRS_H
#define MISTRAL_ANALOGUE_LUT_PAIRS_H
#include <algorithm>
#include <map>
#include <set>
#include "analogue_lut_pins.h"

NEXTPNR_NAMESPACE_BEGIN
inline bool analogue_sole_l5(Context *ctx, const CellInfo *cell)
{
    if (!cell || !cell->constr_children.empty()) return false;
    // Reuse the exact physical-map, protection and whole-ALM ownership
    // checks of the explicit sole-L5 policy. No permutation is performed.
    for (auto port : {id_B, id_C, id_D, id_E})
        if (analogue_lut_pin_swap_allowed(ctx, cell, id_A, port, true)) return true;
    return false;
}

inline std::map<IdString, NetInfo *> analogue_pair_physical_inputs(const CellInfo *first, const CellInfo *second)
{
    std::map<IdString, NetInfo *> inputs;
    for (auto *cell : {first, second})
        for (const auto &port : cell->ports) if (port.second.type == PORT_IN) {
            const auto &pins = cell->pin_data.at(port.first).bel_pins;
            if (!port.second.net || pins.size() != 1) return {};
            auto inserted = inputs.emplace(pins.front(), port.second.net);
            if (!inserted.second && inserted.first->second != port.second.net) return {};
        }
    return inputs;
}

inline BelId analogue_lut_pair_target(Context *ctx, const CellInfo *moving, const CellInfo *peer)
{
    if (moving == peer || !analogue_sole_l5(ctx, moving) || !analogue_sole_l5(ctx, peer)) return BelId();
    const auto from = ctx->bel_data(moving->bel).lab_data;
    const auto to = ctx->bel_data(peer->bel).lab_data;
    if (from.idx == to.idx || (from.lab == to.lab && from.alm == to.alm)) return BelId();
    const auto target = ctx->labs.at(to.lab).alms.at(to.alm).lut_bels.at(from.idx);
    if (!ctx->isValidBelForCellType(moving->type, target) || !moving->testRegion(target) ||
        !ctx->fes_placement_allowed(target, moving) ||
        analogue_pair_physical_inputs(moving, peer).empty()) return BelId();
    return target;
}

inline bool analogue_lut_pair_input_capacity(Context *ctx, const CellInfo *first, const CellInfo *second)
{
    const auto a = ctx->bel_data(first->bel).lab_data, b = ctx->bel_data(second->bel).lab_data;
    if (a.lab != b.lab || a.alm != b.alm || a.idx == b.idx) return false;
    const auto inputs = analogue_pair_physical_inputs(first, second);
    if (inputs.empty()) return false;
    auto &count = ctx->labs.at(a.lab).alms.at(a.alm).unique_input_count;
    // The native counter may deduplicate a net that the preserved maps put
    // on two private pins. Check that conservative physical cost against
    // the unchanged native LAB bound, then restore the native counter.
    struct Restore { int &value; int old; ~Restore() { value = old; } } restore{count, count};
    count = std::max(count, int(inputs.size()));
    return ctx->check_lab_input_count(a.lab);
}

inline bool analogue_lut_pair_capacity_preview(Context *ctx, const CellInfo *moving, const CellInfo *peer,
                                               const CellInfo *displaced = nullptr, BelId destination = BelId())
{
    if (analogue_lut_pair_target(ctx, moving, peer) == BelId()) return false;
    const auto a = ctx->bel_data(moving->bel).lab_data, b = ctx->bel_data(peer->bel).lab_data;
    std::map<int *, int> saved;
    std::set<uint32_t> affected;
    auto counter = [&](const auto &where) -> int & {
        auto &value = ctx->labs.at(where.lab).alms.at(where.alm).unique_input_count;
        saved.emplace(&value, value);
        affected.insert(where.lab);
        return value;
    };
    struct Restore {
        std::map<int *, int> &saved;
        ~Restore() { for (const auto &entry : saved) *entry.first = entry.second; }
    } restore{saved};
    auto &source = counter(a), &target = counter(b);
    if (displaced) {
        if (displaced == moving || displaced == peer || destination == BelId()) return false;
        const auto c = ctx->bel_data(displaced->bel).lab_data, d = ctx->bel_data(destination).lab_data;
        auto &cold_source = counter(c), &cold_target = counter(d);
        if (saved.size() != 4 || cold_target != 0) return false;
        cold_target = cold_source;
        cold_source = 0;
    }
    source = 0;
    target = int(analogue_pair_physical_inputs(moving, peer).size());
    for (auto lab : affected) if (!ctx->check_lab_input_count(lab)) return false;
    return true;
}
NEXTPNR_NAMESPACE_END
#endif
