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
#include "log.h"
#include "nextpnr.h"
#include "pll.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
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
        // TODO: we might need to create missing inputs here in some cases so we can tie them to the correct constant?
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
            bool shifted = phase1 != "0 ps";
            auto phase = mistral_pll::select_phase_25mhz(phase1);
            if (!phase || (shifted && clocks != 2))
                log_error("PLL '%s': phase profile requires two outputs with phase_shift1=10000, 20000 or 30000 ps.\n",
                          ctx->nameOf(ci));
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
                profile[ctx->id("phase_shift2")] = Property("0 ps");
                profile[ctx->id("duty_cycle2")] = Property(duties[2]);
            }
            if (clocks == 4) {
                profile[ctx->id("phase_shift3")] = Property("0 ps");
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
                if (shifted && (fractional || reference_mhz != 50 || output_hz != 25000000 || output1_hz != 25000000 ||
                                duty0 != 50 || duty1 != 50))
                    log_error("PLL '%s': phase profile requires integer 25/25 MHz, 50 MHz reference and 50 percent "
                              "duty.\n",
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
                if (fractional || reference_mhz != 50)
                    log_error("PLL '%s': multi-output profile requires integer feedback and 50 MHz reference.\n",
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
            if (!out || out->users.entries() != 1 || !ctx->is_clkbuf_cell((*out->users.begin()).cell->type) ||
                (*out->users.begin()).port != id_A)
                log_error("PLL '%s': outclk must feed exactly one clock buffer.\n", ctx->nameOf(ci));
            CellInfo *buf = (*out->users.begin()).cell;
            NetInfo *out1 = ci->getPort(ctx->id("outclk[1]"));
            CellInfo *buf1 = nullptr;
            if (clocks >= 2) {
                if (!out1 || out1->users.entries() != 1 ||
                    !ctx->is_clkbuf_cell((*out1->users.begin()).cell->type) ||
                    (*out1->users.begin()).port != id_A)
                    log_error("PLL '%s': outclk[1] must feed exactly one clock buffer.\n", ctx->nameOf(ci));
                buf1 = (*out1->users.begin()).cell;
                if (shifted && (out1->clkconstr || (buf1->getPort(id_Q) && buf1->getPort(id_Q)->clkconstr)))
                    log_error("PLL '%s': shifted output must use the PLL-derived phase constraint, not create_clock.\n",
                              ctx->nameOf(ci));
            }
            NetInfo *out2 = ci->getPort(ctx->id("outclk[2]"));
            CellInfo *buf2 = nullptr;
            if (clocks >= 3) {
                if (!out2 || out2->users.entries() != 1 ||
                    !ctx->is_clkbuf_cell((*out2->users.begin()).cell->type) ||
                    (*out2->users.begin()).port != id_A)
                    log_error("PLL '%s': outclk[2] must feed exactly one clock buffer.\n", ctx->nameOf(ci));
                buf2 = (*out2->users.begin()).cell;
            }
            NetInfo *out3 = ci->getPort(ctx->id("outclk[3]"));
            CellInfo *buf3 = nullptr;
            if (clocks == 4) {
                if (!out3 || out3->users.entries() != 1 ||
                    !ctx->is_clkbuf_cell((*out3->users.begin()).cell->type) ||
                    (*out3->users.begin()).port != id_A)
                    log_error("PLL '%s': outclk[3] must feed exactly one clock buffer.\n", ctx->nameOf(ci));
                buf3 = (*out3->users.begin()).cell;
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
            double generated_hz = mistral_pll::achieved_hz(*config, reference_mhz);
            set_clock(out, ctx->getDelayFromNS(1.0e9 / generated_hz), duty0);
            set_clock(buf->getPort(id_Q), ctx->getDelayFromNS(1.0e9 / generated_hz), duty0);
            if (fractional)
                log_info("PLL '%s': fractional-N requested %.6f Hz, achieved %.9f Hz, error %.9g ppm.\n",
                         ctx->nameOf(ci), double(output_hz), generated_hz,
                         (generated_hz / output_hz - 1.0) * 1.0e6);
            if (buf1) {
                auto second_config = *config;
                second_config.c = c1;
                double generated1_hz = mistral_pll::achieved_hz(second_config, reference_mhz);
                set_clock(out1, ctx->getDelayFromNS(1.0e9 / generated1_hz), duty1);
                set_clock(buf1->getPort(id_Q), ctx->getDelayFromNS(1.0e9 / generated1_hz), duty1);
                if (shifted) {
                    for (NetInfo *net : {out, buf->getPort(id_Q), out1, buf1->getPort(id_Q)}) {
                        net->clkconstr->phase_group = ci->name;
                        net->clkconstr->phase_shift =
                                (net == out1 || net == buf1->getPort(id_Q)) ? ctx->getDelayFromNS(phase->shift_ns) : 0;
                    }
                }
                if (fractional)
                    log_info("PLL '%s': fractional-N second requested %.6f Hz, achieved %.9f Hz, error %.9g ppm.\n",
                             ctx->nameOf(ci), double(output1_hz), generated1_hz,
                             (generated1_hz / output1_hz - 1.0) * 1.0e6);
            }
            if (buf2) {
                set_clock(out2, ctx->getDelayFromNS(1.0e9 / output_hzs[2]), duties[2]);
                set_clock(buf2->getPort(id_Q), ctx->getDelayFromNS(1.0e9 / output_hzs[2]), duties[2]);
            }
            if (buf3) {
                set_clock(out3, ctx->getDelayFromNS(1.0e9 / output_hzs[3]), duties[3]);
                set_clock(buf3->getPort(id_Q), ctx->getDelayFromNS(1.0e9 / output_hzs[3]), duties[3]);
            }
            BelId chosen;
            WireId pad = ctx->getBelPinWire(ref->driver.cell->bel, ref->driver.port);
            for (auto &candidate : ctx->pll_clock_bels) {
                WireId dst = ctx->getBelPinWire(candidate.first, id_refclk);
                if (!ctx->pll_ref_select.count(PipId(pad.node, dst.node)) ||
                    !ctx->checkBelAvail(candidate.first) || !ctx->checkBelAvail(candidate.second))
                    continue;
                if (buf1 && (!ctx->pll_second_clock_bels.count(candidate.first) ||
                             !ctx->checkBelAvail(ctx->pll_second_clock_bels.at(candidate.first))))
                    continue;
                if (buf2 && (!ctx->pll_third_clock_bels.count(candidate.first) ||
                             !ctx->checkBelAvail(ctx->pll_third_clock_bels.at(candidate.first))))
                    continue;
                if (buf3 && (!ctx->pll_fourth_clock_bels.count(candidate.first) ||
                             !ctx->checkBelAvail(ctx->pll_fourth_clock_bels.at(candidate.first))))
                    continue;
                chosen = candidate.first;
                ctx->bindBel(chosen, ci, STRENGTH_LOCKED);
                ctx->bindBel(candidate.second, buf, STRENGTH_LOCKED);
                if (buf1)
                    ctx->bindBel(ctx->pll_second_clock_bels.at(chosen), buf1, STRENGTH_LOCKED);
                if (buf2)
                    ctx->bindBel(ctx->pll_third_clock_bels.at(chosen), buf2, STRENGTH_LOCKED);
                if (buf3)
                    ctx->bindBel(ctx->pll_fourth_clock_bels.at(chosen), buf3, STRENGTH_LOCKED);
                break;
            }
            if (chosen == BelId())
                log_error("PLL '%s': no available dedicated PLL/clock-buffer pair.\n", ctx->nameOf(ci));
            if (buf1)
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
        setup_plls();
        fold_inverted_pll_clock_buffers();
        pack_constants();
        constrain_carries();
        constrain_lutram();
        setup_m10ks();
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
