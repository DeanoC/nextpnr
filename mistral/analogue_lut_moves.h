/* Bounded native LUT relocation eligibility. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_ANALOGUE_LUT_MOVES_H
#define MISTRAL_ANALOGUE_LUT_MOVES_H
#include <set>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN
inline bool analogue_lut_move_allowed(Context *ctx, const CellInfo *cell, BelId target)
{
    if (!cell || !cell->type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                               id_MISTRAL_ALUT5, id_MISTRAL_ALUT6) ||
        cell->bel == BelId() || target == BelId() || target == cell->bel ||
        cell->belStrength > STRENGTH_WEAK || cell->cluster != ClusterId() || cell->region ||
        !cell->constr_children.empty() || !ctx->isValidBelForCellType(cell->type, target) ||
        !cell->testRegion(target) || !ctx->fes_placement_allowed(target, cell))
        return false;
    for (const char *name : {"keep", "dont_touch", "BEL", "FES_SLOT"})
        if (cell->attrs.count(ctx->id(name))) return false;
    const auto &from = ctx->bel_data(cell->bel).lab_data;
    const auto &to = ctx->bel_data(target).lab_data;
    // Preserve physical pin names, including the half-specific E/F pins.
    if (from.idx != to.idx || from.idx > 1 ||
        (from.lab == to.lab && from.alm == to.alm)) return false;
    for (const auto &where : {from, to}) {
        const auto &alm = ctx->labs.at(where.lab).alms.at(where.alm);
        for (auto bel : alm.lut_bels) {
            auto *occupant = ctx->getBoundBelCell(bel);
            if (occupant && occupant != cell) return false;
        }
        for (auto bel : alm.ff_bels) if (ctx->getBoundBelCell(bel)) return false;
    }
    const std::set<IdString> allowed = cell->type == id_MISTRAL_ALUT6 ?
        std::set<IdString>{id_A, id_B, id_C, id_D, from.idx ? id_E1 : id_E0, from.idx ? id_F1 : id_F0} :
        std::set<IdString>{id_A, id_B, from.idx ? id_D : id_C,
                           from.idx ? id_E1 : id_E0, from.idx ? id_F1 : id_F0};
    std::set<IdString> used;
    int inputs = 0;
    for (const auto &port : cell->ports) {
        if (port.second.type == PORT_OUT) {
            if (port.first != id_Q) return false;
            continue;
        }
        auto pin = cell->pin_data.find(port.first);
        if (port.second.type != PORT_IN || !port.second.net || pin == cell->pin_data.end() ||
            (pin->second.state != PIN_SIG && pin->second.state != PIN_INV) ||
            pin->second.bel_pins.size() != 1 || !allowed.count(pin->second.bel_pins.front()) ||
            !used.insert(pin->second.bel_pins.front()).second) return false;
        ++inputs;
    }
    auto q = cell->pin_data.find(id_Q);
    if (inputs != cell->combInfo.lut_input_count || !cell->getPort(id_Q) ||
        q == cell->pin_data.end() || q->second.bel_pins != std::vector<IdString>{id_COMBOUT}) return false;
    // A sole ordinary LUT with no FFs has exactly the same ALM input cost
    // at its destination. Preview only those two counters and reuse the
    // architecture's unchanged LAB capacity check. Full native placement
    // legality still checks both complete LABs before routing a trial.
    auto &source_inputs = ctx->labs.at(from.lab).alms.at(from.alm).unique_input_count;
    auto &target_inputs = ctx->labs.at(to.lab).alms.at(to.alm).unique_input_count;
    if (source_inputs <= 0 || target_inputs != 0) return false;
    struct RestoreInputs {
        int &source, &target;
        int old_source, old_target;
        ~RestoreInputs() { source = old_source; target = old_target; }
    } restore{source_inputs, target_inputs, source_inputs, target_inputs};
    target_inputs = source_inputs;
    source_inputs = 0;
    return ctx->check_lab_input_count(from.lab) && ctx->check_lab_input_count(to.lab);
}
NEXTPNR_NAMESPACE_END
#endif
