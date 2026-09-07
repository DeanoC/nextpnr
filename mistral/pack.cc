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

#include "design_utils.h"
#include "dsp.h"
#include "log.h"
#include "nextpnr.h"
#include "pll.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {

bool is_dsp_multiplier(IdString type)
{
    return type.in(id_MISTRAL_MUL9X9, id_MISTRAL_MUL18X18, id_MISTRAL_MUL27X27);
}

bool dsp_bool_param(const dict<IdString, Property> &params, IdString key, bool def = false)
{
    auto it = params.find(key);
    if (it == params.end())
        return def;
    if (!it->second.is_string)
        return it->second.as_bool();
    const std::string &value = it->second.as_string();
    if (value == "1" || value == "true" || value == "TRUE" || value == "on" || value == "reg" ||
        value == "registered")
        return true;
    if (value == "0" || value == "false" || value == "FALSE" || value == "off" || value == "bypass")
        return false;
    log_error("DSP parameter expects a boolean or register/bypass value, got '%s'.\n", value.c_str());
    return def;
}

bool dsp_reg_param(const dict<IdString, Property> &params, IdString key)
{
    return dsp_bool_param(params, key, false);
}

bool dsp_shared_control_nets_equal(const CellInfo *a, const CellInfo *b)
{
    for (IdString port : {id_CLK, id_ACLR, id_ENA, id_ACCUMULATE, id_SUB, id_NEGATE, id_LOADCONST}) {
        const NetInfo *an = a->getPort(port);
        const NetInfo *bn = b->getPort(port);
        // A hard constant has no net after packing, so compare both the net
        // identity and the retained pin state. Otherwise two M9 lanes tied
        // to opposite constants would be incorrectly clustered and the
        // bitstream would apply the first lane's shared setting to both.
        if (an != bn || a->get_pin_state(port) != b->get_pin_state(port))
            return false;
    }
    return true;
}

bool dsp_control_used(const CellInfo *cell, IdString port)
{
    if (cell->getPort(port) != nullptr)
        return true;
    auto state = cell->get_pin_state(port);
    return state == PIN_1 || state == PIN_INV;
}

bool dsp_has_bus(const CellInfo *cell, const BaseCtx *ctx, const char *base)
{
    std::string prefix = std::string(base) + "[";
    for (const auto &port : cell->ports)
        if (port.first.str(ctx).find(prefix) == 0)
            return true;
    return false;
}

bool dsp_shared_config_equal(const CellInfo *a, const CellInfo *b)
{
    if (dsp_bool_param(a->params, id_A_SIGNED, true) != dsp_bool_param(b->params, id_A_SIGNED, true) ||
        dsp_bool_param(a->params, id_B_SIGNED, true) != dsp_bool_param(b->params, id_B_SIGNED, true))
        return false;
    for (IdString key : {id_INREG_CTRL_AX, id_INREG_CTRL_AY, id_INREG_CTRL_AZ, id_INREG_CTRL_BX,
                         id_INREG_CTRL_BY, id_INREG_CTRL_BZ, id_OREG_CTRL, id_PREADDER_EN, id_PREADDER_SUB,
                         id_CASCADE_EN, id_CASCADE_1ST_EN, id_CHAIN_OUTPUT_EN}) {
        if (dsp_bool_param(a->params, key) != dsp_bool_param(b->params, key))
            return false;
    }
    return dsp_shared_control_nets_equal(a, b);
}

bool is_supported_dsp_param(IdString key)
{
    return key.in(id_A_SIGNED, id_B_SIGNED, id_INREG_CTRL_AX, id_INREG_CTRL_AY, id_INREG_CTRL_AZ,
                  id_INREG_CTRL_BX, id_INREG_CTRL_BY, id_INREG_CTRL_BZ, id_OREG_CTRL, id_PREADDER_EN,
                  id_PREADDER_SUB, id_CASCADE_EN, id_CASCADE_1ST_EN, id_CHAIN_OUTPUT_EN);
}

struct MistralPacker
{
    MistralPacker(Context *ctx) : ctx(ctx) {};
    Context *ctx;

    NetInfo *gnd_net, *vcc_net;

    void init_constant_nets()
    {
        CellInfo *gnd_drv = ctx->createCell(ctx->id("$PACKER_GND_DRV"), id_MISTRAL_CONST);
        gnd_drv->params[id_LUT] = 0;
        gnd_drv->addOutput(id_Q);
        CellInfo *vcc_drv = ctx->createCell(ctx->id("$PACKER_VCC_DRV"), id_MISTRAL_CONST);
        vcc_drv->params[id_LUT] = 1;
        vcc_drv->addOutput(id_Q);
        gnd_net = ctx->createNet(ctx->id("$PACKER_GND_NET"));
        vcc_net = ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        gnd_drv->connectPort(id_Q, gnd_net);
        vcc_drv->connectPort(id_Q, vcc_net);
    }

    CellPinState get_pin_needed_muxval(CellInfo *cell, IdString port)
    {
        NetInfo *net = cell->getPort(port);
        if (net == nullptr || net->driver.cell == nullptr) {
            // Pin is disconnected
            // If a mux value exists already, honour it
            CellPinState exist_mux = cell->get_pin_state(port);
            if (exist_mux != PIN_SIG)
                return exist_mux;
            // Otherwise, look up the default value and use that
            CellPinStyle pin_style = ctx->get_cell_pin_style(cell, port);
            if ((pin_style & PINDEF_MASK) == PINDEF_0)
                return PIN_0;
            else if ((pin_style & PINDEF_MASK) == PINDEF_1)
                return PIN_1;
            else
                return PIN_SIG;
        }
        // Look to see if the driver is an inverter or constant
        IdString drv_type = net->driver.cell->type;
        if (drv_type == id_MISTRAL_NOT)
            return PIN_INV;
        else if (drv_type == id_GND)
            return PIN_0;
        else if (drv_type == id_VCC)
            return PIN_1;
        else
            return PIN_SIG;
    }

    void uninvert_port(CellInfo *cell, IdString port)
    {
        // Rewire a port so it is driven by the input to an inverter
        NetInfo *net = cell->getPort(port);
        NPNR_ASSERT(net != nullptr && net->driver.cell != nullptr && net->driver.cell->type == id_MISTRAL_NOT);
        CellInfo *inv = net->driver.cell;
        cell->disconnectPort(port);

        NetInfo *inv_a = inv->getPort(id_A);
        if (inv_a != nullptr) {
            cell->connectPort(port, inv_a);
        }
    }

    void process_inv_constants(CellInfo *cell)
    {
        // Fold inverters and constants into a cell
        for (auto &port : cell->ports) {
            // Iterate over all inputs
            if (port.second.type != PORT_IN)
                continue;
            IdString port_name = port.first;

            CellPinState req_mux = get_pin_needed_muxval(cell, port_name);
            if (req_mux == PIN_SIG) {
                // No special setting required, ignore
                continue;
            }

            CellPinStyle pin_style = ctx->get_cell_pin_style(cell, port_name);

            if (req_mux == PIN_INV) {
                // Pin is inverted. If there is a hard inverter; then use it
                if (pin_style & PINOPT_INV) {
                    uninvert_port(cell, port_name);
                    cell->pin_data[port_name].state = PIN_INV;
                }
            } else if (req_mux == PIN_0 || req_mux == PIN_1) {
                // Pin is tied to a constant
                // If there is a hard constant option; use it
                if ((pin_style & int(req_mux)) == req_mux) {
                    cell->disconnectPort(port_name);
                    cell->pin_data[port_name].state = req_mux;
                } else {
                    cell->disconnectPort(port_name);
                    // There is no hard constant, we need to connect it to the relevant soft-constant net
                    cell->connectPort(port_name, (req_mux == PIN_1) ? vcc_net : gnd_net);
                }
            }
        }
    }

    void ensure_dsp_control_ports()
    {
        // The Yosys DSP cells intentionally contain only the controls used by
        // the RTL. The physical block still needs explicit defaults on its
        // control inputs, so materialise the omitted inputs before constant
        // folding. This gives bitstream generation a PIN_0/PIN_1 state to
        // encode instead of silently leaving the hardware default selected.
        const std::array<IdString, 7> controls{
                id_CLK, id_ACLR, id_ENA, id_ACCUMULATE, id_SUB, id_NEGATE, id_LOADCONST};
        for (auto &entry : ctx->cells) {
            CellInfo *cell = entry.second.get();
            if (!is_dsp_multiplier(cell->type))
                continue;
            for (IdString control : controls)
                if (!cell->ports.count(control))
                    cell->addInput(control);
        }
    }

    bool is_global_dsp_clock(const NetInfo *net) const
    {
        // MISTRAL_CLKBUF.Q is the output of the dedicated global clock path.
        // Any other source (including an HPS/GPIO signal) needs the DSP's
        // fabric CLKIN alternative.
        return net != nullptr && net->driver.cell != nullptr &&
               net->driver.cell->type.in(id_MISTRAL_CLKBUF, id_MISTRAL_CLKENA) && net->driver.port == id_Q;
    }

    void select_dsp_control_pinmaps()
    {
        const IdString clk_fabric = ctx->id("CLK_FABRIC");
        const IdString aclr_fabric = ctx->id("ACLR_FABRIC");
        for (auto &entry : ctx->cells) {
            CellInfo *cell = entry.second.get();
            if (!is_dsp_multiplier(cell->type))
                continue;

            NetInfo *clk = cell->getPort(id_CLK);
            if (clk != nullptr && !is_global_dsp_clock(clk))
                cell->pin_data[id_CLK].bel_pins = {clk_fabric};

            NetInfo *aclr = cell->getPort(id_ACLR);
            if (aclr != nullptr && !is_global_dsp_clock(aclr))
                cell->pin_data[id_ACLR].bel_pins = {aclr_fabric};
        }
    }

    void trim_design()
    {
        // Remove unused inverters and high/low drivers
        std::vector<IdString> trim_cells;
        std::vector<IdString> trim_nets;
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_MISTRAL_NOT && ci->type != id_GND && ci->type != id_VCC)
                continue;
            IdString port = (ci->type == id_MISTRAL_NOT) ? id_Q : id_Y;
            NetInfo *out = ci->getPort(port);
            if (out == nullptr) {
                trim_cells.push_back(ci->name);
                continue;
            }
            if (!out->users.empty())
                continue;

            ci->disconnectPort(id_A);

            trim_cells.push_back(ci->name);
            trim_nets.push_back(out->name);
        }

        for (IdString rem_net : trim_nets)
            ctx->nets.erase(rem_net);
        for (IdString rem_cell : trim_cells)
            ctx->cells.erase(rem_cell);
    }

    void pack_constants()
    {
        // Iterate through cells
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            // Skip certain cells at this point
            if (ci->type != id_MISTRAL_NOT && ci->type != id_GND && ci->type != id_VCC)
                process_inv_constants(ci);
        }
        // Special case - SDATA can only be trimmed if SLOAD is low
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_MISTRAL_FF)
                continue;
            if (ci->get_pin_state(id_SLOAD) != PIN_0)
                continue;
            ci->disconnectPort(id_SDATA);
        }
        // Remove superfluous inverters and constant drivers
        trim_design();
    }

    void prepare_io()
    {
        // Find the actual IO buffer corresponding to a port; and copy attributes across to it
        // Note that this relies on Yosys to do IO buffer inference, to avoid tristate issues once we get to synthesised
        // JSON. In all cases the nextpnr-inserted IO buffers are removed as redundant.
        for (auto &port : ctx->ports) {
            if (!ctx->cells.count(port.first))
                log_error("Port '%s' doesn't seem to have a corresponding top level IO\n", ctx->nameOf(port.first));
            CellInfo *ci = ctx->cells.at(port.first).get();

            PortRef top_port;
            top_port.cell = nullptr;
            bool is_npnr_iob = false;

            if (ci->type == ctx->id("$nextpnr_ibuf") || ci->type == ctx->id("$nextpnr_iobuf")) {
                // Might have an input buffer (IB etc) connected to it
                is_npnr_iob = true;
                NetInfo *o = ci->getPort(id_O);
                if (o == nullptr)
                    ;
                else if (o->users.entries() > 1)
                    log_error("Top level pin '%s' has multiple input buffers\n", ctx->nameOf(port.first));
                else if (o->users.entries() == 1)
                    top_port = *o->users.begin();
            }
            if (ci->type == ctx->id("$nextpnr_obuf") || ci->type == ctx->id("$nextpnr_iobuf")) {
                // Might have an output buffer (OB etc) connected to it
                is_npnr_iob = true;
                NetInfo *i = ci->getPort(id_I);
                if (i != nullptr && i->driver.cell != nullptr) {
                    if (top_port.cell != nullptr)
                        log_error("Top level pin '%s' has multiple input/output buffers\n", ctx->nameOf(port.first));
                    top_port = i->driver;
                }
                // Edge case of a bidirectional buffer driving an output pin
                if (i->users.entries() > 2) {
                    log_error("Top level pin '%s' has illegal buffer configuration\n", ctx->nameOf(port.first));
                } else if (i->users.entries() == 2) {
                    if (top_port.cell != nullptr)
                        log_error("Top level pin '%s' has illegal buffer configuration\n", ctx->nameOf(port.first));
                    for (auto &usr : i->users) {
                        if (usr.cell->type == ctx->id("$nextpnr_obuf") || usr.cell->type == ctx->id("$nextpnr_iobuf"))
                            continue;
                        top_port = usr;
                        break;
                    }
                }
            }
            if (!is_npnr_iob)
                log_error("Port '%s' doesn't seem to have a corresponding top level IO (internal cell type mismatch)\n",
                          ctx->nameOf(port.first));

            if (top_port.cell == nullptr) {
                log_info("Trimming port '%s' as it is unused.\n", ctx->nameOf(port.first));
            } else {
                // Copy attributes to real IO buffer
                if (ctx->io_attr.count(port.first)) {
                    for (auto &kv : ctx->io_attr.at(port.first)) {
                        top_port.cell->attrs[kv.first] = kv.second;
                    }
                }
                // Make sure that top level net is set correctly
                port.second.net = top_port.cell->ports.at(top_port.port).net;
            }
            // Now remove the nextpnr-inserted buffer
            ci->disconnectPort(id_I);
            ci->disconnectPort(id_O);
            ctx->cells.erase(port.first);
        }
    }

    void pack_io()
    {
        // Step 0: deal with top level inserted IO buffers
        prepare_io();
        // Stage 1: apply constraints
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            // Iterate through all IO buffer primitives
            if (!ctx->is_io_cell(ci->type))
                continue;
            // We need all IO constrained at the moment, unconstrained IO are rare enough not to care
            if (!ci->attrs.count(id_LOC))
                log_error("Found unconstrained IO '%s', these are currently unsupported\n", ctx->nameOf(ci));
            // Convert package pin constraint to bel constraint
            std::string loc = ci->attrs.at(id_LOC).as_string();
            if (loc.compare(0, 4, "PIN_") != 0)
                log_error("Expecting PIN_-prefixed pin for IO '%s', got '%s'\n", ctx->nameOf(ci), loc.c_str());
            auto pin_info = ctx->cyclonev->pin_find_name(loc.substr(4));
            if (pin_info == nullptr)
                log_error("IO '%s' is constrained to invalid pin '%s'\n", ctx->nameOf(ci), loc.c_str());
            BelId bel = ctx->get_io_pin_bel(pin_info);

            if (bel == BelId()) {
                log_error("IO '%s' is constrained to pin %s which is not a supported IO pin.\n", ctx->nameOf(ci),
                          loc.c_str());
            } else {
                log_info("Constraining IO '%s' to pin %s (bel %s)\n", ctx->nameOf(ci), loc.c_str(),
                         ctx->nameOfBel(bel));
                ctx->bindBel(bel, ci, STRENGTH_LOCKED);
            }
        }
    }

    void constrain_carries()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_MISTRAL_ALUT_ARITH)
                continue;
            const NetInfo *cin = ci->getPort(id_CI);
            if (cin != nullptr && cin->driver.cell != nullptr)
                continue; // not the start of a chain
            std::vector<CellInfo *> chain;
            CellInfo *cursor = ci;
            while (true) {
                chain.push_back(cursor);
                const NetInfo *co = cursor->getPort(id_CO);
                if (co == nullptr || co->users.empty())
                    break;
                if (co->users.entries() > 1)
                    log_error("Carry net %s has more than one sink!\n", ctx->nameOf(co));
                auto &usr = *co->users.begin();
                if (usr.port != id_CI)
                    log_error("Carry net %s drives port %s, expected CI\n", ctx->nameOf(co), ctx->nameOf(usr.port));
                cursor = usr.cell;
            }

            chain.at(0)->constr_abs_z = true;
            chain.at(0)->constr_z = 0;
            chain.at(0)->cluster = chain.at(0)->name;

            for (int i = 1; i < int(chain.size()); i++) {
                chain.at(i)->constr_x = 0;
                chain.at(i)->constr_y = -(i / 20);
                // 2 COMB, 4 FF per ALM
                chain.at(i)->constr_z = ((i / 2) % 10) * 6 + (i % 2);
                chain.at(i)->constr_abs_z = true;
                chain.at(i)->cluster = chain.at(0)->name;
                chain.at(0)->constr_children.push_back(chain.at(i));
            }

            if (ctx->debug) {
                log_info("Chain: \n");
                for (int i = 0; i < int(chain.size()); i++) {
                    auto &c = chain.at(i);
                    log_info("    i=%d cell=%s dy=%d z=%d ci=%s co=%s\n", i, ctx->nameOf(c), c->constr_y, c->constr_z,
                             ctx->nameOf(c->getPort(id_CI)), ctx->nameOf(c->getPort(id_CO)));
                }
            }
        }
        // Check we reached all the cells in the above pass
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_MISTRAL_ALUT_ARITH)
                continue;
            if (ci->cluster == ClusterId())
                log_error("Failed to include arith cell '%s' in any chain (CI=%s)\n", ctx->nameOf(ci),
                          ctx->nameOf(ci->getPort(id_CI)));
        }
    }

    void constrain_lutram()
    {
        // We form clusters based on both read and write address; as both being the same makes it more likely these
        // cells should be packed together, too.
        // This makes things easier for the placement legaliser to deal with RAM in LAB-compatible blocks without
        // over-constraining things
        idict<dict<IdString, IdString>> mlab_keys;
        std::vector<std::vector<CellInfo *>> mlab_groups;
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_MISTRAL_MLAB)
                continue;
            auto key = ctx->get_mlab_key(ci, true);
            int key_idx = mlab_keys(key);
            if (key_idx >= int(mlab_groups.size()))
                mlab_groups.resize(key_idx + 1);
            mlab_groups.at(key_idx).push_back(ci);
        }
        // Combine into clusters
        size_t cluster_size = 20;
        for (auto &group : mlab_groups) {
            for (size_t i = 0; i < group.size(); i++) {
                CellInfo *ci = group.at(i);
                CellInfo *base = group.at((i / cluster_size) * cluster_size);
                int cell_index = int(i) % cluster_size;
                int alm = cell_index / 2;
                int alm_cell = cell_index % 2;
                ci->cluster = base->name;
                ci->constr_abs_z = true;
                ci->constr_z = alm * 6 + alm_cell;
                if (cell_index != 0) {
                    // Not the root of a cluster
                    base->constr_children.push_back(ci);
                    ci->constr_x = 0;
                    ci->constr_y = 0;
                }
            }
        }
    }

    void setup_m10ks()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type != id_MISTRAL_M10K)
                continue;

            auto abits = ci->params.at(id_CFG_ABITS).as_int64();
            auto dbits = ci->params.at(id_CFG_DBITS).as_int64();
            NPNR_ASSERT(abits >= 7 && abits <= 13);
            NPNR_ASSERT(dbits == 1 || dbits == 2 || dbits == 5 || dbits == 10 || dbits == 20 || dbits == 40);
            NPNR_ASSERT((1 << abits) * dbits <= 10240);

            log_info("Setting up %ld-bit address, %ld-bit data M10K for %s.\n", abits, dbits,
                     ci->name.str(ctx).c_str());

            // Quartus doesn't seem to generate ADDRSTALL[AB], BYTEENABLE[AB][01].

            // It *does* generate ACLR[01] but leaves them unconnected if unused.

            // Enables.
            // RDEN[1] is left unconnected.
            if (dbits == 40)
                ci->pin_data[ctx->id("A1EN")].bel_pins = {ctx->id("WREN[0]")};
            else
                ci->pin_data[ctx->id("A1EN")].bel_pins = {ctx->id("WREN[1]")};
            ci->pin_data[ctx->id("B1EN")].bel_pins = {ctx->id("RDEN[0]")};

            // Clocks.
            ci->pin_data[ctx->id("CLK1")].bel_pins = {ctx->id("CLKIN[0]")};

            // Enables left unconnected.

            // Address lines.

            // One could remove the std::max here and the `- bit_offset`s here,
            // because they would cancel out, but I think this way is less confusing.
            int addr_offset = std::max(12 - std::max(abits, dbits == 40 ? 8L : 9L), 0L);
            int bit_offset = (abits == 13);
            if (abits == 13) {
                ci->pin_data[ctx->id("A1ADDR[0]")].bel_pins = {ctx->id("DATAAIN[4]")};
                ci->pin_data[ctx->id("B1ADDR[0]")].bel_pins = {ctx->id("DATABIN[19]")};
            }
            for (int bit = bit_offset; bit < abits; bit++) {
                ci->pin_data[ctx->idf("A1ADDR[%d]", bit)].bel_pins = {
                        ctx->idf("ADDRA[%d]", bit + addr_offset - bit_offset)};
                ci->pin_data[ctx->idf("B1ADDR[%d]", bit)].bel_pins = {
                        ctx->idf("ADDRB[%d]", bit + addr_offset - bit_offset)};
            }

            // Data lines
            std::vector<int> offsets;
            offsets.push_back(0);
            if (abits >= 10 && dbits <= 10) {
                offsets.push_back(10);
            }
            if (abits >= 11 && dbits <= 5) {
                offsets.push_back(5);
                offsets.push_back(15);
            }
            if (abits >= 12 && dbits <= 2) {
                offsets.push_back(2);
                offsets.push_back(7);
                offsets.push_back(12);
                offsets.push_back(17);
            }
            if (abits == 13 && dbits == 1) {
                offsets.push_back(1);
                offsets.push_back(3);
                offsets.push_back(6);
                offsets.push_back(8);
                offsets.push_back(11);
                offsets.push_back(13);
                offsets.push_back(16);
                offsets.push_back(18);
            }

            // In this corner case the pin name does not have indexing
            // because it's a single bit wide...
            if (abits == 13 && dbits == 1) {
                for (int offset : offsets)
                    ci->pin_data[ctx->idf("A1DATA")].bel_pins.push_back(ctx->idf("DATAAIN[%d]", offset));
                ci->pin_data[ctx->idf("B1DATA")].bel_pins = {ctx->idf("DATABOUT[0]")};
                continue;
            }

            // 40-bit data mode causes some headaches...
            bit_offset = dbits == 40 ? 20 : 0;

            // Write port
            for (int bit = 0; bit < std::min(dbits, 20L); bit++)
                for (int offset : offsets)
                    ci->pin_data[ctx->idf("A1DATA[%d]", bit)].bel_pins.push_back(ctx->idf("DATAAIN[%d]", bit + offset));

            if (dbits == 40)
                for (int bit = bit_offset; bit < dbits; bit++)
                    ci->pin_data[ctx->idf("A1DATA[%d]", bit)].bel_pins.push_back(
                            ctx->idf("DATABIN[%d]", bit - bit_offset));

            // Read port
            if (dbits == 40)
                for (int bit = 0; bit < 20; bit++)
                    ci->pin_data[ctx->idf("B1DATA[%d]", bit)].bel_pins = {ctx->idf("DATAAOUT[%d]", bit)};

            for (int bit = bit_offset; bit < dbits; bit++)
                ci->pin_data[ctx->idf("B1DATA[%d]", bit)].bel_pins = {ctx->idf("DATABOUT[%d]", bit - bit_offset)};
        }
    }

    void constrain_dsps()
    {
        std::vector<CellInfo *> multipliers;
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (is_dsp_multiplier(ci->type)) {
                for (auto &param : ci->params) {
                    if (!is_supported_dsp_param(param.first))
                        log_error("DSP cell '%s' has unsupported parameter '%s'.\n", ctx->nameOf(ci),
                                  ctx->nameOf(param.first));
                }
                if (dsp_reg_param(ci->params, id_INREG_CTRL_AX) || dsp_reg_param(ci->params, id_INREG_CTRL_AY) ||
                    dsp_reg_param(ci->params, id_INREG_CTRL_AZ) || dsp_reg_param(ci->params, id_INREG_CTRL_BX) ||
                    dsp_reg_param(ci->params, id_INREG_CTRL_BY) || dsp_reg_param(ci->params, id_INREG_CTRL_BZ) ||
                    dsp_reg_param(ci->params, id_OREG_CTRL)) {
                    if (ci->getPort(id_CLK) == nullptr && ci->get_pin_state(id_CLK) != PIN_0 &&
                        ci->get_pin_state(id_CLK) != PIN_1)
                        log_error("DSP cell '%s' enables a register without a CLK port.\n", ctx->nameOf(ci));
                }
                if (ci->type == id_MISTRAL_MUL18X18 && dsp_bool_param(ci->params, id_PREADDER_EN))
                    log_error("MISTRAL_MUL18X18 does not support PREADDER_EN; use the M9 preadder mode.\n");
                if (ci->type == id_MISTRAL_MUL18X18 && dsp_has_bus(ci, ctx, "Z"))
                    log_error("MISTRAL_MUL18X18 does not support a Z preadder port in M18X18P36 mode.\n");
                if (ci->type == id_MISTRAL_MUL9X9 && dsp_has_bus(ci, ctx, "C"))
                    log_error("MISTRAL_MUL9X9 does not support a C addend port.\n");
                if (ci->type == id_MISTRAL_MUL9X9 && dsp_has_bus(ci, ctx, "Z") &&
                    !dsp_bool_param(ci->params, id_PREADDER_EN))
                    log_error("MISTRAL_MUL9X9 Z preadder ports require PREADDER_EN.\n");
                if (ci->type == id_MISTRAL_MUL27X27 &&
                    (dsp_bool_param(ci->params, id_PREADDER_EN) || dsp_has_bus(ci, ctx, "C") ||
                     dsp_has_bus(ci, ctx, "Z")))
                    log_error("MISTRAL_MUL27X27 does not support addend or preadder ports.\n");
                if (ci->type == id_MISTRAL_MUL9X9 &&
                    (dsp_control_used(ci, id_ACCUMULATE) || dsp_control_used(ci, id_SUB) ||
                     dsp_control_used(ci, id_NEGATE) || dsp_control_used(ci, id_LOADCONST) ||
                     dsp_bool_param(ci->params, id_CASCADE_EN) || dsp_bool_param(ci->params, id_CASCADE_1ST_EN) ||
                     dsp_bool_param(ci->params, id_CHAIN_OUTPUT_EN)))
                    log_error("MISTRAL_MUL9X9 does not support accumulator or cascade controls.\n");
                multipliers.push_back(ci);
            }
        }
        if (multipliers.empty())
            return;

        std::sort(multipliers.begin(), multipliers.end(), [](CellInfo *a, CellInfo *b) {
            if (a->type != b->type)
                return a->type.index < b->type.index;
            bool a_signed = dsp_bool_param(a->params, id_A_SIGNED, true);
            bool b_signed = dsp_bool_param(b->params, id_A_SIGNED, true);
            if (a_signed != b_signed)
                return a_signed < b_signed;
            a_signed = dsp_bool_param(a->params, id_B_SIGNED, true);
            b_signed = dsp_bool_param(b->params, id_B_SIGNED, true);
            if (a_signed != b_signed)
                return a_signed < b_signed;
            for (IdString key : {id_INREG_CTRL_AX, id_INREG_CTRL_AY, id_INREG_CTRL_AZ, id_INREG_CTRL_BX,
                                 id_INREG_CTRL_BY, id_INREG_CTRL_BZ, id_OREG_CTRL, id_PREADDER_EN, id_PREADDER_SUB,
                                 id_CASCADE_EN, id_CASCADE_1ST_EN, id_CHAIN_OUTPUT_EN}) {
                bool av = dsp_bool_param(a->params, key);
                bool bv = dsp_bool_param(b->params, key);
                if (av != bv)
                    return av < bv;
            }
            return a->name.str(a->ctx) < b->name.str(b->ctx);
        });

        for (size_t i = 0; i < multipliers.size();) {
            CellInfo *root = multipliers.at(i);
            if (root->type != id_MISTRAL_MUL9X9) {
                ++i;
                continue;
            }
            root->cluster = root->name;
            root->constr_abs_z = true;
            root->constr_z = 0;

            bool a_signed = dsp_bool_param(root->params, id_A_SIGNED, true);
            bool b_signed = dsp_bool_param(root->params, id_B_SIGNED, true);
            size_t end = i + 1;
            while (end < multipliers.size() && end - i < mistral_dsp_lanes.size() &&
                   multipliers.at(end)->type == id_MISTRAL_MUL9X9 &&
                   dsp_bool_param(multipliers.at(end)->params, id_A_SIGNED, true) == a_signed &&
                   dsp_bool_param(multipliers.at(end)->params, id_B_SIGNED, true) == b_signed &&
                   dsp_shared_config_equal(root, multipliers.at(end)))
                ++end;
            for (size_t lane = i; lane < end; ++lane) {
                CellInfo *ci = multipliers.at(lane);
                if (dsp_bool_param(ci->params, id_A_SIGNED, true) != a_signed ||
                    dsp_bool_param(ci->params, id_B_SIGNED, true) != b_signed) {
                    log_error("MISTRAL_MUL9X9 cells '%s' and '%s' disagree on shared DSP signedness; "
                              "three-lane packing requires matching A_SIGNED and B_SIGNED.\n",
                              ctx->nameOf(root), ctx->nameOf(ci));
                }
                if (lane == i)
                    continue;
                ci->cluster = root->name;
                ci->constr_x = 0;
                ci->constr_y = 0;
                ci->constr_abs_z = true;
                ci->constr_z = int(lane - i);
                root->constr_children.push_back(ci);
            }
            i = end;
        }
    }

    void fold_inverted_pll_clock_buffers()
    {
        std::vector<IdString> remove;
        for (auto &entry : ctx->cells) {
            CellInfo *buf = entry.second.get();
            if (buf->type != id_MISTRAL_CLKBUF) continue;
            NetInfo *in = buf->getPort(id_A), *out = buf->getPort(id_Q);
            if (!in || !out || !in->driver.cell || in->driver.cell->type != id_MISTRAL_NOT) continue;
            NetInfo *source = in->driver.cell->getPort(id_A);
            if (!source || !source->driver.cell || source->driver.cell->type != id_MISTRAL_CLKBUF) continue;
            NetInfo *pll_out = source->driver.cell->getPort(id_A);
            if (!pll_out || !pll_out->driver.cell || pll_out->driver.cell->type != id_altera_pll) continue;
            std::vector<PortRef> users;
            bool foldable = true;
            for (auto user : out->users) {
                if (user.cell->type != id_MISTRAL_FF || user.port != id_CLK) foldable = false;
                users.push_back(user);
            }
            if (!foldable || users.empty()) continue;
            if (buf->bel != BelId() || buf->attrs.count(ctx->id("BEL")) || buf->attrs.count(id_LOC) ||
                buf->attrs.count(ctx->id("NEXTPNR_BEL")))
                log_error("PLL inverted clock buffer '%s': placement constraint prevents folding.\n", ctx->nameOf(buf));
            auto differs = [](const DelayPair &a, const DelayPair &b) {
                return a.minDelay() != b.minDelay() || a.maxDelay() != b.maxDelay();
            };
            // An explicitly constrained inverse clock must describe the swapped waveform.
            for (NetInfo *net : {in, out}) {
                if (!net->clkconstr)
                    continue;
                NPNR_ASSERT(source->clkconstr);
                if (source->clkconstr->phase_group != IdString())
                    log_error("PLL inverted clock '%s': explicit clock constraint cannot describe the PLL phase.\n",
                              ctx->nameOf(net));
                if (differs(net->clkconstr->period, source->clkconstr->period) ||
                    differs(net->clkconstr->high, source->clkconstr->low) ||
                    differs(net->clkconstr->low, source->clkconstr->high))
                    log_error("PLL inverted clock '%s': conflicting clock constraint.\n", ctx->nameOf(net));
            }
            // Expose the inverter directly to FF clock pins; pack_constants folds
            // it into their hardware clock inversion and retains the original domain.
            for (auto user : users) {
                user.cell->disconnectPort(id_CLK);
                user.cell->connectPort(id_CLK, in);
            }
            buf->disconnectPort(id_A);
            buf->disconnectPort(id_Q);
            ctx->nets.erase(out->name);
            remove.push_back(buf->name);
        }
        for (IdString name : remove) ctx->cells.erase(name);
    }

    void setup_clock_enables()
    {
        std::vector<IdString> remove;
        auto is_pll_clock = [&](NetInfo *net) {
            return net && net->driver.cell && net->driver.cell->type == id_altera_pll &&
                   net->driver.port.in(id_outclk, ctx->id("outclk[0]"), ctx->id("outclk[1]"),
                                       ctx->id("outclk[2]"), ctx->id("outclk[3]"));
        };
        auto dedicated_source = [&](NetInfo *net) {
            if (net && net->driver.cell && net->driver.cell->type == id_MISTRAL_CLKBUF && net->driver.port == id_Q) {
                NetInfo *tap = net->driver.cell->getPort(id_A);
                if (is_pll_clock(tap))
                    return tap;
            }
            return net;
        };
        for (auto &entry : ctx->cells) {
            CellInfo *ci = entry.second.get();
            if (ci->type != ctx->id("cyclonev_clkena"))
                continue;
            auto parameter = [&](const char *name, const char *fallback) {
                auto it = ci->params.find(ctx->id(name));
                if (it == ci->params.end())
                    return std::string(fallback);
                if (!it->second.is_string)
                    log_error("Clock enable '%s': parameter '%s' must be a string.\n", ctx->nameOf(ci), name);
                return it->second.as_string();
            };
            std::string power_up = parameter("ena_register_power_up", "high");
            if (power_up != "high" && power_up != "low")
                log_error("Clock enable '%s': ena_register_power_up must be 'high' or 'low'.\n", ctx->nameOf(ci));
            std::string clock_type = parameter("clock_type", "auto");
            if (clock_type != "auto" && clock_type != "global clock" && clock_type != "Global Clock")
                log_error("Clock enable '%s': only global clocks are supported.\n", ctx->nameOf(ci));
            std::string register_mode = parameter("ena_register_mode", "always enabled");
            if (register_mode != "falling edge" && register_mode != "double register")
                log_error("Clock enable '%s': unsupported ena_register_mode; require 'falling edge' or 'double register'.\n",
                          ctx->nameOf(ci));
            for (auto expected : {std::make_pair("disable_mode", "low"), std::make_pair("test_syn", "high"),
                                  std::make_pair("lpm_type", "cyclonev_clkena")}) {
                if (parameter(expected.first, expected.second) != expected.second)
                    log_error("Clock enable '%s': unsupported %s; require '%s'.\n",
                              ctx->nameOf(ci), expected.first, expected.second);
            }
            for (auto &param : ci->params)
                if (!param.first.in(ctx->id("clock_type"), ctx->id("ena_register_mode"),
                                    ctx->id("ena_register_power_up"), ctx->id("disable_mode"),
                                    ctx->id("test_syn"), ctx->id("lpm_type")))
                    log_error("Clock enable '%s': unsupported parameter '%s'.\n", ctx->nameOf(ci), ctx->nameOf(param.first));
            for (auto &port : ci->ports)
                if (port.second.net && !port.first.in(ctx->id("inclk"), ctx->id("ena"), id_outclk, ctx->id("enaout")))
                    log_error("Clock enable '%s': unsupported port '%s'.\n", ctx->nameOf(ci), ctx->nameOf(port.first));
            NetInfo *input = dedicated_source(ci->getPort(ctx->id("inclk")));
            NetInfo *enable = ci->getPort(ctx->id("ena"));
            NetInfo *output = ci->getPort(id_outclk);
            if (!is_pll_clock(input))
                log_error("Clock enable '%s': input must come directly from a PLL clock output.\n", ctx->nameOf(ci));
            if (!enable || !enable->driver.cell)
                log_error("Clock enable '%s': ena must be driven.\n", ctx->nameOf(ci));
            if (!output || output->users.entries() != 1 ||
                (*output->users.begin()).cell->type != id_MISTRAL_CLKBUF || (*output->users.begin()).port != id_A)
                log_error("Clock enable '%s': output must feed exactly one unconditional clock buffer.\n", ctx->nameOf(ci));
            CellInfo *buffer = (*output->users.begin()).cell;
            NetInfo *buffered = buffer->getPort(id_Q);
            if (!buffered || buffered->users.empty())
                log_error("Clock enable '%s': buffered output must have users.\n", ctx->nameOf(ci));
            if (buffer->bel != BelId() || buffer->attrs.count(ctx->id("BEL")) || buffer->attrs.count(id_LOC) ||
                buffer->attrs.count(ctx->id("NEXTPNR_BEL")))
                log_error("Clock enable '%s': output buffer placement constraint prevents folding.\n", ctx->nameOf(ci));
            if (output->clkconstr) {
                auto differs = [](const DelayPair &a, const DelayPair &b) {
                    return a.minDelay() != b.minDelay() || a.maxDelay() != b.maxDelay();
                };
                if (buffered->clkconstr && (differs(output->clkconstr->period, buffered->clkconstr->period) ||
                                          differs(output->clkconstr->high, buffered->clkconstr->high) ||
                                          differs(output->clkconstr->low, buffered->clkconstr->low)))
                    log_error("Clock enable '%s': conflicting clock constraints.\n", ctx->nameOf(ci));
                if (!buffered->clkconstr)
                    buffered->clkconstr = std::move(output->clkconstr);
            }
            ci->disconnectPort(id_outclk);
            buffer->disconnectPort(id_A);
            buffer->disconnectPort(id_Q);
            ci->connectPort(id_outclk, buffered);
            ctx->nets.erase(output->name);
            remove.push_back(buffer->name);
            // Yosys can buffer a PLL clock shared by fabric registers and this
            // gate. Keep that running branch and reconnect the gate to its tap.
            ci->disconnectPort(ctx->id("inclk"));
            ci->connectPort(ctx->id("inclk"), input);
            ci->renamePort(ctx->id("inclk"), id_A);
            ci->renamePort(ctx->id("ena"), id_ENA);
            ci->renamePort(id_outclk, id_Q);
            if (ci->ports.count(ctx->id("enaout")))
                ci->renamePort(ctx->id("enaout"), id_ENAOUT);
            ci->params.clear();
            // High is the existing packed-cell default; only low needs an override.
            if (power_up == "low")
                ci->params[ctx->id("ena_register_power_up")] = power_up;
            // Falling-edge mode is the existing packed-cell default; retain the
            // explicit double-register selection for bit generation.
            if (register_mode == "double register")
                ci->params[ctx->id("ena_register_mode")] = register_mode;
            ci->type = id_MISTRAL_CLKENA;
        }
        for (IdString name : remove)
            ctx->cells.erase(name);
        // MISTRAL_CLKENA uses a falling-edge enable register, optionally with a
        // second falling-edge register, and selectable startup state.
        for (auto &entry : ctx->cells) {
            CellInfo *ci = entry.second.get();
            if (ci->type != id_MISTRAL_CLKENA)
                continue;
            NetInfo *input = dedicated_source(ci->getPort(id_A)), *enable = ci->getPort(id_ENA);
            if (!is_pll_clock(input))
                log_error("Clock enable '%s': input must come directly from a PLL clock output.\n", ctx->nameOf(ci));
            if (input != ci->getPort(id_A)) {
                ci->disconnectPort(id_A);
                ci->connectPort(id_A, input);
            }
            if (!enable || !enable->driver.cell || !ci->getPort(id_Q))
                log_error("Clock enable '%s': require driven ENA and connected Q.\n", ctx->nameOf(ci));
            auto mode = ci->params.find(ctx->id("ena_register_mode"));
            if (mode != ci->params.end()) {
                if (!mode->second.is_string ||
                    (mode->second.as_string() != "falling edge" && mode->second.as_string() != "double register"))
                    log_error("Clock enable '%s': no mode overrides; ena_register_mode must be 'falling edge' or 'double register'.\n",
                              ctx->nameOf(ci));
            }
            for (auto &param : ci->params) {
                if (!param.first.in(ctx->id("ena_register_power_up"), ctx->id("ena_register_mode")))
                    log_error("Clock enable '%s': no mode overrides; only ena_register_mode and ena_register_power_up are supported.\n",
                              ctx->nameOf(ci));
                if (param.first == ctx->id("ena_register_power_up") &&
                    (!param.second.is_string || (param.second.as_string() != "high" && param.second.as_string() != "low")))
                    log_error("Clock enable '%s': ena_register_power_up must be 'high' or 'low'.\n", ctx->nameOf(ci));
            }
            for (auto &port : ci->ports)
                if (port.second.net && !port.first.in(id_A, id_ENA, id_Q, id_ENAOUT))
                    log_error("Clock enable '%s': unsupported port '%s'.\n", ctx->nameOf(ci), ctx->nameOf(port.first));
        }
    }

    void setup_plls()
    {
        for (auto &entry : ctx->cells) {
            CellInfo *ci = entry.second.get();
            if (ci->type != id_altera_pll)
                continue;
            int clocks = int_or_default(ci->params, ctx->id("number_of_clocks"), 1);
            if (clocks < 1 || clocks > 4)
                log_error("PLL '%s': number_of_clocks must be 1, 2, 3 or 4.\n", ctx->nameOf(ci));
            auto reference = ci->params.find(ctx->id("reference_clock_frequency"));
            int reference_mhz = reference != ci->params.end() && reference->second.is_string ?
                    mistral_pll::parse_mhz(reference->second.as_string()) : 0;
            if (!mistral_pll::valid_reference(reference_mhz))
                log_error("PLL '%s': reference frequency must be 25, 50 or 100 MHz.\n", ctx->nameOf(ci));
            bool fractional = str_or_default(ci->params, ctx->id("fractional_vco_multiplier"), "false") == "true";
            std::string phase1 = str_or_default(ci->params, ctx->id("phase_shift1"), "0 ps");
            std::array<std::string, 4> phases{"0 ps", phase1, "0 ps", "0 ps"};
            std::array<int, 4> phase_ps{};
            auto freq0 = ci->params.find(ctx->id("output_clock_frequency0"));
            int64_t phase_output_hz = freq0 != ci->params.end() && freq0->second.is_string ?
                    mistral_pll::parse_output_hz(freq0->second.as_string()) : 0;
            bool shifted = false;
            for (int i = 1; i < clocks; ++i) {
                phases[i] = str_or_default(ci->params, ctx->idf("phase_shift%d", i), "0 ps");
                auto phase = mistral_pll::select_phase(phases[i], phase_output_hz);
                if (!phase)
                    log_error("PLL '%s': phase_shift%d must be zero or a checked phase shift for the output frequency.\n", ctx->nameOf(ci), i);
                phase_ps[i] = phase->shift_ps;
                shifted |= phase_ps[i] != 0;
            }
            int duty0 = int_or_default(ci->params, ctx->id("duty_cycle0"), 50);
            int duty1 = int_or_default(ci->params, ctx->id("duty_cycle1"), 50);
            std::array<int, 4> duties{duty0, duty1, 50, 50};
            for (int i = 2; i < clocks; ++i)
                duties[i] = int_or_default(ci->params, ctx->idf("duty_cycle%d", i), 50);
            for (int i = 0; i < clocks; ++i)
                if (duties[i] <= 0 || duties[i] >= 100)
                    log_error("PLL '%s': duty cycle must be an integer percent from 1 to 99.\n", ctx->nameOf(ci));
            if (fractional && (duty0 != 50 || (clocks == 2 && duty1 != 50)))
                log_error("PLL '%s': fractional-N profiles require 50 percent duty cycle.\n", ctx->nameOf(ci));
            // Frequency selection uses only checked feedback/analog tuples.
            // Other unsupported modes and parameters still fail closed.
            dict<IdString, Property> profile = {
                {ctx->id("reference_clock_frequency"), reference->second},
                {ctx->id("operation_mode"), Property("direct")},
                {ctx->id("fractional_vco_multiplier"), Property(fractional ? "true" : "false")},
                {ctx->id("phase_shift0"), Property("0 ps")},
                {ctx->id("number_of_clocks"), Property(clocks)},
                {ctx->id("duty_cycle0"), Property(duty0)},
            };
            if (clocks >= 2) {
                profile[ctx->id("phase_shift1")] = Property(phase1);
                profile[ctx->id("duty_cycle1")] = Property(duty1);
            }
            if (clocks >= 3) {
                profile[ctx->id("phase_shift2")] = Property(phases[2]);
                profile[ctx->id("duty_cycle2")] = Property(duties[2]);
            }
            if (clocks == 4) {
                profile[ctx->id("phase_shift3")] = Property(phases[3]);
                profile[ctx->id("duty_cycle3")] = Property(duties[3]);
            }
            if (ctx->args.device != "5CSEBA6U23I7")
                log_error("PLL '%s': initial PLL profile supports only 5CSEBA6U23I7.\n", ctx->nameOf(ci));
            for (auto &param : ci->params) {
                if (param.first == ctx->id("output_clock_frequency0") ||
                    (clocks >= 2 && param.first == ctx->id("output_clock_frequency1")) ||
                    (clocks >= 3 && param.first == ctx->id("output_clock_frequency2")) ||
                    (clocks == 4 && param.first == ctx->id("output_clock_frequency3")))
                    continue;
                auto expected = profile.find(param.first);
                if (expected == profile.end() || param.second != expected->second)
                    log_error("PLL '%s': unsupported parameter '%s'; only checked direct profiles are supported.\n",
                              ctx->nameOf(ci), ctx->nameOf(param.first));
            }
            for (auto required : {"reference_clock_frequency", "output_clock_frequency0", "operation_mode"})
                if (!ci->params.count(ctx->id(required)))
                    log_error("PLL '%s': explicit parameter '%s' is required.\n", ctx->nameOf(ci), required);
            const auto &frequency = ci->params.at(ctx->id("output_clock_frequency0"));
            int64_t output_hz = frequency.is_string ? mistral_pll::parse_output_hz(frequency.as_string()) : 0;
            auto config = fractional ? mistral_pll::select_fractional(output_hz, reference_mhz) :
                                       mistral_pll::select_hz(output_hz, reference_mhz, duty0);
            if (fractional && clocks == 1 && !config)
                log_error("PLL '%s': fractional-N profile requires 50 MHz reference and 11.2896 or 12.288 MHz output.\n", ctx->nameOf(ci));
            int64_t output1_hz = 0;
            int c1 = 0;
            if (clocks >= 2) {
                auto freq1 = ci->params.find(ctx->id("output_clock_frequency1"));
                if (freq1 == ci->params.end() || !freq1->second.is_string)
                    log_error("PLL '%s': explicit output_clock_frequency1 is required.\n", ctx->nameOf(ci));
                output1_hz = mistral_pll::parse_output_hz(freq1->second.as_string());
                if (shifted && (fractional || (output_hz != 25000000 && output_hz != 50000000 && output_hz != 100000000) ||
                                output1_hz != output_hz || (output_hz != 25000000 && reference_mhz != 50) ||
                                duty0 != 50 || duty1 != 50))
                    log_error("PLL '%s': phase profile requires equal integer 25 MHz outputs with a checked reference, "
                              "or 50/100 MHz outputs with a 50 MHz reference, and 50 percent duty.\n",
                              ctx->nameOf(ci));
                auto dual = fractional ? mistral_pll::select_fractional_dual(output_hz, output1_hz, reference_mhz) :
                                         mistral_pll::select_dual_hz(output_hz, output1_hz, reference_mhz, duty0, duty1);
                if (fractional && !dual)
                    log_error("PLL '%s': fractional-N dual profile requires 50 MHz reference and 12.288/24.576 MHz outputs.\n", ctx->nameOf(ci));
                if (!dual)
                    log_error("PLL '%s': unsupported dual PLL frequencies/duties; require exact decimal MHz from 1 to 100 with exact dividers from one checked 300/320/400 MHz tuple.\n", ctx->nameOf(ci));
                config = dual->feedback;
                c1 = dual->c1;
                if (!ci->getPort(ctx->id("outclk[0]")) || ci->ports.count(id_outclk))
                    log_error("PLL '%s': dual profile requires outclk[0] and outclk[1].\n", ctx->nameOf(ci));
                ci->renamePort(ctx->id("outclk[0]"), id_outclk);
            }
            std::array<int64_t, 4> output_hzs{output_hz, output1_hz, 0, 0};
            if (clocks >= 3) {
                for (int i = 2; i < clocks; ++i) {
                    auto freq = ci->params.find(ctx->idf("output_clock_frequency%d", i));
                    if (freq == ci->params.end() || !freq->second.is_string)
                        log_error("PLL '%s': explicit output_clock_frequency%d is required.\n", ctx->nameOf(ci), i);
                    output_hzs[i] = mistral_pll::parse_output_hz(freq->second.as_string());
                }
                if (fractional)
                    log_error("PLL '%s': multi-output profile requires integer feedback.\n",
                              ctx->nameOf(ci));
                if (shifted)
                    for (int i = 0; i < clocks; ++i)
                        if (output_hzs[i] != output_hz || duties[i] != 50)
                            log_error("PLL '%s': phase profile requires the same frequency on every output with 50 percent duty.\n",
                                      ctx->nameOf(ci));
                auto multi = mistral_pll::select_multi_hz(output_hzs, clocks, reference_mhz, duties);
                if (!multi)
                    log_error("PLL '%s': unsupported multi-output frequencies/duties; require exact 1 to 100 MHz dividers "
                              "from one checked 300/320/400 MHz tuple.\n", ctx->nameOf(ci));
                config = multi->feedback;
                c1 = multi->counters[1];
            }
            if (!config)
                log_error("PLL '%s': unsupported PLL output frequency/duty; require exact decimal MHz from 1 to 100 "
                          "and an exact integer C divider from a checked 300/320 MHz tuple.\n", ctx->nameOf(ci));
            for (auto &port : ci->ports)
                if (!port.first.in(id_refclk, id_outclk, id_locked, id_rst) &&
                    !(clocks >= 2 && port.first == ctx->id("outclk[1]")) &&
                    !(clocks >= 3 && port.first == ctx->id("outclk[2]")) &&
                    !(clocks == 4 && port.first == ctx->id("outclk[3]")))
                    log_error("PLL '%s': unsupported port '%s'.\n", ctx->nameOf(ci), ctx->nameOf(port.first));
            auto reset_state = get_pin_needed_muxval(ci, id_rst);
            if (reset_state == PIN_0) {
                // Keep the established fixed-profile bitstream for inactive reset.
                ci->disconnectPort(id_rst);
                ci->ports.erase(id_rst);
            } else if (reset_state == PIN_1 || !ci->getPort(id_rst) || !ci->getPort(id_rst)->driver.cell) {
                log_error("PLL '%s': rst must be tied low or driven by a signal.\n", ctx->nameOf(ci));
            }

            NetInfo *ref = ci->getPort(id_refclk), *out = ci->getPort(id_outclk);
            NetInfo *buffered_ref = nullptr;
            // clkbufmap promotes a reference also used by fabric registers.
            // The PLL still needs the dedicated pad tap; keep the buffer for
            // its fabric users. Only the unconditional CLKBUF is transparent.
            if (ref && ref->driver.cell && ref->driver.cell->type == id_MISTRAL_CLKBUF &&
                ref->driver.port == id_Q) {
                NetInfo *pad_net = ref->driver.cell->getPort(id_A);
                if (pad_net) {
                    buffered_ref = ref;
                    ci->disconnectPort(id_refclk);
                    ci->connectPort(id_refclk, pad_net);
                    ref = pad_net;
                }
            }
            if (!ref || !ref->driver.cell || ref->driver.cell->type != id_MISTRAL_IB ||
                str_or_default(ref->driver.cell->attrs, id_LOC, "") != "PIN_V11")
                log_error("PLL '%s': initial profile requires a dedicated reference from PIN_V11.\n", ctx->nameOf(ci));
            std::array<NetInfo *, 4> outputs{out, ci->getPort(ctx->id("outclk[1]")),
                                            ci->getPort(ctx->id("outclk[2]")), ci->getPort(ctx->id("outclk[3]"))};
            std::array<std::vector<CellInfo *>, 4> branches;
            for (int i = 0; i < clocks; ++i) {
                if (!outputs[i] || outputs[i]->users.empty())
                    log_error("PLL '%s': output %d must feed clock buffers.\n", ctx->nameOf(ci), i);
                for (auto user : outputs[i]->users) {
                    if (!ctx->is_clkbuf_cell(user.cell->type) || user.port != id_A)
                        log_error("PLL '%s': output %d must feed only clock buffers.\n", ctx->nameOf(ci), i);
                    branches[i].push_back(user.cell);
                    if (phase_ps[i] != 0 && (outputs[i]->clkconstr ||
                        (user.cell->getPort(id_Q) && user.cell->getPort(id_Q)->clkconstr)))
                        log_error("PLL '%s': shifted output must use the PLL-derived phase constraint, not create_clock.\n",
                                  ctx->nameOf(ci));
                }
                // Preserve the ungated branch's established lane independently
                // of cell insertion order; gate ordering is stable by name.
                std::sort(branches[i].begin(), branches[i].end(), [&](CellInfo *a, CellInfo *b) {
                    if (a->type != b->type)
                        return a->type == id_MISTRAL_CLKBUF;
                    return a->name.str(ctx) < b->name.str(ctx);
                });
            }
            auto set_clock = [&](NetInfo *net, int period, int duty = 50) {
                int high = int(int64_t(period) * duty / 100);
                int low = duty == 50 ? high : period - high;
                if (!net)
                    log_error("PLL '%s': disconnected clock.\n", ctx->nameOf(ci));
                if (net->clkconstr && (net->clkconstr->period.minDelay() != period ||
                                      net->clkconstr->period.maxDelay() != period ||
                                      net->clkconstr->high.minDelay() != high || net->clkconstr->high.maxDelay() != high ||
                                      net->clkconstr->low.minDelay() != low || net->clkconstr->low.maxDelay() != low))
                    log_error("PLL '%s': conflicting clock constraint on '%s'.\n", ctx->nameOf(ci), ctx->nameOf(net));
                net->clkconstr.reset(new ClockConstraint());
                net->clkconstr->period = DelayPair(period);
                net->clkconstr->high = DelayPair(high);
                net->clkconstr->low = DelayPair(low);
            };
            // Check the input pin's SDC constraint as well as the buffered net.
            set_clock(ref->driver.cell->getPort(id_PAD), ctx->getDelayFromNS(1000.0 / reference_mhz));
            set_clock(ref, ctx->getDelayFromNS(1000.0 / reference_mhz));
            if (buffered_ref)
                set_clock(buffered_ref, ctx->getDelayFromNS(1000.0 / reference_mhz));
            std::array<double, 4> generated_hzs{mistral_pll::achieved_hz(*config, reference_mhz),
                                               double(output1_hz), double(output_hzs[2]), double(output_hzs[3])};
            if (clocks >= 2) {
                auto second_config = *config;
                second_config.c = c1;
                generated_hzs[1] = mistral_pll::achieved_hz(second_config, reference_mhz);
            }
            for (int i = 0; i < clocks; ++i) {
                int period = ctx->getDelayFromNS(1.0e9 / generated_hzs[i]);
                set_clock(outputs[i], period, duties[i]);
                for (CellInfo *buffer : branches[i])
                    set_clock(buffer->getPort(id_Q), period, duties[i]);
                // Gating suppresses edges; it does not change the phase of the
                // remaining edges. Relate branches of this counter only, unless
                // the existing shifted profile already relates every output.
                if (shifted || branches[i].size() > 1) {
                    IdString group = shifted ? ci->name : ctx->idf("$pll_branch$%s$%d", ctx->nameOf(ci), i);
                    auto set_phase = [&](NetInfo *net) {
                        net->clkconstr->phase_group = group;
                        net->clkconstr->phase_shift = ctx->getDelayFromNS(phase_ps[i] / 1000.0f);
                    };
                    set_phase(outputs[i]);
                    for (CellInfo *buffer : branches[i])
                        set_phase(buffer->getPort(id_Q));
                }
                if (fractional)
                    log_info("PLL '%s': fractional-N %srequested %.6f Hz, achieved %.9f Hz, error %.9g ppm.\n",
                             ctx->nameOf(ci), i == 0 ? "" : "second ", double(output_hzs[i]), generated_hzs[i],
                             (generated_hzs[i] / output_hzs[i] - 1.0) * 1.0e6);
            }
            BelId chosen;
            WireId pad = ctx->getBelPinWire(ref->driver.cell->bel, ref->driver.port);
            std::vector<BelId> candidates;
            for (const auto &candidate : ctx->pll_clock_bels)
                candidates.push_back(candidate.first);
            // Preserve the established V11 site (0,14) before trying (0,31).
            std::sort(candidates.begin(), candidates.end());
            const std::array<int, 4> preferred_lanes{2, 3, 1, 0};
            std::vector<std::pair<int, CellInfo *>> buffers;
            for (int i = 0; i < clocks; ++i)
                buffers.emplace_back(i, branches[i].front());
            for (int i = 0; i < clocks; ++i)
                for (size_t j = 1; j < branches[i].size(); ++j)
                    buffers.emplace_back(i, branches[i][j]);
            for (BelId candidate : candidates) {
                WireId dst = ctx->getBelPinWire(candidate, id_refclk);
                PipId ref_pip(pad.node, dst.node);
                if (!ctx->pll_ref_select.count(ref_pip) || !ctx->checkBelAvail(candidate))
                    continue;
                // The additional CLKIN2/site profile is checked at the board reference only.
                if (ctx->pll_ref_select.at(ref_pip) == 6 && reference_mhz != 50)
                    continue;
                std::vector<BelId> selected(buffers.size());
                bool available = true;
                for (size_t i = 0; i < buffers.size(); ++i) {
                    int output = buffers[i].first;
                    const auto &options = ctx->pll_clock_bels.at(candidate)[output];
                    auto try_lane = [&](int lane) {
                        for (BelId clock : options) {
                            if (ctx->bel_data(clock).block_index != lane || !ctx->checkBelAvail(clock) ||
                                std::find(selected.begin(), selected.end(), clock) != selected.end())
                                continue;
                            selected[i] = clock;
                            return true;
                        }
                        return false;
                    };
                    if (!try_lane(preferred_lanes[output]))
                        for (int lane : preferred_lanes)
                            if (try_lane(lane))
                                break;
                    if (selected[i] == BelId()) {
                        available = false;
                        break;
                    }
                }
                if (!available)
                    continue;
                chosen = candidate;
                ctx->bindBel(chosen, ci, STRENGTH_LOCKED);
                for (size_t i = 0; i < buffers.size(); ++i)
                    ctx->bindBel(selected[i], buffers[i].second, STRENGTH_LOCKED);
                break;
            }
            if (chosen == BelId())
                log_error("PLL '%s': no available dedicated PLL/clock-buffer pair.\n", ctx->nameOf(ci));
            if (clocks >= 2)
                log_info("PLL '%s': second output %.9g MHz, C7=%d.\n", ctx->nameOf(ci),
                         output1_hz / 1.0e6, c1);
            log_info("PLL '%s': %d MHz -> %.9g MHz, direct, M=%d N=%d C6=%d, bel %s\n",
                     ctx->nameOf(ci), reference_mhz, output_hz / 1.0e6, config->m, config->n, config->c, ctx->nameOfBel(chosen));
        }
    }

    void run()
    {
        init_constant_nets();
        pack_io();
        setup_clock_enables();
        setup_plls();
        fold_inverted_pll_clock_buffers();
        ensure_dsp_control_ports();
        pack_constants();
        select_dsp_control_pinmaps();
        constrain_carries();
        constrain_lutram();
        setup_m10ks();
        constrain_dsps();
    }
};
}; // namespace

bool Arch::pack()
{
    MistralPacker packer(getCtx());
    packer.run();

    assignArchInfo();

    return true;
}

NEXTPNR_NAMESPACE_END
