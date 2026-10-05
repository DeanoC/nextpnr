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

#include "log.h"
#include "nextpnr.h"
#include "util.h"

#include <cctype>
#include <string>

NEXTPNR_NAMESPACE_BEGIN

void Arch::create_gpio(int x, int y)
{
    for (int z = 0; z < 4; z++) {
        // Notional pad wire
        WireId pad = add_wire(x, y, idf("PAD[%d]", z));
        BelId bel = add_bel(x, y, idf("IO[%d]", z), id_MISTRAL_IO);
        add_bel_pin(bel, id_PAD, PORT_INOUT, pad);
        if (has_port(CycloneV::GPIO, x, y, z, CycloneV::DATAOUT, 0)) {
            // FIXME: is the port index of zero always correct?
            add_bel_pin(bel, id_I, PORT_IN, get_port(CycloneV::GPIO, x, y, z, CycloneV::DATAOUT, 0));
            add_bel_pin(bel, id_D_L, PORT_IN, get_port(CycloneV::GPIO, x, y, z, CycloneV::DATAOUT, 0));
            add_bel_pin(bel, id_OE, PORT_IN, get_port(CycloneV::GPIO, x, y, z, CycloneV::OEIN, 0));
            add_bel_pin(bel, id_O, PORT_OUT, get_port(CycloneV::GPIO, x, y, z, CycloneV::DATAIN, 0));
        }
        if (has_port(CycloneV::GPIO, x, y, z, CycloneV::DATAOUT, 1))
            add_bel_pin(bel, id_D_H, PORT_IN, get_port(CycloneV::GPIO, x, y, z, CycloneV::DATAOUT, 1));
        if (has_port(CycloneV::GPIO, x, y, z, CycloneV::CLKOUT, 0))
            add_bel_pin(bel, id_CLK, PORT_IN, get_port(CycloneV::GPIO, x, y, z, CycloneV::CLKOUT, 0));
        if (has_port(CycloneV::GPIO, x, y, z, CycloneV::CLKIN, 0))
            add_bel_pin(bel, id_CLKIN, PORT_IN, get_port(CycloneV::GPIO, x, y, z, CycloneV::CLKIN, 0));
        if (has_port(CycloneV::GPIO, x, y, z, CycloneV::DATAIN, 3)) {
            add_bel_pin(bel, id_Q, PORT_OUT, get_port(CycloneV::GPIO, x, y, z, CycloneV::DATAIN, 3));
            add_bel_pin(bel, id_Q_H, PORT_OUT, get_port(CycloneV::GPIO, x, y, z, CycloneV::DATAIN, 3));
        }
        if (has_port(CycloneV::GPIO, x, y, z, CycloneV::DATAIN, 2))
            add_bel_pin(bel, id_Q_L, PORT_OUT, get_port(CycloneV::GPIO, x, y, z, CycloneV::DATAIN, 2));
        // I/O register clock enables (input; output and OE) and the pad's
        // shared asynchronous clear.
        if (has_port(CycloneV::GPIO, x, y, z, CycloneV::CEIN))
            add_bel_pin(bel, id_CEIN, PORT_IN, get_port(CycloneV::GPIO, x, y, z, CycloneV::CEIN));
        if (has_port(CycloneV::GPIO, x, y, z, CycloneV::CEOUT))
            add_bel_pin(bel, id_CEOUT, PORT_IN, get_port(CycloneV::GPIO, x, y, z, CycloneV::CEOUT));
        if (has_port(CycloneV::GPIO, x, y, z, CycloneV::ACLR))
            add_bel_pin(bel, id_ACLR, PORT_IN, get_port(CycloneV::GPIO, x, y, z, CycloneV::ACLR));
        bel_data(bel).block_index = z;
    }
}

bool Arch::is_io_cell(IdString cell_type) const
{
    // Return true if a cell is an IO buffer cell type
    switch (cell_type.index) {
    case ID_MISTRAL_IB:
    case ID_MISTRAL_SDRIN:
    case ID_MISTRAL_DDRIN:
    case ID_MISTRAL_OB:
    case ID_MISTRAL_SDROUT:
    case ID_MISTRAL_SDRIO:
    case ID_MISTRAL_DDROUT:
    case ID_MISTRAL_DDRBIDIR:
    case ID_MISTRAL_IO:
        return true;
    default:
        return false;
    }
}

BelId Arch::get_io_pin_bel(const CycloneV::pin_info_t *pin) const
{
    auto pad = pin->pad;
    CycloneV::xycoords pos = CycloneV::xycoords{pad & 0x3FFF};
    return bel_by_block_idx(pos.x(), pos.y(), id_MISTRAL_IO, (pad >> 14));
}

namespace {

std::string upper_words(const std::string &value)
{
    // Upper-case and collapse whitespace so "maximum  current" and
    // "MAXIMUM CURRENT" compare equal.
    std::string out;
    bool space = false;
    for (char c : value) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = !out.empty();
            continue;
        }
        if (space)
            out += ' ';
        space = false;
        out += char(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

Arch::IoElectrical Arch::get_io_electrical(const CellInfo *cell) const
{
    IoElectrical io;
    auto attr = [&](const char *name, std::string &value) {
        auto found = cell->attrs.find(id(name));
        if (found == cell->attrs.end())
            return false;
        value = found->second.is_string ? found->second.as_string() : std::to_string(found->second.as_int64());
        return true;
    };
    auto on_off = [&](const char *name, bool &flag) {
        std::string value;
        if (!attr(name, value))
            return;
        std::string v = upper_words(value);
        if (v == "ON")
            flag = true;
        else if (v == "OFF")
            flag = false;
        else
            log_error("IO '%s': %s must be ON or OFF, got '%s'.\n", nameOf(cell), name, value.c_str());
    };
    auto delay = [&](const char *name, int max, int &out) {
        std::string value;
        if (!attr(name, value))
            return;
        std::string v = upper_words(value);
        if (v.empty() || v.size() > 2 || v.find_first_not_of("0123456789") != std::string::npos || std::stoi(v) > max)
            log_error("IO '%s': %s must be an integer from 0 to %d, got '%s'.\n", nameOf(cell), name, max,
                      value.c_str());
        out = std::stoi(v);
    };

    std::string value;
    if (attr("IO_STANDARD", value)) {
        std::string v = upper_words(value);
        if (v == "3.3-V LVTTL")
            io.lvcmos = false;
        else if (v == "3.3-V LVCMOS")
            io.lvcmos = true;
        else
            log_error("IO '%s': IO_STANDARD \"%s\" is not supported; the checked Cyclone V settings cover "
                      "\"3.3-V LVTTL\" and \"3.3-V LVCMOS\".\n",
                      nameOf(cell), value.c_str());
    }
    if (attr("CURRENT_STRENGTH_NEW", value)) {
        // Quartus accepts 4, 8 and 16 mA for 3.3-V LVTTL and only 2 mA for
        // 3.3-V LVCMOS; the maximum shares the 16 mA/2 mA encoding.
        std::string v = upper_words(value);
        if (v == "MAXIMUM CURRENT" || (!io.lvcmos && v == "16MA") || (io.lvcmos && v == "2MA") ||
            (io.lvcmos && v == "MINIMUM CURRENT"))
            io.drive_strength = CycloneV::V3P3_LVTTL_16MA_LVCMOS_2MA;
        else if (!io.lvcmos && (v == "4MA" || v == "MINIMUM CURRENT"))
            io.drive_strength = CycloneV::V3P3_LVTTL_4MA;
        else if (!io.lvcmos && v == "8MA")
            io.drive_strength = CycloneV::V3P3_LVTTL_8MA;
        else
            log_error("IO '%s': CURRENT_STRENGTH_NEW '%s' is not supported by %s (use %s or MINIMUM/MAXIMUM "
                      "CURRENT).\n",
                      nameOf(cell), value.c_str(), io.lvcmos ? "3.3-V LVCMOS" : "3.3-V LVTTL",
                      io.lvcmos ? "2MA" : "4MA, 8MA, 16MA");
    }
    if (attr("SLEW_RATE", value)) {
        std::string v = upper_words(value);
        if (v == "0")
            io.slow_slew = true;
        else if (v == "1")
            io.slow_slew = false;
        else
            log_error("IO '%s': SLEW_RATE must be 0 (slow) or 1 (fast), got '%s'.\n", nameOf(cell), value.c_str());
    }
    on_off("WEAK_PULL_UP_RESISTOR", io.weak_pullup);
    on_off("ENABLE_BUS_HOLD_CIRCUITRY", io.bus_hold);
    on_off("PCI_IO", io.clamp_diode); // obsolete Quartus name for CLAMPING_DIODE
    on_off("CLAMPING_DIODE", io.clamp_diode);
    if (io.weak_pullup && io.bus_hold)
        log_error("IO '%s': ENABLE_BUS_HOLD_CIRCUITRY and WEAK_PULL_UP_RESISTOR cannot both be ON.\n",
                  nameOf(cell));
    for (const char *name : {"OUTPUT_TERMINATION", "INPUT_TERMINATION"})
        if (attr(name, value) && upper_words(value) != "OFF")
            log_error("IO '%s': %s '%s' is not available on the 3.3 V I/O standards.\n", nameOf(cell), name,
                      value.c_str());

    delay("D1_DELAY", 31, io.d1_delay);
    delay("D3_DELAY", 7, io.d3_delay);
    delay("D5_DELAY", 31, io.d5_delay);
    delay("D5_OE_DELAY", 31, io.d5_oe_delay);
    for (const char *name : {"D2_DELAY", "D3_FINE_DELAY", "D4_DELAY", "D6_DELAY", "D1_FINE_DELAY", "D4_FINE_DELAY",
                             "D5_FINE_DELAY", "D6_FINE_DELAY", "D5_OCT_DELAY", "D6_OCT_DELAY", "D6_OE_DELAY",
                             "D6_OE_FINE_DELAY", "OPEN_DRAIN_OUTPUT"})
        if (attr(name, value))
            log_error("IO '%s': %s is not supported.\n", nameOf(cell), name);
    return io;
}

void Arch::check_io_electrical() const
{
    for (const auto &entry : cells) {
        const CellInfo *ci = entry.second.get();
        if (!is_io_cell(ci->type))
            continue;
        IoElectrical io = get_io_electrical(ci);
        bool output = ci->type.in(id_MISTRAL_OB, id_MISTRAL_SDROUT, id_MISTRAL_DDROUT, id_MISTRAL_DDRBIDIR) ||
                      (ci->type == id_MISTRAL_IO && ci->getPort(id_OE) != nullptr);
        bool comb_input = ci->type == id_MISTRAL_IB || (ci->type == id_MISTRAL_IO && ci->getPort(id_O) != nullptr);
        bool comb_output = ci->type == id_MISTRAL_OB || (ci->type == id_MISTRAL_IO && ci->getPort(id_OE) != nullptr);
        // Quartus ignores drive settings on input-only pins and the D1 chain
        // without an input register (warning 176437); only reject requests
        // whose decoded encoding has not been checked.
        if (io.d1_delay >= 0 && ci->type != id_MISTRAL_SDRIN) {
            if (ci->type.in(id_MISTRAL_DDRIN, id_MISTRAL_DDRBIDIR))
                log_error("IO '%s': D1_DELAY is only supported with FAST_INPUT_REGISTER, not a DDR input.\n",
                          nameOf(ci));
            // A bidirectional FAST_INPUT_REGISTER is MISTRAL_SDRIO. The D1 chain
            // is only written for MISTRAL_SDRIN, so accepting it would drop the delay.
            if (ci->type == id_MISTRAL_SDRIO)
                log_error("IO '%s': D1_DELAY on a bidirectional FAST_INPUT_REGISTER is not encoded.\n",
                          nameOf(ci));
            log_warning("IO '%s': D1_DELAY applies only to an input register and is ignored.\n", nameOf(ci));
        }
        if (io.d3_delay >= 0 && !comb_input)
            log_error("IO '%s': D3_DELAY is only supported on a combinational input path.\n", nameOf(ci));
        if (io.d5_delay >= 0 && !(comb_output || ci->type == id_MISTRAL_SDROUT))
            log_error("IO '%s': D5_DELAY is only supported on a combinational or FAST_OUTPUT_REGISTER output.\n",
                      nameOf(ci));
        if (io.d5_oe_delay >= 0 && !comb_output)
            log_error("IO '%s': D5_OE_DELAY is only supported on a combinational output.\n", nameOf(ci));
        if (!output && (io.slow_slew || io.drive_strength != CycloneV::V3P3_LVTTL_16MA_LVCMOS_2MA))
            log_info("IO '%s': slew rate and current strength do not apply to an input and are ignored.\n",
                     nameOf(ci));
    }
}

NEXTPNR_NAMESPACE_END
