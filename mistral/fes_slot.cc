/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2021  gatecat <gatecat@ds0.me>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "json11.hpp"
#include "log.h"
#include "nextpnr.h"
#include "util.h"

using json11::Json;

NEXTPNR_NAMESPACE_BEGIN

namespace {

std::string fes_encode_snapshot(const Json &payload)
{
    std::string encoded;
    static const char hex[] = "0123456789abcdef";
    for (unsigned char ch : payload.dump()) {
        encoded += hex[ch >> 4];
        encoded += hex[ch & 15];
    }
    return encoded;
}

Json fes_decode_snapshot(const Property &saved, const char *description)
{
    if (!saved.is_string)
        log_error("Invalid frozen %s encoding.\n", description);
    const std::string encoded = saved.as_string();
    std::string decoded;
    if (encoded.size() % 2 != 0)
        log_error("Invalid frozen %s encoding.\n", description);
    auto nibble = [&](char ch) {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        log_error("Invalid frozen %s encoding.\n", description);
    };
    for (size_t i = 0; i < encoded.size(); i += 2)
        decoded += char((nibble(encoded[i]) << 4) | nibble(encoded[i + 1]));
    std::string error;
    Json payload = Json::parse(decoded, error);
    if (!error.empty() || payload.dump() != decoded)
        log_error("Invalid frozen %s payload.\n", description);
    return payload;
}

bool fes_skip_cart_cell(IdString type)
{
    return type.in(id_MISTRAL_IB, id_MISTRAL_OB, id_MISTRAL_IO, id_MISTRAL_BUF, id_MISTRAL_CLKENA,
                   id_MISTRAL_CLKBUF, id_cyclonev_hps_interface_mpu_general_purpose,
                   id_cyclonev_hps_interface_peripheral_i2c);
}

NetInfo *find_named_net(Context *ctx, const std::string &name)
{
    IdString id = ctx->id(name);
    if (ctx->nets.count(id))
        return ctx->nets.at(id).get();
    if (ctx->net_aliases.count(id))
        return ctx->getNetByAlias(id);
    return nullptr;
}

// Yosys drops plug_addr[0:5] netnames once they alias gp_in[15:10].
NetInfo *find_plug_addr_bit(Context *ctx, int index)
{
    auto found = ctx->cells.find(ctx->id(stringf("plug_addr_ff_%d", index)));
    if (found != ctx->cells.end()) {
        NetInfo *q = found->second->getPort(id_Q);
        if (q != nullptr)
            return q;
    }
    return find_named_net(ctx, stringf("plug_addr[%d]", index));
}

NetInfo *shell_clock_net(Context *ctx)
{
    const IdString selection = ctx->id("fes/slot_clock");
    if (ctx->settings.count(selection)) {
        const std::string name = ctx->settings.at(selection).as_string();
        NetInfo *clock = find_named_net(ctx, name);
        if (clock == nullptr || clock->driver.cell == nullptr)
            log_error("FES slot clock '%s' is not a driven shell net.\n", name.c_str());
        if (ctx->fes_cell_is_slot(clock->driver.cell))
            log_error("FES slot clock '%s' is driven by the cart, not the shell.\n", name.c_str());
        return clock;
    }
    NetInfo *clock = nullptr;
    for (auto &item : ctx->cells) {
        CellInfo *ci = item.second.get();
        if (ci->type != id_MISTRAL_FF)
            continue;
        if (ctx->fes_cell_is_slot(ci))
            continue;
        NetInfo *clk = ci->getPort(id_CLK);
        if (clk != nullptr) {
            if (clock != nullptr && clock != clk)
                log_error("FES shell has multiple clocks; select the socket clock with --fes-slot-clock.\n");
            clock = clk;
        }
    }
    if (clock != nullptr)
        return clock;
    clock = find_named_net(ctx, "FPGA_CLK1_50");
    if (clock == nullptr || clock->driver.cell == nullptr)
        log_error("FES shell has no driven socket clock; use --fes-slot-clock.\n");
    return clock;
}

CellInfo *find_rdata_ff(Context *ctx, const std::string &port)
{
    std::string suffix;
    if (port == "plug_rdata")
        suffix = "0";
    else if (port.size() > 11 && port.compare(0, 11, "plug_rdata[") == 0)
        suffix = port.substr(11, port.size() - 12);
    else
        return nullptr;
    const std::string cell_name = "plug_rdata_ff_" + suffix;
    auto found = ctx->cells.find(ctx->id(cell_name));
    if (found == ctx->cells.end())
        return nullptr;
    return found->second.get();
}

Property fes_parse_property(const Json &val)
{
    if (val.is_number())
        return Property(val.int_value(), 32);
    return Property::from_string(val.string_value());
}

bool fes_json_const(const Json &bit, char &value)
{
    if (!bit.is_string())
        return false;
    const std::string &s = bit.string_value();
    if (s.size() != 1)
        return false;
    value = s[0];
    return value == '0' || value == '1' || value == 'x';
}

bool fes_json_signal(const Json &bit, int &id)
{
    if (bit.is_number()) {
        id = bit.int_value();
        return true;
    }
    return false;
}

void fes_rip_net_routing(Context *ctx, NetInfo *net)
{
    if (net == nullptr)
        return;
    while (!net->wires.empty()) {
        auto it = net->wires.begin();
        if (it->second.pip != PipId())
            ctx->unbindPip(it->second.pip);
        else
            ctx->unbindWire(it->first);
    }
}

void fes_unbind_port_wires(Context *ctx, CellInfo *cell, IdString port)
{
    if (cell == nullptr || cell->bel == BelId())
        return;
    NetInfo *net = cell->getPort(port);
    if (net == nullptr)
        return;
    std::vector<WireId> wires;
    auto add = [&](WireId w) {
        if (w != WireId() && ctx->getBoundWireNet(w) == net)
            wires.push_back(w);
    };
    if (cell->pin_data.count(port)) {
        for (IdString bp : cell->pin_data.at(port).bel_pins)
            add(ctx->getBelPinWire(cell->bel, bp));
    }
    add(ctx->getBelPinWire(cell->bel, port));
    for (WireId w : wires) {
        if (ctx->getBoundWireNet(w) == net)
            ctx->unbindWire(w);
    }
}

void fes_trim_net_orphans(Context *ctx, NetInfo *net)
{
    if (net == nullptr || net->wires.empty())
        return;
    if (net->users.empty()) {
        // Detaching the vacant return FF can strand its route-through source.
        // With no remaining sink, even a locked source wire is an orphan.
        fes_rip_net_routing(ctx, net);
        return;
    }
    pool<WireId> used;
    WireId src = ctx->getNetinfoSourceWire(net);
    if (src != WireId())
        used.insert(src);
    for (auto usr : net->users) {
        // Imported routed shells have physical BEL bindings before the
        // scaffold restores logical pin mappings. Inspect those bindings
        // directly; querying logical sink wires here can address a packed
        // port that no longer exists on the physical BEL.
        if (usr.cell == nullptr || usr.cell->bel == BelId())
            continue;
        for (IdString pin : ctx->getBelPins(usr.cell->bel)) {
            WireId cursor = ctx->getBelPinWire(usr.cell->bel, pin);
            if (cursor == WireId() || ctx->getBoundWireNet(cursor) != net)
                continue;
            int guard = 0;
            while (cursor != WireId() && net->wires.count(cursor) && guard++ < 100000) {
                used.insert(cursor);
                PipId pip = net->wires.at(cursor).pip;
                if (pip == PipId())
                    break;
                cursor = ctx->getPipSrcWire(pip);
            }
        }
    }
    std::vector<WireId> extra;
    for (auto &it : net->wires) {
        if (!used.count(it.first))
            extra.push_back(it.first);
    }
    for (WireId w : extra) {
        if (net->wires.count(w))
            ctx->unbindWire(w);
    }
}

NetInfo *rdata_sink_net(Context *ctx, int index)
{
    CellInfo *ff = find_rdata_ff(ctx, stringf("plug_rdata[%d]", index));
    if (ff == nullptr)
        return find_named_net(ctx, stringf("plug_rdata_d[%d]", index));
    // Vacant DATAIN is packed onto $PACKER_GND_NET with every other const-0
    // pin. Unbind only this FF's sink wire, then detach; ripping the shared
    // net would make the cart M10K drive chip-wide GND (SCLR, HPS, ...).
    NetInfo *old = ff->getPort(id_DATAIN);
    fes_unbind_port_wires(ctx, ff, id_DATAIN);
    if (ff->getPort(id_DATAIN) != nullptr)
        ff->disconnectPort(id_DATAIN);
    fes_trim_net_orphans(ctx, old);
    IdString name = ctx->id(stringf("fes_rdata[%d]", index));
    int suffix = 0;
    while (ctx->nets.count(name))
        name = ctx->id(stringf("fes_rdata[%d]$%d", index, suffix++));
    NetInfo *sink = ctx->createNet(name);
    if (!ff->ports.count(id_DATAIN))
        ff->addInput(id_DATAIN);
    ff->pin_data[id_DATAIN].state = PIN_SIG;
    ff->connectPort(id_DATAIN, sink);
    return sink;
}

} // namespace

bool Arch::fes_cell_is_slot(const CellInfo *cell) const { return fes_cell_slot_region(cell) != IdString(); }

void Arch::fes_rip_reserved_shell_pips()
{
    Context *ctx = getCtx();
    int ripped = 0;
    for (auto &item : ctx->nets) {
        NetInfo *net = item.second.get();
        if (fes_cell_is_slot(net->driver.cell))
            continue;
        std::vector<PipId> pips;
        for (auto &wire : net->wires) {
            if (wire.second.pip != PipId() && fes_pip_in_socket(wire.second.pip))
                pips.push_back(wire.second.pip);
        }
        for (PipId pip : pips) {
            ctx->unbindPip(pip);
            ++ripped;
        }
    }
    log_info("FES ripped %d reserved-tile pips from shell nets.\n", ripped);
}

void Arch::save_fes_pin_maps()
{
    // Routing does not describe unused memory lanes or folded hard constants.
    // Preserve the complete physical mapping alongside the frozen netlist.
    for (auto &item : getCtx()->cells) {
        CellInfo *ci = item.second.get();
        if (ci->bel == BelId())
            continue;
        Json::object ports;
        for (const auto &pin : ci->pin_data) {
            Json::array data{int(pin.second.state)};
            for (IdString physical : pin.second.bel_pins)
                data.emplace_back(physical.str(getCtx()));
            ports[pin.first.str(getCtx())] = data;
        }
        // The generic JSON writer does not escape quotes in attributes.
        // Hex keeps this architecture-specific payload unambiguous there.
        Json payload = Json::object{{"count", int(ports.size())}, {"pins", ports}};
        ci->attrs[id("FES_PINMAP_V1")] = fes_encode_snapshot(payload);
    }
    Json::array physical_labs;
    for (const auto &lab : labs) {
        Loc loc = getBelLocation(lab.alms[0].lut_bels[0]);
        Json::array state{int(lab.aclr_used[0]), int(lab.aclr_used[1])};
        for (const auto &alm : lab.alms) {
            for (int value : {int(alm.l6_mode), int(alm.carry_mode), alm.clk_ena_idx[0], alm.clk_ena_idx[1],
                              alm.aclr_idx[0], alm.aclr_idx[1]})
                state.emplace_back(value);
        }
        physical_labs.emplace_back(Json::array{loc.x, loc.y, int(lab.is_mlab), state});
    }
    getCtx()->attrs[id("FES_LABSTATE_V1")] =
            fes_encode_snapshot(Json::object{{"device", args.device}, {"labs", physical_labs}});
}

void Arch::lock_fes_scaffold()
{
    Context *ctx = getCtx();
    for (const auto &attr : ctx->attrs) {
        const std::string key = attr.first.str(ctx);
        if (key.compare(0, 13, "FES_LABSTATE_") == 0 && key != "FES_LABSTATE_V1")
            log_error("Unsupported frozen LAB-state version.\n");
    }
    auto lab_snapshot = ctx->attrs.find(id("FES_LABSTATE_V1"));
    if (lab_snapshot == ctx->attrs.end())
        log_error("Missing frozen LAB state; rebuild the shell with physical snapshot metadata.\n");
    Json lab_payload = fes_decode_snapshot(lab_snapshot->second, "LAB state");
    if (!lab_payload.is_object() || lab_payload.object_items().size() != 2 ||
        !lab_payload["device"].is_string() || lab_payload["device"].string_value() != args.device ||
        !lab_payload["labs"].is_array() || lab_payload["labs"].array_items().size() != labs.size())
        log_error("Invalid frozen LAB-state device or geometry.\n");
    for (size_t index = 0; index < labs.size(); ++index) {
        auto &lab = labs[index];
        const auto &entry = lab_payload["labs"][index];
        Loc loc = getBelLocation(lab.alms[0].lut_bels[0]);
        if (!entry.is_array() || entry.array_items().size() != 4 || entry[0].number_value() != loc.x ||
            entry[1].number_value() != loc.y || entry[2].number_value() != int(lab.is_mlab) ||
            !entry[0].is_number() || !entry[1].is_number() || !entry[2].is_number() ||
            !entry[3].is_array() || entry[3].array_items().size() != 62)
            log_error("Invalid frozen LAB-state geometry at index %zu.\n", index);
        const auto &state = entry[3].array_items();
        for (size_t offset = 0; offset < state.size(); ++offset) {
            const int max_value = offset >= 2 && ((offset - 2) % 6 == 2 || (offset - 2) % 6 == 3) ? 2 : 1;
            if (!state[offset].is_number() || state[offset].number_value() != state[offset].int_value() ||
                state[offset].int_value() < 0 || state[offset].int_value() > max_value)
                log_error("Invalid frozen LAB-state field at index %zu.%zu.\n", index, offset);
        }
        lab.aclr_used = {bool(state[0].int_value()), bool(state[1].int_value())};
        for (size_t alm_index = 0; alm_index < 10; ++alm_index) {
            auto &alm = lab.alms[alm_index];
            size_t start = 2 + 6 * alm_index;
            alm.l6_mode = state[start].int_value();
            alm.carry_mode = state[start + 1].int_value();
            alm.clk_ena_idx = {state[start + 2].int_value(), state[start + 3].int_value()};
            alm.aclr_idx = {state[start + 4].int_value(), state[start + 5].int_value()};
        }
    }
    // Rip is optional; mixed USER/WEAK trees make GPU router1 check abort.
    // fes_rip_reserved_shell_pips();
    if (fes_has_reserved_rect) {
        int m10ks = 0;
        for (BelId bel : getBels()) {
            if (getBelType(bel) != id_MISTRAL_M10K || !fes_bel_region.count(bel))
                continue;
            if (m10ks < 16)
                log_info("FES reserved M10K %s (region '%s')\n", getBelName(bel).str(getCtx()).c_str(),
                         fes_bel_region.at(bel).c_str(getCtx()));
            ++m10ks;
        }
        log_info("FES reserved M10K count %d\n", m10ks);
    }
    log_info("FES locking scaffold routing...\n");
    int locked_cells = 0;
    for (auto &item : ctx->cells) {
        CellInfo *ci = item.second.get();
        if (ci->bel == BelId() || fes_cell_is_slot(ci))
            continue;
        ci->belStrength = STRENGTH_LOCKED;
        fes_frozen_cells.emplace(ci, ci->bel);
        ++locked_cells;
        for (const auto &attr : ci->attrs) {
            const std::string key = attr.first.str(ctx);
            if (key.compare(0, 11, "FES_PINMAP_") == 0 && key != "FES_PINMAP_V1")
                log_error("Unsupported frozen pin-map version for %s.\n", ctx->nameOf(ci));
        }
        auto saved = ci->attrs.find(id("FES_PINMAP_V1"));
        if (saved != ci->attrs.end()) {
            Json payload = fes_decode_snapshot(saved->second, "pin-map");
            const Json &ports = payload["pins"];
            if (!payload.is_object() || payload.object_items().size() != 2 ||
                !ports.is_object() || !payload["count"].is_number() ||
                payload["count"].number_value() != double(ports.object_items().size()))
                log_error("Invalid frozen pin map for %s.\n", ctx->nameOf(ci));
            for (const auto &port : ci->ports) {
                if (port.second.net != nullptr && !ports.object_items().count(port.first.str(ctx)))
                    log_error("Incomplete frozen pin map for %s.%s.\n", ctx->nameOf(ci), port.first.c_str(ctx));
            }
            ci->pin_data.clear();
            for (const auto &entry : ports.object_items()) {
                const auto &data = entry.second.array_items();
                if (!entry.second.is_array() || data.empty() || !data[0].is_number() ||
                    data[0].number_value() != data[0].int_value() || data[0].int_value() < PIN_SIG ||
                    data[0].int_value() > PIN_INV)
                    log_error("Invalid frozen pin state for %s.%s.\n", ctx->nameOf(ci), entry.first.c_str());
                auto &pin = ci->pin_data[id(entry.first)];
                pin.state = CellPinState(data[0].int_value());
                std::set<IdString> seen;
                for (size_t i = 1; i < data.size(); ++i) {
                    if (!data[i].is_string() || !bel_data(ci->bel).pins.count(id(data[i].string_value())))
                        log_error("Invalid frozen physical pin for %s.%s.\n", ctx->nameOf(ci), entry.first.c_str());
                    auto logical = ci->ports.find(id(entry.first));
                    PortType direction = getBelPinType(ci->bel, id(data[i].string_value()));
                    if (logical != ci->ports.end() && direction != PORT_INOUT && direction != logical->second.type)
                        log_error("Wrong frozen pin direction for %s.%s.\n", ctx->nameOf(ci), entry.first.c_str());
                    if (!seen.insert(id(data[i].string_value())).second)
                        log_error("Duplicate frozen physical pin for %s.%s.\n", ctx->nameOf(ci), entry.first.c_str());
                    pin.bel_pins.push_back(id(data[i].string_value()));
                }
                auto logical = ci->ports.find(id(entry.first));
                if (logical != ci->ports.end() && logical->second.net != nullptr && pin.bel_pins.empty() &&
                    logical->second.net->driver.cell != nullptr && (pin.state == PIN_SIG || pin.state == PIN_INV))
                    log_error("Empty frozen signal pin map for %s.%s.\n", ctx->nameOf(ci), entry.first.c_str());
            }
            continue;
        }
        log_error("Missing frozen pin map for %s; rebuild the shell with physical snapshot metadata.\n",
                  ctx->nameOf(ci));
    }
    // Control inversion and LUT annotations were initially derived before
    // restoring folded state. Recompute them without default pin remapping.
    std::vector<BelId> restored;
    for (auto &item : ctx->cells) {
        CellInfo *ci = item.second.get();
        if (ci->bel == BelId() || fes_cell_is_slot(ci))
            continue;
        if (is_comb_cell(ci->type) || ci->type.in(id_MISTRAL_MLAB, id_MISTRAL_BUF))
            assign_comb_info(ci);
        else if (ci->type == id_MISTRAL_FF)
            assign_ff_info(ci);
        restored.push_back(ci->bel);
    }
    for (BelId bel : restored)
        update_bel(bel);
    int locked = 0;
    for (auto &item : ctx->nets) {
        NetInfo *net = item.second.get();
        if (net->wires.empty() || fes_cell_is_slot(net->driver.cell))
            continue;
        ctx->lockNetRouting(item.first);
        ++locked;
    }
    log_info("FES scaffold locked %d cells and routing on %d nets.\n", locked_cells, locked);
}

bool Arch::fes_net_touches_slot(const NetInfo *net) const
{
    if (net == nullptr)
        return false;
    if (fes_cell_is_slot(net->driver.cell))
        return true;
    for (auto &user : net->users) {
        if (fes_cell_is_slot(user.cell))
            return true;
    }
    return false;
}

bool Arch::fes_pip_in_socket(PipId pip) const
{
    if (!fes_has_reserved_rect)
        return false;
    Loc loc = getPipLocation(pip);
    for (const auto &rect : fes_reserved_rects) {
        if (loc.x >= rect.x0 && loc.x <= rect.x1 && loc.y >= rect.y0 && loc.y <= rect.y1)
            return true;
    }
    return false;
}

void Arch::note_fes_cram_region(const std::string &spec)
{
#ifndef MISTRAL_ROUTING_MUX_CRAM_BITS
    log_error("Physical CRAM routing fence requires Mistral routing-mux coordinates.\n");
#else
    int x0, y0, x1, y1;
    char extra;
    if (sscanf(spec.c_str(), "%d,%d,%d,%d%c", &x0, &y0, &x1, &y1, &extra) != 4 || x0 < 0 || y0 < 0 ||
        x1 <= x0 || y1 <= y0 || uint32_t(x1) > cyclonev->get_cram_sx() || uint32_t(y1) > cyclonev->get_cram_sy())
        log_error("Invalid FES CRAM region; expected half-open x0,y0,x1,y1.\n");
    fes_cram_region = {x0, y0, x1, y1};
    fes_has_cram_region = true;
    std::vector<std::pair<uint32_t, uint32_t>> bits;
    for (uint32_t ri = 0; ri < cyclonev->rnode_index_count(); ri++) {
        const auto *ro = cyclonev->ri2ro(ri);
        if (ro == nullptr)
            continue;
        const auto node = ro->rc();
        if (!cyclonev->rnode_mux_cram_bits(node, bits))
            log_error("Missing routing mux physical coordinates.\n");
        bool inside = true;
        for (const auto &bit : bits) {
            if (bit.first < uint32_t(x0) || bit.first >= uint32_t(x1) || bit.second < uint32_t(y0) ||
                bit.second >= uint32_t(y1)) {
                inside = false;
                break;
            }
        }
        if (inside)
            fes_cram_allowed_muxes.insert(node);
    }
    // Snapshot before cart merge can detach any original return-path stubs.
    // Exact old selections remain legal; a different source at an outside mux
    // is forbidden even for a mixed shell/cart constant net.
    for (const auto &item : getCtx()->nets)
        for (const auto &wire : item.second->wires)
            if (wire.second.pip != PipId())
                fes_frozen_pips.insert(wire.second.pip);
    log_info("FES physical CRAM fence admits %zu muxes and preserves %zu frozen pips.\n",
             fes_cram_allowed_muxes.size(), fes_frozen_pips.size());
#endif
}

bool Arch::fes_pip_preserves_cram(PipId pip) const
{
    // Synthetic BEL edges are not routing muxes: write_routing skips them.
    // Their cell/control configuration is governed by frozen physical state
    // and the placement fence, and the producer checks the final CRAM bytes.
    return !fes_has_cram_region || WireId(pip.src).is_nextpnr_created() || WireId(pip.dst).is_nextpnr_created() ||
           fes_frozen_pips.count(pip) || fes_cram_allowed_muxes.count(pip.dst);
}

bool Arch::fes_pip_in_plug_halo(PipId pip) const
{
    if (!fes_has_reserved_rect)
        return false;
    Loc loc = getPipLocation(pip);
    for (const auto &rect : fes_reserved_rects) {
        if (loc.x >= rect.x0 - 6 && loc.x <= rect.x1 + 6 && loc.y >= rect.y0 - 6 && loc.y <= rect.y1 + 6)
            return true;
    }
    return false;
}

bool Arch::fes_pip_reaches_net_shell_tile(PipId pip, const NetInfo *net) const
{
    if (net == nullptr)
        return false;
    Loc loc = getPipLocation(pip);
    auto hits_shell_pin = [&](WireId wire) {
        auto found = wires.find(wire);
        if (found == wires.end())
            return false;
        for (auto &pin : found->second.bel_pins) {
            CellInfo *bound = getBoundBelCell(pin.bel);
            if (bound != nullptr && !fes_cell_is_slot(bound))
                return true;
        }
        return false;
    };
    if (hits_shell_pin(WireId(pip.src)) || hits_shell_pin(WireId(pip.dst)))
        return true;
    auto check_cell = [&](const CellInfo *cell) {
        if (cell == nullptr || fes_cell_is_slot(cell) || cell->bel == BelId())
            return false;
        Loc bel = getBelLocation(cell->bel);
        return bel.x == loc.x && bel.y == loc.y;
    };
    if (check_cell(net->driver.cell))
        return true;
    for (auto &user : net->users) {
        if (check_cell(user.cell))
            return true;
    }
    return false;
}

void Arch::merge_fes_cart(const std::string &filename, const std::string &region)
{
    Context *ctx = getCtx();
    // A run with no FES_RESERVED_RECT at all has no placement fencing to
    // validate against (matches the pre-multi-region behaviour); once any
    // region is declared, an unknown --fes-cart-region is a real mistake.
    if (fes_has_reserved_rect) {
        IdString region_id = id(region);
        auto absorbed = fes_region_absorbed_by.find(region_id);
        if (absorbed != fes_region_absorbed_by.end())
            log_error("FES cart region '%s' was absorbed into group '%s' by FES_RESERVED_RECT_GROUP and is no "
                      "longer independently available; merge '%s' against '%s' instead.\n",
                      region.c_str(), absorbed->second.c_str(ctx), filename.c_str(), absorbed->second.c_str(ctx));
        if (!fes_region_bels.count(region_id))
            log_error("FES cart region '%s' has no matching FES_RESERVED_RECT; declare it with FES_RESERVED_RECT "
                      "\"%s x0 y0 x1 y1\" before merging '%s'.\n",
                      region.c_str(), region.c_str(), filename.c_str());
    }
    fes_active_cart_region = region;
    log_info("FES parsing cart JSON '%s' into region '%s'...\n", filename.c_str(), region.c_str());
    std::ifstream in(filename);
    if (!in)
        log_error("Failed to open FES cart JSON '%s'.\n", filename.c_str());
    std::string json_str((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    Json root = Json::parse(json_str, error);
    if (root.is_null())
        log_error("Loading FES cart JSON '%s' failed: %s.\n", filename.c_str(), error.c_str());
    Json modules = root["modules"];
    Json mod = modules["cart"];
    if (mod.is_null() && modules.is_object() && !modules.object_items().empty())
        mod = modules.object_items().begin()->second;
    if (mod.is_null())
        log_error("FES cart JSON '%s' has no cart module.\n", filename.c_str());

    std::map<int, std::pair<std::string, int>> top_bits;
    const Json &ports = mod["ports"];
    if (ports.is_object()) {
        for (const auto &item : ports.object_items()) {
            const auto &bits = item.second["bits"].array_items();
            for (int i = 0; i < int(bits.size()); i++) {
                int id;
                if (fes_json_signal(bits[i], id))
                    top_bits[id] = {item.first, i};
            }
        }
    }

    std::map<int, NetInfo *> bit_nets;
    auto intern_bit_net = [&](int id) -> NetInfo * {
        auto found = bit_nets.find(id);
        if (found != bit_nets.end())
            return found->second;
        NetInfo *net = ctx->createNet(ctx->id(stringf("fes_cart$bit%d", id)));
        bit_nets[id] = net;
        return net;
    };

    int mapped_ib = 0, missed_ib = 0, mapped_ob = 0;
    std::set<int> socket_clock_bits;
    const Json &cells = mod["cells"];
    if (cells.is_object()) {
        for (const auto &item : cells.object_items()) {
            const std::string type = item.second["type"].string_value();
            const Json &conns = item.second["connections"];
            if (type == "MISTRAL_IB") {
                const auto &pad = conns["PAD"].array_items();
                const auto &out = conns["O"].array_items();
                for (int i = 0; i < int(out.size()) && i < int(pad.size()); i++) {
                    int pad_id, out_id;
                    if (!fes_json_signal(pad[i], pad_id) || !fes_json_signal(out[i], out_id))
                        continue;
                    auto top = top_bits.find(pad_id);
                    if (top == top_bits.end())
                        continue;
                    NetInfo *shell = nullptr;
                    if (top->second.first == "FPGA_CLK1_50") {
                        shell = shell_clock_net(ctx);
                        socket_clock_bits.insert(out_id);
                    } else if (top->second.first == "plug_addr")
                        shell = find_plug_addr_bit(ctx, top->second.second);
                    if (shell != nullptr) {
                        bit_nets[out_id] = shell;
                        ++mapped_ib;
                    } else {
                        ++missed_ib;
                    }
                }
            } else if (type == "MISTRAL_OB") {
                const auto &in_bits = conns["I"].array_items();
                const auto &pad = conns["PAD"].array_items();
                for (int i = 0; i < int(in_bits.size()) && i < int(pad.size()); i++) {
                    int pad_id, in_id;
                    if (!fes_json_signal(pad[i], pad_id) || !fes_json_signal(in_bits[i], in_id))
                        continue;
                    auto top = top_bits.find(pad_id);
                    if (top == top_bits.end() || top->second.first != "plug_rdata")
                        continue;
                    NetInfo *sink = rdata_sink_net(ctx, top->second.second);
                    if (sink != nullptr) {
                        bit_nets[in_id] = sink;
                        ++mapped_ob;
                    } else {
                        ++missed_ib;
                    }
                }
            }
        }
    }

    // Synthesis may insert transparent clock buffers. Trace only those from
    // the declared cart clock input; a divider, gate or constant is a distinct
    // clock domain and must not be silently replaced by the shell clock.
    bool changed;
    do {
        changed = false;
        for (const auto &item : cells.object_items()) {
            const std::string type = item.second["type"].string_value();
            if (type != "MISTRAL_CLKBUF" && type != "MISTRAL_BUF")
                continue;
            const Json &conns = item.second["connections"];
            const auto &input = conns["A"].array_items();
            const auto &output = conns["Q"].array_items();
            int from, to;
            if (input.size() == 1 && output.size() == 1 && fes_json_signal(input[0], from) &&
                fes_json_signal(output[0], to) && socket_clock_bits.count(from))
                changed |= socket_clock_bits.insert(to).second;
        }
    } while (changed);

    int added = 0;
    if (cells.is_object()) {
        for (const auto &item : cells.object_items()) {
            const Json &src = item.second;
            const std::string type_name = src["type"].string_value();
            IdString type = ctx->id(type_name);
            if (fes_skip_cart_cell(type))
                continue;
            IdString dst_name = ctx->id(std::string("fes_cart$") + item.first);
            int suffix = 0;
            while (ctx->cells.count(dst_name))
                dst_name = ctx->id(stringf("fes_cart$%s$%d", item.first.c_str(), suffix++));
            CellInfo *dst = ctx->createCell(dst_name, type);
            const Json &params = src["parameters"];
            if (params.is_object()) {
                for (const auto &param : params.object_items())
                    dst->params[ctx->id(param.first)] = fes_parse_property(param.second);
            }
            const Json &attrs = src["attributes"];
            if (attrs.is_object()) {
                for (const auto &attr : attrs.object_items()) {
                    if (attr.first == "NEXTPNR_BEL" || attr.first == "BEL_STRENGTH" || attr.first == "ROUTING")
                        continue;
                    dst->attrs[ctx->id(attr.first)] = fes_parse_property(attr.second);
                }
            }
            dst->attrs[id("FES_SLOT")] = Property(region);

            const Json &dirs = src["port_directions"];
            const Json &conns = src["connections"];
            const std::vector<std::string> required_clocks =
                    type == id_MISTRAL_FF ? std::vector<std::string>{"CLK"}
                    : type == id_MISTRAL_M10K ? std::vector<std::string>{"CLK1"}
                    : type == id_MISTRAL_M10K_TDP ? std::vector<std::string>{"CLK1", "CLK2"}
                                                : std::vector<std::string>{};
            for (const auto &port : required_clocks)
                if (conns[port].array_items().size() != 1 || dirs[port].string_value() != "input")
                    log_error("FES cart '%s.%s' requires one declared socket clock input.\n",
                              item.first.c_str(), port.c_str());
            if (conns.is_object()) {
                for (const auto &conn : conns.object_items()) {
                    const auto &bits = conn.second.array_items();
                    const std::string dir = dirs[conn.first].string_value();
                    PortType ptype = PORT_IN;
                    if (dir == "output")
                        ptype = PORT_OUT;
                    else if (dir == "inout")
                        ptype = PORT_INOUT;
                    for (int i = 0; i < int(bits.size()); i++) {
                        const std::string pname =
                                bits.size() == 1 ? conn.first : stringf("%s[%d]", conn.first.c_str(), i);
                        IdString pid = ctx->id(pname);
                        if (ptype == PORT_OUT)
                            dst->addOutput(pid);
                        else if (ptype == PORT_INOUT)
                            dst->addInout(pid);
                        else
                            dst->addInput(pid);
                        char constant = 0;
                        int signal = 0;
                        const bool clock_port =
                                (type == id_MISTRAL_FF && pid == id_CLK) ||
                                (type.in(id_MISTRAL_M10K, id_MISTRAL_M10K_TDP) && pid.in(id_CLK1, id_CLK2));
                        if (clock_port &&
                            (!fes_json_signal(bits[i], signal) || !socket_clock_bits.count(signal)))
                            log_error("FES cart '%s.%s' does not use the declared socket clock; "
                                      "derived, gated and constant cart clocks are unsupported.\n",
                                      item.first.c_str(), pname.c_str());
                        if (fes_json_const(bits[i], constant)) {
                            if (ptype != PORT_IN)
                                log_error("FES cart cell '%s' has a constant output port.\n", item.first.c_str());
                            if (!ctx->nets.count(ctx->id("$PACKER_GND_NET")) ||
                                !ctx->nets.count(ctx->id("$PACKER_VCC_NET")))
                                log_error("FES cart merge requires a packed shell with constant nets.\n");
                            // Let the existing unbound packer choose the hard
                            // constant mux or a routed shell constant. Dropping
                            // this value would apply the pin's default instead.
                            dst->pin_data[pid].state = constant == '1' ? PIN_1 : PIN_0;
                            continue;
                        }
                        if (!fes_json_signal(bits[i], signal))
                            continue;
                        NetInfo *mapped = intern_bit_net(signal);
                        if (ptype == PORT_OUT && mapped->driver.cell != nullptr) {
                            CellInfo *old = mapped->driver.cell;
                            old->disconnectPort(mapped->driver.port);
                        }
                        dst->connectPort(pid, mapped);
                    }
                }
            }
            assign_default_pinmap(dst);
            ++added;
        }
    }
    NetInfo *clk = shell_clock_net(ctx);
    if (clk != nullptr) {
        for (auto &item : ctx->cells) {
            CellInfo *ci = item.second.get();
            if (!ci->type.in(id_MISTRAL_M10K, id_MISTRAL_M10K_TDP, id_MISTRAL_FF) || !fes_cell_is_slot(ci))
                continue;
            // FES sockets have one declared clock. Synthesis inserts clock
            // buffers which are removed at the cart boundary; reconnect all
            // supported sequential cells, including writable true-dual RAM.
            const std::vector<IdString> ports = ci->type == id_MISTRAL_FF
                                                      ? std::vector<IdString>{id_CLK}
                                                      : ci->type == id_MISTRAL_M10K_TDP
                                                                ? std::vector<IdString>{id_CLK1, id_CLK2}
                                                                : std::vector<IdString>{id_CLK1};
            for (IdString port : ports) {
                NetInfo *existing = ci->getPort(port);
                if (existing == clk)
                    continue;
                if (existing != nullptr)
                    ci->disconnectPort(port);
                if (!ci->ports.count(port))
                    ci->addInput(port);
                ci->pin_data[port].state = PIN_SIG;
                ci->connectPort(port, clk);
            }
        }
    }
    assignArchInfo();
    fes_fence_active = true;
    for (auto &item : ctx->cells) {
        CellInfo *ci = item.second.get();
        if (ci->type != id_MISTRAL_M10K || !fes_cell_is_slot(ci))
            continue;
        for (auto &port : ci->ports) {
            const std::string pname = port.first.c_str(ctx);
            if (pname.compare(0, 6, "B1ADDR") != 0 || port.second.net == nullptr)
                continue;
            log_info("FES %s.%s <= %s\n", ci->name.c_str(ctx), pname.c_str(),
                     port.second.net->name.c_str(ctx));
        }
    }
    log_info("FES cart merged %d cells from '%s' (ib=%d ob=%d missed=%d).\n", added, filename.c_str(),
             mapped_ib, mapped_ob, missed_ib);
}

void Arch::fes_constrain_slot_region()
{
    Context *ctx = getCtx();
    if (!fes_has_reserved_rect || fes_bel_region.empty())
        return;
    // Bucket every FES_SLOT cell by its declared region name. Two carts
    // merged onto two disjoint FES_RESERVED_RECT regions (for example a
    // cartridge slot and an expansion slot on the same shell) each get their
    // own Region and capacity report; fes_placement_allowed() is the hard
    // gate that keeps a region's cells off another region's BELs.
    dict<IdString, std::vector<CellInfo *>> by_region;
    for (auto &item : ctx->cells) {
        CellInfo *ci = item.second.get();
        if (ci->isPseudo())
            continue;
        IdString region_name = fes_cell_slot_region(ci);
        if (region_name != IdString())
            by_region[region_name].push_back(ci);
    }
    if (by_region.empty())
        return;
    // The placers only search near a cell's analytic or random location.
    // Without a Region they sample the whole chip, of which a socket is a
    // tiny fraction, so legalisation degenerates into chip-wide random
    // probing. fes_placement_allowed() remains the hard gate.
    for (auto &entry : by_region) {
        IdString region_name = entry.first;
        std::vector<CellInfo *> &slot_cells = entry.second;
        if (!fes_region_bels.count(region_name))
            log_error("FES cart cell(s) claim undeclared FES_SLOT region '%s'; declare it with FES_RESERVED_RECT "
                      "\"%s x0 y0 x1 y1\".\n",
                      region_name.c_str(ctx), region_name.c_str(ctx));
        IdString region_id = id("$FES_SLOT_" + region_name.str(ctx));
        if (!ctx->region.count(region_id)) {
            std::unique_ptr<Region> region(new Region());
            region->name = region_id;
            region->constr_bels = true;
            for (BelId bel : fes_region_bels.at(region_name))
                region->bels.insert(bel);
            ctx->region[region_id] = std::move(region);
        }
        Region *region = ctx->region.at(region_id).get();
        for (CellInfo *ci : slot_cells)
            ci->region = region;
        log_info("FES slot region '%s' constrains %zu cart cells to %zu reserved BELs.\n", region_name.c_str(ctx),
                 slot_cells.size(), fes_region_bels.at(region_name).size());
        fes_any_slot_region_active = true;
        fes_report_slot_capacity(region_name, slot_cells);
    }
}

// Static capacity check of the cart against the usable part of the reserved
// rectangle. Every bound here is necessary, not sufficient: a cart that
// passes may still fail detailed legalisation, but a cart that fails cannot
// be placed by any placer, so the run stops with the failing figure instead
// of legalising for minutes.
void Arch::fes_report_slot_capacity(IdString region_name, const std::vector<CellInfo *> &slot_cells) const
{
    const Context *ctx = getCtx();
    const auto &region_bels = fes_region_bels.at(region_name);
    // Usable LABs: reserved to this region, and not holding a frozen shell
    // cell. A LAB with any locked non-slot occupant is excluded wholesale by
    // fes_placement_allowed() because its control set is immutable.
    int usable_labs = 0, frozen_labs = 0;
    std::map<int, std::vector<int>> usable_rows;
    for (const auto &lab : labs) {
        BelId first = lab.alms[0].lut_bels[0];
        if (!region_bels.count(first))
            continue;
        bool frozen = false;
        for (const auto &alm : lab.alms) {
            for (BelId other : {alm.lut_bels[0], alm.lut_bels[1], alm.ff_bels[0], alm.ff_bels[1], alm.ff_bels[2],
                                alm.ff_bels[3]}) {
                const CellInfo *occupant = getBoundBelCell(other);
                if (occupant && !fes_cell_is_slot(occupant) && occupant->belStrength >= STRENGTH_LOCKED)
                    frozen = true;
            }
        }
        if (frozen) {
            ++frozen_labs;
            continue;
        }
        ++usable_labs;
        Loc loc = getBelLocation(first);
        usable_rows[loc.x].push_back(loc.y);
    }
    int max_run = 0;
    for (auto &column : usable_rows) {
        std::sort(column.second.begin(), column.second.end());
        int run = 0, prev = std::numeric_limits<int>::min();
        for (int y : column.second) {
            run = (y == prev + 1) ? run + 1 : 1;
            prev = y;
            max_run = std::max(max_run, run);
        }
    }
    // Other reserved BELs (M10K, DSP, ...) that are free or already hold a
    // slot cell of this same region.
    dict<IdString, int> other_bels;
    for (BelId bel : region_bels) {
        IdString type = getBelType(bel);
        if (type.in(id_MISTRAL_COMB, id_MISTRAL_MCOMB, id_MISTRAL_FF))
            continue;
        const CellInfo *occupant = getBoundBelCell(bel);
        if (occupant && fes_cell_slot_region(occupant) != region_name)
            continue;
        other_bels[getBelBucketForBel(bel)]++;
    }

    int comb_cells = 0, ff_cells = 0, chains = 0, chain_rows = 0, ff_fabric = 0;
    int lut_inputs = 0, pairable_luts = 0, sdata_inputs = 0;
    dict<IdString, int> other_cells;
    bool clock_global = true;
    // FF control-set lower bound. A LAB has one SCLR, one SLOAD, two ACLR and
    // three ENA selectors, but ENA, SCLR, ACLR and a non-global CLK share four
    // LAB DATAIN lines (lab.cc LabCtrlSetWorker), so a LAB with an SCLR keeps
    // only two ENA lines (one if its clock is not global).
    struct Group
    {
        int ffs = 0;
        std::set<const NetInfo *> enas;
    };
    std::map<const NetInfo *, Group> sclr_groups;
    for (CellInfo *ci : slot_cells) {
        BelBucketId bucket = getBelBucketForCellType(ci->type);
        if (bucket == id_MISTRAL_COMB) {
            ++comb_cells;
            // Same accounting as update_alm_input_count: used inputs less
            // those shared with the previous carry cell.
            lut_inputs += std::max(0, ci->combInfo.used_lut_input_count - ci->combInfo.chain_shared_input_count);
            if (ci->type != id_MISTRAL_ALUT_ARITH && ci->type != id_MISTRAL_MLAB)
                ++pairable_luts;
        } else if (bucket == id_MISTRAL_FF) {
            ++ff_cells;
            const auto &cs = ci->ffInfo.ctrlset;
            if (cs.clk.net != nullptr && !cs.clk.net->is_global)
                clock_global = false;
            Group &group = sclr_groups[cs.sclr.net];
            ++group.ffs;
            // An enable equal to the LAB's SCLR or (non-global) clock signal
            // shares that DATAIN line in LabCtrlSetWorker and costs no slot.
            const bool shares_sclr = cs.ena == cs.sclr;
            const bool shares_clk = cs.clk.net != nullptr && !cs.clk.net->is_global && cs.ena == cs.clk;
            if (cs.ena.net != nullptr && !shares_sclr && !shares_clk)
                group.enas.insert(cs.ena.net);
            // Needs a route-through LUT half or the E/F input unless it is
            // paired with the LUT that drives it (pair_unbound_lut_ffs).
            const NetInfo *datain = ci->getPort(id_DATAIN);
            const CellInfo *driver = datain ? datain->driver.cell : nullptr;
            const bool paired = driver != nullptr && ci->cluster != ClusterId() && ci->cluster == driver->name;
            if (!paired)
                ++ff_fabric;
            if (ci->ffInfo.sdata)
                ++sdata_inputs;
        } else {
            other_cells[bucket]++;
        }
        // Every chain root, including a one-cell chain, is pinned to z=0 of
        // some LAB by constrain_carries and so consumes a LAB root.
        if (ci->type == id_MISTRAL_ALUT_ARITH && ci->cluster == ci->name) {
            ++chains;
            int rows = 1;
            for (const CellInfo *child : ci->constr_children)
                rows = std::max(rows, 1 - child->constr_y);
            chain_rows = std::max(chain_rows, rows);
        }
    }
    const int ena_with_sclr = clock_global ? 2 : 1;
    const int ena_without_sclr = clock_global ? 3 : 2;
    int ctrl_labs = 0;
    for (const auto &group : sclr_groups) {
        const int slots = group.first != nullptr ? ena_with_sclr : ena_without_sclr;
        const int by_count = (group.second.ffs + 19) / 20;
        const int by_ena = (int(group.second.enas.size()) + slots - 1) / slots;
        ctrl_labs += group.first != nullptr ? std::max(by_count, by_ena) : 0;
    }
    // FF BELs 1 and 3 of each ALM are rejected by is_alm_legal, so 20 per LAB.
    const int comb_bels = 20 * usable_labs, ff_bels = 20 * usable_labs;
    // check_lab_input_count admits 42 unique ALM inputs per LAB. Two LUTs in
    // one ALM may share at most two inputs, so the best case shares two per
    // pair of non-arithmetic LUTs; unpaired FF data and SDATA add one each.
    const int min_lab_inputs = std::max(0, lut_inputs - 2 * (pairable_luts / 2)) + ff_fabric + sdata_inputs;
    const int input_labs = (min_lab_inputs + 41) / 42;
    log_info("FES slot capacity (region '%s'): %d usable LABs (%d frozen), longest vertical run %d.\n",
             region_name.c_str(ctx), usable_labs, frozen_labs, max_run);
    log_info("FES slot capacity: comb %d/%d, FF %d/%d (%d unpaired need a route-through or E/F input; %d free LUT "
             "halves), carry chains %d (longest %d LAB rows).\n",
             comb_cells, comb_bels, ff_cells, ff_bels, ff_fabric, comb_bels - comb_cells, chains, chain_rows);
    log_info("FES slot capacity: %zu FF control-set groups by SCLR need at least %d LABs (clock %s, %d ENA per SCLR "
             "LAB).\n",
             sclr_groups.size(), ctrl_labs, clock_global ? "global" : "not global", ena_with_sclr);
    log_info("FES slot capacity: LAB inputs need at least %d LABs at 42 unique inputs each (%d LUT inputs, best-case "
             "%d shared, %d FF fabric inputs).\n",
             input_labs, lut_inputs, 2 * (pairable_luts / 2), ff_fabric + sdata_inputs);
    for (const auto &item : other_cells)
        log_info("FES slot capacity: %s %d/%d.\n", item.first.c_str(ctx), item.second,
                 other_bels.count(item.first) ? other_bels.at(item.first) : 0);

    std::vector<std::string> failures;
    if (comb_cells > comb_bels)
        failures.push_back(stringf("%d combinational cells exceed %d usable COMB BELs", comb_cells, comb_bels));
    if (ff_cells > ff_bels)
        failures.push_back(stringf("%d flip-flops exceed %d usable FF BELs", ff_cells, ff_bels));
    if (ctrl_labs > usable_labs)
        failures.push_back(
                stringf("FF control sets need at least %d LABs but %d are usable", ctrl_labs, usable_labs));
    if (input_labs > usable_labs)
        failures.push_back(stringf("LAB input bandwidth needs at least %d LABs but %d are usable", input_labs,
                                   usable_labs));
    if (chains > usable_labs)
        failures.push_back(stringf("%d carry chains exceed %d usable LAB roots", chains, usable_labs));
    if (chain_rows > max_run)
        failures.push_back(stringf("a carry chain spans %d LAB rows but the longest usable column run is %d",
                                   chain_rows, max_run));
    for (const auto &item : other_cells) {
        const int avail = other_bels.count(item.first) ? other_bels.at(item.first) : 0;
        if (item.second > avail)
            failures.push_back(stringf("%d %s cells exceed %d reserved BELs", item.second, item.first.c_str(ctx),
                                       avail));
    }
    if (failures.empty())
        return;
    for (const auto &failure : failures)
        log_nonfatal_error("FES slot capacity (region '%s'): %s.\n", region_name.c_str(ctx), failure.c_str());
    log_error("FES cart does not fit region '%s'; enlarge its FES_RESERVED_RECT or reduce the cart.\n",
              region_name.c_str(ctx));
}

NEXTPNR_NAMESPACE_END
