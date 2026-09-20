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

#include <fstream>
#include <iterator>
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
        if (ci->attrs.count(ctx->id("FES_SLOT")) && ci->attrs.at(ctx->id("FES_SLOT")).as_bool())
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

bool Arch::fes_cell_is_slot(const CellInfo *cell) const
{
    if (cell == nullptr || !cell->attrs.count(id("FES_SLOT")))
        return false;
    return cell->attrs.at(id("FES_SLOT")).as_bool();
}

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

void Arch::lock_fes_scaffold()
{
    Context *ctx = getCtx();
    // Rip is optional; mixed USER/WEAK trees make GPU router1 check abort.
    // fes_rip_reserved_shell_pips();
    if (fes_has_reserved_rect) {
        int m10ks = 0;
        for (BelId bel : getBels()) {
            if (getBelType(bel) != id_MISTRAL_M10K)
                continue;
            Loc loc = getBelLocation(bel);
            if (loc.x < fes_rect_x0 || loc.x > fes_rect_x1 || loc.y < fes_rect_y0 || loc.y > fes_rect_y1)
                continue;
            if (m10ks < 16)
                log_info("FES reserved M10K %s\n", getBelName(bel).str(getCtx()).c_str());
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
        ++locked_cells;
        for (auto &port : ci->ports) {
            NetInfo *net = port.second.net;
            if (net == nullptr)
                continue;
            std::vector<IdString> found;
            for (IdString bp : getBelPins(ci->bel)) {
                WireId w = getBelPinWire(ci->bel, bp);
                if (w != WireId() && getBoundWireNet(w) == net)
                    found.push_back(bp);
            }
            if (!found.empty())
                ci->pin_data[port.first].bel_pins = std::move(found);
        }
    }
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
    return loc.x >= fes_rect_x0 && loc.x <= fes_rect_x1 && loc.y >= fes_rect_y0 && loc.y <= fes_rect_y1;
}

bool Arch::fes_pip_in_plug_halo(PipId pip) const
{
    if (!fes_has_reserved_rect)
        return false;
    Loc loc = getPipLocation(pip);
    return loc.x >= fes_rect_x0 - 6 && loc.x <= fes_rect_x1 + 6 && loc.y >= fes_rect_y0 - 6 &&
           loc.y <= fes_rect_y1 + 6;
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

void Arch::merge_fes_cart(const std::string &filename)
{
    Context *ctx = getCtx();
    log_info("FES parsing cart JSON '%s'...\n", filename.c_str());
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
            dst->attrs[id("FES_SLOT")] = Property(1);

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

NEXTPNR_NAMESPACE_END
