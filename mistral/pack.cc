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
    return type.in(id_MISTRAL_MUL9X9, id_MISTRAL_MUL18X18, id_MISTRAL_MUL27X27,
                   id_MISTRAL_MUL18X19, id_MISTRAL_MUL18X19_COMBINED);
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
        dsp_bool_param(a->params, id_B_SIGNED, true) != dsp_bool_param(b->params, id_B_SIGNED, true) ||
        dsp_bool_param(a->params, id_C_SIGNED, true) != dsp_bool_param(b->params, id_C_SIGNED, true) ||
        dsp_bool_param(a->params, id_D_SIGNED, true) != dsp_bool_param(b->params, id_D_SIGNED, true))
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
    return key.in(id_A_SIGNED, id_B_SIGNED, id_C_SIGNED, id_D_SIGNED, id_INREG_CTRL_AX, id_INREG_CTRL_AY,
                  id_INREG_CTRL_AZ,
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

    void ensure_m10k_control_ports()
    {
        // Yosys only emits the clear ports when the source primitive uses
        // them.  Materialise both physical clear inputs before constant
        // folding so an omitted input is represented as PIN_0 and is encoded
        // as an inactive clear rather than being left at the M10K default.
        for (auto &entry : ctx->cells) {
            CellInfo *cell = entry.second.get();
            if (!cell->type.in(id_MISTRAL_M10K, id_MISTRAL_M10K_TDP))
                continue;
            for (IdString control : {id_ACLR0, id_ACLR1})
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

    void pack_sdr_outputs()
    {
        std::vector<CellInfo *> outputs;
        IdString request = ctx->id("FAST_OUTPUT_REGISTER");
        for (auto &entry : ctx->cells) {
            auto *io = entry.second.get();
            if (!io->attrs.count(request)) continue;
            const auto &value = io->attrs.at(request);
            if (value.is_string && value.as_string() == "OFF") continue;
            if (!value.is_string || value.as_string() != "ON")
                log_error("FAST_OUTPUT_REGISTER on '%s' must be ON or OFF.\n", ctx->nameOf(io));
            outputs.push_back(io);
        }
        for (auto *io : outputs) {
            auto fail = [&](const char *reason) {
                log_error("SDR output '%s': %s.\n", ctx->nameOf(io), reason);
            };
            if (io->type != id_MISTRAL_OB) fail("FAST_OUTPUT_REGISTER requires a unidirectional output");
            NetInfo *out = io->getPort(id_I);
            if (!out || !out->driver.cell || out->driver.cell->type != id_MISTRAL_FF || out->driver.port != id_Q ||
                out->users.entries() != 1)
                fail("require a directly connected MISTRAL_FF with no other Q consumers");
            CellInfo *ff = out->driver.cell;
            if (!ff->params.empty()) fail("unsupported register parameters");
            for (auto port : {id_ENA, id_ACLR, id_SCLR, id_SLOAD}) {
                bool high = port.in(id_ENA, id_ACLR);
                if (!ff->getPort(port) || get_pin_needed_muxval(ff, port) != (high ? PIN_1 : PIN_0))
                    fail("require constant ENA/ACLR high and SCLR/SLOAD low");
            }
            NetInfo *data = ff->getPort(id_DATAIN), *clock = ff->getPort(id_CLK);
            if (!data || !data->driver.cell) fail("register data must be driven");
            if (!clock || !clock->driver.cell || clock->driver.cell->type.in(id_GND, id_VCC, id_MISTRAL_CONST))
                fail("register clock must be driven and nonconstant");
            NetInfo *source = clock;
            if (ctx->is_clkbuf_cell(source->driver.cell->type)) {
                if (source->driver.port != id_Q) fail("clock buffer must drive Q");
                source = source->driver.cell->getPort(id_A);
            }
            if (!source || !source->driver.cell || source->driver.cell->type.in(id_MISTRAL_NOT, id_GND, id_VCC, id_MISTRAL_CONST))
                fail("require a noninverted clock source");
            auto loc = ctx->getBelLocation(io->bel);
            int bi = ctx->bel_data(io->bel).block_index;
            auto dqs = ctx->cyclonev->p2p_to(CycloneV::pnode(CycloneV::GPIO, loc.x, loc.y, CycloneV::PNONE, bi, -1));
            if (!dqs || !ctx->has_port(CycloneV::GPIO, loc.x, loc.y, bi, CycloneV::CLKOUT, 0))
                fail("selected pad has no supported SDR register/clock path");
            if (!ctx->is_clkbuf_cell(clock->driver.cell->type)) {
                CellInfo *buffer = nullptr;
                for (const auto &user : clock->users)
                    if (user.cell->type == id_MISTRAL_CLKBUF && user.port == id_A) { buffer = user.cell; break; }
                if (!buffer) {
                    buffer = ctx->createCell(ctx->idf("%s$sdr_clkbuf", ctx->nameOf(io)), id_MISTRAL_CLKBUF);
                    buffer->addInput(id_A);
                    buffer->addOutput(id_Q);
                    buffer->connectPort(id_A, clock);
                    buffer->connectPort(id_Q, ctx->createNet(ctx->idf("%s$sdr_clock", ctx->nameOf(io))));
                }
                clock = buffer->getPort(id_Q);
            }
            if (!clock || !clock->driver.cell) fail("clock buffer must have a connected Q output");
            io->disconnectPort(id_I);
            io->connectPort(id_I, data);
            io->type = id_MISTRAL_SDROUT;
            io->addInput(id_CLK);
            io->connectPort(id_CLK, clock);
            for (auto &port : ff->ports) ff->disconnectPort(port.first);
            log_info("Packed SDR output register '%s' into %s.\n", ctx->nameOf(ff), ctx->nameOfBel(io->bel));
            ctx->nets.erase(out->name);
            ctx->cells.erase(ff->name);
        }
        if (!outputs.empty())
            log_warning("SDR output registers: setup/hold and clock-to-pad timing are uncharacterized; "
                        "reported fabric Fmax does not establish output-interface timing closure.\n");
    }

    void pack_sdr_inputs()
    {
        std::vector<CellInfo *> inputs;
        IdString request = ctx->id("FAST_INPUT_REGISTER");
        for (auto &entry : ctx->cells) {
            auto *ib = entry.second.get();
            if (!ib->attrs.count(request))
                continue;
            const auto &value = ib->attrs.at(request);
            if (value.is_string && value.as_string() == "OFF")
                continue;
            if (!value.is_string || value.as_string() != "ON")
                log_error("FAST_INPUT_REGISTER on '%s' must be ON or OFF.\n", ctx->nameOf(ib));
            inputs.push_back(ib);
        }
        for (auto *ib : inputs) {
            auto fail = [&](const char *reason) { log_error("SDR input '%s': %s.\n", ctx->nameOf(ib), reason); };
            if (ib->type != id_MISTRAL_IB)
                fail("FAST_INPUT_REGISTER requires a unidirectional input");
            NetInfo *data = ib->getPort(id_O);
            if (!data || data->users.entries() != 1)
                fail("input buffer output must drive exactly one register data input");
            auto data_user = *data->users.begin();
            if (data_user.cell->type != id_MISTRAL_FF || data_user.port != id_DATAIN)
                fail("require a directly connected MISTRAL_FF data input");
            CellInfo *ff = data_user.cell;
            if (ff->getPort(id_DATAIN) != data)
                fail("register data must be driven by this input buffer");
            if (!ff->params.empty())
                fail("unsupported register parameters");
            for (auto port : {id_ENA, id_ACLR, id_SCLR, id_SLOAD}) {
                bool high = port.in(id_ENA, id_ACLR);
                if (!ff->getPort(port) || get_pin_needed_muxval(ff, port) != (high ? PIN_1 : PIN_0))
                    fail("require constant ENA/ACLR high and SCLR/SLOAD low");
            }
            NetInfo *captured = ff->getPort(id_Q);
            NetInfo *clock = ff->getPort(id_CLK);
            if (!captured)
                fail("register Q must be connected");
            if (!clock || !clock->driver.cell || clock->driver.cell->type.in(id_GND, id_VCC, id_MISTRAL_CONST))
                fail("register clock must be driven and nonconstant");
            NetInfo *source = clock;
            if (ctx->is_clkbuf_cell(source->driver.cell->type)) {
                if (source->driver.port != id_Q)
                    fail("clock buffer must drive Q");
                source = source->driver.cell->getPort(id_A);
            }
            if (!source || !source->driver.cell ||
                source->driver.cell->type.in(id_MISTRAL_NOT, id_GND, id_VCC, id_MISTRAL_CONST))
                fail("require a noninverted clock source");
            auto loc = ctx->getBelLocation(ib->bel);
            int bi = ctx->bel_data(ib->bel).block_index;
            auto dqs = ctx->cyclonev->p2p_to(CycloneV::pnode(CycloneV::GPIO, loc.x, loc.y, CycloneV::PNONE, bi, -1));
            if (!dqs || !ctx->has_port(CycloneV::GPIO, loc.x, loc.y, bi, CycloneV::CLKIN, 0) ||
                !ctx->has_port(CycloneV::GPIO, loc.x, loc.y, bi, CycloneV::DATAIN, 3))
                fail("selected pad has no supported SDR input register/clock path");
            if (!ctx->is_clkbuf_cell(clock->driver.cell->type)) {
                CellInfo *buffer = nullptr;
                for (const auto &user : clock->users)
                    if (user.cell->type == id_MISTRAL_CLKBUF && user.port == id_A) {
                        buffer = user.cell;
                        break;
                    }
                if (!buffer) {
                    buffer = ctx->createCell(ctx->idf("%s$sdr_clkbuf", ctx->nameOf(ib)), id_MISTRAL_CLKBUF);
                    buffer->addInput(id_A);
                    buffer->addOutput(id_Q);
                    buffer->connectPort(id_A, clock);
                    buffer->connectPort(id_Q, ctx->createNet(ctx->idf("%s$sdr_clock", ctx->nameOf(ib))));
                }
                clock = buffer->getPort(id_Q);
            }
            if (!clock || !clock->driver.cell)
                fail("clock buffer must have a connected Q output");

            ib->disconnectPort(id_O);
            ib->ports.erase(id_O);
            ff->disconnectPort(id_Q);
            ib->addOutput(id_Q);
            ib->connectPort(id_Q, captured);
            ib->addInput(id_CLK);
            ib->connectPort(id_CLK, clock);
            ib->pin_data[id_CLK].bel_pins = {ctx->id("CLKIN")};
            ib->type = id_MISTRAL_SDRIN;
            for (auto &port : ff->ports)
                ff->disconnectPort(port.first);
            log_info("Packed SDR input register '%s' into %s.\n", ctx->nameOf(ff), ctx->nameOfBel(ib->bel));
            ctx->nets.erase(data->name);
            ctx->cells.erase(ff->name);
        }
        if (!inputs.empty())
            log_warning("SDR input registers: setup/hold and GPIO register clock-to-Q timing are uncharacterized; "
                        "reported fabric Fmax does not establish input-interface timing closure.\n");
    }

    bool check_altiobuf_params(CellInfo *ci, const std::map<std::string, std::vector<std::string>> &allowed,
                               const char *profile)
    {
        bool valid = true;
        for (const auto &param : ci->params) {
            if (param.first == ctx->id("number_of_channels"))
                continue;
            auto it = allowed.find(param.first.str(ctx));
            if (it == allowed.end() || !param.second.is_string ||
                std::find(it->second.begin(), it->second.end(), param.second.as_string()) == it->second.end()) {
                log_error("%s '%s': unsupported parameter; require the checked width-one profile.\n", profile,
                          ctx->nameOf(ci));
                valid = false;
            }
        }
        return valid;
    }

    bool check_altiobuf_ports(CellInfo *ci, const std::set<std::string> &allowed, const char *profile)
    {
        bool valid = true;
        for (const auto &port : ci->ports) {
            if (!allowed.count(port.first.str(ctx))) {
                log_error("%s '%s': unsupported port '%s'.\n", profile, ctx->nameOf(ci),
                          port.first.str(ctx).c_str());
                valid = false;
            }
        }
        return valid;
    }

    CellInfo *find_altiobuf_io_user(NetInfo *net, IdString port, CellInfo *primitive, const char *profile)
    {
        CellInfo *io = nullptr;
        for (const auto &user : net->users) {
            if (user.cell == primitive)
                continue;
            if (user.cell->type == id_MISTRAL_IO && user.port == port) {
                if (io != nullptr)
                    log_error("%s '%s': pad net has more than one MISTRAL_IO user.\n", profile,
                              ctx->nameOf(primitive));
                io = user.cell;
            }
        }
        return io;
    }

    void pack_altiobuf_inputs()
    {
        std::vector<CellInfo *> cells;
        const IdString type = ctx->id("altiobuf_in");
        for (auto &entry : ctx->cells)
            if (entry.second->type == type)
                cells.push_back(entry.second.get());

        for (CellInfo *ci : cells) {
            bool valid = true;
            auto fail = [&](const char *reason) {
                log_error("altiobuf input '%s': %s.\n", ctx->nameOf(ci), reason);
                valid = false;
            };
            if (int_or_default(ci->params, ctx->id("number_of_channels"), 1) != 1)
                fail("requires number_of_channels=1");
            valid &= check_altiobuf_params(
                    ci, {{"enable_bus_hold", {"FALSE"}}, {"use_differential_mode", {"FALSE"}}},
                    "altiobuf input");
            valid &= check_altiobuf_ports(ci, {"datain", "dataout"}, "altiobuf input");

            const IdString datain = ctx->id("datain"), dataout = ctx->id("dataout");
            NetInfo *pad_net = ci->getPort(datain), *data_net = ci->getPort(dataout);
            if (!pad_net || !data_net)
                fail("datain and dataout must be connected");
            CellInfo *ib = nullptr;
            if (pad_net) {
                // MISTRAL_IB.O is the driver of the net that Yosys gives to
                // altiobuf_in.datain.  The primitive itself is the sole user.
                if (pad_net->driver.cell && pad_net->driver.cell->type == id_MISTRAL_IB &&
                    pad_net->driver.port == id_O)
                    ib = pad_net->driver.cell;
                if (pad_net->users.entries() != 1 || ib == nullptr)
                    fail("datain must directly connect to one input buffer");
            }
            if (data_net &&
                (!data_net->driver.cell || data_net->driver.cell != ci || data_net->driver.port != dataout))
                fail("dataout must be driven by this primitive");
            if (!valid)
                continue;

            ci->disconnectPort(datain);
            ci->disconnectPort(dataout);
            ib->disconnectPort(id_O);
            ib->connectPort(id_O, data_net);
            if (pad_net->users.empty() && pad_net->driver.cell == nullptr)
                ctx->nets.erase(pad_net->name);
            IdString name = ci->name;
            BelId bel = ib->bel;
            ctx->cells.erase(name);
            log_info("Packed altiobuf input '%s' into %s.\n", ctx->nameOf(name), ctx->nameOfBel(bel));
        }
    }

    void pack_altiobuf_outputs()
    {
        std::vector<CellInfo *> cells;
        const IdString type = ctx->id("altiobuf_out");
        for (auto &entry : ctx->cells)
            if (entry.second->type == type)
                cells.push_back(entry.second.get());

        for (CellInfo *ci : cells) {
            bool valid = true;
            auto fail = [&](const char *reason) {
                log_error("altiobuf output '%s': %s.\n", ctx->nameOf(ci), reason);
                valid = false;
            };
            if (int_or_default(ci->params, ctx->id("number_of_channels"), 1) != 1)
                fail("requires number_of_channels=1");
            valid &= check_altiobuf_params(
                    ci, {{"enable_bus_hold", {"FALSE"}},
                         {"use_differential_mode", {"FALSE"}},
                         {"use_oe", {"FALSE"}}},
                    "altiobuf output");
            valid &= check_altiobuf_ports(ci, {"datain", "dataout"}, "altiobuf output");

            const IdString datain = ctx->id("datain"), dataout = ctx->id("dataout");
            NetInfo *data_net = ci->getPort(datain), *pad_net = ci->getPort(dataout);
            if (!data_net || !pad_net)
                fail("datain and dataout must be connected");
            if (data_net && data_net->driver.cell == nullptr)
                fail("datain must have a driver");
            CellInfo *ob = nullptr;
            if (pad_net) {
                for (const auto &user : pad_net->users) {
                    if (user.cell->type == id_MISTRAL_OB && user.port == id_I) {
                        if (ob != nullptr)
                            fail("dataout must directly connect to one output buffer");
                        ob = user.cell;
                    }
                }
                if (pad_net->users.entries() != 1 || ob == nullptr)
                    fail("dataout must directly connect to one output buffer");
            }
            if (pad_net &&
                (!pad_net->driver.cell || pad_net->driver.cell != ci || pad_net->driver.port != dataout))
                fail("dataout must be driven by this primitive");
            if (!valid)
                continue;

            ci->disconnectPort(datain);
            ci->disconnectPort(dataout);
            ob->disconnectPort(id_I);
            ob->connectPort(id_I, data_net);
            if (pad_net->users.empty() && pad_net->driver.cell == nullptr)
                ctx->nets.erase(pad_net->name);
            IdString name = ci->name;
            BelId bel = ob->bel;
            ctx->cells.erase(name);
            log_info("Packed altiobuf output '%s' into %s.\n", ctx->nameOf(name), ctx->nameOfBel(bel));
        }
    }

    void pack_altiobuf_bidir()
    {
        std::vector<CellInfo *> cells;
        const IdString type = ctx->id("altiobuf_bidir");
        for (auto &entry : ctx->cells)
            if (entry.second->type == type)
                cells.push_back(entry.second.get());

        for (CellInfo *ci : cells) {
            bool valid = true;
            auto fail = [&](const char *reason) {
                log_error("altiobuf bidirectional I/O '%s': %s.\n", ctx->nameOf(ci), reason);
                valid = false;
            };
            if (int_or_default(ci->params, ctx->id("number_of_channels"), 1) != 1)
                fail("requires number_of_channels=1");
            valid &= check_altiobuf_params(ci, {{"enable_bus_hold", {"OFF", "FALSE"}}},
                                           "altiobuf bidirectional I/O");
            valid &= check_altiobuf_ports(ci, {"dataio", "oe", "datain", "dataout"},
                                          "altiobuf bidirectional I/O");

            const IdString dataio = ctx->id("dataio"), oe = ctx->id("oe"), datain = ctx->id("datain"),
                           dataout = ctx->id("dataout");
            NetInfo *pad_net = ci->getPort(dataio), *oe_net = ci->getPort(oe), *data_net = ci->getPort(datain),
                    *out_net = ci->getPort(dataout);
            if (!pad_net || !oe_net || !data_net)
                fail("dataio, oe and datain must be connected");
            if (pad_net && pad_net->driver.cell != nullptr)
                fail("dataio must use an undriven pad-side net");
            if (data_net && data_net->driver.cell == nullptr)
                fail("datain must have a driver");
            CellInfo *io = nullptr;
            if (pad_net) {
                io = find_altiobuf_io_user(pad_net, id_I, ci, "altiobuf bidirectional I/O");
                if (pad_net->users.entries() != 2)
                    fail("dataio must directly connect to one MISTRAL_IO pad");
            }
            if (!io)
                fail("dataio must directly connect to one MISTRAL_IO pad");
            if (io && io->getPort(id_O) != nullptr)
                fail("MISTRAL_IO pad already has an input path");
            if (io && !ctx->bel_data(io->bel).pins.count(id_O))
                fail("selected MISTRAL_IO pad has no fabric output path");
            if (out_net) {
                if (out_net == data_net)
                    fail("dataout must use a separate net from datain");
                if (!out_net->driver.cell || out_net->driver.cell != ci || out_net->driver.port != dataout)
                    fail("dataout must be driven by this primitive");
            }
            if (!valid)
                continue;

            ci->disconnectPort(dataio);
            ci->disconnectPort(oe);
            ci->disconnectPort(datain);
            if (out_net)
                ci->disconnectPort(dataout);
            io->disconnectPort(id_I);
            io->connectPort(id_I, data_net);
            io->disconnectPort(id_OE);
            io->connectPort(id_OE, oe_net);
            if (out_net) {
                // The primitive currently drives the fabric output net.
                // Replace that driver with MISTRAL_IO.O and leave all users
                // (including a top-level MISTRAL_OB) in place.
                io->addOutput(id_O);
                io->connectPort(id_O, out_net);
            }
            if (pad_net->users.empty() && pad_net->driver.cell == nullptr)
                ctx->nets.erase(pad_net->name);
            if (out_net && out_net->users.empty() && out_net->driver.cell == nullptr)
                ctx->nets.erase(out_net->name);
            IdString name = ci->name;
            BelId bel = io->bel;
            ctx->cells.erase(name);
            log_info("Packed altiobuf bidirectional I/O '%s' into %s.\n", ctx->nameOf(name), ctx->nameOfBel(bel));
        }
    }

    void pack_altiobufs()
    {
        pack_altiobuf_inputs();
        pack_altiobuf_outputs();
        pack_altiobuf_bidir();
    }

    void pack_ddr_inputs()
    {
        std::vector<CellInfo *> inputs;
        IdString type = ctx->id("altddio_in");
        for (auto &entry : ctx->cells)
            if (entry.second->type == type)
                inputs.push_back(entry.second.get());
        for (auto *ddr : inputs) {
            auto fail = [&](const char *reason) { log_error("DDR input '%s': %s.\n", ctx->nameOf(ddr), reason); };
            if (int_or_default(ddr->params, ctx->id("width"), 1) != 1)
                fail("input capture requires width=1");
            const std::map<std::string, std::vector<std::string>> allowed = {
                    {"intended_device_family", {"Cyclone V"}}, {"power_up_high", {"OFF"}},
                    {"invert_input_clocks", {"OFF"}}, {"lpm_type", {"altddio_in"}},
                    {"lpm_hint", {"UNUSED"}}};
            for (const auto &param : ddr->params) {
                if (param.first == ctx->id("width"))
                    continue;
                auto it = allowed.find(param.first.str(ctx));
                if (it == allowed.end() || !param.second.is_string ||
                    std::find(it->second.begin(), it->second.end(), param.second.as_string()) == it->second.end())
                    fail("unsupported parameter; require the checked DDR input profile");
            }
            for (const auto &port : ddr->ports) {
                const std::string name = port.first.str(ctx);
                if (name != "datain" && name != "inclock" && name != "inclocken" && name != "aset" &&
                    name != "aclr" && name != "sset" && name != "sclr" && name != "dataout_h" &&
                    name != "dataout_l")
                    fail("unsupported port");
            }
            const IdString datain = ctx->id("datain"), inclock = ctx->id("inclock"), inclocken = ctx->id("inclocken");
            const IdString aset = ctx->id("aset"), aclr = ctx->id("aclr"), sset = ctx->id("sset"), sclr = ctx->id("sclr");
            const IdString dataout_h = ctx->id("dataout_h"), dataout_l = ctx->id("dataout_l");
            for (auto port : {inclocken, aset, aclr, sset, sclr}) {
                bool high = port == inclocken;
                if (!ddr->getPort(port) || get_pin_needed_muxval(ddr, port) != (high ? PIN_1 : PIN_0))
                    fail("enable must be high and set/clear controls low");
            }
            NetInfo *data = ddr->getPort(datain);
            if (!data || !data->driver.cell || data->driver.cell->type != id_MISTRAL_IB ||
                data->driver.port != id_O || data->users.entries() != 1)
                fail("datain must be driven directly by one input buffer");
            CellInfo *ib = data->driver.cell;
            NetInfo *high = ddr->getPort(dataout_h), *low = ddr->getPort(dataout_l);
            if (!high || !low || high == low)
                fail("dataout_h and dataout_l must be separate connected nets");
            NetInfo *clock = ddr->getPort(inclock);
            if (!clock || !clock->driver.cell || clock->driver.cell->type.in(id_GND, id_VCC, id_MISTRAL_CONST))
                fail("inclock must be driven by a clock, not a constant");
            NetInfo *source = clock;
            if (ctx->is_clkbuf_cell(source->driver.cell->type)) {
                if (source->driver.port != id_Q)
                    fail("clock buffer must drive Q");
                source = source->driver.cell->getPort(id_A);
            }
            if (!source || !source->driver.cell ||
                source->driver.cell->type.in(id_MISTRAL_NOT, id_GND, id_VCC, id_MISTRAL_CONST))
                fail("require a noninverted clock source");
            auto loc = ctx->getBelLocation(ib->bel);
            int bi = ctx->bel_data(ib->bel).block_index;
            auto dqs = ctx->cyclonev->p2p_to(CycloneV::pnode(CycloneV::GPIO, loc.x, loc.y, CycloneV::PNONE, bi, -1));
            if (!dqs || !ctx->has_port(CycloneV::GPIO, loc.x, loc.y, bi, CycloneV::CLKIN, 0) ||
                !ctx->has_port(CycloneV::GPIO, loc.x, loc.y, bi, CycloneV::DATAIN, 2) ||
                !ctx->has_port(CycloneV::GPIO, loc.x, loc.y, bi, CycloneV::DATAIN, 3))
                fail("selected pad has no supported DDR input register/clock path");
            if (!ctx->is_clkbuf_cell(clock->driver.cell->type)) {
                CellInfo *buffer = nullptr;
                for (const auto &user : clock->users)
                    if (user.cell->type == id_MISTRAL_CLKBUF && user.port == id_A) {
                        buffer = user.cell;
                        break;
                    }
                if (!buffer) {
                    buffer = ctx->createCell(ctx->idf("%s$ddr_clkbuf", ctx->nameOf(ddr)), id_MISTRAL_CLKBUF);
                    buffer->addInput(id_A);
                    buffer->addOutput(id_Q);
                    buffer->connectPort(id_A, clock);
                    buffer->connectPort(id_Q, ctx->createNet(ctx->idf("%s$ddr_clock", ctx->nameOf(ddr))));
                }
                clock = buffer->getPort(id_Q);
            }
            if (!clock || !clock->driver.cell)
                fail("clock buffer must have a connected Q output");

            ddr->disconnectPort(datain);
            ddr->disconnectPort(inclock);
            ddr->disconnectPort(dataout_h);
            ddr->disconnectPort(dataout_l);
            ib->disconnectPort(id_O);
            ib->ports.erase(id_O);
            ib->addOutput(id_Q_H);
            ib->connectPort(id_Q_H, high);
            ib->addOutput(id_Q_L);
            ib->connectPort(id_Q_L, low);
            ib->addInput(id_CLK);
            ib->connectPort(id_CLK, clock);
            ib->pin_data[id_CLK].bel_pins = {ctx->id("CLKIN")};
            ib->type = id_MISTRAL_DDRIN;
            for (auto &port : ddr->ports)
                ddr->disconnectPort(port.first);
            log_info("Packed DDR input '%s' into %s.\n", ctx->nameOf(ddr), ctx->nameOfBel(ib->bel));
            ctx->nets.erase(data->name);
            ctx->cells.erase(ddr->name);
        }
        if (!inputs.empty())
            log_warning("DDR input registers: setup/hold, GPIO register clock-to-Q, and Q-to-fabric timing are "
                        "uncharacterized; "
                        "reported fabric Fmax does not establish input-interface timing closure.\n");
    }

    void pack_ddr_outputs()
    {
        std::vector<CellInfo *> cells;
        for (auto &entry : ctx->cells)
            if (entry.second->type == ctx->id("altddio_out"))
                cells.push_back(entry.second.get());
        bool fabric_data_outputs = false;
        for (CellInfo *ci : cells) {
            auto fail = [&](const char *reason) { log_error("DDR output '%s': %s.\n", ctx->nameOf(ci), reason); };
            if (int_or_default(ci->params, ctx->id("width"), 1) != 1)
                fail("DDR output requires width=1");
            const std::map<std::string, std::vector<std::string>> allowed = {
                {"intended_device_family", {"Cyclone V"}}, {"power_up_high", {"OFF"}},
                {"oe_reg", {"UNUSED", "UNREGISTERED"}}, {"extend_oe_disable", {"UNUSED", "OFF"}},
                {"invert_output", {"OFF"}}, {"lpm_type", {"altddio_out"}}, {"lpm_hint", {"UNUSED"}}};
            for (const auto &param : ci->params) {
                if (param.first == ctx->id("width")) continue;
                auto it = allowed.find(param.first.str(ctx));
                if (it == allowed.end() || !param.second.is_string ||
                    std::find(it->second.begin(), it->second.end(), param.second.as_string()) == it->second.end())
                    fail("unsupported parameter; require the checked DDR output profile");
            }
            for (const auto &port : ci->ports) {
                const std::string name = port.first.str(ctx);
                if (name != "datain_h" && name != "datain_l" && name != "dataout" && name != "outclock" &&
                    name != "outclocken" && name != "oe" && name != "oe_out" && name != "aclr" &&
                    name != "aset" && name != "sclr" && name != "sset")
                    fail("unsupported port");
            }
            for (const char *name : {"outclocken", "oe", "aclr", "aset", "sclr", "sset"}) {
                IdString port = ctx->id(name);
                bool high = port == ctx->id("outclocken") || port == ctx->id("oe");
                if (ci->getPort(port) && get_pin_needed_muxval(ci, port) != (high ? PIN_1 : PIN_0))
                    fail("enable/OE must be constant high and resets constant low");
            }
            if (ci->getPort(ctx->id("oe_out"))) fail("oe_out must be unused");
            IdString datain_h = ctx->id("datain_h"), datain_l = ctx->id("datain_l");
            auto h = get_pin_needed_muxval(ci, datain_h);
            auto l = get_pin_needed_muxval(ci, datain_l);
            bool have_h = ci->getPort(datain_h) != nullptr;
            bool have_l = ci->getPort(datain_l) != nullptr;
            bool h_constant = h == PIN_0 || h == PIN_1;
            bool l_constant = l == PIN_0 || l == PIN_1;
            bool clock_forward = (have_h && have_l &&
                                  ((h == PIN_1 && l == PIN_0) || (h == PIN_0 && l == PIN_1)));
            bool fabric_data = !clock_forward;
            if (fabric_data && (!have_h || !have_l || h_constant || l_constant)) {
                fail("fabric DDR data requires two connected nonconstant datain_h/datain_l nets");
                // Do not transform a malformed primitive: data_h/data_l below
                // are only valid for the two-net fabric-data form.
                continue;
            }
            NetInfo *out = ci->getPort(ctx->id("dataout"));
            if (!out || out->users.entries() != 1) fail("dataout must drive exactly one output buffer");
            auto sink = *out->users.begin();
            if (sink.cell->type != id_MISTRAL_OB || sink.port != id_I)
                fail("dataout must directly drive a unidirectional output pin");
            CellInfo *io = sink.cell;
            auto loc = ctx->getBelLocation(io->bel);
            int bi = ctx->bel_data(io->bel).block_index;
            auto dqs = ctx->cyclonev->p2p_to(CycloneV::pnode(CycloneV::GPIO, loc.x, loc.y, CycloneV::PNONE, bi, -1));
            if (!dqs || !ctx->has_port(CycloneV::GPIO, loc.x, loc.y, bi, CycloneV::CLKOUT, 0) ||
                !ctx->has_port(CycloneV::GPIO, loc.x, loc.y, bi, CycloneV::DATAOUT, 1))
                fail("selected pad has no supported DDR register/clock path");
            NetInfo *clock = ci->getPort(ctx->id("outclock"));
            if (!clock || !clock->driver.cell || clock->driver.cell->type.in(id_GND, id_VCC, id_MISTRAL_CONST))
                fail("outclock must be driven by a clock, not a constant");
            if (ctx->is_clkbuf_cell(clock->driver.cell->type) && clock->driver.port != id_Q)
                fail("outclock must use the clock buffer Q output");
            if (!ctx->is_clkbuf_cell(clock->driver.cell->type)) {
                CellInfo *buffer = nullptr;
                for (const auto &user : clock->users)
                    if (user.cell->type == id_MISTRAL_CLKBUF && user.port == id_A) {
                        buffer = user.cell;
                        break;
                    }
                if (!buffer) {
                    buffer = ctx->createCell(ctx->idf("%s$ddr_clkbuf", ctx->nameOf(ci)), id_MISTRAL_CLKBUF);
                    buffer->addInput(id_A);
                    buffer->addOutput(id_Q);
                    buffer->connectPort(id_A, clock);
                    NetInfo *buffered = ctx->createNet(ctx->idf("%s$ddr_clock", ctx->nameOf(ci)));
                    buffer->connectPort(id_Q, buffered);
                }
                clock = buffer->getPort(id_Q);
            }
            if (!clock || !clock->driver.cell) fail("clock buffer must have a connected Q output");
            io->disconnectPort(id_I);
            io->ports.erase(id_I);
            io->type = id_MISTRAL_DDROUT;
            NetInfo *data_h = ci->getPort(datain_h), *data_l = ci->getPort(datain_l);
            if (fabric_data) {
                io->addInput(id_D_H);
                io->connectPort(id_D_H, data_h);
                io->addInput(id_D_L);
                io->connectPort(id_D_L, data_l);
                io->params.erase(id_DDR_HIGH);
                fabric_data_outputs = true;
            } else {
                io->params[id_DDR_HIGH] = h == PIN_1;
            }
            io->addInput(id_CLK);
            io->connectPort(id_CLK, clock);
            for (auto &port : ci->ports) ci->disconnectPort(port.first);
            if (fabric_data)
                log_info("Packed DDR output data '%s' into %s.\n", ctx->nameOf(ci), ctx->nameOfBel(io->bel));
            else
                log_info("Packed DDR clock forwarder '%s' into %s (%s phase).\n", ctx->nameOf(ci),
                         ctx->nameOfBel(io->bel), h == PIN_1 ? "normal" : "inverted");
            ctx->nets.erase(out->name);
            ctx->cells.erase(ci->name);
        }
        if (fabric_data_outputs)
            log_warning("DDR output data: GPIO register setup/hold and clock-to-pad timing are uncharacterized; "
                        "reported fabric Fmax does not establish output-interface timing closure.\n");
    }

    void pack_ddr_bidir()
    {
        std::vector<CellInfo *> cells;
        IdString type = ctx->id("altddio_bidir");
        for (auto &entry : ctx->cells)
            if (entry.second->type == type)
                cells.push_back(entry.second.get());

        for (CellInfo *ddr : cells) {
            bool valid = true;
            auto fail = [&](const char *reason) {
                valid = false;
                log_error("DDR bidirectional I/O '%s': %s.\n", ctx->nameOf(ddr), reason);
            };
            if (int_or_default(ddr->params, ctx->id("width"), 1) != 1)
                fail("requires width=1");
            const std::map<std::string, std::vector<std::string>> allowed = {
                    {"intended_device_family", {"Cyclone V"}},
                    {"power_up_high", {"OFF"}},
                    {"oe_reg", {"UNUSED", "UNREGISTERED"}},
                    {"extend_oe_disable", {"UNUSED", "OFF"}},
                    {"implement_input_in_lcell", {"UNUSED"}},
                    {"invert_output", {"OFF"}},
                    {"lpm_type", {"altddio_bidir"}},
                    {"lpm_hint", {"UNUSED"}}};
            for (const auto &param : ddr->params) {
                if (param.first == ctx->id("width"))
                    continue;
                auto it = allowed.find(param.first.str(ctx));
                if (it == allowed.end() || !param.second.is_string ||
                    std::find(it->second.begin(), it->second.end(), param.second.as_string()) == it->second.end())
                    fail("unsupported parameter; require the checked DDR bidirectional I/O profile");
            }
            for (const auto &port : ddr->ports) {
                const std::string name = port.first.str(ctx);
                if (name != "datain_h" && name != "datain_l" && name != "inclock" && name != "inclocken" &&
                    name != "outclock" && name != "outclocken" && name != "aset" && name != "aclr" &&
                    name != "sset" && name != "sclr" && name != "oe" && name != "dataout_h" &&
                    name != "dataout_l" && name != "combout" && name != "oe_out" &&
                    name != "dqsundelayedout" && name != "padio")
                    fail("unsupported port");
            }
            if (!valid)
                continue;
            for (const char *name : {"inclocken", "outclocken", "aset", "aclr", "sset", "sclr"}) {
                IdString port = ctx->id(name);
                bool high = port == ctx->id("inclocken") || port == ctx->id("outclocken");
                if (!ddr->getPort(port) || get_pin_needed_muxval(ddr, port) != (high ? PIN_1 : PIN_0))
                    fail("enable must be high and set/clear controls low");
            }
            if (ddr->getPort(ctx->id("oe")) == nullptr)
                fail("oe must be connected");
            if (ddr->getPort(ctx->id("oe_out")) || ddr->getPort(ctx->id("dqsundelayedout")))
                fail("oe_out and dqsundelayedout must be unused");
            if (!valid)
                continue;

            IdString datain_h = ctx->id("datain_h"), datain_l = ctx->id("datain_l");
            IdString dataout_h = ctx->id("dataout_h"), dataout_l = ctx->id("dataout_l");
            IdString combout = ctx->id("combout"), padio = ctx->id("padio");
            NetInfo *data_h = ddr->getPort(datain_h), *data_l = ddr->getPort(datain_l);
            if (!data_h || !data_l || data_h == data_l || get_pin_needed_muxval(ddr, datain_h) != PIN_SIG ||
                get_pin_needed_muxval(ddr, datain_l) != PIN_SIG)
                fail("fabric DDR data requires two connected nonconstant datain_h/datain_l nets");
            NetInfo *captured_h = ddr->getPort(dataout_h), *captured_l = ddr->getPort(dataout_l);
            NetInfo *combined = ddr->getPort(combout);
            if (!captured_h || !captured_l || !combined || captured_h == captured_l || captured_h == combined ||
                captured_l == combined)
                fail("dataout_h/dataout_l and combout must be connected to separate fabric nets");
            NetInfo *pad_net = ddr->getPort(padio);
            if (!pad_net)
                fail("padio must connect to a constrained GPIO pad");
            CellInfo *io = nullptr;
            if (pad_net) {
                for (const auto &user : pad_net->users) {
                    // Yosys models an inout's pad net on the output-buffer
                    // side of MISTRAL_IO (I), while PAD remains the
                    // top-level package net.  The altddio_bidir padio is
                    // therefore a user of I rather than PAD here.
                    if (user.cell->type == id_MISTRAL_IO && user.port.in(id_I, id_PAD)) {
                        io = user.cell;
                        break;
                    }
                }
            }
            if (!io)
                fail("padio must directly connect to a constrained MISTRAL_IO pad");
            if (!valid)
                continue;

            NetInfo *inclock = ddr->getPort(ctx->id("inclock"));
            NetInfo *outclock = ddr->getPort(ctx->id("outclock"));
            if (!inclock || !outclock || inclock != outclock || !inclock->driver.cell ||
                inclock->driver.cell->type.in(id_GND, id_VCC, id_MISTRAL_CONST) ||
                outclock->driver.cell->type.in(id_GND, id_VCC, id_MISTRAL_CONST))
                fail("inclock and outclock must be driven by the same clock");
            if (!valid)
                continue;
            NetInfo *source = inclock;
            if (ctx->is_clkbuf_cell(source->driver.cell->type)) {
                if (source->driver.port != id_Q)
                    fail("clock buffer must drive Q");
            } else {
                if (source->driver.cell->type.in(id_MISTRAL_NOT, id_GND, id_VCC, id_MISTRAL_CONST))
                    fail("require a noninverted clock source");
                if (!valid)
                    continue;
                CellInfo *buffer = nullptr;
                for (const auto &user : inclock->users)
                    if (user.cell->type == id_MISTRAL_CLKBUF && user.port == id_A) {
                        buffer = user.cell;
                        break;
                    }
                if (!buffer) {
                    buffer = ctx->createCell(ctx->idf("%s$ddr_bidir_clkbuf", ctx->nameOf(ddr)), id_MISTRAL_CLKBUF);
                    buffer->addInput(id_A);
                    buffer->addOutput(id_Q);
                    buffer->connectPort(id_A, inclock);
                    buffer->connectPort(id_Q, ctx->createNet(ctx->idf("%s$ddr_bidir_clock", ctx->nameOf(ddr))));
                }
                inclock = buffer->getPort(id_Q);
            }
            if (!inclock || !inclock->driver.cell)
                fail("clock buffer must have a connected Q output");
            if (!valid)
                continue;

            auto loc = ctx->getBelLocation(io->bel);
            int bi = ctx->bel_data(io->bel).block_index;
            auto dqs = ctx->cyclonev->p2p_to(CycloneV::pnode(CycloneV::GPIO, loc.x, loc.y, CycloneV::PNONE, bi, -1));
            if (!dqs || !ctx->has_port(CycloneV::GPIO, loc.x, loc.y, bi, CycloneV::CLKOUT, 0) ||
                !ctx->has_port(CycloneV::GPIO, loc.x, loc.y, bi, CycloneV::CLKIN, 0) ||
                !ctx->has_port(CycloneV::GPIO, loc.x, loc.y, bi, CycloneV::DATAOUT, 1) ||
                !ctx->has_port(CycloneV::GPIO, loc.x, loc.y, bi, CycloneV::DATAIN, 2) ||
                !ctx->has_port(CycloneV::GPIO, loc.x, loc.y, bi, CycloneV::DATAIN, 3))
                fail("selected pad has no supported DDR bidirectional register/clock path");
            if (!valid)
                continue;

            NetInfo *oe = ddr->getPort(ctx->id("oe"));

            auto replace_input = [&](IdString port, NetInfo *net) {
                if (io->getPort(port))
                    io->disconnectPort(port);
                else
                    io->addInput(port);
                io->connectPort(port, net);
            };
            auto replace_output = [&](IdString port, NetInfo *net) {
                if (io->getPort(port))
                    io->disconnectPort(port);
                else
                    io->addOutput(port);
                io->connectPort(port, net);
            };
            if (io->getPort(id_I)) {
                io->disconnectPort(id_I);
                io->ports.erase(id_I);
            }
            // The altddio outputs currently own these nets.  Release their
            // drivers before attaching the corresponding GPIO BEL outputs;
            // NetInfo permits only one driver.
            ddr->disconnectPort(dataout_h);
            ddr->disconnectPort(dataout_l);
            ddr->disconnectPort(combout);
            replace_input(id_D_H, data_h);
            replace_input(id_D_L, data_l);
            replace_input(id_CLK, inclock);
            replace_input(id_CLKIN, inclock);
            replace_input(id_OE, oe);
            replace_output(id_O, combined);
            replace_output(id_Q_H, captured_h);
            replace_output(id_Q_L, captured_l);
            ddr->disconnectPort(padio);
            for (auto &port : ddr->ports)
                ddr->disconnectPort(port.first);
            io->type = id_MISTRAL_DDRBIDIR;
            log_info("Packed DDR bidirectional I/O '%s' into %s.\n", ctx->nameOf(ddr), ctx->nameOfBel(io->bel));
            ctx->cells.erase(ddr->name);
        }
        if (!cells.empty())
            log_warning("DDR bidirectional I/O: GPIO register setup/hold, clock-to-pad and clock-to-fabric timing are "
                        "uncharacterized; reported fabric Fmax does not establish interface timing closure.\n");
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
            auto init = ci->params.find(id_INIT);
            if (init != ci->params.end() && (init->second.is_string || init->second.str.size() > 32))
                log_error("MLAB '%s': INIT must be a numeric value of at most 32 bits.\n", ctx->nameOf(ci));
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

    void setup_tdp_m10k(CellInfo *ci)
    {
        int dbits = int_or_default(ci->params, id_CFG_DBITS, 10);
        int abits = int_or_default(ci->params, id_CFG_ABITS, 10);
        int bdbits = int_or_default(ci->params, id_CFG_RD_DBITS, dbits);
        int babits = int_or_default(ci->params, id_CFG_RD_ABITS, abits);
        bool mixed = bool_or_default(ci->params, id_CFG_MIXED_WIDTH, false);
        bool byte_enable = bool_or_default(ci->params, id_CFG_BYTE_ENABLE, false);
        auto geometry = [](int a, int d) { return (d == 10 && a == 10) || (d == 20 && a == 9); };
        if (mixed && byte_enable)
            log_error("M10K '%s': true dual-port cannot combine mixed widths and byte enables.\n", ctx->nameOf(ci));
        if (mixed && (!ci->params.count(id_CFG_RD_ABITS) || !ci->params.count(id_CFG_RD_DBITS)))
            log_error("M10K '%s': mixed TDP requires explicit B geometry (CFG_RD_ABITS/CFG_RD_DBITS).\n", ctx->nameOf(ci));
        if (!geometry(abits, dbits) || !geometry(babits, bdbits))
            log_error("M10K '%s': true dual-port requires 1024x10 or 512x20 on each port.\n", ctx->nameOf(ci));
        if (!mixed && (babits != abits || bdbits != dbits))
            log_error("M10K '%s': unequal TDP B geometry requires CFG_MIXED_WIDTH=1.\n", ctx->nameOf(ci));
        if (byte_enable && dbits != 20)
            log_error("M10K '%s': true dual-port byte enables require 512x20 ports.\n", ctx->nameOf(ci));
        if (byte_enable) {
            for (const auto &port : ci->ports) {
                const auto &name = port.first.str(ctx);
                if ((name.find("A1BE[") == 0 || name.find("B1BE[") == 0) &&
                    name != "A1BE[0]" && name != "A1BE[1]" && name != "B1BE[0]" && name != "B1BE[1]")
                    log_error("M10K '%s': true dual-port byte masks must have exactly two bits.\n", ctx->nameOf(ci));
            }
            for (char side : {'A', 'B'}) {
                for (int bit = 0; bit < 2; bit++) {
                    IdString port = ctx->idf("%c1BE[%d]", side, bit);
                    if (!ci->getPort(port))
                        log_error("M10K '%s': true dual-port byte mode requires connected %s.\n",
                                  ctx->nameOf(ci), ctx->nameOf(port));
                    ci->pin_data[port].bel_pins = {ctx->idf("BYTEENABLE%c[%d]", side, bit)};
                }
            }
        }
        auto hard_constant = [&](IdString port) {
            CellPinState state = ci->get_pin_state(port);
            return state == PIN_0 || state == PIN_1;
        };
        auto tied_low = [&](IdString port) {
            return ci->get_pin_state(port) == PIN_0 || ci->getPort(port) == gnd_net;
        };
        bool clk1_signal = ci->getPort(id_CLK1) != nullptr;
        bool clk2_signal = ci->getPort(id_CLK2) != nullptr;
        bool clk1_constant = !clk1_signal && hard_constant(id_CLK1);
        bool clk2_constant = !clk2_signal && hard_constant(id_CLK2);
        if ((!clk1_signal && !clk1_constant) || (!clk2_signal && !clk2_constant))
            log_error("M10K '%s': true dual-port requires both clocks or an explicit constant on an unused port.\n",
                      ctx->nameOf(ci));
        if (clk1_constant && !tied_low(id_A1EN))
            log_error("M10K '%s': constant CLK1 is only valid when A1EN is tied low.\n", ctx->nameOf(ci));
        if (clk2_constant && !tied_low(id_B1EN))
            log_error("M10K '%s': constant CLK2 is only valid when B1EN is tied low.\n", ctx->nameOf(ci));
        for (IdString port : {id_A1EN, id_B1EN, id_A1WE, id_B1WE})
            if (!ci->getPort(port))
                log_error("M10K '%s': true dual-port requires connected %s.\n", ctx->nameOf(ci), ctx->nameOf(port));
        ci->params[id_CFG_ABITS] = abits;
        ci->params[id_CFG_DBITS] = dbits;
        ci->params[id_CFG_DUAL_CLOCK] = 1;
        if (mixed) {
            ci->params[id_CFG_RD_ABITS] = babits;
            ci->params[id_CFG_RD_DBITS] = bdbits;
        }
        // Match the retained Quartus mixed-TDP control mux assignments.
        bool unequal = dbits != bdbits;
        bool swap_wren = unequal && dbits == 20;
        if (clk1_signal)
            ci->pin_data[id_CLK1].bel_pins = {ctx->id("CLKIN[0]")};
        if (clk2_signal)
            ci->pin_data[id_CLK2].bel_pins = {ctx->id("CLKIN[1]")};
        ci->pin_data[id_ACLR0].bel_pins = {ctx->id("ACLR[0]")};
        ci->pin_data[id_ACLR1].bel_pins = {ctx->id("ACLR[1]")};
        ci->pin_data[id_A1EN].bel_pins = {ctx->idf("ENABLE[%d]", unequal ? 0 : 1)};
        ci->pin_data[id_B1EN].bel_pins = {ctx->idf("ENABLE[%d]", unequal ? 1 : 0)};
        ci->pin_data[id_A1WE].bel_pins = {ctx->idf("WREN[%d]", swap_wren ? 1 : 0)};
        ci->pin_data[id_B1WE].bel_pins = {ctx->idf("WREN[%d]", swap_wren ? 0 : 1)};
        for (int port = 0; port < 2; port++) {
            char side = port == 0 ? 'A' : 'B';
            int addr_bits = port == 0 ? abits : babits;
            int data_bits = port == 0 ? dbits : bdbits;
            for (int bit = 0; bit < addr_bits; bit++)
                ci->pin_data[ctx->idf("%c1ADDR[%d]", side, bit)].bel_pins = {
                        ctx->idf("ADDR%c[%d]", side, bit + 12 - addr_bits)};
            for (int bit = 0; bit < data_bits; bit++) {
                auto &pins = ci->pin_data[ctx->idf("%c1DATA[%d]", side, bit)].bel_pins;
                pins = {ctx->idf("DATA%cIN[%d]", side, bit)};
                if (data_bits == 10)
                    pins.push_back(ctx->idf("DATA%cIN[%d]", side, bit + 10));
                ci->pin_data[ctx->idf("%c1Q[%d]", side, bit)].bel_pins = {
                        ctx->idf("DATA%cOUT[%d]", side, bit)};
            }
        }
    }

    void fold_m10k_constant_clock(CellInfo *ci, IdString port)
    {
        NetInfo *net = ci->getPort(port);
        bool value;
        if (net == gnd_net)
            value = false;
        else if (net == vcc_net)
            value = true;
        else if (net != nullptr && net->driver.cell != nullptr && net->driver.cell->type == id_GND)
            value = false;
        else if (net != nullptr && net->driver.cell != nullptr && net->driver.cell->type == id_VCC)
            value = true;
        else
            return;

        // process_inv_constants() normally folds direct GND/VCC drivers via
        // PINSTYLE_CLK. An inverter whose input is a constant is rewired to
        // the soft constant net with PIN_INV, however, so normalize that
        // composition here before assigning a physical CLKIN pin.
        if (ci->get_pin_state(port) == PIN_INV)
            value = !value;
        ci->disconnectPort(port);
        ci->pin_data[port].state = value ? PIN_1 : PIN_0;
    }

    void setup_m10k_address_stalls(CellInfo *ci)
    {
        // ADDRSTALLA/B are dedicated M10K GOUT inputs.  They are ordinary
        // fabric controls: unlike the clock and clear muxes, Quartus does
        // not program a separate mode bit for them.  Keep a connected port
        // visible so the router can reach the physical BEL pin.
        if (ci->getPort(id_ADDRSTALLA) != nullptr)
            ci->pin_data[id_ADDRSTALLA].bel_pins = {ctx->id("ADDRSTALLA")};
        if (ci->getPort(id_ADDRSTALLB) != nullptr)
            ci->pin_data[id_ADDRSTALLB].bel_pins = {ctx->id("ADDRSTALLB")};
    }

    void setup_mixed_m10k(CellInfo *ci)
    {
        int wb = ci->params.at(id_CFG_DBITS).as_int64();
        int wa = ci->params.at(id_CFG_ABITS).as_int64();
        int rb = int_or_default(ci->params, id_CFG_RD_DBITS, wb);
        int ra = int_or_default(ci->params, id_CFG_RD_ABITS, wa);
        bool byte_enable = bool_or_default(ci->params, id_CFG_BYTE_ENABLE, false);
        bool output_reg_a = bool_or_default(ci->params, id_CFG_OUT_REG_A, false);
        bool output_reg_b = bool_or_default(ci->params, id_CFG_OUT_REG_B, false);
        auto geometry = [](int a, int d) {
            return (d == 10 && a == 10) || (d == 20 && a == 9) || (d == 40 && a == 8);
        };
        if (!geometry(wa, wb) || !geometry(ra, rb))
            log_error("M10K '%s': mixed widths require 1024x10, 512x20 or 256x40 ports.\n", ctx->nameOf(ci));
        if (output_reg_a && rb != 40)
            log_error("M10K '%s': CFG_OUT_REG_A requires true dual-port or a 40-bit read.\n",
                      ctx->nameOf(ci));
        if (bool_or_default(ci->params, id_CFG_ASYNC_READ, false) && (output_reg_a || output_reg_b))
            log_error("M10K '%s': CFG_ASYNC_READ cannot use CFG_OUT_REG_A/B.\n", ctx->nameOf(ci));
        if (!bool_or_default(ci->params, id_CFG_DUAL_CLOCK, false) || ci->getPort(id_CLK1) == nullptr || ci->getPort(id_CLK2) == nullptr)
            log_error("M10K '%s': mixed widths require CFG_DUAL_CLOCK=1 and both clocks.\n", ctx->nameOf(ci));
        if (byte_enable || ci->getPort(ctx->id("A1BE[0]")) || ci->getPort(ctx->id("A1BE[1]"))) {
            for (const auto &port : ci->ports) {
                const auto &name = port.first.str(ctx);
                if (name.find("A1BE[") == 0 && name != "A1BE[0]" && name != "A1BE[1]")
                    log_error("M10K '%s': mixed-width byte masks must have exactly two bits.\n", ctx->nameOf(ci));
            }
            if (!byte_enable)
                log_error("M10K '%s': mixed-width A1BE ports require CFG_BYTE_ENABLE=1.\n", ctx->nameOf(ci));
            if (wb != 20)
                log_error("M10K '%s': mixed-width byte enables require a 20-bit write port.\n", ctx->nameOf(ci));
            for (int bit = 0; bit < 2; bit++) {
                IdString port = ctx->idf("A1BE[%d]", bit);
                if (!ci->getPort(port))
                    log_error("M10K '%s': mixed-width byte-enable mode requires connected %s.\n", ctx->nameOf(ci),
                              ctx->nameOf(port));
                ci->pin_data[port].bel_pins = {ctx->idf("BYTEENABLEA[%d]", bit)};
            }
        }
        ci->pin_data[id_A1EN].bel_pins = {ctx->id("WREN[0]"), ctx->id("ENABLE[1]")};
        ci->pin_data[id_B1EN].bel_pins = {ctx->id("ENABLE[0]")};
        ci->pin_data[id_CLK1].bel_pins = {ctx->id("CLKIN[0]")};
        ci->pin_data[id_CLK2].bel_pins = {ctx->id("CLKIN[1]")};
        ci->pin_data[id_ACLR0].bel_pins = {ctx->id("ACLR[0]")};
        ci->pin_data[id_ACLR1].bel_pins = {ctx->id("ACLR[1]")};
        for (int bit = 0; bit < wa; bit++)
            ci->pin_data[ctx->idf("A1ADDR[%d]", bit)].bel_pins = {ctx->idf("ADDRA[%d]", bit + 12 - wa)};
        for (int bit = 0; bit < ra; bit++)
            ci->pin_data[ctx->idf("B1ADDR[%d]", bit)].bel_pins = {ctx->idf("ADDRB[%d]", bit + 12 - ra)};
        for (int bit = 0; bit < wb; bit++) {
            auto &pins = ci->pin_data[ctx->idf("A1DATA[%d]", bit)].bel_pins;
            pins = {ctx->idf(bit < 20 ? "DATAAIN[%d]" : "DATABIN[%d]", bit % 20)};
            if (wb == 10)
                pins.push_back(ctx->idf("DATAAIN[%d]", bit + 10));
        }
        for (int bit = 0; bit < rb; bit++)
            ci->pin_data[ctx->idf("B1DATA[%d]", bit)].bel_pins = {
                ctx->idf(rb == 40 && bit < 20 ? "DATAAOUT[%d]" : "DATABOUT[%d]", bit % 20)};
    }

    void setup_m10ks()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            if (ci->type == id_MISTRAL_M10K_TDP) {
                // Both SDP and TDP occupy one existing physical M10K BEL.
                ci->type = id_MISTRAL_M10K;
                ci->params[id_CFG_TDP] = 1;
            }
            if (ci->type != id_MISTRAL_M10K)
                continue;
            fold_m10k_constant_clock(ci, id_CLK1);
            fold_m10k_constant_clock(ci, id_CLK2);
            setup_m10k_address_stalls(ci);
            if (bool_or_default(ci->params, id_CFG_TDP, false)) {
                setup_tdp_m10k(ci);
                continue;
            }

            if (bool_or_default(ci->params, id_CFG_MIXED_WIDTH, false)) {
                setup_mixed_m10k(ci);
                continue;
            }
            if (ci->params.count(id_CFG_RD_DBITS) || ci->params.count(id_CFG_RD_ABITS))
                log_error("M10K '%s': separate read geometry requires CFG_MIXED_WIDTH=1.\n", ctx->nameOf(ci));

            auto abits = ci->params.at(id_CFG_ABITS).as_int64();
            auto dbits = ci->params.at(id_CFG_DBITS).as_int64();
            NPNR_ASSERT(abits >= 7 && abits <= 13);
            NPNR_ASSERT(dbits == 1 || dbits == 2 || dbits == 5 || dbits == 10 || dbits == 20 || dbits == 40);
            NPNR_ASSERT((1 << abits) * dbits <= 10240);

            // An unclocked read group is represented by the absence of its
            // read-enable port, matching memory_libmap's clocks 1 0 form.
            // CFG_ASYNC_READ makes that contract explicit for native Yosys
            // primitive JSON. Keep both forms so older hand-written
            // primitives remain useful.
            bool user_b1en = ci->getPort(id_B1EN) != nullptr;
            bool async_read = bool_or_default(ci->params, id_CFG_ASYNC_READ, false) || !user_b1en;
            bool output_reg_a = bool_or_default(ci->params, id_CFG_OUT_REG_A, false);
            bool output_reg_b = bool_or_default(ci->params, id_CFG_OUT_REG_B, false);
            if (output_reg_a && dbits != 40)
                log_error("M10K '%s': CFG_OUT_REG_A requires true dual-port or a 40-bit read.\n",
                          ctx->nameOf(ci));
            if (async_read) {
                // Preserve the explicit mode so the bitstream writer does
                // not need to infer it from the omitted B1EN port.
                ci->params[id_CFG_ASYNC_READ] = 1;
                if (user_b1en) {
                    // A constant-high read enable is useful even for the
                    // unclocked JSON contract.  Some Cyclone V M10K sites
                    // power up with the omitted enable path inactive, while
                    // an explicit ENABLE[0] route is reliable.  Preserve
                    // dynamic and constant-low controls as errors: they do
                    // not describe a flow-through read.
                    NetInfo *b1en = ci->getPort(id_B1EN);
                    bool tied_high = ci->get_pin_state(id_B1EN) == PIN_1 || b1en == vcc_net;
                    if (!tied_high)
                        log_error("M10K '%s': CFG_ASYNC_READ requires B1EN tied high.\n", ctx->nameOf(ci));
                } else {
                    // Keep the logical omission at the JSON boundary, but
                    // materialise the hardware's constant-high read enable.
                    // The soft VCC net is routed to ENABLE[0] below rather
                    // than relying on the M10K site's power-up default.
                    ci->addInput(id_B1EN);
                    ci->connectPort(id_B1EN, vcc_net);
                }
                if (output_reg_a || output_reg_b)
                    log_error("M10K '%s': CFG_ASYNC_READ cannot use CFG_OUT_REG_A/B.\n", ctx->nameOf(ci));
                if (bool_or_default(ci->params, id_CFG_DUAL_CLOCK, false) || ci->getPort(id_CLK2) != nullptr)
                    log_error("M10K '%s': CFG_ASYNC_READ cannot use CFG_DUAL_CLOCK or CLK2.\n",
                              ctx->nameOf(ci));
                // An active clear selects the M10K output register, which is
                // incompatible with a combinational read result.  Omitted
                // controls have already been materialised and folded to
                // PIN_0 by ensure_m10k_control_ports/pack_constants.
                for (IdString clear : {id_ACLR0, id_ACLR1}) {
                    // A connected constant-zero control is folded to PIN_0
                    // before setup and is valid.  Reject active constants and
                    // fabric nets, which would select the M10K output
                    // register and change the read port back to synchronous.
                    if (ci->get_pin_state(clear) != PIN_0)
                        log_error("M10K '%s': CFG_ASYNC_READ requires inactive %s.\n", ctx->nameOf(ci),
                                  ctx->nameOf(clear));
                }

                // A flow-through simple-dual read with a constant rden_b is
                // represented by the absence of B1EN at the JSON boundary.
                // The internal port added above gives the physical BEL an
                // explicit constant-high ENABLE[0] route.
            }

            log_info("Setting up %ld-bit address, %ld-bit data M10K for %s.\n", abits, dbits,
                     ci->name.str(ctx).c_str());

            // Quartus usually ties ADDRSTALL[AB] low, but explicit primitive
            // users may drive either dedicated control from fabric.

            // It *does* generate ACLR[01]. Keep both logical inputs so the
            // bitstream can retain explicit reset constants.

            // Enables.
            bool byte_enable = bool_or_default(ci->params, id_CFG_BYTE_ENABLE, false);
            auto byte_enable_port = [&](int bit) { return ci->getPort(ctx->idf("A1BE[%d]", bit)); };
            if (byte_enable && dbits != 20)
                log_error("M10K '%s': CFG_BYTE_ENABLE is currently supported only for 20-bit data.\n",
                          ctx->nameOf(ci));
            if (byte_enable && (byte_enable_port(0) == nullptr || byte_enable_port(1) == nullptr))
                log_error("M10K '%s': CFG_BYTE_ENABLE requires a connected A1BE[1:0] port.\n",
                          ctx->nameOf(ci));
            if (!byte_enable && (byte_enable_port(0) != nullptr || byte_enable_port(1) != nullptr))
                log_error("M10K '%s': A1BE requires CFG_BYTE_ENABLE=1.\n", ctx->nameOf(ci));
            // The byte-enabled 20-bit mode uses positive WREN[0] and the
            // corresponding core-enable lane. Legacy cells retain the
            // established active-low WREN[1] mapping (or WREN[0] at 40 bit).
            if (byte_enable)
                ci->pin_data[id_A1EN].bel_pins = {ctx->id("WREN[0]"), ctx->id("ENABLE[1]")};
            else if (dbits == 40)
                ci->pin_data[ctx->id("A1EN")].bel_pins = {ctx->id("WREN[0]")};
            else
                ci->pin_data[ctx->id("A1EN")].bel_pins = {ctx->id("WREN[1]")};
            if (byte_enable) {
                for (int bit = 0; bit < 2; bit++) {
                    IdString port = ctx->idf("A1BE[%d]", bit);
                    // Quartus leaves constant-high byte enables on the
                    // M10K's default source.  Preserve that encoding for the
                    // flow-through read mode; a low or dynamic mask still
                    // needs an ordinary fabric route.
                    bool constant_high = ci->getPort(port) == vcc_net ||
                                         (ci->getPort(port) == nullptr && ci->get_pin_state(port) == PIN_1);
                    if (async_read && constant_high) {
                        ci->disconnectPort(port);
                        ci->pin_data[port].state = PIN_1;
                    } else {
                        ci->pin_data[port].bel_pins = {ctx->idf("BYTEENABLEA[%d]", bit)};
                    }
                }
            }

            // Legacy cells use CLK1 for both ports; new SDP cells have an
            // independent read clock on CLK2.
            bool dual_clock = bool_or_default(ci->params, id_CFG_DUAL_CLOCK, false);
            bool clk2_signal = ci->getPort(id_CLK2) != nullptr;
            bool clk2_constant = !clk2_signal &&
                                 (ci->get_pin_state(id_CLK2) == PIN_0 || ci->get_pin_state(id_CLK2) == PIN_1);
            if (async_read)
                ci->pin_data[id_B1EN].bel_pins = {ctx->id("ENABLE[0]")};
            else if (ci->getPort(id_B1EN) != nullptr)
                ci->pin_data[id_B1EN].bel_pins = {ctx->id(dual_clock ? "ENABLE[0]" : "RDEN[0]")};
            if (ci->getPort(id_CLK1) == nullptr) {
                // A read-only async memory has no write edge to route.  Its
                // mapper supplies an inactive A1EN and a folded constant
                // CLK1; accepting that shape avoids fabricating a clock just
                // to feed an unused write half of the M10K.
                bool clk1_constant = ci->get_pin_state(id_CLK1) == PIN_0 || ci->get_pin_state(id_CLK1) == PIN_1;
                bool a1en_low = ci->getPort(id_A1EN) == gnd_net || ci->get_pin_state(id_A1EN) == PIN_0;
                bool a1en_high = ci->getPort(id_A1EN) == vcc_net || ci->get_pin_state(id_A1EN) == PIN_1;
                bool write_disabled = dbits == 40 ? a1en_low : a1en_high;
                if (!(async_read && clk1_constant && write_disabled))
                    log_error("M10K '%s' requires a connected %s clock.\n", ctx->nameOf(ci),
                              "CLK1");
            }
            if (dual_clock && !clk2_signal && !clk2_constant)
                log_error("M10K '%s' requires a connected CLK2 clock or an explicit constant on an unused read port.\n",
                          ctx->nameOf(ci));
            if (clk2_constant && ci->get_pin_state(id_B1EN) != PIN_0 && ci->getPort(id_B1EN) != gnd_net)
                log_error("M10K '%s': constant CLK2 is only valid when B1EN is tied low.\n", ctx->nameOf(ci));
            if (!dual_clock && clk2_signal)
                log_error("M10K '%s': CLK2 requires CFG_DUAL_CLOCK=1.\n", ctx->nameOf(ci));
            // The M10K clock mux is split between the two physical halves.
            // Quartus feeds both CLKIN pins for a flow-through simple-dual
            // port, even when the two logical clocks are the same signal.
            // Keep the shared-clock legacy path on CLKIN[0], but fan an
            // asynchronous read clock onto both physical sinks so the read
            // half cannot remain on an unclocked/default branch.
            if (async_read)
                ci->pin_data[id_CLK1].bel_pins = {ctx->id("CLKIN[0]"), ctx->id("CLKIN[1]")};
            else
                ci->pin_data[id_CLK1].bel_pins = {ctx->id("CLKIN[0]")};
            if (dual_clock && clk2_signal)
                ci->pin_data[id_CLK2].bel_pins = {ctx->id("CLKIN[1]")};
            ci->pin_data[id_ACLR0].bel_pins = {ctx->id("ACLR[0]")};
            ci->pin_data[id_ACLR1].bel_pins = {ctx->id("ACLR[1]")};

            // Other clock-enable pins remain unconnected.

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
                if (ci->type.in(id_MISTRAL_MUL18X19, id_MISTRAL_MUL18X19_COMBINED) &&
                    (dsp_bool_param(ci->params, id_PREADDER_EN) || dsp_has_bus(ci, ctx, "Z")))
                    log_error("18x19 DSP modes do not support preadder ports.\n");
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
                if (ci->type == id_MISTRAL_MUL18X19 &&
                    (dsp_control_used(ci, id_ACCUMULATE) || dsp_control_used(ci, id_SUB) ||
                     dsp_control_used(ci, id_NEGATE) || dsp_control_used(ci, id_LOADCONST) ||
                     dsp_bool_param(ci->params, id_CASCADE_EN) || dsp_bool_param(ci->params, id_CASCADE_1ST_EN) ||
                     dsp_bool_param(ci->params, id_CHAIN_OUTPUT_EN)))
                    log_error("MISTRAL_MUL18X19 does not support accumulator or arithmetic controls.\n");
                if (ci->type == id_MISTRAL_MUL18X19_COMBINED &&
                    (dsp_control_used(ci, id_ACCUMULATE) || dsp_control_used(ci, id_NEGATE) ||
                     dsp_control_used(ci, id_LOADCONST) || dsp_bool_param(ci->params, id_CASCADE_EN) ||
                     dsp_bool_param(ci->params, id_CASCADE_1ST_EN) ||
                     dsp_bool_param(ci->params, id_CHAIN_OUTPUT_EN)))
                    log_error("MISTRAL_MUL18X19_COMBINED only supports the SUB control.\n");
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
            a_signed = dsp_bool_param(a->params, id_C_SIGNED, true);
            b_signed = dsp_bool_param(b->params, id_C_SIGNED, true);
            if (a_signed != b_signed)
                return a_signed < b_signed;
            a_signed = dsp_bool_param(a->params, id_D_SIGNED, true);
            b_signed = dsp_bool_param(b->params, id_D_SIGNED, true);
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
                log_error("PLL '%s': fractional-N selector requires a 50 MHz reference and a 1-100 MHz output with a bounded 400-500 MHz shared VCO.\n",
                          ctx->nameOf(ci));
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
                    log_error("PLL '%s': fractional-N dual selector requires a 50 MHz reference and exact shared-VCO output counters in the bounded 400-500 MHz window.\n",
                              ctx->nameOf(ci));
                if (!dual)
                    log_error("PLL '%s': unsupported dual PLL frequencies/duties; require exact decimal MHz from 1 to 100 with exact dividers from one checked 300/320/400/520 MHz tuple.\n", ctx->nameOf(ci));
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
                              "from one checked 300/320/400/520 MHz tuple.\n", ctx->nameOf(ci));
                config = multi->feedback;
                c1 = multi->counters[1];
            }
            if (!config)
                log_error("PLL '%s': unsupported PLL output frequency/duty; require exact decimal MHz from 1 to 100 "
                          "and an exact integer C divider from a checked 300/320/520 MHz tuple.\n", ctx->nameOf(ci));
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
        pack_altiobufs();
        pack_ddr_inputs();
        pack_sdr_inputs();
        pack_sdr_outputs();
        pack_ddr_outputs();
        pack_ddr_bidir();
        setup_clock_enables();
        setup_plls();
        fold_inverted_pll_clock_buffers();
        ensure_dsp_control_ports();
        ensure_m10k_control_ports();
        pack_constants();
        select_dsp_control_pinmaps();
        constrain_carries();
        constrain_lutram();
        setup_m10ks();
        trim_design();
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
