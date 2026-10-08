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
#include "timing.h"
#include "util.h"

#include <algorithm>

NEXTPNR_NAMESPACE_BEGIN

// This file contains functions related to our custom LAB structure, including creating the LAB bels; checking the
// legality of LABs; and manipulating LUT inputs and equations

// LAB/ALM structure creation functions
namespace {
static void create_alm(Arch *arch, int x, int y, int z, uint32_t lab_idx)
{
    auto &lab = arch->labs.at(lab_idx);
    auto &alm = lab.alms.at(z);
    auto block_type = lab.is_mlab ? CycloneV::MLAB : CycloneV::LAB;
    // Create the control set and E/F selection - each is shared by a pair of FFs. E/F (like the data path, SCLR and
    // SLOAD) is per ALM half, while the CLK/ENA and ACLR selectors are per control group (ALMInfo::ctrl_group).
    for (int i = 0; i < 2; i++) {
        // Wires
        alm.sel_clk[i] = arch->add_wire(x, y, arch->idf("CLK%c[%d]", i ? 'B' : 'T', z));
        alm.sel_ena[i] = arch->add_wire(x, y, arch->idf("ENA%c[%d]", i ? 'B' : 'T', z));
        alm.sel_aclr[i] = arch->add_wire(x, y, arch->idf("ACLR%c[%d]", i ? 'B' : 'T', z));
        alm.sel_ef[i] = arch->add_wire(x, y, arch->idf("%cEF[%d]", i ? 'B' : 'T', z));
        // Muxes - three CLK/ENA per LAB, two ACLR
        for (int j = 0; j < 3; j++) {
            arch->add_pip(lab.clk_wires[j], alm.sel_clk[i]);
            arch->add_pip(lab.ena_wires[j], alm.sel_ena[i]);
            if (j < 2)
                arch->add_pip(lab.aclr_wires[j], alm.sel_aclr[i]);
        }
        // E/F pips
        // Note that the F choice is mirrored, F from the other half is picked
        arch->add_pip(arch->get_port(block_type, x, y, z, i ? CycloneV::E1 : CycloneV::E0), alm.sel_ef[i]);
        arch->add_pip(arch->get_port(block_type, x, y, z, i ? CycloneV::F0 : CycloneV::F1), alm.sel_ef[i]);
    }
    // Create the combinational part of ALMs.
    // There are two of these, for the two LUT outputs, and these also contain the carry chain and associated logic
    // Each one has all 8 ALM inputs as input pins. In many cases only a subset of these are used; depending on mode;
    // and the bel-cell pin mappings are used to handle this post-placement without losing flexibility
    for (int i = 0; i < 2; i++) {
        // Carry/share wires are a bit tricky due to all the different permutations
        WireId carry_in, share_in;
        WireId carry_out, share_out;
        if (z == 0 && i == 0) {
            carry_in = arch->add_wire(x, y, id_CI);
            share_in = arch->add_wire(x, y, id_SHAREIN);
            if (y < (arch->getGridDimY() - 1)) {
                // Carry is split at tile boundary (TTO_DIS bit), add a PIP to represent this.
                // TODO: what about BTO_DIS, in the middle of the LAB?
                arch->add_pip(arch->add_wire(x, y + 1, id_CO), carry_in);
                arch->add_pip(arch->add_wire(x, y + 1, id_SHAREOUT), share_in);
            }
        } else {
            // Output from last combinational unit
            carry_in = arch->add_wire(x, y, arch->idf("CARRY[%d]", (z * 2 + i) - 1));
            share_in = arch->add_wire(x, y, arch->idf("SHARE[%d]", (z * 2 + i) - 1));
        }

        if (z == 9 && i == 1) {
            carry_out = arch->add_wire(x, y, id_CO);
            share_out = arch->add_wire(x, y, id_SHAREOUT);
        } else {
            carry_out = arch->add_wire(x, y, arch->idf("CARRY[%d]", z * 2 + i));
            share_out = arch->add_wire(x, y, arch->idf("SHARE[%d]", z * 2 + i));
        }

        BelId bel =
                arch->add_bel(x, y, arch->idf("ALM%d_COMB%d", z, i), lab.is_mlab ? id_MISTRAL_MCOMB : id_MISTRAL_COMB);
        // LUT/MUX inputs
        arch->add_bel_pin(bel, id_A, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::A));
        arch->add_bel_pin(bel, id_B, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::B));
        arch->add_bel_pin(bel, id_C, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::C));
        arch->add_bel_pin(bel, id_D, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::D));
        arch->add_bel_pin(bel, id_E0, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::E0));
        arch->add_bel_pin(bel, id_E1, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::E1));
        arch->add_bel_pin(bel, id_F0, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::F0));
        arch->add_bel_pin(bel, id_F1, PORT_IN, arch->get_port(block_type, x, y, z, CycloneV::F1));
        // Carry/share chain
        arch->add_bel_pin(bel, id_CI, PORT_IN, carry_in);
        arch->add_bel_pin(bel, id_SHAREIN, PORT_IN, share_in);
        arch->add_bel_pin(bel, id_CO, PORT_OUT, carry_out);
        arch->add_bel_pin(bel, id_SHAREOUT, PORT_OUT, share_out);
        // Combinational output
        alm.comb_out[i] = arch->add_wire(x, y, arch->idf("COMBOUT[%d]", z * 2 + i));
        arch->add_bel_pin(bel, id_COMBOUT, PORT_OUT, alm.comb_out[i]);
        if (lab.is_mlab) {
            // Write address - shared between all ALMs in a LAB
            arch->add_bel_pin(bel, id_WA0, PORT_IN, arch->get_port(block_type, x, y, 2, CycloneV::F1));
            arch->add_bel_pin(bel, id_WA1, PORT_IN, arch->get_port(block_type, x, y, 3, CycloneV::F1));
            arch->add_bel_pin(bel, id_WA2, PORT_IN, arch->get_port(block_type, x, y, 7, CycloneV::F1));
            arch->add_bel_pin(bel, id_WA3, PORT_IN, arch->get_port(block_type, x, y, 6, CycloneV::F1));
            arch->add_bel_pin(bel, id_WA4, PORT_IN, arch->get_port(block_type, x, y, 1, CycloneV::F1));
            // Write clock and enable appear to be based on bottom FF
            arch->add_bel_pin(bel, id_WCLK, PORT_IN, alm.sel_clk[1]);
            arch->add_bel_pin(bel, id_WE, PORT_IN, alm.sel_ena[1]);
        }
        // Assign indexing
        alm.lut_bels.at(i) = bel;
        auto &b = arch->bel_data(bel);
        b.lab_data.lab = lab_idx;
        b.lab_data.alm = z;
        b.lab_data.idx = i;
    }

    // Create the flipflops and associated routing
    const CycloneV::port_type_t outputs[4] = {CycloneV::FFT0, CycloneV::FFT1, CycloneV::FFB0, CycloneV::FFB1};
    const CycloneV::port_type_t l_outputs[4] = {CycloneV::FFT1L, CycloneV::FFB1L};

    for (int i = 0; i < 4; i++) {
        // FF input, selected by *PKREG*
        alm.ff_in[i] = arch->add_wire(x, y, arch->idf("FFIN[%d]", (z * 4) + i));
        arch->add_pip(alm.comb_out[i / 2], alm.ff_in[i]);
        arch->add_pip(alm.sel_ef[i / 2], alm.ff_in[i]);
        // FF bel. FF0 and FF3 take TCLK_SEL/TCLR_SEL, FF1 and FF2 take BCLK_SEL/BCLR_SEL.
        BelId bel = arch->add_bel(x, y, arch->idf("ALM%d_FF%d", z, i), id_MISTRAL_FF);
        const int group = ALMInfo::ctrl_group(i);
        arch->add_bel_pin(bel, id_CLK, PORT_IN, alm.sel_clk[group]);
        arch->add_bel_pin(bel, id_ENA, PORT_IN, alm.sel_ena[group]);
        arch->add_bel_pin(bel, id_ACLR, PORT_IN, alm.sel_aclr[group]);
        arch->add_bel_pin(bel, id_SCLR, PORT_IN, lab.sclr_wire);
        arch->add_bel_pin(bel, id_SLOAD, PORT_IN, lab.sload_wire);
        arch->add_bel_pin(bel, id_DATAIN, PORT_IN, alm.ff_in[i]);
        arch->add_bel_pin(bel, id_SDATA, PORT_IN, alm.sel_ef[i / 2]);

        // FF output
        alm.ff_out[i] = arch->add_wire(x, y, arch->idf("FFOUT[%d]", (z * 4) + i));
        arch->add_bel_pin(bel, id_Q, PORT_OUT, alm.ff_out[i]);
        // Output mux (*DFF*)
        WireId out = arch->get_port(block_type, x, y, z, outputs[i]);
        arch->add_pip(alm.ff_out[i], out);
        arch->add_pip(alm.comb_out[i / 2], out);
        // 'L' output mux where applicable
        if (i == 1 || i == 3) {
            WireId l_out = arch->get_port(block_type, x, y, z, l_outputs[i / 2]);
            arch->add_pip(alm.ff_out[i], l_out);
            arch->add_pip(alm.comb_out[i / 2], l_out);
        }

        lab.alms.at(z).ff_bels.at(i) = bel;
        auto &b = arch->bel_data(bel);
        b.lab_data.lab = lab_idx;
        b.lab_data.alm = z;
        b.lab_data.idx = i;
    }

    // TODO: MLAB-specific pins
}
} // namespace

void Arch::create_lab(int x, int y, bool is_mlab)
{
    uint32_t lab_idx = labs.size();
    labs.emplace_back();

    auto &lab = labs.back();

    lab.is_mlab = is_mlab;
    auto block_type = is_mlab ? CycloneV::MLAB : CycloneV::LAB;

    // Create common control set configuration. This is actually a subset of what's possible, but errs on the side of
    // caution due to incomplete documentation

    // Clocks - by default hardcode to CLKA choices, as both CLKA and CLKB coming from general routing causes
    // unexpected permutations. With --mistral-clkb each of the three LAB clocks (CLKk_SEL) may also take CLKB, which
    // comes from CLKIN[1] (dedicated) or DATAIN[1] (general, CLKB_SEL=DIN1). assign_control_sets reserves the exact
    // source of every used clock wire in that mode, so the router cannot mix CLKA/CLKB sources.
    for (int i = 0; i < 3; i++) {
        lab.clk_wires[i] = add_wire(x, y, idf("CLK%d", i));
        add_pip(get_port(block_type, x, y, -1, CycloneV::CLKIN, 0), lab.clk_wires[i]);  // dedicated routing
        add_pip(get_port(block_type, x, y, -1, CycloneV::DATAIN, 0), lab.clk_wires[i]); // general routing
        if (args.lab_clkb && !is_mlab) {
            add_pip(get_port(block_type, x, y, -1, CycloneV::CLKIN, 1), lab.clk_wires[i]);  // CLKB, dedicated
            add_pip(get_port(block_type, x, y, -1, CycloneV::DATAIN, 1), lab.clk_wires[i]); // CLKB, general
        }
    }

    // Enables - while it looks from the config like there are choices for these, it seems like EN0_SEL actually selects
    // SCLR not ENA0 and EN1_SEL actually selects SLOAD?
    lab.ena_wires[0] = get_port(block_type, x, y, -1, CycloneV::DATAIN, 2);
    lab.ena_wires[1] = get_port(block_type, x, y, -1, CycloneV::DATAIN, 3);
    lab.ena_wires[2] = get_port(block_type, x, y, -1, CycloneV::DATAIN, 0);

    // ACLRs - only consider general routing for now
    lab.aclr_wires[0] = get_port(block_type, x, y, -1, CycloneV::DATAIN, 3);
    lab.aclr_wires[1] = get_port(block_type, x, y, -1, CycloneV::DATAIN, 2);

    // SCLR and SLOAD - as above it seems like these might be selectable using the "EN*_SEL" bits but play it safe for
    // now
    lab.sclr_wire = get_port(block_type, x, y, -1, CycloneV::DATAIN, 3);
    lab.sload_wire = get_port(block_type, x, y, -1, CycloneV::DATAIN, 1);

    for (int i = 0; i < 10; i++) {
        create_alm(this, x, y, i, lab_idx);
    }
}

// Cell handling and annotation functions
namespace {
ControlSig get_ctrlsig(const Context *ctx, const CellInfo *cell, IdString port, bool explicit_const = false)
{
    ControlSig result;
    result.net = cell->getPort(port);
    if (result.net == nullptr && explicit_const) {
        // For ENA, 1 (and 0) are explicit control set choices even though they aren't routed, as "no ENA" still
        // consumes a clock+ENA pair
        CellPinState st = PIN_1;
        result.net = ctx->nets.at((st == PIN_1) ? ctx->id("$PACKER_VCC_NET") : ctx->id("$PACKER_GND_NET")).get();
    }
    if (cell->pin_data.count(port))
        result.inverted = cell->pin_data.at(port).state == PIN_INV;
    else
        result.inverted = false;
    return result;
}
} // namespace

bool Arch::is_comb_cell(IdString cell_type) const
{
    // Return true if a cell is a combinational cell type, to be a placed at a MISTRAL_COMB location
    switch (cell_type.index) {
    case ID_MISTRAL_ALUT6:
    case ID_MISTRAL_ALUT5:
    case ID_MISTRAL_ALUT4:
    case ID_MISTRAL_ALUT3:
    case ID_MISTRAL_ALUT2:
    case ID_MISTRAL_NOT:
    case ID_MISTRAL_CONST:
    case ID_MISTRAL_ALUT_ARITH:
        return true;
    default:
        return false;
    }
}

dict<IdString, IdString> Arch::get_mlab_key(const CellInfo *cell, bool include_raddr) const
{
    dict<IdString, IdString> key;
    for (auto &port : cell->ports) {
        if (port.first.in(id_A1DATA, id_B1DATA))
            continue;
        if (!include_raddr && port.first.str(this).find("B1ADDR") == 0)
            continue;
        key[port.first] = port.second.net ? port.second.net->name : IdString();
    }
    if (cell->pin_data.count(id_CLK1) && cell->pin_data.at(id_CLK1).state == PIN_INV)
        key[id_WCLK_INV] = id_Y;
    if (cell->pin_data.count(id_A1EN) && cell->pin_data.at(id_A1EN).state == PIN_INV)
        key[id_WE_INV] = id_Y;
    return key;
}

void Arch::assign_comb_info(CellInfo *cell) const
{
    cell->combInfo.is_carry = false;
    cell->combInfo.is_shared = false;
    cell->combInfo.is_extended = false;
    cell->combInfo.carry_start = false;
    cell->combInfo.carry_end = false;
    cell->combInfo.chain_shared_input_count = 0;
    cell->combInfo.mlab_group = -1;

    if (cell->type == id_MISTRAL_MLAB) {
        cell->combInfo.wclk = get_ctrlsig(getCtx(), cell, id_CLK1);
        cell->combInfo.we = get_ctrlsig(getCtx(), cell, id_A1EN, true);
        cell->combInfo.lut_input_count = 5;
        cell->combInfo.lut_bits_count = 32;
        for (int i = 0; i < 5; i++)
            cell->combInfo.lut_in[i] = cell->getPort(idf("B1ADDR[%d]", i));
        auto key = get_mlab_key(cell);
        cell->combInfo.mlab_group = mlab_groups(key);
        cell->combInfo.comb_out = cell->getPort(id_B1DATA);
    } else if (cell->type == id_MISTRAL_ALUT_ARITH) {
        cell->combInfo.is_carry = true;
        cell->combInfo.lut_input_count = 5;
        cell->combInfo.lut_bits_count = 32;

        // This is a special case in terms of naming
        const std::array<IdString, 5> arith_pins{id_A, id_B, id_C, id_D0, id_D1};
        {
            int i = 0;
            for (auto pin : arith_pins) {
                cell->combInfo.lut_in[i++] = cell->getPort(pin);
            }
        }

        const NetInfo *ci = cell->getPort(id_CI);
        const NetInfo *co = cell->getPort(id_CO);

        cell->combInfo.comb_out = cell->getPort(id_SO);
        cell->combInfo.carry_start = (ci == nullptr) || (ci->driver.cell == nullptr);
        cell->combInfo.carry_end = (co == nullptr) || (co->users.empty());

        // Compute cross-ALM routing sharing - only check the z=0 case inside ALMs
        if (cell->constr_z > 0 && ((cell->constr_z % 2) == 0) && ci) {
            const CellInfo *prev = ci->driver.cell;
            if (prev != nullptr) {
                for (int i = 0; i < 5; i++) {
                    const NetInfo *a = cell->getPort(arith_pins[i]);
                    if (a == nullptr)
                        continue;
                    const NetInfo *b = prev->getPort(arith_pins[i]);
                    if (a == b)
                        ++cell->combInfo.chain_shared_input_count;
                }
            }
        }

    } else {
        cell->combInfo.comb_out = cell->getPort(id_Q);
        cell->combInfo.lut_input_count = 0;
        switch (cell->type.index) {
        case ID_MISTRAL_ALUT6:
            ++cell->combInfo.lut_input_count;
            cell->combInfo.lut_in[5] = cell->getPort(id_F);
            [[fallthrough]];
        case ID_MISTRAL_ALUT5:
            ++cell->combInfo.lut_input_count;
            cell->combInfo.lut_in[4] = cell->getPort(id_E);
            [[fallthrough]];
        case ID_MISTRAL_ALUT4:
            ++cell->combInfo.lut_input_count;
            cell->combInfo.lut_in[3] = cell->getPort(id_D);
            [[fallthrough]];
        case ID_MISTRAL_ALUT3:
            ++cell->combInfo.lut_input_count;
            cell->combInfo.lut_in[2] = cell->getPort(id_C);
            [[fallthrough]];
        case ID_MISTRAL_ALUT2:
            ++cell->combInfo.lut_input_count;
            cell->combInfo.lut_in[1] = cell->getPort(id_B);
            [[fallthrough]];
        case ID_MISTRAL_BUF: // used to route through to FFs etc
        case ID_MISTRAL_NOT: // used for inverters that map to LUTs
            ++cell->combInfo.lut_input_count;
            cell->combInfo.lut_in[0] = cell->getPort(id_A);
            [[fallthrough]];
        case ID_MISTRAL_CONST:
            // MISTRAL_CONST is a nextpnr-inserted cell type for 0-input, constant-generating LUTs
            break;
        default:
            log_error("unexpected combinational cell type %s\n", getCtx()->nameOf(cell->type));
        }
        // Note that this relationship won't hold for extended mode, when that is supported
        cell->combInfo.lut_bits_count = (1 << cell->combInfo.lut_input_count);
    }
    cell->combInfo.used_lut_input_count = 0;
    for (int i = 0; i < cell->combInfo.lut_input_count; i++)
        if (cell->combInfo.lut_in[i])
            ++cell->combInfo.used_lut_input_count;
}

void Arch::assign_ff_info(CellInfo *cell) const
{
    cell->ffInfo.ctrlset.clk = get_ctrlsig(getCtx(), cell, id_CLK);
    cell->ffInfo.ctrlset.ena = get_ctrlsig(getCtx(), cell, id_ENA, true);
    cell->ffInfo.ctrlset.aclr = get_ctrlsig(getCtx(), cell, id_ACLR);
    cell->ffInfo.ctrlset.sclr = get_ctrlsig(getCtx(), cell, id_SCLR);
    cell->ffInfo.ctrlset.sload = get_ctrlsig(getCtx(), cell, id_SLOAD);
    // If SCLR is used, but SLOAD isn't, then it seems like we need to pretend as if SLOAD is connected GND (so set
    // [BT]SLOAD_EN inside the ALMs, and clear SLOAD_INV)
    if (cell->ffInfo.ctrlset.sclr.net != nullptr && cell->ffInfo.ctrlset.sload.net == nullptr) {
        cell->ffInfo.ctrlset.sload.net = nets.at(id("$PACKER_GND_NET")).get();
        cell->ffInfo.ctrlset.sload.inverted = false;
    }

    cell->ffInfo.sdata = cell->getPort(id_SDATA);
    cell->ffInfo.datain = cell->getPort(id_DATAIN);
}

// Validity checking functions
bool Arch::is_alm_legal(uint32_t lab, uint8_t alm) const
{
    auto &alm_data = labs.at(lab).alms.at(alm);
    // Get cells into an array for fast access
    std::array<const CellInfo *, 2> luts{getBoundBelCell(alm_data.lut_bels[0]), getBoundBelCell(alm_data.lut_bels[1])};
    std::array<const CellInfo *, 4> ffs{getBoundBelCell(alm_data.ff_bels[0]), getBoundBelCell(alm_data.ff_bels[1]),
                                        getBoundBelCell(alm_data.ff_bels[2]), getBoundBelCell(alm_data.ff_bels[3])};
    int used_lut_bits = 0;

    int total_lut_inputs = 0;
    // TODO: for more complex modes like extended/arithmetic, it might not always be possible for any LUT input to map
    // to any of the ALM half inputs particularly shared and extended mode will need more thought and probably for this
    // to be revisited
    for (int i = 0; i < 2; i++) {
        if (!luts[i])
            continue;
        total_lut_inputs += luts[i]->combInfo.lut_input_count;
        used_lut_bits += luts[i]->combInfo.lut_bits_count;
    }
    // An ALM only has 64 bits of storage. In theory some of these cases might be legal because of overlap between the
    // two functions, but the current placer is unlikely to stumble upon these cases frequently without anything to
    // guide it, and the cost of checking them here almost certainly outweighs any marginal benefit in supporting them,
    // at least for now.
    if (used_lut_bits > 64)
        return false;

    if (total_lut_inputs > 8) {
        NPNR_ASSERT(luts[0] && luts[1]); // something has gone badly wrong if this fails!
        // Make sure that LUT inputs are not overprovisioned
        int shared_lut_inputs = 0;
        // Even though this N^2 search looks inefficient, it's unlikely a set lookup or similar is going to be much
        // better given the low N.
        for (int i = 0; i < luts[1]->combInfo.lut_input_count; i++) {
            const NetInfo *sig = luts[1]->combInfo.lut_in[i];
            for (int j = 0; j < luts[0]->combInfo.lut_input_count; j++) {
                if (sig == luts[0]->combInfo.lut_in[j]) {
                    ++shared_lut_inputs;
                    break;
                }
            }
        }
        if ((total_lut_inputs - shared_lut_inputs) > 8)
            return false;
    }

    bool carry_mode = (luts[0] && luts[0]->combInfo.is_carry) || (luts[1] && luts[1]->combInfo.is_carry);

    // No mixing of carry and non-carry
    if (luts[0] && luts[1] && luts[0]->combInfo.is_carry != luts[1]->combInfo.is_carry)
        return false;

    // For each ALM half; check FF control set sharing and input routeability
    for (int i = 0; i < 2; i++) {
        // There are two ways to route from the fabric into FF data - either routing through a LUT or using the E/F
        // signals and SLOAD=1 (*PKREF*)
        bool route_thru_lut_avail = !luts[i] && !carry_mode && (total_lut_inputs < 8) && (used_lut_bits < 64);
        // E/F is available if this LUT is using 3 or fewer inputs - this is conservative and sharing can probably
        // improve this situation. (1 - i) because the F input to EF_SEL is mirrored.
        bool ef_available = (!luts[1 - i] || (luts[1 - i]->combInfo.used_lut_input_count <= 2));
        if (lab_ff4 && carry_mode && ef_available) {
            // EF_SEL of half i picks E_i or the other half's F. An arithmetic LUT reserves E for D0 and F for D1
            // regardless of its input count, so check those pins exactly.
            auto arith_uses = [&](int half, int input) {
                return luts[half] != nullptr && luts[half]->combInfo.lut_in[input] != nullptr;
            };
            ef_available = !arith_uses(i, 3) || !arith_uses(1 - i, 4);
        }
        // Control set checking
        bool found_ff = false;

        FFControlSet ctrlset;
        for (int j = 0; j < 2; j++) {
            const CellInfo *ff = ffs[i * 2 + j];
            if (!ff)
                continue;
            // FF1 and FF3 (the secondary registers) take their clock, enable and async clear from the other half's
            // selectors (see ALMInfo::ctrl_group). Only the --mistral-ff4 model below accounts for that.
            // MLAB clock/clear tables in Mistral are not Quartus-verified, so MLABs keep the two-FF model.
            if (j == 1 && (!lab_ff4 || labs.at(lab).is_mlab))
                return false;
            if (found_ff) {
                // Two FFs in the same half share EF_SEL, SCLR_DIS and SLOAD_EN, but not the clock/enable or async
                // clear selection; those are checked per control group below.
                if (!(ctrlset.sclr == ff->ffInfo.ctrlset.sclr) || !(ctrlset.sload == ff->ffInfo.ctrlset.sload))
                    return false;
                // A real sync load makes both FFs load the half's single E/F signal. Yosys never infers SLOAD; keep
                // such FFs (and any SDATA user) alone in their half rather than prove the shared-SDATA cases.
                if (ff4_has_sync_load(ctrlset) || ff->ffInfo.sdata != nullptr || ffs[i * 2]->ffInfo.sdata != nullptr)
                    return false;
                // Both FFs of the half own its two general-routing outputs (FFx0, FFx1) and the local FFx1L.
                // Quartus rejects any other use of this half's LUT output, even within the LAB.
                if (luts[i] && !ff4_lut_feeds_only(luts[i], ffs[i * 2], ff))
                    return false;
            } else {
                ctrlset = ff->ffInfo.ctrlset;
            }
            // SDATA must use the E/F input
            // TODO: rare case of two FFs with the same SDATA in the same ALM half
            if (ff->ffInfo.sdata) {
                if (!ef_available)
                    return false;
                ef_available = false;
            }
            // Find a way of routing the input through fabric, if it's not driven by the LUT
            if (ff->ffInfo.datain && (!luts[i] || (ff->ffInfo.datain != luts[i]->combInfo.comb_out))) {
                bool helper_allowed = route_thru_lut_avail;
                if (helper_allowed && !fes_bel_region.empty()) {
                    CellInfo candidate(const_cast<Context *>(getCtx()), ff->name, id_MISTRAL_BUF);
                    auto slot = ff->attrs.find(id("FES_SLOT"));
                    if (slot != ff->attrs.end())
                        candidate.attrs[id("FES_SLOT")] = slot->second;
                    helper_allowed = fes_placement_allowed(alm_data.lut_bels[i], &candidate);
                }
                if (helper_allowed)
                    route_thru_lut_avail = false;
                else if (ef_available)
                    ef_available = false;
                else
                    return false;
            }
            found_ff = true;
        }
    }

    if (lab_ff4) {
        // TCLK_SEL/TCLR_SEL serve FF0 and FF3, BCLK_SEL/BCLR_SEL serve FF1 and FF2. Each selector picks one LAB
        // clock+enable pair and one async clear slot; an FF without an async clear still follows its group's
        // TCLR_SEL/BCLR_SEL, so a missing clear is also a mismatch.
        for (auto group : {std::make_pair(0, 3), std::make_pair(1, 2)}) {
            const CellInfo *a = ffs[group.first], *b = ffs[group.second];
            if (a == nullptr || b == nullptr)
                continue;
            const auto &x = a->ffInfo.ctrlset, &y = b->ffInfo.ctrlset;
            if (!(x.clk == y.clk) || !(x.ena == y.ena) || !(x.aclr == y.aclr))
                return false;
        }
    }

    return true;
}

bool Arch::ff4_has_sync_load(const FFControlSet &ctrlset) const
{
    // assign_ff_info ties SLOAD to the packer GND net when only SCLR is used; that is not a real load.
    return ctrlset.sload.net != nullptr && ctrlset.sload.net->name != id("$PACKER_GND_NET");
}

bool Arch::ff4_lut_feeds_only(const CellInfo *lut, const CellInfo *ff_a, const CellInfo *ff_b) const
{
    const NetInfo *out = lut->combInfo.comb_out;
    if (out == nullptr)
        return true;
    for (const auto &user : out->users) {
        if ((user.cell != ff_a && user.cell != ff_b) || user.port != id_DATAIN)
            return false;
    }
    return true;
}

void Arch::update_alm_input_count(uint32_t lab, uint8_t alm)
{
    // TODO: duplication with above
    auto &alm_data = labs.at(lab).alms.at(alm);
    // Get cells into an array for fast access
    std::array<const CellInfo *, 2> luts{getBoundBelCell(alm_data.lut_bels[0]), getBoundBelCell(alm_data.lut_bels[1])};
    std::array<const CellInfo *, 4> ffs{getBoundBelCell(alm_data.ff_bels[0]), getBoundBelCell(alm_data.ff_bels[1]),
                                        getBoundBelCell(alm_data.ff_bels[2]), getBoundBelCell(alm_data.ff_bels[3])};
    auto lut_input_count = [](const CellInfo *cell) -> int {
        if (cell == nullptr)
            return -1;
        // JSON reload bindBel runs before assignArchInfo fills combInfo.
        int n = cell->combInfo.lut_input_count;
        if (n < 0 || n > int(cell->combInfo.lut_in.size()))
            return -1;
        return n;
    };
    int total_inputs = 0;
    int total_lut_inputs = 0;
    for (int i = 0; i < 2; i++) {
        if (!luts[i] || lut_input_count(luts[i]) < 0)
            continue;
        // MLAB that has been clustered with other MLABs (due to shared read port) costs no extra inputs
        if (luts[i]->combInfo.mlab_group != -1 && luts[i]->constr_z > 2) {
            alm_data.unique_input_count = 0;
            return;
        }

        total_lut_inputs += luts[i]->combInfo.used_lut_input_count - luts[i]->combInfo.chain_shared_input_count;
    }
    int shared_lut_inputs = 0;
    const int n0 = lut_input_count(luts[0]);
    const int n1 = lut_input_count(luts[1]);
    if (n0 >= 0 && n1 >= 0) {
        for (int i = 0; i < n1; i++) {
            const NetInfo *sig = luts[1]->combInfo.lut_in[i];
            if (!sig)
                continue;
            for (int j = 0; j < n0; j++) {
                if (sig == luts[0]->combInfo.lut_in[j]) {
                    ++shared_lut_inputs;
                    break;
                }
            }
            if (shared_lut_inputs >= 2 && luts[0]->combInfo.mlab_group == -1) {
                // only 2 inputs have guaranteed sharing in non-MLAB mode, without routeability based LUT permutation at
                // least
                break;
            }
        }
    }
    total_inputs = std::max(0, total_lut_inputs - shared_lut_inputs);
    for (int i = 0; i < 4; i++) {
        const CellInfo *ff = ffs[i];
        if (!ff)
            continue;
        if (ff->ffInfo.sdata)
            ++total_inputs;
        // FF input doesn't consume routing resources if driven by associated LUT
        if (ff->ffInfo.datain && (!luts[i / 2] || ff->ffInfo.datain != luts[i / 2]->combInfo.comb_out))
            ++total_inputs;
    }
    alm_data.unique_input_count = total_inputs;
}

bool Arch::check_lab_input_count(uint32_t lab) const
{
    // There are only 46 TD signals available to route signals from general routing to the ALM input. Currently, we
    // check the total sum of ALM inputs is less than 42; 46 minus 4 FF control inputs. This is a conservative check for
    // several reasons, because LD signals are also available for feedback routing from ALM output to input, and because
    // TD signals may be shared if the same net routes to multiple ALMs. But these cases will need careful handling and
    // LUT permutation during routing to be useful; and in any event conservative LAB packing will help nextpnr's
    // currently perfunctory place and route algorithms to achieve satisfactory runtimes.
    int count = 0;
    auto &lab_data = labs.at(lab);
    for (int i = 0; i < 10; i++) {
        count += lab_data.alms.at(i).unique_input_count;
    }
    return (count <= 42);
}

bool Arch::check_mlab_groups(uint32_t lab) const
{
    auto &lab_data = labs.at(lab);
    if (!lab_data.is_mlab)
        return true;
    int found_group = -2;
    for (const auto &alm_data : lab_data.alms) {
        std::array<const CellInfo *, 2> luts{getBoundBelCell(alm_data.lut_bels[0]),
                                             getBoundBelCell(alm_data.lut_bels[1])};
        for (const CellInfo *lut : luts) {
            if (!lut)
                continue;
            if (found_group == -2)
                found_group = lut->combInfo.mlab_group;
            else if (found_group != lut->combInfo.mlab_group)
                return false;
        }
    }
    if (found_group >= 0) {
        for (const auto &alm_data : lab_data.alms) {
            std::array<const CellInfo *, 4> ffs{
                    getBoundBelCell(alm_data.ff_bels[0]), getBoundBelCell(alm_data.ff_bels[1]),
                    getBoundBelCell(alm_data.ff_bels[2]), getBoundBelCell(alm_data.ff_bels[3])};
            for (const CellInfo *ff : ffs) {
                if (ff)
                    return false; // be conservative and don't allow LUTRAMs and FFs together
            }
        }
    }
    return true;
}

namespace {
bool check_assign_sig(ControlSig &sig_set, const ControlSig &sig)
{
    if (sig.net == nullptr) {
        return true;
    } else if (sig_set == sig) {
        return true;
    } else if (sig_set.net == nullptr) {
        sig_set = sig;
        return true;
    } else {
        return false;
    }
};

template <size_t N> bool check_assign_sig(std::array<ControlSig, N> &sig_set, const ControlSig &sig)
{
    if (sig.net == nullptr)
        return true;
    for (size_t i = 0; i < N; i++)
        if (sig_set[i] == sig) {
            return true;
        } else if (sig_set[i].net == nullptr) {
            sig_set[i] = sig;
            return true;
        }
    return false;
};

// DATAIN mapping rules - which LAB DATAIN signals can be used for ENA and ACLR
static constexpr std::array<int, 3> ena_datain{2, 3, 0};
static constexpr std::array<int, 2> aclr_datain{3, 2};

struct LabCtrlSetWorker
{

    ControlSig clk{}, sload{}, sclr{};
    std::array<ControlSig, 2> aclr{};
    std::array<ControlSig, 3> ena{};

    std::array<ControlSig, 4> datain{};

    bool run(const Arch *arch, uint32_t lab)
    {
        // Strictly speaking the constraint is up to 2 unique CLK and 3 CLK+ENA pairs. For now we simplify this to 1 CLK
        // and 3 ENA though.
        bool open_aclr = false;
        for (uint8_t alm = 0; alm < 10; alm++) {
            for (uint8_t i = 0; i < 4; i++) {
                const CellInfo *ff = arch->getBoundBelCell(arch->labs.at(lab).alms.at(alm).ff_bels.at(i));
                if (ff == nullptr)
                    continue;
                if (!check_assign_sig(clk, ff->ffInfo.ctrlset.clk))
                    return false;
                if (!check_assign_sig(sload, ff->ffInfo.ctrlset.sload))
                    return false;
                if (!check_assign_sig(sclr, ff->ffInfo.ctrlset.sclr))
                    return false;
                if (!check_assign_sig(aclr, ff->ffInfo.ctrlset.aclr))
                    return false;
                if (!check_assign_sig(ena, ff->ffInfo.ctrlset.ena))
                    return false;
                if (ff->ffInfo.ctrlset.aclr.net == nullptr)
                    open_aclr = true;
            }
        }
        // Check for overuse of the shared, LAB-wide datain signals
        if (clk.net != nullptr && !clk.net->is_global)
            if (!check_assign_sig(datain[0], clk)) // CLK only needs DATAIN[0] if it's not global
                return false;
        if (!check_assign_sig(datain[1], sload))
            return false;
        if (!check_assign_sig(datain[3], sclr))
            return false;
        for (const auto &aclr_sig : aclr) {
            // Check both possibilities that ACLR can map to
            // TODO: ACLR could be global, too
            if (check_assign_sig(datain[aclr_datain[0]], aclr_sig))
                continue;
            if (check_assign_sig(datain[aclr_datain[1]], aclr_sig))
                continue;
            // Failed to find any free ACLR-capable DATAIN
            return false;
        }
        for (const auto &ena_sig : ena) {
            // Check all 3 possibilities that ACLR can map to
            // TODO: ACLR could be global, too
            if (check_assign_sig(datain[ena_datain[0]], ena_sig))
                continue;
            if (check_assign_sig(datain[ena_datain[1]], ena_sig))
                continue;
            if (check_assign_sig(datain[ena_datain[2]], ena_sig))
                continue;
            // Failed to find any free ENA-capable DATAIN
            return false;
        }
        // An open ACLR pin still follows the half's clear slot. Both slots
        // already carrying a clear leaves that flop on a live reset.
        if (open_aclr && aclr[0].net != nullptr && aclr[1].net != nullptr)
            return false;
        return true;
    }
};

// Opt-in (--mistral-ff4/--mistral-clkb) control-set model. A LAB has two clock sources, CLKA (CLKIN[0] or DATAIN[0]) and CLKB (CLKIN[1] or
// DATAIN[1]), and three clock+enable pairs k; pair k takes CLKA or CLKB (CLKk_SEL), its own polarity (CLKk_INV) and its
// own enable, which comes from DATAIN[ena_datain[k]] only when ENk_EN is set. Quartus 17.0.2 uses exactly this for two
// clocks and for clk/~clk in one LAB (mistral/tests/lab_ff4).
// A clock net that route_globals routes over the dedicated clock network reaches the LAB on CLKIN; any other clock
// comes from the fabric on DATAIN. (NetInfo::is_global is not maintained by this architecture.)
bool dedicated_clock(const NetInfo *net)
{
    return net != nullptr && net->driver.cell != nullptr && net->driver.port == id_Q &&
           net->driver.cell->type.in(id_MISTRAL_CLKENA, id_MISTRAL_CLKBUF);
}

struct LabPairWorker
{
    struct Pair
    {
        ControlSig clk, ena;
    };
    // Results, indexed by hardware pair k (CLKk/ENk) and clock source (0 = CLKA, 1 = CLKB)
    std::array<Pair, 3> pairs{};
    std::array<bool, 3> pair_used{};
    std::array<int, 3> pair_source{-1, -1, -1};
    std::array<const NetInfo *, 2> source{};
    ControlSig sload{}, sclr{};
    std::array<ControlSig, 2> aclr{};
    std::array<ControlSig, 4> datain{};
    int max_clocks = 2;
    // assign_ff_info's placeholders: ENA tied to VCC means no enable, SLOAD tied to GND is the SCLR-only workaround
    IdString vcc_name, gnd_name;

    bool real_enable(const ControlSig &ena) const { return ena.net != nullptr && ena.net->name != vcc_name; }
    bool real_load(const ControlSig &load) const { return load.net != nullptr && load.net->name != gnd_name; }

    int pair_index(const FFControlSet &ctrlset) const
    {
        for (int k = 0; k < 3; k++)
            if (pair_used[k] && pairs[k].clk == ctrlset.clk && pairs[k].ena == ctrlset.ena)
                return k;
        return -1;
    }

    bool run(const Arch *arch, uint32_t lab)
    {
        vcc_name = arch->id("$PACKER_VCC_NET");
        gnd_name = arch->id("$PACKER_GND_NET");
        std::vector<Pair> found;
        std::vector<const NetInfo *> clocks;
        int edge = -1;
        bool open_aclr = false;
        for (uint8_t alm = 0; alm < 10; alm++) {
            for (uint8_t i = 0; i < 4; i++) {
                const CellInfo *ff = arch->getBoundBelCell(arch->labs.at(lab).alms.at(alm).ff_bels.at(i));
                if (ff == nullptr)
                    continue;
                const auto &cs = ff->ffInfo.ctrlset;
                if (!check_assign_sig(sload, cs.sload) || !check_assign_sig(sclr, cs.sclr) ||
                    !check_assign_sig(aclr, cs.aclr))
                    return false;
                if (cs.aclr.net == nullptr)
                    open_aclr = true;
                if (std::none_of(found.begin(), found.end(),
                                 [&](const Pair &p) { return p.clk == cs.clk && p.ena == cs.ena; })) {
                    if (found.size() == 3)
                        return false; // three clock+enable pairs per LAB
                    found.push_back(Pair{cs.clk, cs.ena});
                }
                if (cs.clk.net != nullptr && std::find(clocks.begin(), clocks.end(), cs.clk.net) == clocks.end()) {
                    if (int(clocks.size()) == max_clocks)
                        return false; // two clock signals per LAB (CLKA, CLKB), one without --mistral-clkb
                    clocks.push_back(cs.clk.net);
                }
                // Mixed clock edges in one LAB (per-pair CLKk_INV) are part of --mistral-clkb
                if (max_clocks == 1 && cs.clk.net != nullptr) {
                    if (edge < 0)
                        edge = cs.clk.inverted;
                    else if (edge != int(cs.clk.inverted))
                        return false;
                }
            }
        }
        // LAB-wide DATAIN users that do not depend on the pair assignment. The SLOAD that assign_ff_info ties to GND
        // for SCLR-only flops is disabled by SLOAD_EN=0 and leaves DATAIN[1] free, as in Quartus.
        if (real_load(sload) && !check_assign_sig(datain[1], sload))
            return false;
        if (!check_assign_sig(datain[3], sclr))
            return false;
        for (const auto &aclr_sig : aclr) {
            if (check_assign_sig(datain[aclr_datain[0]], aclr_sig))
                continue;
            if (check_assign_sig(datain[aclr_datain[1]], aclr_sig))
                continue;
            return false;
        }
        if (open_aclr && aclr[0].net != nullptr && aclr[1].net != nullptr)
            return false;
        // Try both CLKA/CLKB orders, then every placement of the pairs on the three hardware pairs, keeping the first
        // that fits the DATAIN lines. Without --mistral-clkb the only clock source is CLKA (DATAIN[0] / CLKIN[0]);
        // swap 1 would park a fabric clock on CLKB and assign_control_sets would reserve a pip that was never added.
        for (int swap = 0; swap < (max_clocks > 1 ? 2 : 1); swap++) {
            if (swap == 1 && clocks.empty())
                break;
            std::array<const NetInfo *, 2> src{};
            for (size_t c = 0; c < clocks.size(); c++)
                src[(c + swap) % 2] = clocks[c];
            auto din = datain;
            bool ok = true;
            for (int s = 0; s < 2 && ok; s++)
                if (src[s] != nullptr && !dedicated_clock(src[s]))
                    ok = check_assign_sig(din[s], ControlSig{src[s], false}); // CLKA_SEL/CLKB_SEL = DIN0/DIN1
            if (!ok)
                continue;
            std::array<int, 3> perm{0, 1, 2};
            do {
                // found[n] goes to hardware pair perm[n]
                auto try_din = din;
                bool fits = true;
                for (size_t n = 0; n < found.size() && fits; n++)
                    if (real_enable(found[n].ena))
                        fits = check_assign_sig(try_din[ena_datain[perm[n]]], found[n].ena);
                if (!fits)
                    continue;
                pair_used = {false, false, false};
                pair_source = {-1, -1, -1};
                for (size_t n = 0; n < found.size(); n++) {
                    int k = perm[n];
                    pair_used[k] = true;
                    pairs[k] = found[n];
                    if (found[n].clk.net != nullptr)
                        pair_source[k] = found[n].clk.net == src[0] ? 0 : 1;
                }
                source = src;
                datain = try_din;
                return true;
            } while (std::next_permutation(perm.begin(), perm.end()));
        }
        return false;
    }
};

}; // namespace

bool Arch::lab_pair_model(uint32_t lab) const { return (args.lab_clkb || lab_ff4) && !labs.at(lab).is_mlab; }

bool Arch::is_lab_ctrlset_legal(uint32_t lab) const
{
    if (lab_pair_model(lab)) {
        LabPairWorker worker;
        worker.max_clocks = args.lab_clkb ? 2 : 1;
        return worker.run(this, lab);
    }
    LabCtrlSetWorker worker;
    return worker.run(this, lab);
}

void Arch::lab_pre_route()
{
    log_info("Preparing LABs for routing...\n");
    std::vector<uint32_t> fresh_labs;
    // A user BEL lock fixes the site on a fresh route. LUT pins are still
    // reassigned, and a flip-flop with no LUT still gets a data route-through.
    // A scaffold reload locks the restored cells at STRENGTH_LOCKED. Rewriting
    // that LAB clears the restored pin map and can disconnect the flip-flop's
    // DATAIN, so the LUT mask no longer matches the frozen routes.
    for (uint32_t lab = 0; lab < labs.size(); lab++) {
        bool scaffold = false;
        for (uint8_t alm = 0; alm < 10 && !scaffold; alm++) {
            const auto &alm_data = labs.at(lab).alms.at(alm);
            for (BelId bel : {alm_data.lut_bels[0], alm_data.lut_bels[1], alm_data.ff_bels[0], alm_data.ff_bels[1],
                              alm_data.ff_bels[2], alm_data.ff_bels[3]}) {
                CellInfo *cell = getBoundBelCell(bel);
                if (cell != nullptr && cell->belStrength == STRENGTH_LOCKED) {
                    scaffold = true;
                    break;
                }
            }
        }
        if (scaffold)
            continue;
        fresh_labs.push_back(lab);
        assign_control_sets(lab);
        for (uint8_t alm = 0; alm < 10; alm++)
            reassign_alm_inputs(lab, alm);
    }
    // Experimental heuristic: use a single arrival snapshot after legalising
    // the pin maps. Final routing and setup/hold analysis must qualify it.
    if (arrival_pin_assignment) {
        TimingAnalyser timing(getCtx());
        timing.setup();
        int changed = 0;
        for (uint32_t lab : fresh_labs)
            for (uint8_t alm = 0; alm < 10; ++alm)
                for (int half = 0; half < 2; ++half) {
                    CellInfo *cell = getBoundBelCell(labs.at(lab).alms.at(alm).lut_bels[half]);
                    if (!cell)
                        continue;
                    dict<IdString, delay_t> arrival;
                    for (const auto &port : cell->ports) {
                        delay_t value;
                        if (port.second.type == PORT_IN &&
                            timing.get_max_arrival(CellPortKey(cell->name, port.first), value))
                            arrival[port.first] = value;
                    }
                    changed += optimise_private_lut_pins(lab, alm, half, arrival);
                }
        log_info("Arrival-based private LUT pin assignment changed %d input mappings.\n", changed);
    }
}

int Arch::optimise_private_lut_pins(uint32_t lab, uint8_t alm, int half, const dict<IdString, delay_t> &arrival)
{
    auto &data = labs.at(lab).alms.at(alm);
    CellInfo *cell = getBoundBelCell(data.lut_bels[half]);
    CellInfo *mate = getBoundBelCell(data.lut_bels[1 - half]);
    if (!cell || cell->belStrength == STRENGTH_LOCKED || data.carry_mode || data.l6_mode ||
        !cell->type.in(id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4, id_MISTRAL_ALUT5))
        return 0;
    // E/F inputs may also feed registers directly. Leave those ALMs alone;
    // registers driven by COMBOUT are unaffected by a LUT input permutation.
    for (BelId bel : data.ff_bels) {
        CellInfo *ff = getBoundBelCell(bel);
        if (!ff)
            continue;
        NetInfo *din = ff->getPort(id_DATAIN);
        if (ff->belStrength == STRENGTH_LOCKED || ff->getPort(id_SDATA) ||
            (din && din != cell->getPort(id_Q) && (!mate || din != mate->getPort(id_Q))))
            return 0;
    }
    pool<IdString> reserved;
    if (mate)
        for (const auto &port : mate->ports)
            if (port.second.type == PORT_IN && port.second.net)
                for (IdString pin : mate->pin_data.at(port.first).bel_pins)
                    reserved.insert(pin);
    std::vector<IdString> logical, physical;
    const std::array<IdString, 5> inputs{id_A, id_B, id_C, id_D, id_E};
    dict<IdString, delay_t> delays;
    for (IdString port : inputs) {
        if (!cell->getPort(port) || !arrival.count(port))
            continue;
        auto &pins = cell->pin_data.at(port).bel_pins;
        if (pins.size() != 1 || reserved.count(pins[0]))
            continue;
        DelayQuad delay;
        if (!getCellDelay(cell, port, id_Q, delay))
            continue;
        logical.push_back(port);
        physical.push_back(pins[0]);
        delays[pins[0]] = delay.maxDelay();
    }
    std::stable_sort(logical.begin(), logical.end(), [&](IdString a, IdString b) { return arrival.at(a) > arrival.at(b); });
    std::stable_sort(physical.begin(), physical.end(), [&](IdString a, IdString b) { return delays.at(a) < delays.at(b); });
    int changed = 0;
    for (size_t i = 0; i < logical.size(); ++i) {
        auto &pins = cell->pin_data.at(logical[i]).bel_pins;
        changed += pins[0] != physical[i];
        pins[0] = physical[i];
    }
    return changed;
}

void Arch::assign_control_sets(uint32_t lab)
{
    // Set up reservations for checkPipAvail for control set signals
    // This will be needed because clock and CE are routed together and must be kept together, there isn't free choice
    // e.g. CLK0 & ENA0 must be use for one control set, and CLK1 & ENA1 for another, they can't be mixed and matched
    // Similarly for how inverted & noninverted variants must be kept separate
    LabCtrlSetWorker worker;
    LabPairWorker pair_worker;
    // The pair model (opt-in) charges LAB DATAIN lines exactly as Quartus does; the default model is conservative.
    const bool clkb = lab_pair_model(lab);
    pair_worker.max_clocks = args.lab_clkb ? 2 : 1;
    bool legal = clkb ? pair_worker.run(this, lab) : worker.run(this, lab);
    if (!legal) {
        log_warning("Skipping LAB %u control-set reservation (frozen scaffold or illegal after cart merge).\n",
                    unsigned(lab));
        return;
    }
    auto &lab_data = labs.at(lab);
    const auto &datain = clkb ? pair_worker.datain : worker.datain;
    Loc lab_loc = getBelLocation(lab_data.alms.at(0).lut_bels.at(0));
    auto block_type = lab_data.is_mlab ? CycloneV::MLAB : CycloneV::LAB;
    // --mistral-clkb: fix the source of a LAB clock wire, so each CLKk_SEL and CLKA_SEL/CLKB_SEL has one meaning
    auto reserve_clock_source = [&](int k, int source, const NetInfo *net) {
        CycloneV::port_type_t port = dedicated_clock(net) ? CycloneV::CLKIN : CycloneV::DATAIN;
        reserve_route(get_port(block_type, lab_loc.x, lab_loc.y, -1, port, source), lab_data.clk_wires[k]);
    };

    for (int j = 0; j < 2; j++) {
        lab_data.aclr_used[j] = false;
    }

    for (uint8_t alm = 0; alm < 10; alm++) {
        auto &alm_data = lab_data.alms.at(alm);
        if (lab_data.is_mlab) {
            for (uint8_t i = 0; i < 2; i++) {
                BelId lut_bel = alm_data.lut_bels.at(i);
                const CellInfo *lut = getBoundBelCell(lut_bel);
                if (!lut || lut->combInfo.mlab_group == -1)
                    continue;
                WireId wclk_wire = getBelPinWire(lut_bel, id_WCLK);
                WireId we_wire = getBelPinWire(lut_bel, id_WE);
                // Force use of CLK0/ENA0 for LUTRAMs. Might have to revisit if we ever support packing LUTRAMs and FFs
                reserve_route(lab_data.clk_wires[0], wclk_wire);
                reserve_route(lab_data.ena_wires[0], we_wire);
                if (clkb && lut->combInfo.wclk.net != nullptr)
                    reserve_clock_source(0, 0, lut->combInfo.wclk.net);
            }
        }
        for (uint8_t i = 0; i < 4; i++) {
            BelId ff_bel = alm_data.ff_bels.at(i);
            const CellInfo *ff = getBoundBelCell(ff_bel);
            if (ff == nullptr)
                continue;
            // The clock/enable and clear selectors are shared per control group, not per half
            const int group = ALMInfo::ctrl_group(i);
            ControlSig ena_sig = ff->ffInfo.ctrlset.ena;
            WireId clk_wire = getBelPinWire(ff_bel, id_CLK);
            WireId ena_wire = getBelPinWire(ff_bel, id_ENA);
            if (clkb) {
                int k = pair_worker.pair_index(ff->ffInfo.ctrlset);
                NPNR_ASSERT(k >= 0);
                if (getCtx()->debug)
                    log_info("Assigned CLK/ENA pair %d to FF %s (%s)\n", k, nameOf(ff), getCtx()->nameOfBel(ff_bel));
                reserve_route(lab_data.clk_wires[k], clk_wire);
                reserve_route(lab_data.ena_wires[k], ena_wire);
                alm_data.clk_ena_idx[group] = k;
            } else {
                for (int j = 0; j < 3; j++) {
                    if (ena_sig == datain[ena_datain[j]]) {
                        if (getCtx()->debug) {
                            log_info("Assigned CLK/ENA set %d to FF %s (%s)\n", j, nameOf(ff),
                                     getCtx()->nameOfBel(ff_bel));
                        }
                        // Without --mistral-clkb every LAB clock carries the one clock from CLKA
                        reserve_route(lab_data.clk_wires[0], clk_wire);
                        reserve_route(lab_data.ena_wires[j], ena_wire);
                        alm_data.clk_ena_idx[group] = j;
                        break;
                    }
                }
            }
            ControlSig aclr_sig = ff->ffInfo.ctrlset.aclr;
            WireId aclr_wire = getBelPinWire(ff_bel, id_ACLR);
            for (int j = 0; j < 2; j++) {
                // TODO: could be global ACLR, too
                if (aclr_sig == datain[aclr_datain[j]]) {
                    if (getCtx()->debug) {
                        log_info("Assigned ACLR set %d to FF %s (%s)\n", i, nameOf(ff), getCtx()->nameOfBel(ff_bel));
                    }
                    reserve_route(lab_data.aclr_wires[j], aclr_wire);
                    lab_data.aclr_used[j] = (aclr_sig.net != nullptr);
                    alm_data.aclr_idx[group] = j;
                    break;
                }
            }
        }
    }
    if (clkb) {
        for (int k = 0; k < 3; k++)
            if (pair_worker.pair_used[k] && pair_worker.pair_source[k] >= 0)
                reserve_clock_source(k, pair_worker.pair_source[k], pair_worker.source[pair_worker.pair_source[k]]);
    }
    // Park open flops on a fresh route. A scaffold reload locks those cells
    // at STRENGTH_LOCKED, and lab_pre_route leaves that LAB alone, so
    // lock_fes_scaffold parks a stale V1 ACLR index itself.
    park_open_aclr(lab);
}

int Arch::park_open_aclr(uint32_t lab)
{
    // An open ACLR pin still follows BCLR_SEL/TCLR_SEL. An unused slot's
    // SEL defaults to a DATAIN, live whenever another flop uses that slot.
    // Park every open control group (FF0+FF3 or FF1+FF2) on a free slot. Both
    // slots already in use cannot be repaired from a snapshot and must be
    // re-routed.
    auto &lab_data = labs.at(lab);
    int inactive_aclr = -1;
    for (int j = 0; j < 2; j++) {
        if (!lab_data.aclr_used[j]) {
            inactive_aclr = j;
            break;
        }
    }
    int moved = 0;
    for (uint8_t alm = 0; alm < 10; alm++) {
        auto &alm_data = lab_data.alms.at(alm);
        for (int group = 0; group < 2; group++) {
            bool open = false;
            bool driven = false;
            bool placed = false;
            for (int i = 0; i < 4; i++) {
                if (ALMInfo::ctrl_group(i) != group)
                    continue;
                const CellInfo *ff = getBoundBelCell(alm_data.ff_bels.at(i));
                if (ff == nullptr)
                    continue;
                placed = true;
                if (ff->ffInfo.ctrlset.aclr.net == nullptr)
                    open = true;
                else
                    driven = true;
            }
            if (!placed || !open || driven)
                continue;
            int slot = alm_data.aclr_idx[group];
            if (slot < 0 || slot > 1 || !lab_data.aclr_used[slot])
                continue;
            if (inactive_aclr < 0) {
                Loc loc = getBelLocation(lab_data.alms.at(0).lut_bels[0]);
                log_error("FES LAB (%d, %d) has an open flip-flop on a live clear and both ACLR slots are used. "
                          "Re-route this shell.\n",
                          loc.x, loc.y);
            }
            alm_data.aclr_idx[group] = inactive_aclr;
            ++moved;
        }
    }
    return moved;
}

namespace {
// Gets the name of logical LUT pin i for a given cell
static IdString get_lut_pin(CellInfo *cell, int i)
{
    const std::array<IdString, 6> log_pins{id_A, id_B, id_C, id_D, id_E, id_F};
    const std::array<IdString, 5> log_pins_arith{id_A, id_B, id_C, id_D0, id_D1};
    return (cell->type == id_MISTRAL_ALUT_ARITH) ? log_pins_arith.at(i) : log_pins.at(i);
}

static void assign_lut6_inputs(CellInfo *cell, int lut)
{
    std::array<IdString, 6> phys_pins{id_A, id_B, id_C, id_D, (lut == 1) ? id_E1 : id_E0, (lut == 1) ? id_F1 : id_F0};
    int phys_idx = 0;
    for (int i = 0; i < 6; i++) {
        IdString log = get_lut_pin(cell, i);
        if (!cell->ports.count(log) || cell->ports.at(log).net == nullptr)
            continue;
        cell->pin_data[log].bel_pins.clear();
        cell->pin_data[log].bel_pins.push_back(phys_pins.at(phys_idx++));
    }
}

static void assign_mlab_inputs(Context *ctx, CellInfo *cell, int lut)
{
    cell->pin_data[id_CLK1].bel_pins = {id_WCLK};
    cell->pin_data[id_A1EN].bel_pins = {id_WE};
    cell->pin_data[id_A1DATA].bel_pins = {(lut == 1) ? id_E1 : id_E0};
    cell->pin_data[id_B1DATA].bel_pins = {id_COMBOUT};
    cell->pin_data[id_A1EN].bel_pins = {id_WE};

    std::array<IdString, 6> raddr_pins{id_A, id_B, id_C, id_D, id_F0};
    for (int i = 0; i < 5; i++) {
        cell->pin_data[ctx->idf("A1ADDR[%d]", i)].bel_pins = {ctx->idf("WA%d", i)};
        cell->pin_data[ctx->idf("B1ADDR[%d]", i)].bel_pins = {raddr_pins.at(i)};
    }
}

} // namespace

void Arch::assign_alm_lut_inputs(uint32_t lab, uint8_t alm)
{
    // Based on the usage of LUTs inside the ALM, set up cell-bel pin map for the combinational cells in the ALM
    // so that each physical bel pin is only used for one net; and the logical functions can be implemented correctly.
    // Route-through insertion is separate so placement can preview these pins without changing the netlist.
    auto &alm_data = labs.at(lab).alms.at(alm);
    alm_data.l6_mode = false;
    alm_data.carry_mode = false;
    std::array<CellInfo *, 2> luts{getBoundBelCell(alm_data.lut_bels[0]), getBoundBelCell(alm_data.lut_bels[1])};

    bool found_mlab = false;
    for (int i = 0; i < 2; i++) {
        // Currently we treat LUT6s and MLABs as a special case, as they never share inputs or have fixed mappings
        if (!luts[i])
            continue;
        if (luts[i]->combInfo.is_carry)
            alm_data.carry_mode = true;
        if (luts[i]->type == id_MISTRAL_ALUT6) {
            alm_data.l6_mode = true;
            NPNR_ASSERT(luts[1 - i] == nullptr); // only allow one LUT6 per ALM and no other LUTs
            assign_lut6_inputs(luts[i], i);
        } else if (luts[i]->type == id_MISTRAL_MLAB) {
            found_mlab = true;
            assign_mlab_inputs(getCtx(), luts[i], i);
        }
    }

    if (!alm_data.l6_mode && !found_mlab) {
        // In L5 mode; which is what we use in this case
        //  - A and B are shared
        //  - C, E0, and F0 are exclusive to the top LUT5 secion
        //  - D, E1, and F1 are exclusive to the bottom LUT5 section
        // First find up to two shared inputs
        dict<IdString, int> shared_nets;
        if (luts[0] && luts[1]) {
            const int n0 = std::max(0, std::min(luts[0]->combInfo.lut_input_count, 6));
            const int n1 = std::max(0, std::min(luts[1]->combInfo.lut_input_count, 6));
            for (int i = 0; i < n0; i++) {
                for (int j = 0; j < n1; j++) {
                    if (luts[0]->combInfo.lut_in[i] == nullptr)
                        continue;
                    if (luts[0]->combInfo.lut_in[i] != luts[1]->combInfo.lut_in[j])
                        continue;
                    IdString net = luts[0]->combInfo.lut_in[i]->name;
                    if (shared_nets.count(net))
                        continue;
                    int idx = int(shared_nets.size());
                    shared_nets[net] = idx;
                    if (shared_nets.size() >= 2)
                        goto shared_search_done;
                }
            }
        shared_search_done:;
        }
        // A and B can be used for half-specific nets if not assigned to shared nets
        bool a_avail = shared_nets.size() == 0, b_avail = shared_nets.size() <= 1;
        // Do the actual port assignment
        for (int i = 0; i < 2; i++) {
            if (!luts[i])
                continue;
            // Work out which physical ports are available
            std::vector<IdString> avail_phys_ports;
            // D/C always available and dedicated to the half, in L5 mode
            avail_phys_ports.push_back((i == 1) ? id_D : id_C);
            // In arithmetic mode, Ei can only be used for D0 and Fi can only be used for D1
            // otherwise, these are general and dedicated to one half
            if (!luts[i]->combInfo.is_carry) {
                avail_phys_ports.push_back((i == 1) ? id_E1 : id_E0);
                avail_phys_ports.push_back((i == 1) ? id_F1 : id_F0);
            }
            // A and B might be used for shared signals, or already used by the other half
            if (b_avail)
                avail_phys_ports.push_back(id_B);
            if (a_avail)
                avail_phys_ports.push_back(id_A);
            int phys_idx = 0;

            for (int j = 0; j < std::max(0, std::min(luts[i]->combInfo.lut_input_count, 6)); j++) {
                IdString log = get_lut_pin(luts[i], j);
                auto &bel_pins = luts[i]->pin_data[log].bel_pins;
                bel_pins.clear();

                NetInfo *net = luts[i]->getPort(log);
                if (net == nullptr) {
                    // Disconnected inputs don't need to be allocated a pin, because the router won't be routing these
                    continue;
                } else if (shared_nets.count(net->name)) {
                    // This pin is to be allocated one of the shared nets
                    bel_pins.push_back(shared_nets.at(net->name) ? id_B : id_A);
                } else if (log == id_D0) {
                    // Arithmetic
                    bel_pins.push_back((i == 1) ? id_E1 : id_E0); // reserved
                } else if (log == id_D1) {
                    bel_pins.push_back((i == 1) ? id_F1 : id_F0); // reserved
                } else {
                    // Allocate from the general pool of available physical pins
                    if (phys_idx >= int(avail_phys_ports.size()))
                        continue;
                    IdString phys = avail_phys_ports.at(phys_idx++);
                    bel_pins.push_back(phys);
                    // Mark A/B unavailable for the other LUT, if needed
                    if (phys == id_A)
                        a_avail = false;
                    else if (phys == id_B)
                        b_avail = false;
                }
            }
        }
    }
}

CellInfo *Arch::get_alm_route_through_ff(uint32_t lab, uint8_t alm, uint8_t half) const
{
    const auto &data = labs.at(lab).alms.at(alm);
    if (getBoundBelCell(data.lut_bels.at(half)) || data.l6_mode || data.carry_mode)
        return nullptr;
    for (int n = 0; n < 2; ++n) {
        // FF0 top and FF3 bottom have priority in the four-register model.
        const int j = (half == 1 && lab_ff4) ? 1 - n : n;
        auto *ff = getBoundBelCell(data.ff_bels.at(half * 2 + j));
        if (!ff || !ff->ffInfo.datain || ff->belStrength == STRENGTH_LOCKED)
            continue;
        // Selection is shared with placement STA; a virtual helper must obey
        // the same FES reservation rules as the helper inserted for routing.
        CellInfo candidate(const_cast<Context *>(getCtx()), idf("%s$ROUTETHRU", nameOf(ff)), id_MISTRAL_BUF);
        auto slot = ff->attrs.find(id("FES_SLOT"));
        if (slot != ff->attrs.end())
            candidate.attrs[id("FES_SLOT")] = slot->second;
        if (fes_placement_allowed(data.lut_bels.at(half), &candidate))
            return ff;
    }
    return nullptr;
}

void Arch::reassign_alm_inputs(uint32_t lab, uint8_t alm)
{
    assign_alm_lut_inputs(lab, alm);
    auto &alm_data = labs.at(lab).alms.at(alm);
    // FF route-through insertion; selection is also used by placement STA.
    for (uint8_t i = 0; i < 2; ++i) {
        auto *ff = get_alm_route_through_ff(lab, alm, i);
        if (!ff)
            continue;
        CellInfo candidate(getCtx(), idf("%s$ROUTETHRU", nameOf(ff)), id_MISTRAL_BUF);
        auto slot = ff->attrs.find(id("FES_SLOT"));
        if (slot != ff->attrs.end())
            candidate.attrs[id("FES_SLOT")] = slot->second;
        if (!fes_placement_allowed(alm_data.lut_bels[i], &candidate))
            continue;
        CellInfo *rt_lut = createCell(candidate.name, candidate.type);
        // The route-through becomes the FF's DATAIN sink. Preserve the
        // socket boundary marker so FES routing still recognizes it as a
        // cart endpoint when checking pips inside the socket.
        rt_lut->attrs = candidate.attrs;
        rt_lut->addInput(id_A);
        rt_lut->addOutput(id_Q);
        // Disconnect the original data input to the FF, and connect it to the route-thru LUT instead
        NetInfo *datain = ff->getPort(id_DATAIN);
        ff->disconnectPort(id_DATAIN);
        rt_lut->connectPort(id_A, datain);
        rt_lut->connectPorts(id_Q, ff, id_DATAIN);
        // Assign route-thru LUT physical ports, input goes to the first half-specific input
        rt_lut->pin_data[id_A].bel_pins.push_back(i ? id_D : id_C);
        rt_lut->pin_data[id_Q].bel_pins.push_back(id_COMBOUT);
        assign_comb_info(rt_lut);
        // Place the route-thru LUT at the relevant combinational bel
        bindBel(alm_data.lut_bels[i], rt_lut, STRENGTH_STRONG);
    }

    // TODO: in the future, as well as the reassignment here we will also have pseudo PIPs in front of the ALM so that
    // the router can permute LUTs for routeability; too. Here we will need to lock out some of those PIPs depending on
    // the usage of the ALM, as not all inputs are always interchangeable.
    // Get cells into an array for fast access
}

// Arithmetic/MLAB placement scaffold. Boolean LUTs use lut_placement_pin;
// sharing and half-specific reservations are resolved before routing.
const dict<IdString, IdString> Arch::comb_pinmap = {
        {id_A, id_F0}, // fastest input first
        {id_B, id_E0}, {id_C, id_D}, {id_D, id_C},       {id_D0, id_C},       {id_D1, id_B},
        {id_E, id_B},  {id_F, id_A}, {id_Q, id_COMBOUT}, {id_SO, id_COMBOUT},
};

IdString Arch::lut_placement_pin(const CellInfo *cell, IdString port) const
{
    if (!arrival_pin_assignment)
        return comb_pinmap.at(port);
    if (!cell->type.in(id_MISTRAL_NOT, id_MISTRAL_BUF, id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                       id_MISTRAL_ALUT5, id_MISTRAL_ALUT6) ||
        port == id_Q)
        return comb_pinmap.at(port);
    const std::array<IdString, 6> inputs{id_A, id_B, id_C, id_D, id_E, id_F};
    // Estimate an isolated LUT. C/D and the two E/F halves have equal cell
    // delays; pairing can still reserve A/B for shared signals later.
    const std::array<IdString, 5> l5_pins{id_C, id_E0, id_F0, id_B, id_A};
    const std::array<IdString, 6> l6_pins{id_A, id_B, id_C, id_D, id_E0, id_F0};
    int allocated = 0;
    for (IdString input : inputs) {
        if (input == port)
            return cell->type == id_MISTRAL_ALUT6 ? l6_pins.at(allocated) : l5_pins.at(allocated);
        if (cell->getPort(input) != nullptr)
            ++allocated;
    }
    return comb_pinmap.at(port);
}

namespace {
// gets the value of the ith LUT init property of a given cell
uint64_t get_lut_init(const CellInfo *cell, int i)
{
    if (cell->type == id_MISTRAL_NOT) {
        return 1;
    } else if (cell->type == id_MISTRAL_BUF) {
        return 2;
    } else {
        IdString prop;
        if (cell->type == id_MISTRAL_ALUT_ARITH)
            prop = (i == 1) ? id_LUT1 : id_LUT0;
        else
            prop = id_LUT;
        auto fnd = cell->params.find(prop);
        if (fnd == cell->params.end())
            return 0;
        else
            return fnd->second.as_int64();
    }
}
// gets the state of a physical pin when evaluating the a given bit of LUT init for
bool get_phys_pin_val(bool l6_mode, bool arith_mode, int bit, IdString pin)
{
    switch (pin.index) {
    case ID_A:
        return (bit >> 0) & 0x1;
    case ID_B:
        return (bit >> 1) & 0x1;
    case ID_C:
        return (l6_mode && bit >= 32) ? ((bit >> 3) & 0x1) : ((bit >> 2) & 0x1);
    case ID_D:
        return (l6_mode && bit < 32) ? ((bit >> 3) & 0x1) : ((bit >> 2) & 0x1);
    case ID_E0:
    case ID_E1:
        return l6_mode ? ((bit >> 5) & 0x1) : ((bit >> 3) & 0x1);
    case ID_F0:
    case ID_F1:
        return arith_mode ? ((bit >> 3) & 0x1) : ((bit >> 4) & 0x1);
    default:
        NPNR_ASSERT_FALSE("unknown physical pin!");
    }
}

static const std::array<int, 64> mlab_permute = {0,  1,  4,  5,  8,  9,  12, 13, 29, 28, 25, 24, 21, 20, 17, 16,
                                                 2,  3,  6,  7,  10, 11, 14, 15, 31, 30, 27, 26, 23, 22, 19, 18,
                                                 32, 33, 36, 37, 40, 41, 44, 45, 61, 60, 57, 56, 53, 52, 49, 48,
                                                 34, 35, 38, 39, 42, 43, 46, 47, 63, 62, 59, 58, 55, 54, 51, 50};

// MLABs have permuted init values in hardware, we need to correct for this
uint64_t permute_mlab_init(uint64_t orig)
{
    uint64_t result = 0;
    for (int i = 0; i < 64; i++) {
        if ((orig >> uint64_t(i)) & 0x1) {
            result |= (uint64_t(1) << uint64_t(mlab_permute.at(i)));
        }
    }
    return result;
}

} // namespace

// In RAM mode address 0 selects the last bit of each logical 32-bit half.
// Storage is active-low, then uses the same MLAB CRAM permutation as logic.
uint64_t Arch::compute_mlab_mask(uint32_t lab, uint8_t alm)
{
    uint64_t mask = 0;
    const auto &alm_data = labs.at(lab).alms.at(alm);
    for (int lane = 0; lane < 2; ++lane) {
        const CellInfo *cell = getBoundBelCell(alm_data.lut_bels[lane]);
        if (cell == nullptr)
            continue; // An unused half retains the existing zero initialization.
        if (cell->type != id_MISTRAL_MLAB)
            continue;
        auto init = cell->params.find(id_INIT);
        if (init == cell->params.end())
            continue;
        uint32_t bits = uint32_t(init->second.as_int64());
        for (int addr = 0; addr < 32; ++addr)
            if ((bits >> addr) & 1)
                mask |= uint64_t(1) << (32 * lane + 31 - addr);
    }
    return permute_mlab_init(~mask);
}

uint64_t Arch::compute_lut_mask(uint32_t lab, uint8_t alm)
{
    uint64_t mask = 0;
    auto &alm_data = labs.at(lab).alms.at(alm);
    std::array<CellInfo *, 2> luts{getBoundBelCell(alm_data.lut_bels[0]), getBoundBelCell(alm_data.lut_bels[1])};

    for (int i = 0; i < 2; i++) {
        CellInfo *lut = luts[i];
        if (!lut)
            continue;
        // FF DATAIN routethroughs are inserted after placement as MISTRAL_BUF.
        // They still require a LUT truth table, including folded pin inversion.
        if (!is_comb_cell(lut->type) && !lut->type.in(id_MISTRAL_BUF, id_MISTRAL_MLAB))
            continue;
        int offset = ((i == 1) && !alm_data.l6_mode) ? 32 : 0;
        bool arith = lut->combInfo.is_carry;
        for (int j = 0; j < (alm_data.l6_mode ? 64 : 32); j++) {
            // Evaluate LUT function at this point
            uint64_t init = get_lut_init(lut, (arith && j >= 16) ? 1 : 0);

            int index = 0;
            for (int k = 0; k < lut->combInfo.lut_input_count; k++) {
                IdString log_pin = get_lut_pin(lut, k);
                int init_idx = k;
                if (arith) {
                    // D0 only affects lower half; D1 upper half
                    if (k == 3 && j >= 16)
                        continue;
                    if (k == 4) {
                        if (j < 16)
                            continue;
                        else
                            init_idx = 3;
                    }
                }
                CellPinState state = lut->get_pin_state(log_pin);
                if (state == PIN_0) {
                    continue;
                } else if (state == PIN_1) {
                    index |= (1 << init_idx);
                    continue;
                }
                // Ignore if no associated physical pin
                if (lut->getPort(log_pin) == nullptr || lut->pin_data.at(log_pin).bel_pins.empty())
                    continue;
                // ALM inputs appear to be inverted by default (TODO: check!)
                // so only invert if an inverter has _not_ been folded into the pin
                bool inverted = (state != PIN_INV);
                // Depermute physical pin
                IdString phys_pin = lut->pin_data.at(log_pin).bel_pins.at(0);
                if (get_phys_pin_val(alm_data.l6_mode, arith, j, phys_pin) != inverted)
                    index |= (1 << init_idx);
            }
            if ((init >> index) & 0x1) {
                mask |= (1ULL << uint64_t(j + offset));
            }
        }
    }

    // TODO: always inverted, or just certain paths?
    mask = ~mask;

    if (labs.at(lab).is_mlab)
        mask = permute_mlab_init(mask);

#if 1
    if (getCtx()->debug) {
        auto pos = alm_data.lut_bels[0].pos;
        log("ALM %03d.%03d.%d\n", pos.x(), pos.y(), alm);
        for (int i = 0; i < 2; i++) {
            log("    LUT%d: ", i);
            if (luts[i]) {
                log("%s:%s", nameOf(luts[i]), nameOf(luts[i]->type));
                for (auto &pin : luts[i]->pin_data) {
                    if (!luts[i]->ports.count(pin.first) || luts[i]->ports.at(pin.first).type != PORT_IN)
                        continue;
                    log(" %s:", nameOf(pin.first));
                    if (pin.second.state == PIN_0)
                        log("0");
                    else if (pin.second.state == PIN_1)
                        log("1");
                    else if (pin.second.state == PIN_INV)
                        log("~");
                    for (auto bp : pin.second.bel_pins)
                        log("%s", nameOf(bp));
                }
            } else {
                log("<null>");
            }
            log("\n");
        }
        log("INIT: %016lx\n", mask);
        log("\n");
    }
#endif

    return mask;
}

NEXTPNR_NAMESPACE_END
