/* Native LUT input trials; routing and timing acceptance belong to the caller. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_ANALOGUE_LUT_PINS_H
#define MISTRAL_ANALOGUE_LUT_PINS_H
#include <array>
#include <set>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN
// A separate opt-in may change private L5 inputs next to independent FFs
// only when their complete local data path is the recognised static EF
// bypass. All FF nets stay outside the two rerouted input nets. Clock
// support is deliberately limited to an explicitly constrained external
// input through ordinary clock buffers; no generated-clock inference.
inline bool analogue_static_ff_bypass_l5_allowed(Context *ctx, const CellInfo *cell,
                                                 IdString first, IdString second)
{
    if (!cell || cell->bel == BelId() || !cell->type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3,
        id_MISTRAL_ALUT4, id_MISTRAL_ALUT5) || first == second) return false;
    auto *a = cell->getPort(first), *b = cell->getPort(second);
    auto pa = cell->pin_data.find(first), pb = cell->pin_data.find(second);
    if (!a || !b || a == b || pa == cell->pin_data.end() || pb == cell->pin_data.end() ||
        pa->second.bel_pins.size() != 1 || pb->second.bel_pins.size() != 1 ||
        (pa->second.state != PIN_SIG && pa->second.state != PIN_INV) ||
        (pb->second.state != PIN_SIG && pb->second.state != PIN_INV)) return false;
    const auto &where = ctx->bel_data(cell->bel).lab_data;
    const auto &alm = ctx->labs.at(where.lab).alms.at(where.alm);
    if (where.idx > 1 || alm.l6_mode || alm.carry_mode ||
        !ctx->isBelLocationValid(cell->bel)) return false;
    const std::set<IdString> private_pins{where.idx ? id_D : id_C, where.idx ? id_E1 : id_E0,
                                         where.idx ? id_F1 : id_F0};
    if (!private_pins.count(pa->second.bel_pins.front()) || !private_pins.count(pb->second.bel_pins.front()))
        return false;
    const auto wa = ctx->getBelPinWire(cell->bel, pa->second.bel_pins.front());
    const auto wb = ctx->getBelPinWire(cell->bel, pb->second.bel_pins.front());
    if (wa == wb || ctx->getBoundWireNet(wa) != a || ctx->getBoundWireNet(wb) != b) return false;
    bool have_ff = false;
    for (int i = 0; i < 4; ++i) {
        auto *ff = ctx->getBoundBelCell(alm.ff_bels.at(i));
        if (!ff) continue;
        have_ff = true;
        // Preserve the existing native EF resource rule, even where a
        // looser physical sharing argument might look attractive.
        auto *opposite = ctx->getBoundBelCell(alm.lut_bels.at(1 - i / 2));
        if (opposite && opposite->combInfo.used_lut_input_count > 2) return false;
        if (ff->type != id_MISTRAL_FF || ff->belStrength >= STRENGTH_LOCKED || ff->region ||
            ff->cluster != ClusterId()) return false;
        for (const char *name : {"keep", "dont_touch", "BEL", "FES_SLOT"})
            if (ff->attrs.count(ctx->id(name))) return false;
        for (auto port : {id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA})
            if (ff->getPort(port)) return false;
        for (const auto &port : ff->ports)
            if (port.second.net == a || port.second.net == b) return false;
        auto *clock = ff->getPort(id_CLK), *data = ff->getPort(id_DATAIN);
        if (!clock || !clock->clkconstr || !data || data == cell->getPort(id_Q) ||
            ff->get_pin_state(id_DATAIN) != PIN_SIG) return false;
        std::set<IdString> seen;
        auto *root = clock;
        int buffers = 0;
        while (root && root->driver.cell && root->driver.cell->type == id_MISTRAL_CLKBUF) {
            auto *driver = root->driver.cell;
            if (root->driver.port != id_Q || driver->getPort(id_Q) != root || driver->ports.size() != 2 ||
                !seen.insert(root->name).second ||
                ++buffers > 16) return false;
            root = driver->getPort(id_A);
        }
        if (!root || !root->driver.cell || root->driver.cell->type != id_MISTRAL_IB || root->driver.port != id_O ||
            root->driver.cell->getPort(id_O) != root)
            return false;
        const auto ef = alm.sel_ef.at(i / 2), input = alm.ff_in.at(i);
        auto ef_binding = data->wires.find(ef), in_binding = data->wires.find(input);
        if (ef_binding == data->wires.end() || in_binding == data->wires.end() ||
            ef_binding->second.pip == PipId() || in_binding->second.pip == PipId() ||
            ctx->getPipSrcWire(in_binding->second.pip) != ef ||
            ctx->getPipDstWire(in_binding->second.pip) != input ||
            ctx->getPipDstWire(ef_binding->second.pip) != ef ||
            ctx->getBoundWireNet(ef) != data || ctx->getBoundWireNet(input) != data) return false;
        const auto source = ctx->getPipSrcWire(ef_binding->second.pip);
        const auto e = ctx->getBelPinWire(cell->bel, i / 2 ? id_E1 : id_E0);
        const auto mirrored_f = ctx->getBelPinWire(cell->bel, i / 2 ? id_F0 : id_F1);
        if ((source != e && source != mirrored_f) || source == wa || source == wb ||
            ctx->getBoundWireNet(source) != data) return false;
    }
    return have_ff;
}

inline bool analogue_lut_pin_swap_allowed(Context *ctx, const CellInfo *cell, IdString first, IdString second,
                                          bool sole_l5 = false, bool static_ff_bypass_l5 = false)
{
    if (!cell || !cell->type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                               id_MISTRAL_ALUT5, id_MISTRAL_ALUT6) || cell->bel == BelId() ||
        cell->belStrength > STRENGTH_WEAK || cell->cluster != ClusterId() || cell->region || first == second)
        return false;
    for (const char *name : {"keep", "dont_touch", "BEL", "FES_SLOT"})
        if (cell->attrs.count(ctx->id(name)))
            return false;
    const auto &where = ctx->bel_data(cell->bel).lab_data;
    const auto &alm = ctx->labs.at(where.lab).alms.at(where.alm);
    const int half = where.idx;
    const int inputs = cell->type == id_MISTRAL_ALUT6 ? 6 : cell->type == id_MISTRAL_ALUT5 ? 5 :
                       cell->type == id_MISTRAL_ALUT4 ? 4 : cell->type == id_MISTRAL_ALUT3 ? 3 : 2;
    const bool l5 = inputs < 6;
    if ((sole_l5 && !l5) || (static_ff_bypass_l5 && (!l5 || sole_l5))) return false;
    if (half < 0 || half > 1 || alm.l6_mode != !l5 || alm.carry_mode)
        return false;
    auto *peer = ctx->getBoundBelCell(alm.lut_bels.at(1 - half));
    if (sole_l5 && peer) return false;
    if (peer && (!l5 || !peer->type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                                         id_MISTRAL_ALUT5) || peer->belStrength >= STRENGTH_LOCKED ||
                 peer->region || peer->cluster != ClusterId())) return false;
    if (peer) for (const char *name : {"keep", "dont_touch", "BEL", "FES_SLOT"})
        if (peer->attrs.count(ctx->id(name))) return false;
    for (BelId bel : alm.ff_bels) {
        auto *ff = ctx->getBoundBelCell(bel);
        if (ff && ((l5 && !static_ff_bypass_l5) || ff->belStrength >= STRENGTH_LOCKED ||
                   (!l5 && (ff->getPort(id_SDATA) ||
                    (ff->getPort(id_DATAIN) && ff->getPort(id_DATAIN) != cell->getPort(id_Q))))))
            return false;
    }
    const std::array<IdString, 6> ports{id_A, id_B, id_C, id_D, id_E, id_F};
    bool have_first = false, have_second = false;
    std::set<IdString> physical;
    for (int i = 0; i < inputs; ++i) {
        const auto port = ports[i];
        have_first |= port == first;
        have_second |= port == second;
        auto pin = cell->pin_data.find(port);
        if (!cell->getPort(port) || pin == cell->pin_data.end() || pin->second.bel_pins.size() != 1 ||
            (pin->second.state != PIN_SIG && pin->second.state != PIN_INV) ||
            !physical.insert(pin->second.bel_pins.front()).second)
            return false;
    }
    if (!have_first || !have_second || cell->getPort(first) == cell->getPort(second)) return false;
    if (!l5) return physical == std::set<IdString>{id_A, id_B, id_C, id_D,
                                                 half ? id_E1 : id_E0, half ? id_F1 : id_F0};
    const std::set<IdString> private_pins{half ? id_D : id_C, half ? id_E1 : id_E0, half ? id_F1 : id_F0};
    for (auto pin : physical) if (pin != id_A && pin != id_B && !private_pins.count(pin)) return false;
    // A separate opt-in may use shared A/B only when this LUT is the
    // sole occupant of the whole ALM. All four FFs were rejected above.
    // Ordinary and private-L5 callers retain their original ownership rule.
    if (sole_l5) return true;
    if (!private_pins.count(cell->pin_data.at(first).bel_pins.front()) ||
        !private_pins.count(cell->pin_data.at(second).bel_pins.front())) return false;
    // Only half-private L5 pins move. Shared A/B signals, every peer pin
    // and all FF selectors retain their exact parent ownership.
    if (peer) for (const auto &port : peer->ports) if (port.second.type == PORT_IN) {
        auto pin = peer->pin_data.find(port.first);
        if (pin == peer->pin_data.end() || pin->second.bel_pins.size() != 1 ||
            private_pins.count(pin->second.bel_pins.front())) return false;
    }
    return !static_ff_bypass_l5 || analogue_static_ff_bypass_l5_allowed(ctx, cell, first, second);
}

inline bool analogue_swap_lut_pins(Context *ctx, CellInfo *cell, IdString first, IdString second,
                                   bool sole_l5 = false, bool static_ff_bypass_l5 = false)
{
    if (!analogue_lut_pin_swap_allowed(ctx, cell, first, second, sole_l5, static_ff_bypass_l5))
        return false;
    std::swap(cell->pin_data.at(first).bel_pins, cell->pin_data.at(second).bel_pins);
    return true;
}
NEXTPNR_NAMESPACE_END
#endif
