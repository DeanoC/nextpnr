/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2021  Lofty <dan.ravensloft@gmail.com>
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
 */

#include "nextpnr.h"
#include "util.h"
#include "lut_route_minimum.h"

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <limits>

NEXTPNR_NAMESPACE_BEGIN

bool Arch::useRoutedDelayRanges() const
{
    return bool_or_default(getCtx()->settings,getCtx()->id("mistral/routed_delay_ranges"),false);
}

bool Arch::useClockEdgeRanges() const
{
    const bool enabled = bool_or_default(getCtx()->settings, getCtx()->id("mistral/clock_edge_ranges"), false);
    if (enabled && !useRoutedDelayRanges())
        log_error("--mistral-clock-edge-ranges requires --mistral-routed-delay-ranges.\n");
    return enabled;
}

void Arch::validate_lut_route_min_reference() const
{
    if (!use_lut_route_min_reference)
        return;
    if (!use_lut_core_reference || !useRoutedDelayRanges() || getChipName() != "5CSEBA6U23I7")
        log_error("--mistral-lut-route-min-reference requires 5CSEBA6U23I7, "
                  "--mistral-lut-core-reference and --mistral-routed-delay-ranges.\n");
}

const char *Arch::lut_route_min_reference_identity() const
{
    return lut_route_minimum::reference_sha256;
}

namespace {

constexpr delay_t block_input_table_delay = 175;

bool boolean_lut_input(const CellInfo *cell, IdString port)
{
    const int width = cell->type == id_MISTRAL_ALUT6                  ? 6
                      : cell->type == id_MISTRAL_ALUT5                ? 5
                      : cell->type == id_MISTRAL_ALUT4                ? 4
                      : cell->type == id_MISTRAL_ALUT3                ? 3
                      : cell->type == id_MISTRAL_ALUT2                ? 2
                      : cell->type.in(id_MISTRAL_BUF, id_MISTRAL_NOT) ? 1
                                                                      : 0;
    const std::array<IdString, 6> inputs{id_A, id_B, id_C, id_D, id_E, id_F};
    return std::find(inputs.begin(), inputs.begin() + width, port) != inputs.begin() + width;
}

// Reference local-register envelope (ps): Quartus 17.0.2, 5CSEBA6U23I7,
// io-registers SDR/DDR/OE fixtures, all four slow/fast temperature corners.
// Normalize the data CELL delay against the internal clock CELL delay;
// package, input buffers and interconnect stay outside these local arcs.
// Observed maxima: CEIN setup 115, hold 659; output/OE setup 109,
// hold 309; input clock-to-fabric 866. Round outward to 10 ps. Clock-to-Q
// uses a conservative zero minimum, not the minimum of a reference fit.
// Pad capture, pad output and asynchronous clear are intentionally excluded.
// SDROUT and SDRIO both keep the zero delay-chain defaults of these fits.
bool gpio_fabric_profile(const Context *ctx, const CellInfo *cell)
{
    if (ctx->getChipName() != "5CSEBA6U23I7" ||
        !cell->type.in(id_MISTRAL_SDRIN, id_MISTRAL_DDRIN, id_MISTRAL_SDRIO, id_MISTRAL_SDROUT))
        return false;
    const auto io = ctx->get_io_electrical(cell);
    return !io.lvcmos && !io.bus_hold && io.d1_delay < 0 && io.d3_delay < 0 &&
           io.d5_delay < 0 && io.d5_oe_delay < 0;
}

// Opt-in local FF clear recovery reference. The retained four-corner fixtures
// cover every FF/ALM position, constant/dynamic/inverted enables and 100/50/
// 74.25 MHz rising clocks. Recovery CELL-minus-clock maximum is 164 ps;
// round outward to 170 ps. Observed removal maximum is 75 ps, but retain
// the existing 270 ps hold floor. This mode never loosens an existing check.
// It supplies no reset pulse, routing fast-corner, MTBF or board qualification.
bool ff_reset_reference(const Context *ctx, const CellInfo *cell)
{
    if (!bool_or_default(ctx->settings, ctx->id("mistral/ff_reset_reference"), false))
        return false;
    // Post-route calibration queries every FF port, including disconnected
    // inactive clear pins. This exact packed constant proves no reset path
    // exists; it must retain the data approximation used by calibration.
    if (!cell->getPort(id_ACLR) && cell->get_pin_state(id_ACLR) == PIN_1)
        return false;
    auto reject = [&]() {
        log_error("FF reset reference does not cover cell '%s': require 5CSEBA6U23I7, "
                  "rising exact clock period >=10ns and high/low >=5ns, driven active-low clear, "
                  "enabled or dynamic ENA, and inactive SCLR/SLOAD.\n", cell->name.c_str(ctx));
    };
    if (ctx->getChipName() != "5CSEBA6U23I7" || cell->type != id_MISTRAL_FF ||
        !cell->getPort(id_CLK) || !cell->getPort(id_ACLR) ||
        !cell->getPort(id_ACLR)->driver.cell ||
        cell->get_pin_state(id_CLK) != PIN_SIG || cell->get_pin_state(id_ACLR) != PIN_SIG)
        reject();
    const auto *clock = cell->getPort(id_CLK)->clkconstr.get();
    if (!clock || clock->period.minDelay() != clock->period.maxDelay() ||
        clock->high.minDelay() != clock->high.maxDelay() || clock->low.minDelay() != clock->low.maxDelay() ||
        clock->period.minDelay() < 10000 || clock->high.minDelay() < 5000 || clock->low.minDelay() < 5000 ||
        clock->high.minDelay() + clock->low.minDelay() != clock->period.minDelay())
        reject();
    for (auto port : {id_SCLR, id_SLOAD})
        if (cell->getPort(port) || cell->get_pin_state(port) != PIN_0)
            reject();
    auto ena = cell->get_pin_state(id_ENA);
    if (!((ena == PIN_1 && !cell->getPort(id_ENA)) ||
          ((ena == PIN_SIG || ena == PIN_INV) && cell->getPort(id_ENA) && cell->getPort(id_ENA)->driver.cell)))
        reject();
    return true;
}

bool gpio_input_registered(const Context *ctx, const CellInfo *cell)
{
    return cell->type.in(id_MISTRAL_SDRIN, id_MISTRAL_DDRIN) ||
           (cell->type == id_MISTRAL_SDRIO && int_or_default(cell->params, ctx->id("IOREG_IN"), 0));
}

bool gpio_output_registered(const Context *ctx, const CellInfo *cell)
{
    return cell->type == id_MISTRAL_SDROUT ||
           (cell->type == id_MISTRAL_SDRIO && int_or_default(cell->params, ctx->id("IOREG_OUT"), 0));
}

// Explicit reference-profile opt-in: these envelopes cover the retained
// four-corner fits, not an arbitrary pin/electrical configuration or board.
// Check the physical BEL as well as the pin assignment; stale attributes
// must not qualify a pad that was moved to a different site.
bool gpio_pad_profile(const Context *ctx, const CellInfo *cell, bool dq_only)
{
    auto profile = cell->attrs.find(ctx->id("NEXTPNR_GPIO_TIMING_PROFILE"));
    if (profile == cell->attrs.end() || !profile->second.is_string ||
        profile->second.as_string() != "QUARTUS_17_0_2_RAMTEST" || ctx->getChipName() != "5CSEBA6U23I7" ||
        !cell->type.in(id_MISTRAL_SDRIN, id_MISTRAL_DDRIN, id_MISTRAL_SDRIO, id_MISTRAL_SDROUT, id_MISTRAL_DDROUT))
        return false;
    auto loc = cell->attrs.find(id_LOC);
    if (loc == cell->attrs.end() || !loc->second.is_string)
        return false;
    const std::string pin = loc->second.as_string();
    const char *dq[] = {"V12", "E8", "D11", "W12", "AH13", "D8", "AH14", "AF7",
                        "AE24", "AD23", "AE6", "AE23", "AG14", "AD5", "AF4", "AH3"};
    const char *out[] = {"Y11", "AA26", "AA13", "AA11", "W11", "Y19", "AB23", "AC23", "AC22",
                         "C12", "AB26", "AD17", "D12", "Y17", "AB25", "AG13", "AF13",
                         "AG10", "AA19", "AA18", "Y18", "W14"};
    bool found = false;
    if (cell->type == id_MISTRAL_DDROUT) {
        auto high = cell->params.find(id_DDR_HIGH);
        if (cell->getPort(id_I) || cell->getPort(id_D_H) || cell->getPort(id_D_L) || high == cell->params.end() ||
            high->second.is_string || (high->second.as_int64() != 0 && high->second.as_int64() != 1))
            return false;
        found = pin == "PIN_AD20";
    } else {
        for (const char *name : dq)
            found |= pin == std::string("PIN_") + name;
        if (!dq_only)
            for (const char *name : out)
                found |= pin == std::string("PIN_") + name;
    }
    if (!found || cell->bel == BelId())
        return false;
    auto package_pin = ctx->cyclonev->pin_find_name(pin.substr(4));
    if (!package_pin || ctx->get_io_pin_bel(package_pin) != cell->bel)
        return false;
    const auto io = ctx->get_io_electrical(cell);
    if (io.lvcmos || io.bus_hold || io.d1_delay >= 0 || io.d3_delay >= 0 || io.d5_delay >= 0 || io.d5_oe_delay >= 0 ||
        io.drive_strength != CycloneV::V3P3_LVTTL_16MA_LVCMOS_2MA || io.slow_slew || io.weak_pullup || io.clamp_diode)
        return false;
    if (int_or_default(cell->params, ctx->id("IOREG_IN_ACLR"), 0) ||
        int_or_default(cell->params, ctx->id("IOREG_IN_CE"), 0))
        return false;
    // Asynchronous clear, inverted clocks and enabled clock enables were not
    // covered by the pad reference fixture. Missing control ports mean the
    // packer's default inactive clear / enabled clock.
    for (IdString port : {id_CLK, id_CLKIN})
        if (cell->getPort(port) && cell->get_pin_state(port) != PIN_SIG)
            return false;
    if (cell->getPort(id_ACLR) && cell->get_pin_state(id_ACLR) != PIN_0)
        return false;
    for (IdString port : {id_CEIN, id_CEOUT})
        if (cell->getPort(port) && cell->get_pin_state(port) != PIN_1)
            return false;
    return true;
}

bool gpio_pad_load(const Context *ctx, const CellInfo *cell)
{
    // Far C is in farads in Quartus QSF. Support numeric scientific notation
    // and the common pF suffix; reject unknown suffixes and omitted loads.
    auto attr = cell->attrs.find(ctx->id("BOARD_MODEL_FAR_C"));
    if (attr == cell->attrs.end() || !attr->second.is_string)
        return false;
    const std::string value = attr->second.as_string();
    char *end = nullptr;
    double load = std::strtod(value.c_str(), &end);
    if (end == value.c_str()) return false;
    if (*end == 'p' || *end == 'P') { load *= 1e-12; ++end; }
    return *end == '\0' && std::isfinite(load) && load >= 0 && load <= 30e-12;
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
    return def;
}

bool dsp_reg_param(const dict<IdString, Property> &params, IdString key)
{
    return dsp_bool_param(params, key, false);
}

IdString dsp_register_key(const CellInfo *cell, const std::string &port)
{
    if (port.find("A[") == 0)
        return id_INREG_CTRL_AX;
    if (port.find("B[") == 0)
        return id_INREG_CTRL_AY;
    if (port.find("C[") == 0)
        return id_INREG_CTRL_BX;
    if (port.find("D[") == 0)
        return id_INREG_CTRL_BY;
    if (port.find("Z[") == 0)
        return id_INREG_CTRL_AZ;
    return IdString();
}

// cyclonev_hps_interface_fpga2sdram arcs (ps), referenced to the atom pins:
// Quartus 17.0.2 TimeQuest, 5CSEBA6U23I7, Slow 1100mV 100C, worst bit of
// each port (outputs: worst of two fits, their CELL delay depends on load).
// TimeQuest times each pin through a CELL delay to or from an internal
// f2sdram~FF node whose uTsu/uTh/uTco are zero, and clocks that node through
// a CELL delay from the port's clock pin. Setup is the data CELL delay less
// the clock one, hold the reverse, and clock-to-output the clock plus output
// CELL delays, with a minimum of zero. Quartus also times a few ganged-port
// controls (e.g. rd_valid_N) from the other ports' clocks; those arcs are
// not modelled.
struct F2sdramGroup
{
    const char *name;
    const char *clock;
    bool output;
    int ports;
    int max[6]; // setup, or maximum clock-to-output
    int hold[6];
};

const F2sdramGroup f2sdram_groups[] = {
        {"cmd_data", "cmd_port_clk", false, 6, {1423, 1392, 1466, 1378, 1388, 1459}, {292, 238, 353, 230, 189, 244}},
        {"cmd_valid", "cmd_port_clk", false, 6, {1320, 1263, 1254, 1196, 1129, 1126}, {116, 20, 107, 2, 2, 2}},
        {"wrack_ready", "cmd_port_clk", false, 6, {1136, 1138, 1148, 1192, 1292, 1327}, {2, 33, 285, 131, 175, 232}},
        {"cmd_ready", "cmd_port_clk", true, 6, {1214, 1177, 1307, 1222, 1245, 1235}, {}},
        {"wrack_data", "cmd_port_clk", true, 6, {1286, 1265, 1325, 1269, 1314, 1306}, {}},
        {"wrack_valid", "cmd_port_clk", true, 6, {1304, 1314, 1349, 1274, 1338, 1311}, {}},
        {"wr_data", "wr_clk", false, 4, {913, 1067, 513, 465}, {395, 323, 380, 311}},
        {"wr_valid", "wr_clk", false, 4, {950, 965, 837, 1112}, {2, 2, 2, 2}},
        {"wr_ready", "wr_clk", true, 4, {1178, 1185, 1218, 1170}, {}},
        {"rd_ready", "rd_clk", false, 4, {1129, 1050, 1081, 1044}, {2, 2, 2, 2}},
        {"rd_data", "rd_clk", true, 4, {1379, 1416, 1417, 1431}, {}},
        {"rd_valid", "rd_clk", true, 4, {1294, 1286, 1306, 1239}, {}},
};

// The group and port of a registered pin "<group>_<port>[<bit>]"; nullptr
// for the clock and cfg_* configuration pins.
const F2sdramGroup *f2sdram_group(const std::string &name, int &index)
{
    size_t end = std::min(name.find('['), name.size());
    size_t sep = name.rfind('_', end);
    if (sep == std::string::npos || sep + 2 != end || name[sep + 1] < '0' || name[sep + 1] > '9')
        return nullptr;
    index = name[sep + 1] - '0';
    for (const auto &group : f2sdram_groups)
        if (index < group.ports && name.compare(0, sep, group.name) == 0)
            return &group;
    return nullptr;
}

bool f2sdram_clock_pin(const std::string &name)
{
    return name.find("cmd_port_clk_") == 0 || name.find("wr_clk_") == 0 || name.find("rd_clk_") == 0;
}

// Unused ports have their clock tied off. A constant is not a clock domain,
// so the pins of such a port stay untimed.
bool f2sdram_clocked(const CellInfo *cell, IdString clock)
{
    const NetInfo *net = cell->getPort(clock);
    return net != nullptr && net->driver.cell != nullptr &&
           !net->driver.cell->type.in(id_GND, id_VCC, id_MISTRAL_CONST);
}

// M10K read clock-to-output (ps) from the RAM's clock pin: Quartus 17.0.2
// TimeQuest, 5CSEBA6U23I7, Slow 1100mV 100C, the envelope over every width,
// bit, edge and fit of each class in mistral/tests/m10k/clock-to-out.
// TimeQuest times an unregistered read from the port's read register through
// the array: about 1.6 ns of internal clock and 1.0 ns of read (2.5 + 0.2 ns
// when either SDP port is 40 bits wide). A registered read starts at the
// output register. The maximum is TimeQuest's setup analysis and the minimum
// its hold analysis, except that an unregistered read keeps the 1004 ps
// minimum every read used to have: this timer checks hold at the slow corner
// only, where TimeQuest holds such a read for at least 1.40 ns, but at the
// fast corners for only 0.72 ns.
DelayQuad m10k_clock_to_out(const Context *ctx, const CellInfo *cell, const std::string &port)
{
    bool tdp = bool_or_default(cell->params, id_CFG_TDP, false);
    int dbits = int_or_default(cell->params, id_CFG_DBITS, 10);
    int rdbits = bool_or_default(cell->params, id_CFG_MIXED_WIDTH, false) ?
            int_or_default(cell->params, id_CFG_RD_DBITS, dbits) : dbits;
    bool wide = !tdp && (dbits == 40 || rdbits == 40);
    // TDP A1Q reads the A output half and B1Q the B half. A 40-bit SDP read
    // takes B1DATA[19:0] from the A half and B1DATA[39:20] from the B half.
    size_t open = port.find('[');
    int bit = open == std::string::npos ? 0 : std::atoi(port.c_str() + open + 1);
    bool a_half = tdp ? port.find("A1Q") == 0 : rdbits == 40 && bit < 20;
    if (ctx->m10k_output_registered(cell, a_half)) {
        if (tdp)
            return a_half ? DelayQuad{592, 744} : DelayQuad{569, 709};
        return wide ? DelayQuad{574, 749} : DelayQuad{566, 709};
    }
    if (tdp)
        return DelayQuad{1004, a_half ? 2610 : 2581};
    return DelayQuad{1004, wide ? 2704 : 2620};
}

} // namespace

TimingPortClass Arch::getPortTimingClass(const CellInfo *cell, IdString port, int &clockInfoCount) const
{
    clockInfoCount = 0;
    const bool io_timed = settings.count(id("timing/io_delays"));
    if (io_timed && cell->type == id_MISTRAL_IB && port == id_O)
        return TMG_STARTPOINT;
    if (io_timed && cell->type == id_MISTRAL_OB && port == id_I)
        return TMG_ENDPOINT;
    if (io_timed && cell->type == id_MISTRAL_IO) {
        if (port == id_O) return TMG_STARTPOINT;
        if (port.in(id_I, id_OE)) return TMG_ENDPOINT;
        return TMG_IGNORE;
    }
    if (gpio_fabric_profile(getCtx(), cell)) {
        if (port.in(id_CLK, id_CLKIN)) return TMG_CLOCK_INPUT;
        if (gpio_input_registered(getCtx(), cell) && port.in(id_Q, id_Q_H, id_Q_L, id_CEIN)) {
            clockInfoCount = 1;
            return port == id_CEIN ? TMG_REGISTER_INPUT : TMG_REGISTER_OUTPUT;
        }
        const bool oe = cell->type == id_MISTRAL_SDRIO && int_or_default(cell->params, id("IOREG_OE"), 0);
        if ((port == id_I && gpio_output_registered(getCtx(), cell)) || (port == id_OE && oe) ||
            (port == id_CEOUT && (gpio_output_registered(getCtx(), cell) || oe))) {
            clockInfoCount = 1;
            return TMG_REGISTER_INPUT;
        }
        // Remaining pad and asynchronous-control paths use the unsupported
        // classifications below; fabric arcs do not imply interface closure.
    }
    if (cell->type.in(id_MISTRAL_SDRIN, id_MISTRAL_DDRIN)) {
        // The Mistral database has no characterized GPIO input-register
        // setup/hold or register clock-to-Q model.
        if (port == id_CLK)
            return TMG_CLOCK_INPUT;
        return port.in(id_CEIN, id_ACLR) ? TMG_ENDPOINT : TMG_IGNORE;
    }
    if (cell->type == id_MISTRAL_SDROUT) {
        // No characterized GPIO register setup/hold or clock-to-pad arcs.
        if (port == id_CLK) return TMG_CLOCK_INPUT;
        return port.in(id_I, id_CEOUT, id_ACLR) ? TMG_ENDPOINT : TMG_IGNORE;
    }
    if (cell->type == id_MISTRAL_SDRIO) {
        // Registered bidirectional pad: register inputs are uncharacterized
        // endpoints; combinational pad paths stay untimed like MISTRAL_IO.
        if (port.in(id_CLK, id_CLKIN))
            return TMG_CLOCK_INPUT;
        if (port.in(id_CEIN, id_CEOUT, id_ACLR))
            return TMG_ENDPOINT;
        if (port == id_I && int_or_default(cell->params, id("IOREG_OUT"), 0))
            return TMG_ENDPOINT;
        if (port == id_OE && int_or_default(cell->params, id("IOREG_OE"), 0))
            return TMG_ENDPOINT;
        return TMG_IGNORE;
    }
    if (cell->type == id_MISTRAL_DDROUT) {
        // No characterized GPIO register setup/hold or clock-to-pad arcs.
        if (port == id_CLK)
            return TMG_CLOCK_INPUT;
        if (port.in(id_D_H, id_D_L))
            return TMG_ENDPOINT;
        return TMG_IGNORE;
    }
    if (cell->type == id_MISTRAL_DDRBIDIR) {
        // The Mistral database has no characterized bidirectional GPIO
        // register setup/hold, clock-to-pad, or clock-to-fabric arcs.
        if (port.in(id_CLK, id_CLKIN))
            return TMG_CLOCK_INPUT;
        if (port.in(id_D_H, id_D_L, id_OE))
            return TMG_ENDPOINT;
        return TMG_IGNORE;
    }
    if (cell->type == id_MISTRAL_CLKENA) {
        if (port == id_A)
            return TMG_CLOCK_INPUT;
        if (port == id_Q)
            return TMG_GEN_CLOCK;
        // The hardware captures ENA on the falling edge, but the backend has
        // no characterized setup/hold model for that clock-control register.
        if (port == id_ENA)
            return TMG_ENDPOINT;
        // Status from the enable register has no characterized clock-to-Q arc.
        if (port == id_ENAOUT)
            return TMG_STARTPOINT;
    }
    if (cell->type == id_altera_pll) {
        if (port == id_refclk)
            return TMG_CLOCK_INPUT;
        if (port == id_outclk || port.str(this).rfind("outclk[", 0) == 0)
            return TMG_GEN_CLOCK;
        // Asynchronous control endpoint; no PLL recovery/removal model.
        if (port == id_rst)
            return TMG_ENDPOINT;
        if (port == id_locked)
            return TMG_STARTPOINT;
    }
    if (cell->type == id_cyclonev_hps_interface_fpga2sdram) {
        const auto &name = port.str(this);
        int index;
        const F2sdramGroup *group = f2sdram_group(name, index);
        // cfg_* are static configuration inputs.
        if (group == nullptr)
            return f2sdram_clock_pin(name) ? TMG_CLOCK_INPUT : TMG_IGNORE;
        if (!f2sdram_clocked(cell, idf("%s_%d", group->clock, index)))
            return TMG_IGNORE;
        clockInfoCount = 1;
        return group->output ? TMG_REGISTER_OUTPUT : TMG_REGISTER_INPUT;
    }
    if (cell->type == id_cyclonev_hps_interface_hps2fpga_light_weight) {
        if (port == id("clk"))
            return TMG_CLOCK_INPUT;
        auto bridge_port = cell->ports.find(port);
        if (bridge_port == cell->ports.end())
            return TMG_IGNORE;
        // The HPS manual identifies the fabric clock domain, but does not
        // provide AXI setup/hold or clock-to-output characterization. Keep
        // connected fabric paths visible as unclocked boundaries, with no
        // fabricated timing budget, and report the coverage gap once.
        if (bridge_port->second.net != nullptr) {
            std::lock_guard<std::mutex> lock(hps_lw_timing_warning_mutex);
            if (!hps_lw_timing_warning_emitted) {
                log_warning("Timing for HPS lightweight bridge '%s' is not covered: no AXI setup/hold or "
                            "clock-to-output characterization is available; connected AXI pins are reported "
                            "as unclocked timing boundaries.\n",
                            cell->name.c_str(this));
                hps_lw_timing_warning_emitted = true;
            }
        }
        if (bridge_port->second.type == PORT_IN)
            return TMG_ENDPOINT;
        if (bridge_port->second.type == PORT_OUT)
            return TMG_STARTPOINT;
        return TMG_IGNORE;
    }
    if (cell->type.in(id_MISTRAL_MUL9X9, id_MISTRAL_MUL18X18, id_MISTRAL_MUL27X27,
                      id_MISTRAL_MUL18X19, id_MISTRAL_MUL18X19_COMBINED)) {
        const auto &name = port.str(this);
        if (port == id_CLK)
            return TMG_CLOCK_INPUT;
        if (name.find("A[") == 0 || name.find("B[") == 0 || name.find("C[") == 0 ||
            name.find("D[") == 0 || name.find("Z[") == 0) {
            IdString reg_key = dsp_register_key(cell, name);
            if (reg_key != IdString() && dsp_reg_param(cell->params, reg_key)) {
                clockInfoCount = 1;
                return TMG_REGISTER_INPUT;
            }
            return TMG_COMB_INPUT;
        }
        if (name.find("Y[") == 0) {
            if (dsp_reg_param(cell->params, id_OREG_CTRL)) {
                clockInfoCount = 1;
                return TMG_REGISTER_OUTPUT;
            }
            return TMG_COMB_OUTPUT;
        }
        if (port.in(id_ACLR, id_ENA, id_ACCUMULATE, id_SUB, id_NEGATE, id_LOADCONST))
            return TMG_ENDPOINT;
    }
    if (cell->type.in(id_MISTRAL_NOT, id_MISTRAL_BUF, id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                      id_MISTRAL_ALUT5, id_MISTRAL_ALUT6)) {
        if (port.in(id_A, id_B, id_C, id_D, id_E, id_F))
            return TMG_COMB_INPUT;
        if (port == id_Q)
            return TMG_COMB_OUTPUT;
    } else if (cell->type == id_MISTRAL_ALUT_ARITH) {
        if (port.in(id_A, id_B, id_C, id_D0, id_D1, id_CI))
            return TMG_COMB_INPUT;
        if (port.in(id_SO, id_CO))
            return TMG_COMB_OUTPUT;
    } else if (cell->type == id_MISTRAL_FF) {
        if (port == id_CLK) {
            return TMG_CLOCK_INPUT;
        }
        // ACLR is considered synchronous for timing purposes.
        else if (port.in(id_DATAIN, id_ACLR, id_ENA, id_SCLR, id_SLOAD, id_SDATA)) {
            clockInfoCount = 1;
            return TMG_REGISTER_INPUT;
        } else if (port == id_Q) {
            clockInfoCount = 1;
            return TMG_REGISTER_OUTPUT;
        }
    } else if (cell->type == id_MISTRAL_MLAB) {
        if (port == id_CLK1) {
            return TMG_CLOCK_INPUT;
        } else if (port.in(id_A1DATA, id_A1EN) || port.str(this).find("A1ADDR") == 0) {
            clockInfoCount = 1;
            return TMG_REGISTER_INPUT;
        } else if (port.str(this).find("B1ADDR") == 0) {
            return TMG_COMB_INPUT;
        } else if (port.in(id_B1DATA)) {
            return TMG_COMB_OUTPUT;
        }
    } else if (cell->type == id_MISTRAL_M10K) {
        const auto &name = port.str(this);
        const bool async_read = bool_or_default(cell->params, id_CFG_ASYNC_READ, false) ||
                                cell->getPort(id_B1EN) == nullptr;
        if (bool_or_default(cell->params, id_CFG_TDP, false)) {
            if (port.in(id_CLK1, id_CLK2))
                return TMG_CLOCK_INPUT;
            if (port.in(id_ACLR0, id_ACLR1))
                return TMG_ENDPOINT;
            if (port.in(id_ADDRSTALLA, id_ADDRSTALLB)) {
                clockInfoCount = 1;
                return TMG_REGISTER_INPUT;
            }
            if (name.find("A1Q") == 0 || name.find("B1Q") == 0) {
                if (async_read)
                    return TMG_COMB_OUTPUT;
                clockInfoCount = 1;
                return TMG_REGISTER_OUTPUT;
            }
            if (name.find("A1") == 0 || name.find("B1") == 0) {
                clockInfoCount = 1;
                return TMG_REGISTER_INPUT;
            }
            return TMG_IGNORE;
        }
        if (async_read) {
            if (port.in(id_CLK1, id_CLK2))
                return TMG_CLOCK_INPUT;
            if (port.in(id_ACLR0, id_ACLR1))
                return TMG_ENDPOINT;
            if (port.in(id_ADDRSTALLA, id_ADDRSTALLB)) {
                clockInfoCount = 1;
                return TMG_REGISTER_INPUT;
            }
            if (port.in(id_A1DATA, id_A1EN, id_A1BE) || name.find("A1DATA[") == 0 ||
                name.find("A1BE[") == 0 || name.find("A1ADDR") == 0) {
                clockInfoCount = 1;
                return TMG_REGISTER_INPUT;
            }
            if (name.find("B1ADDR") == 0)
                return TMG_COMB_INPUT;
            if (port == id_B1DATA || name.find("B1DATA[") == 0)
                return TMG_COMB_OUTPUT;
            return TMG_IGNORE;
        }
        if (port.in(id_CLK1, id_CLK2)) {
            return TMG_CLOCK_INPUT;
        } else if (port.in(id_ACLR0, id_ACLR1)) {
            return TMG_ENDPOINT;
        } else if (port.in(id_ADDRSTALLA, id_ADDRSTALLB)) {
            clockInfoCount = 1;
            return TMG_REGISTER_INPUT;
        } else if (port.in(id_A1DATA, id_A1EN, id_A1BE, id_B1EN) || name.find("A1DATA[") == 0 ||
                   name.find("A1BE[") == 0 ||
                   name.find("A1ADDR") == 0 || name.find("B1ADDR") == 0) {
            clockInfoCount = 1;
            return TMG_REGISTER_INPUT;
        } else if (port == id_B1DATA || name.find("B1DATA[") == 0) {
            clockInfoCount = 1;
            return TMG_REGISTER_OUTPUT;
        }
    }
    return TMG_IGNORE;
}

std::vector<RegisteredIoTiming> Arch::getRegisteredIoTiming(const CellInfo *cell, IdString pad, bool input) const
{
    if (pad != id_PAD || !gpio_pad_profile(getCtx(), cell, input || cell->type == id_MISTRAL_SDRIO))
        return {};
    const bool ddr_in = cell->type == id_MISTRAL_DDRIN ||
            (cell->type == id_MISTRAL_SDRIO && int_or_default(cell->params, id("IOREG_IN"), 0) &&
             int_or_default(cell->params, id("IOREG_IN_DDR"), 0));
    TimingClockingInfo timing{};
    if (cell->type == id_MISTRAL_DDROUT) {
        if (input || !gpio_pad_load(getCtx(), cell)) return {};
        // Constant clock-forwarder fits in both polarities reproduce the
        // varying-data AD20 mux envelope. Its muxsel and register clock pins
        // share identical routing prefixes at every checked corner/edge;
        // check_mux_clock_frame.py audits that reference frame explicitly.
        const bool high = cell->params.at(id_DDR_HIGH).as_int64() != 0;
        timing.clock_port = id_CLK;
        timing.edge = RISING_EDGE;
        timing.clockToQ = DelayQuad(0, 5400);
        RegisteredIoTiming rising{high ? id("rise") : id("fall"), timing};
        timing.edge = FALLING_EDGE;
        timing.clockToQ = DelayQuad(0, 5380);
        return {rising, {high ? id("fall") : id("rise"), timing}};
    }
    // Outward-rounded complete pad envelopes from input-zero-reference30.json.
    // Controlled D1/D3=0 fits match decoded native defaults on all 16 DQ pins;
    // unconstrained Quartus fits can silently insert different delay chains.
    // SDR matches the DDR high word.
    // (both transitions, all actual pins, all four corners). Input hold uses
    // the largest signed requirement, not the smallest observed pad delay.
    if (input) {
        if (!gpio_input_registered(getCtx(), cell)) return {};
        timing.clock_port = cell->type == id_MISTRAL_SDRIO ? id_CLKIN : id_CLK;
        timing.edge = RISING_EDGE;
        timing.setup = DelayPair(1910); timing.hold = DelayPair(-310);
        DelayQuad prefix;
        if (gpio_core_clock_prefix(cell,id_Q,prefix)) {
            // The retained pad envelope starts at the internal clock pin.
            // Move it to the native DCMUX cut using the independently
            // measured local prefix, without crediting global clock delay.
            timing.setup = DelayPair(1910-prefix.minRiseDelay());
            timing.hold = DelayPair(-310+prefix.maxRiseDelay());
        }
        RegisteredIoTiming high{ddr_in ? id("high") : id("rise"), timing};
        if (!ddr_in) return {high};
        timing.edge = FALLING_EDGE;
        timing.setup = DelayPair(1900); timing.hold = DelayPair(-300);
        return {high, {id("low"), timing}};
    }
    if (!gpio_pad_load(getCtx(), cell) || !gpio_output_registered(getCtx(), cell))
        return {};
    // Mixed combinational data/OE would require additional boundary arcs.
    // Reject the whole direction instead of reporting only the data register.
    if (cell->type == id_MISTRAL_SDRIO && !int_or_default(cell->params, id("IOREG_OE"), 0))
        return {};
    timing.clock_port = id_CLK;
    timing.edge = RISING_EDGE;
    // Zero is a conservative early bound. Do not promote the fastest observed
    // fit to a guaranteed silicon minimum. Use the 30pF late bound at all
    // supported declared loads, without extrapolating a load curve.
    timing.clockToQ = DelayQuad(0, 5280);
    DelayQuad prefix;
    if (gpio_core_clock_prefix(cell,id_I,prefix))
        timing.clockToQ = DelayQuad(0,5280+prefix.maxRiseDelay());
    RegisteredIoTiming data{id("data"), timing};
    if (cell->type == id_MISTRAL_SDROUT)
        return {data};
    timing.clockToQ = DelayQuad(0, 5420);
    if (gpio_core_clock_prefix(cell,id_OE,prefix))
        timing.clockToQ = DelayQuad(0,5420+prefix.maxRiseDelay());
    return {data, {id("oe"), timing}};
}

std::vector<PrimitiveClockRequirement> Arch::getPrimitiveClockRequirements(const CellInfo *cell) const
{
    std::vector<PrimitiveClockRequirement> result;
    // Query the same profile as pad arcs. These requirements are normalized
    // to GPIO clock ingress, including local early/late CELL distortion.
    // The DDR input's hidden low-word retiming remains an opaque primitive;
    // its period/pulse requirements must still be met when data paths are cut.
    const bool native_gpio = cell->type.in(id_MISTRAL_SDROUT,id_MISTRAL_SDRIO) &&
            bool_or_default(getCtx()->settings,getCtx()->id("mistral/gpio_core_reference"),false);
    if (native_gpio) {
        // Mandatory even without pad-profile attributes or dynamic data:
        // common STA requires an inferred physical waveform. Its same-edge
        // period also enforces the local reference's 10ns domain boundary.
        if (cell->type == id_MISTRAL_SDRIO)
            result.push_back({id_CLKIN,10000,170,190});
        result.push_back({id_CLK,10000,cell->type == id_MISTRAL_SDRIO ? 810 : 790,
                         cell->type == id_MISTRAL_SDRIO ? 780 : 770});
    } else {
        if (!getRegisteredIoTiming(cell, id_PAD, true).empty())
            result.push_back({cell->type == id_MISTRAL_SDRIO ? id_CLKIN : id_CLK, 1540, 170, 190});
        if (!getRegisteredIoTiming(cell, id_PAD, false).empty())
            result.push_back({id_CLK, 1540, cell->type == id_MISTRAL_SDRIO ? 810 : 790,
                             cell->type == id_MISTRAL_SDRIO ? 780 : 770});
    }
    for (auto &requirement : result) {
        DelayQuad prefix;
        if (gpio_core_clock_prefix(cell,requirement.clock_port == id_CLKIN ? id_Q : id_I,prefix)) {
            DelayQuad oe;
            if (requirement.clock_port == id_CLK && cell->type == id_MISTRAL_SDRIO &&
                gpio_core_clock_prefix(cell,id_OE,oe))
                prefix = DelayQuad{std::min(prefix.minRiseDelay(),oe.minRiseDelay()),
                                   std::max(prefix.maxRiseDelay(),oe.maxRiseDelay()),
                                   std::min(prefix.minFallDelay(),oe.minFallDelay()),
                                   std::max(prefix.maxFallDelay(),oe.maxFallDelay())};
            requirement.min_high += std::max<delay_t>(0,prefix.maxRiseDelay()-prefix.minFallDelay());
            requirement.min_low += std::max<delay_t>(0,prefix.maxFallDelay()-prefix.minRiseDelay());
        }
    }
    return result;
}

TimingClockingInfo Arch::getPortClockingInfo(const CellInfo *cell, IdString port, int index) const
{
    TimingClockingInfo timing{};
    if (gpio_core_reference(cell, port, timing))
        return timing;
    if (gpio_fabric_profile(getCtx(), cell)) {
        const bool input = port.in(id_Q, id_Q_H, id_Q_L, id_CEIN);
        timing.clock_port = input && cell->type == id_MISTRAL_SDRIO ? id_CLKIN : id_CLK;
        // DDIO low data is retimed to the rising edge before entering fabric.
        // Its external pad capture edge is separate and is not modeled here.
        timing.edge = RISING_EDGE;
        if (port.in(id_Q, id_Q_H, id_Q_L))
            timing.clockToQ = DelayQuad{0, 870};
        else {
            timing.setup = DelayPair{120, 120};
            timing.hold = input ? DelayPair{660, 660} : DelayPair{310, 310};
        }
        return timing;
    }
    if (cell->type.in(id_MISTRAL_MUL9X9, id_MISTRAL_MUL18X18, id_MISTRAL_MUL27X27,
                      id_MISTRAL_MUL18X19, id_MISTRAL_MUL18X19_COMBINED)) {
        timing.clock_port = id_CLK;
        timing.edge = RISING_EDGE;
        const auto &name = port.str(this);
        if (name.find("A[") == 0 || name.find("B[") == 0 || name.find("C[") == 0 ||
            name.find("D[") == 0 || name.find("Z[") == 0) {
            IdString reg_key = dsp_register_key(cell, name);
            if (reg_key != IdString() && dsp_reg_param(cell->params, reg_key)) {
                timing.setup = DelayPair{125, 125};
                timing.hold = DelayPair{42, 42};
            }
        } else if (name.find("Y[") == 0 && dsp_reg_param(cell->params, id_OREG_CTRL)) {
            timing.clockToQ = DelayQuad{1000};
        }
        return timing;
    } else if (cell->type == id_cyclonev_hps_interface_fpga2sdram) {
        int index;
        const F2sdramGroup *group = f2sdram_group(port.str(this), index);
        NPNR_ASSERT(group != nullptr);
        timing.clock_port = idf("%s_%d", group->clock, index);
        timing.edge = RISING_EDGE;
        if (group->output) {
            timing.clockToQ = DelayQuad{0, group->max[index]};
        } else {
            timing.setup = DelayPair{group->max[index], group->max[index]};
            timing.hold = DelayPair{group->hold[index], group->hold[index]};
        }
        return timing;
    } else if (cell->type == id_MISTRAL_FF) {
        timing.clock_port = id_CLK;
        auto clock_pin = cell->pin_data.find(id_CLK);
        timing.edge = clock_pin != cell->pin_data.end() && clock_pin->second.state == PIN_INV ?
                FALLING_EDGE : RISING_EDGE;
        if (port == id_ACLR && ff_reset_reference(getCtx(), cell)) {
            // The ordinary setup/hold equations express recovery/removal here.
            timing.setup = DelayPair{170, 170};
            timing.hold = DelayPair{270, 270};
            timing.clockToQ = DelayQuad{};
        } else if (port.in(id_DATAIN, id_ACLR, id_ENA, id_SCLR, id_SLOAD, id_SDATA)) {
            // Default ACLR handling retains the historical data approximation.
            timing.setup = DelayPair{-196, -196};
            timing.hold = DelayPair{270, 270};
            timing.clockToQ = DelayQuad{};
        } else if (port == id_Q) {
            timing.setup = DelayPair{};
            timing.hold = DelayPair{};
            timing.clockToQ = DelayQuad{731};
        }
        ff_core_reference(cell, port, timing);
        return timing;
    } else if (cell->type == id_MISTRAL_MLAB) {
        timing.clock_port = id_CLK1;
        timing.edge = RISING_EDGE;
        if (port.in(id_A1DATA, id_A1EN) || port.str(this).find("A1ADDR") == 0) {
            timing.setup = DelayPair{86, 86};
            timing.hold = DelayPair{42, 42};
            timing.clockToQ = DelayQuad{};
        }
        return timing;
    } else if (cell->type == id_MISTRAL_M10K) {
        const auto &name = port.str(this);
        const bool async_read = bool_or_default(cell->params, id_CFG_ASYNC_READ, false) ||
                                cell->getPort(id_B1EN) == nullptr;
        auto clock_edge = [&](IdString clock_port) {
            auto clock_pin = cell->pin_data.find(clock_port);
            return clock_pin != cell->pin_data.end() && clock_pin->second.state == PIN_INV ? FALLING_EDGE : RISING_EDGE;
        };
        if (bool_or_default(cell->params, id_CFG_TDP, false)) {
            timing.clock_port = name.find("B1") == 0 ? id_CLK2 : id_CLK1;
            if (port == id_ADDRSTALLB)
                timing.clock_port = id_CLK2;
            timing.edge = clock_edge(timing.clock_port);
            if ((name.find("A1Q") == 0 || name.find("B1Q") == 0) && !async_read) {
                timing.clockToQ = m10k_clock_to_out(getCtx(), cell, name);
            } else {
                timing.hold = DelayPair{42, 42};
                if (name.find("ADDR") != std::string::npos)
                    timing.setup = DelayPair{125, 125};
                else if (name.find("DATA") != std::string::npos)
                    timing.setup = DelayPair{97, 97};
                else if (port.in(id_A1WE, id_B1WE) || name.find("A1BE[") == 0 || name.find("B1BE[") == 0)
                    timing.setup = DelayPair{140, 140};
                else if (port.in(id_A1EN, id_B1EN))
                    timing.setup = DelayPair{161, 161};
            }
            return timing;
        }
        if (async_read && (name.find("B1ADDR") == 0 || port == id_B1DATA || name.find("B1DATA[") == 0))
            return timing;
        bool read_port = port.in(id_B1DATA, id_B1EN) || name.find("B1DATA[") == 0 || name.find("B1ADDR") == 0;
        timing.clock_port = read_port && bool_or_default(cell->params, id_CFG_DUAL_CLOCK, false) ? id_CLK2 : id_CLK1;
        if (port == id_ADDRSTALLB && bool_or_default(cell->params, id_CFG_DUAL_CLOCK, false))
            timing.clock_port = id_CLK2;
        timing.edge = clock_edge(timing.clock_port);
        if (port.in(id_ADDRSTALLA, id_ADDRSTALLB)) {
            timing.setup = DelayPair{125, 125};
            timing.hold = DelayPair{42, 42};
            timing.clockToQ = DelayQuad{};
        } else if (port.str(this).find("A1ADDR") == 0 || port.str(this).find("B1ADDR") == 0) {
            timing.setup = DelayPair{125, 125};
            timing.hold = DelayPair{42, 42};
            timing.clockToQ = DelayQuad{};
        } else if (port == id_A1DATA || name.find("A1DATA[") == 0) {
            timing.setup = DelayPair{97, 97};
            timing.hold = DelayPair{42, 42};
            timing.clockToQ = DelayQuad{};
        } else if (port == id_A1EN || port == id_A1BE || name.find("A1BE[") == 0) {
            timing.setup = DelayPair{140, 140};
            timing.hold = DelayPair{42, 42};
            timing.clockToQ = DelayQuad{};
        } else if (port == id_B1EN) {
            timing.setup = DelayPair{161, 161};
            timing.hold = DelayPair{42, 42};
            timing.clockToQ = DelayQuad{};
        } else if (port == id_B1DATA || name.find("B1DATA[") == 0) {
            timing.setup = DelayPair{};
            timing.hold = DelayPair{};
            timing.clockToQ = m10k_clock_to_out(getCtx(), cell, name);
        }
        return timing;
    }
    NPNR_ASSERT_FALSE("unreachable");
}

bool Arch::get_lut_pin_delay(IdString physical, bool l6, DelayQuad &delay) const
{
    switch (physical.index) {
    case ID_A:
        // L5 and L6 share this slow input. Quartus slow 1100 mV 100°C
        // combout on 5CSEBA6U23I7 is about 598 ps.
        delay = DelayQuad{592, 605, 567, 573};
        return true;
    case ID_B:
        delay = DelayQuad{580, 583, 560, 574};
        return true;
    case ID_C:
    case ID_D:
        // C/D exchange the middle mux levels between the two L6
        // halves; keep the earliest early and latest late delays.
        delay = l6 ? DelayQuad{429, 499, 440, 512} : DelayQuad{432, 499, 444, 512};
        return true;
    case ID_E0:
    case ID_E1:
        // Slow 1100 mV 100°C envelope of the two ALMs retained in
        // mistral/tests/lut6-select. Rise is a rising input (rr/rf)
        // and fall is a falling input (fr/ff). The previous 400 ps
        // fall bound sat above every measured E transition.
        delay = DelayQuad{204, 297, 217, 335};
        return true;
    case ID_F0:
    case ID_F1:
        delay = DelayQuad{90, 96, 83, 97};
        return true;
    default:
        return false;
    }
}

bool Arch::getCellDelay(const CellInfo *cell, IdString fromPort, IdString toPort, DelayQuad &delay) const
{
    if(lut_core_delay(cell,fromPort,toPort,delay)) {
        delay += get_lut_input_delay(cell,fromPort);
        return true;
    }
    if (cell->type.in(id_MISTRAL_MUL9X9, id_MISTRAL_MUL18X18, id_MISTRAL_MUL27X27,
                      id_MISTRAL_MUL18X19, id_MISTRAL_MUL18X19_COMBINED) &&
        toPort.str(this).find("Y[") == 0) {
        // Cyclone V arcs from Yosys techlibs/intel_alm/common/dsp_sim.v.
        const auto &from_name = fromPort.str(this);
        if (from_name.find("A[") == 0 ||
            (from_name.find("C[") == 0 && cell->type.in(id_MISTRAL_MUL18X19, id_MISTRAL_MUL18X19_COMBINED))) {
            delay = cell->type == id_MISTRAL_MUL18X18 ? DelayQuad{3180} :
                    cell->type == id_MISTRAL_MUL27X27 ? DelayQuad{3732} :
                    cell->type.in(id_MISTRAL_MUL18X19, id_MISTRAL_MUL18X19_COMBINED) ? DelayQuad{3180} :
                                                                                         DelayQuad{2818};
            return true;
        }
        if (from_name.find("B[") == 0 || from_name.find("D[") == 0 ||
            (from_name.find("C[") == 0 && cell->type == id_MISTRAL_MUL18X18)) {
            delay = cell->type == id_MISTRAL_MUL18X18 ? DelayQuad{3982} :
                    cell->type == id_MISTRAL_MUL27X27 ? DelayQuad{3928} :
                    cell->type.in(id_MISTRAL_MUL18X19, id_MISTRAL_MUL18X19_COMBINED) ? DelayQuad{3982} :
                                                                                         DelayQuad{3051};
            return true;
        }
        // The 9x9 preadder feeds the multiplier through its Y operand. The
        // 18x18 P36 mode uses C as a 36-bit addend; model both with the
        // conservative B-side datapath delay.
        if (from_name.find("Z[") == 0 && cell->type == id_MISTRAL_MUL9X9 &&
            dsp_bool_param(cell->params, id_PREADDER_EN)) {
            delay = DelayQuad{3051};
            return true;
        }
        if (from_name.find("C[") == 0 && cell->type == id_MISTRAL_MUL18X18) {
            delay = DelayQuad{3982};
            return true;
        }
    }
    // Based on 1.1V 100C timing corner of sx120f, using delays from LUT input to DFF input.

    // I have many regrets about naming my cell ports how I did...

    // TODO list:
    // - MLABs-as-LABs have different timings to LABs
    // - speed grades

    if (cell->type.in(id_MISTRAL_NOT, id_MISTRAL_BUF, id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                      id_MISTRAL_ALUT5, id_MISTRAL_ALUT6)) {
        if (toPort == id_Q) {
            // Logical input names no longer identify mux levels after
            // reassign_alm_inputs. Before binding, use the placement pin map
            // so hypothetical cells and their real trials use the same model.
            const int width = cell->type == id_MISTRAL_ALUT6 ? 6 : cell->type == id_MISTRAL_ALUT5 ? 5 :
                              cell->type == id_MISTRAL_ALUT4 ? 4 : cell->type == id_MISTRAL_ALUT3 ? 3 :
                              cell->type == id_MISTRAL_ALUT2 ? 2 : 1;
            const std::array<IdString, 6> inputs{id_A, id_B, id_C, id_D, id_E, id_F};
            if (std::find(inputs.begin(), inputs.begin() + width, fromPort) == inputs.begin() + width)
                return false;
            // Match the isolated-cell allocation used by reassign_alm_inputs.
            // comb_pinmap is a routing scaffold, not the final mux allocation.
            IdString physical = lut_placement_pin(cell, fromPort);
            auto pin = cell->pin_data.find(fromPort);
            // Planning copies can inherit a BEL and pin map from a cell of
            // another type. Only its actual occupant owns the physical map.
            if (cell->bel != BelId() && getBoundBelCell(cell->bel) == cell && pin != cell->pin_data.end() &&
                pin->second.bel_pins.size() == 1)
                physical = pin->second.bel_pins.front();
            const bool l6 = cell->type == id_MISTRAL_ALUT6;
            if (!get_lut_pin_delay(physical, l6, delay))
                return false;
            delay += get_lut_input_delay(cell, fromPort);
            return true;
        }
    } else if (cell->type == id_MISTRAL_ALUT_ARITH) {
        if (toPort == id_CO) {
            if (fromPort == id_A) {
                delay = DelayQuad{/* RF */ 1005, /* RR */ 1082, /* FF */ 971, /* FR */ 1048};
                return true;
            } else if (fromPort == id_B) {
                delay = DelayQuad{/* RF */ 986, /* RR */ 1062, /* FF */ 976, /* FR */ 1052};
                return true;
            } else if (fromPort == id_C) {
                delay = DelayQuad{/* RF */ 736, /* RR */ 813, /* FF */ 775, /* FR */ 800};
                return true;
            } else if (fromPort == id_D0) {
                delay = DelayQuad{/* RF */ 822, /* RR */ 866, /* FF */ 837, /* FR */ 849};
                return true;
            } else if (fromPort == id_D1) {
                delay = DelayQuad{/* RF */ 1122, /* RR */ 1198, /* FF */ 1128, /* FR */ 1197};
                return true;
            } else if (fromPort == id_CI) {
                // Divided by 2 to account for delay being across ALM rather than across ALUT.
                // Maybe this should be a routing delay.
                delay = DelayQuad{/* RR */ 63 / 2, /* RR */ 71 / 2, /* FF */ 63 / 2, /* FR */ 71 / 2};
                return true;
            }
        } else if (toPort == id_SO) {
            if (fromPort == id_A) {
                delay = DelayQuad{/* RF */ 1300, /* RR */ 1342, /* FF */ 1266, /* FR */ 1308};
                return true;
            } else if (fromPort == id_B) {
                delay = DelayQuad{/* RF */ 1280, /* RR */ 1323, /* FF */ 1270, /* FR */ 1313};
                return true;
            } else if (fromPort == id_C) {
                delay = DelayQuad{/* RF */ 866, /* RR */ 892, /* FF */ 908, /* FR */ 927};
                return true;
            } else if (fromPort == id_D0) {
                delay = DelayQuad{/* RR */ 779, /* RF */ 887, /* FR */ 761, /* FF */ 883};
                return true;
            } else if (fromPort == id_D1) {
                delay = DelayQuad{/* RR */ 700, /* RF */ 785, /* FR */ 696, /* FF */ 782};
                return true;
            } else if (fromPort == id_CI) {
                delay = DelayQuad{/* RR */ 350, /* RF */ 352, /* FF */ 361, /* FR */ 368};
                return true;
            }
        }
    } else if (cell->type == id_MISTRAL_MLAB) {
        if (toPort == id_B1DATA) {
            if (fromPort.str(this) == "B1ADDR[0]") {
                delay = DelayQuad{/* RF */ 473, /* RR */ 487, /* FR */ 452, /* FF */ 476};
                return true;
            } else if (fromPort.str(this) == "B1ADDR[1]") {
                delay = DelayQuad{/* RF */ 472, /* RR */ 475, /* FR */ 444, /* FF */ 460};
                return true;
            } else if (fromPort.str(this) == "B1ADDR[2]") {
                delay = DelayQuad{/* RF */ 343, /* RR */ 347, /* FR */ 358, /* FF */ 382};
                return true;
            } else if (fromPort.str(this) == "B1ADDR[3]") {
                delay = DelayQuad{/* RF */ 263, /* RR */ 268, /* FF */ 256, /* FR */ 284};
                return true;
            } else if (fromPort.str(this) == "B1ADDR[4]") {
                delay = DelayQuad{/* RF */ 89, /* RR */ 96, /* FF */ 73, /* FR */ 93};
                return true;
            }
        }
    } else if (cell->type == id_MISTRAL_M10K &&
               (bool_or_default(cell->params, id_CFG_ASYNC_READ, false) || cell->getPort(id_B1EN) == nullptr) &&
               (((toPort == id_B1DATA || toPort.str(this).find("B1DATA[") == 0) &&
                 fromPort.str(this).find("B1ADDR") == 0) ||
                (bool_or_default(cell->params, id_CFG_TDP, false) &&
                 (((toPort.str(this).find("A1Q") == 0) && fromPort.str(this).find("A1ADDR") == 0) ||
                  ((toPort.str(this).find("B1Q") == 0) && fromPort.str(this).find("B1ADDR") == 0))))) {
        // Mistral does not yet contain a characterized M10K address-to-data
        // arc.  Use a conservative 1.5 ns estimate so an asynchronous read
        // is visible to host timing without pretending to be silicon data.
        delay = DelayQuad{1500};
        return true;
    }

    return false;
}

DelayQuad Arch::getPipDelay(PipId pip) const
{
    DelayQuad table = getPipDelayTable(pip);
    return pip_delay_calibrated ? getPipDelayCalibrated(pip, table) : table;
}

DelayQuad Arch::getPipDelayTable(PipId pip) const
{
    WireId src = getPipSrcWire(pip), dst = getPipDstWire(pip);

    if (src.is_nextpnr_created())
        return DelayQuad{20};

    // This is guesswork based on average of (interconnect delay / number of pips)
    auto src_type = src.node.t();

    switch (src_type) {
    // Measured with the analogue model on the FES ZX81, ColecoVision and
    // Pong cores (mistral/tests/gpurouter/qor.py --arc-dump): these types
    // had placeholder entries of 0 or 20 ps but simulate at 44-176 ps on
    // every design, so each arc through them was 50-150 ps optimistic.
    case CycloneV::rnode_type_t::GOUT:
        return DelayQuad{block_input_table_delay};
    case CycloneV::rnode_type_t::LD:
        return DelayQuad{105};
    case CycloneV::rnode_type_t::TCLK:
        return DelayQuad{45};
    case CycloneV::rnode_type_t::XCLKB2A:
        return DelayQuad{50};
    case CycloneV::rnode_type_t::SCLK:
        return DelayQuad{136, 136, 139, 139};
    case CycloneV::rnode_type_t::SCLKB1:
        return DelayQuad{296, 296, 370, 370};
    case CycloneV::rnode_type_t::SCLKB2:
        return DelayQuad{71, 71, 83, 83};
    case CycloneV::rnode_type_t::HCLK:
        return DelayQuad{183, 183, 239, 239};
    case CycloneV::rnode_type_t::HCLKB:
        return DelayQuad{165, 165, 244, 244};
    case CycloneV::rnode_type_t::XCLKB1:
        return DelayQuad{97, 97, 125, 125};
    case CycloneV::rnode_type_t::GIN:
        return DelayQuad{100};
    case CycloneV::rnode_type_t::H14:
        return DelayQuad{273, 286, 288, 291};
    case CycloneV::rnode_type_t::H3:
        return DelayQuad{196, 226, 163, 173};
    case CycloneV::rnode_type_t::H6:
        return DelayQuad{220, 275, 199, 217};
    case CycloneV::rnode_type_t::V12:
        return DelayQuad{361, 374, 337, 340};
    case CycloneV::rnode_type_t::V2:
        return DelayQuad{214, 231, 163, 175};
    case CycloneV::rnode_type_t::V4:
        return DelayQuad{290, 294, 243, 245};
    case CycloneV::rnode_type_t::WM:
        // WM explicitly has zero delay.
        return DelayQuad{0};
    case CycloneV::rnode_type_t::TD:
        return DelayQuad{208, 208, 177, 177};
    default:
        return dst.is_nextpnr_created() ? DelayQuad{20} : DelayQuad{0};
    }
}

DelayQuad Arch::get_lut_input_delay(const CellInfo *cell, IdString port) const
{
    if (!boolean_lut_input(cell, port))
        return DelayQuad(0);
    // Use the existing GOUT table bound until a routed observation is
    // available. Zero is the conservative early bound: an estimated mux
    // must not manufacture hold margin before analogue signoff.
    const DelayQuad estimate{0, block_input_table_delay};
    if ((!analogue_cache_valid && !bitstream_configured) || cell->bel == BelId() || getBoundBelCell(cell->bel) != cell)
        return estimate;
    auto p = cell->ports.find(port);
    if (p == cell->ports.end() || !p->second.net)
        return estimate;
    auto *net = p->second.net;
    if (!net->users.count(p->second.user_idx))
        return estimate;
    const auto &user = net->users.at(p->second.user_idx);
    if (user.cell != cell || user.port != port)
        return estimate; // A planning copy can inherit another cell's index.
    if (analogue_cache_valid) {
        auto fnd = analogue_arc_cache.find(&user);
        if (fnd != analogue_arc_cache.end() && fnd->second.ok)
            return fnd->second.lut_input_delay;
    } else if (bitstream_configured && !net->wires.empty()) {
        DelayQuad route, input;
        if (analogue_arc_delay(net, user, route, nullptr, &input))
            return input;
    }
    if(use_lut_core_reference && bitstream_configured)
        log_error("LUT core reference requires a complete native input-buffer observation for '%s.%s'.\n",
                  cell->name.c_str(getCtx()),port.c_str(getCtx()));
    return estimate;
}

bool Arch::getArcDelayOverride(const NetInfo *net_info, const PortRef &sink, DelayQuad &delay) const
{
    // A cached observation stays valid while the arc's route is unchanged,
    // also after the bitstream state has been dropped for re-routing: the
    // analogue repair removes the entries of every net it rips up, so the
    // router sees the analogue delay of the routes it keeps and the delay
    // table only for the ones it moves.
    if (analogue_cache_valid) {
        auto fnd = analogue_arc_cache.find(&sink);
        if (fnd != analogue_arc_cache.end()) {
            // The full observation is used for route-candidate comparison.
            // STA charges its terminal LUT mux through getCellDelay instead.
            delay = fnd->second.delay - fnd->second.lut_input_delay;
            return fnd->second.ok;
        }
    }
    if (!this->bitstream_configured)
        return false;
    DelayQuad input;
    const bool ok = analogue_arc_delay(net_info, sink, delay, nullptr, &input);
    if (ok)
        delay -= input;
    return ok;
}

bool Arch::analogue_arc_delay(const NetInfo *net_info, const PortRef &sink, DelayQuad &delay,
                              std::vector<AnalogueHop> *hops, DelayQuad *lut_input_delay) const
{
    WireId src_wire = getCtx()->getNetinfoSourceWire(net_info);
    WireId dst_wire = getCtx()->getNetinfoSinkWire(net_info, sink, 0);
    NPNR_ASSERT(src_wire != WireId());
    if (lut_input_delay)
        *lut_input_delay = DelayQuad(0);
    const bool terminal_lut = boolean_lut_input(sink.cell, sink.port) && !dst_wire.is_nextpnr_created() &&
                              dst_wire.node.t() == CycloneV::GOUT;
    const bool strict_lut = terminal_lut && use_lut_core_reference;
    const bool minimum_reference = strict_lut && use_lut_route_min_reference;
    if (minimum_reference)
        validate_lut_route_min_reference();
    // SCLR has a physical GOUT pin, while ENA/ACLR and bypass data have
    // virtual suffixes that already simulate this buffer. The reference FF
    // constraint begins after it, so retain its delay on the routed net.
    const bool terminal_ff = sink.cell->type == id_MISTRAL_FF && sink.port == id_SCLR &&
                             !dst_wire.is_nextpnr_created() && dst_wire.node.t() == CycloneV::GOUT &&
                             bool_or_default(getCtx()->settings, getCtx()->id("mistral/ff_core_reference"), false);
    TimingClockingInfo gpio_timing{};
    const bool terminal_gpio = sink.port.in(id_I,id_OE) &&
                               gpio_core_reference(sink.cell,sink.port,gpio_timing);
    const auto &driver = net_info->driver;
    const bool gpio_source = driver.cell && driver.port == id_Q &&
                             gpio_core_reference(driver.cell,driver.port,gpio_timing);
    DelayQuad gpio_prefix;
    const bool gpio_clock = sink.port.in(id_CLK,id_CLKIN) &&
                           gpio_core_clock_prefix(sink.cell,sink.port == id_CLKIN ? id_Q : id_I,gpio_prefix);
    const bool strict_gpio = terminal_gpio || gpio_source || gpio_clock;
    auto unavailable = [&]() {
        if (strict_lut)
            log_error("LUT core reference requires a complete native analogue observation for net '%s' to '%s.%s'.\n",
                      net_info->name.c_str(getCtx()),sink.cell->name.c_str(getCtx()),sink.port.c_str(getCtx()));
        if (terminal_ff)
            log_error("FF core reference requires a complete SCLR input-buffer observation for '%s'.\n",
                      sink.cell->name.c_str(getCtx()));
        if (strict_gpio)
            log_error("GPIO core reference requires a complete native analogue observation for net '%s' to '%s.%s'.\n",
                      net_info->name.c_str(getCtx()),sink.cell->name.c_str(getCtx()),sink.port.c_str(getCtx()));
        return false;
    };

    bool inverted = false;
    mistral::AnalogSim::wave input_wave[2], output_wave[2];
    mistral::AnalogSim::time_interval output_delays[2];
    mistral::AnalogSim::time_interval output_delay_sum[2];
    std::vector<std::pair<mistral::CycloneV::rnode_index, int>> outputs;
    auto temp = mistral::CycloneV::T_100;
    auto est = mistral::CycloneV::EST_SLOW;

    output_delay_sum[0].mi = 0;
    output_delay_sum[0].mx = 0;
    output_delay_sum[1].mi = 0;
    output_delay_sum[1].mx = 0;

    // Mistral's analogue simulator propagates from source to destination,
    // but nextpnr finds paths from destination to source, so some slight
    // contortions are necessary.

    std::vector<PipId> pips;

    WireId cursor = dst_wire;
    while (cursor != WireId() && cursor != src_wire) {
        auto it = net_info->wires.find(cursor);

        if (it == net_info->wires.end())
            break;

        PipId pip = it->second.pip;
        if (pip == PipId())
            break;

        pips.push_back(pip);
        cursor = getPipSrcWire(pip);
    }
    if ((terminal_ff || strict_gpio || strict_lut) && cursor != src_wire)
        return unavailable();

    for (size_t i = 0; i < pips.size() + size_t(terminal_lut || terminal_ff || terminal_gpio); ++i) {
        const bool terminal = i == pips.size();
        PipId pip = terminal ? PipId() : pips[pips.size() - 1 - i];
        auto src = terminal ? dst_wire : getPipSrcWire(pip);
        auto dst = terminal ? WireId() : getPipDstWire(pip);

        // A dedicated GPIO -> FPLL edge represents the COMBOUT clock tap,
        // not a load on the GPIO's fabric DATAIN routing node. Mistral has
        // no analogue model for that tap; retain the estimated arc instead.
        if (!terminal && pll_ref_select.count(pip))
            return unavailable();

        // An unfinished hop is not a zero-delay observation. Retain completed
        // prefixes even when a later hop cannot be simulated.
        AnalogueHop hop{pip, 0, 0, 0};
        if (hops && !terminal)
            hop.table = getPipDelayTable(pip).maxDelay();

        if (src.is_nextpnr_created()) {
            if (hops)
                hops->push_back(hop);
            continue;
        }

        // A nextpnr-created destination (bel pin) never appears among the
        // circuit outputs; the nodes that drive one have no analogue circuit
        const CycloneV::rnode_index src_ri = cyclonev->rc2ri(src.node);
        const CycloneV::rnode_index dst_ri =
                terminal || dst.is_nextpnr_created() ? 0xffffffff : cyclonev->rc2ri(dst.node);

        auto mode = cyclonev->rnode_timing_get_mode(src_ri);
        NPNR_ASSERT(mode != mistral::CycloneV::RTM_UNSUPPORTED);
        if (minimum_reference && !lut_route_minimum::data_element(src.node.t()) &&
            !(src.node.t() == CycloneV::WM &&
              (mode == CycloneV::RTM_P2P || mode == CycloneV::RTM_NO_DELAY)))
            log_error("LUT route minimum reference does not cover routing element '%s'.\n",
                      src.node.to_string().c_str());

        auto inverting = cyclonev->rnode_is_inverting(src_ri);

        if (mode == mistral::CycloneV::RTM_P2P) {
            if (hops && !terminal)
                hops->push_back(hop);
            if (inverting == mistral::CycloneV::INV_YES || inverting == mistral::CycloneV::INV_PROGRAMMABLE)
                inverted = !inverted;
            continue;
        }

        if (mode == mistral::CycloneV::RTM_NO_DELAY) {
            if (hops && !terminal)
                hops->push_back(hop);
            if (inverting)
                inverted = !inverted;
            continue;
        }

        if (input_wave[0].empty()) {
            if (gpio_source) {
#ifdef MISTRAL_GPIO_SOURCE_WAVE
                const auto position = driver.cell->bel.pos;
                const auto bi = int8_t(bel_data(driver.cell->bel).block_index);
                const CycloneV::pnode_coords source{CycloneV::GPIO,position,CycloneV::DATAIN,bi,3};
                const bool rising = cyclonev->gpio_timing_build_input_wave(source,src_ri,temp,CycloneV::DELAY_MAX,
                        inverted ? CycloneV::RF_FALL : CycloneV::RF_RISE,est,input_wave[0]);
                const bool falling = cyclonev->gpio_timing_build_input_wave(source,src_ri,temp,CycloneV::DELAY_MAX,
                        inverted ? CycloneV::RF_RISE : CycloneV::RF_FALL,est,input_wave[1]);
                if (!rising || !falling)
                    return unavailable();
#else
                log_error("GPIO core reference requires Mistral with MISTRAL_GPIO_SOURCE_WAVE; rebuild nextpnr.\n");
#endif
            } else {
                cyclonev->rnode_timing_build_input_wave(src_ri, temp, CycloneV::DELAY_MAX,
                                                    inverted ? mistral::CycloneV::RF_FALL : mistral::CycloneV::RF_RISE,
                                                    est, input_wave[0]);
                cyclonev->rnode_timing_build_input_wave(src_ri, temp, CycloneV::DELAY_MAX,
                                                    inverted ? mistral::CycloneV::RF_RISE : mistral::CycloneV::RF_FALL,
                                                    est, input_wave[1]);
            }
            if (input_wave[mistral::CycloneV::RF_RISE].empty() || input_wave[mistral::CycloneV::RF_FALL].empty())
                return unavailable();
        }

        for (int edge = 0; edge != 2; edge++) {
            auto actual_edge = edge       ? inverted ? mistral::CycloneV::RF_RISE : mistral::CycloneV::RF_FALL
                               : inverted ? mistral::CycloneV::RF_FALL
                                          : mistral::CycloneV::RF_RISE;
            mistral::AnalogSim sim;
            int input = -1;
            std::vector<std::pair<mistral::CycloneV::rnode_index, int>> outputs;
            cyclonev->rnode_timing_build_circuit(src_ri, temp, CycloneV::DELAY_MAX, actual_edge, sim, input, outputs);
            if ((strict_gpio || strict_lut) && input < 0)
                return unavailable();
            sim.set_input_wave(input, input_wave[edge]);
            auto o = std::find_if(
                    outputs.begin(), outputs.end(),
                    [&](std::pair<mistral::CycloneV::rnode_index, int> output) { return output.first == dst_ri; });
            if ((strict_gpio || strict_lut) && o == outputs.end())
                return unavailable();
            NPNR_ASSERT(o != outputs.end());

            output_wave[edge].clear();
            sim.set_output_wave(o->second, output_wave[edge], output_delays[edge]);
            sim.run();
            if (minimum_reference &&
                !lut_route_minimum::enclose_minimum(lut_route_minimum::slow100_data_scale, output_delays[edge]))
                log_error("LUT route minimum reference does not cover the interval at '%s'.\n",
                          src.node.to_string().c_str());
            if ((terminal_ff || strict_gpio || strict_lut) && (output_wave[edge].empty() || !std::isfinite(output_delays[edge].mi) ||
                                !std::isfinite(output_delays[edge].mx) ||
                                output_delays[edge].mi > output_delays[edge].mx))
                return unavailable();
            if(strict_lut) {
                for(const auto &point:output_wave[edge])
                    if(!std::isfinite(point.t) || !std::isfinite(point.v))return unavailable();
                for(double v:{output_delay_sum[edge].mi+output_delays[edge].mi,
                              output_delay_sum[edge].mx+output_delays[edge].mx})
                    if(v*1e12 <= std::numeric_limits<delay_t>::min() ||
                       v*1e12 >= std::numeric_limits<delay_t>::max())return unavailable();
            }
            cyclonev->rnode_timing_trim_wave(temp, CycloneV::DELAY_MAX, output_wave[edge], input_wave[edge]);

            output_delay_sum[edge].mi += output_delays[edge].mi;
            output_delay_sum[edge].mx += output_delays[edge].mx;
            if (terminal && terminal_lut && lut_input_delay) {
                auto &d = edge ? lut_input_delay->fall : lut_input_delay->rise;
                if (minimum_reference) {
                    delay_t early, late;
                    if (!lut_route_minimum::outward_ps(output_delays[edge].mi, true, early) ||
                        !lut_route_minimum::outward_ps(output_delays[edge].mx, false, late))
                        return unavailable();
                    d = DelayPair(early, late);
                } else {
                    d = DelayPair(delay_t(output_delays[edge].mi * 1e12), delay_t(output_delays[edge].mx * 1e12));
                }
            }
            if (hops && !terminal)
                (edge ? hop.fall : hop.rise) = delay_t(output_delays[edge].mx * 1e12);
        }

        if (hops && !terminal)
            hops->push_back(hop);
        if (inverting == mistral::CycloneV::INV_YES || inverting == mistral::CycloneV::INV_PROGRAMMABLE)
            inverted = !inverted;
    }

    if (minimum_reference) {
        delay_t early_rise, late_rise, early_fall, late_fall;
        if (!lut_route_minimum::outward_ps(output_delay_sum[0].mi, true, early_rise) ||
            !lut_route_minimum::outward_ps(output_delay_sum[0].mx, false, late_rise) ||
            !lut_route_minimum::outward_ps(output_delay_sum[1].mi, true, early_fall) ||
            !lut_route_minimum::outward_ps(output_delay_sum[1].mx, false, late_fall))
            return unavailable();
        delay = DelayQuad{early_rise, late_rise, early_fall, late_fall};
    } else {
        delay = DelayQuad{delay_t(output_delay_sum[0].mi * 1e12), delay_t(output_delay_sum[0].mx * 1e12),
                          delay_t(output_delay_sum[1].mi * 1e12), delay_t(output_delay_sum[1].mx * 1e12)};
    }

    return true;
}

delay_t Arch::predictDelay(BelId src_bel, IdString src_pin, BelId dst_bel, IdString dst_pin) const
{
    // Placement-time arc prediction, calibrated against routed pip-table delays. Leaving a LAB costs about
    // 0.65 ns along a row and about 0.9 ns once the arc changes row; further distance adds comparatively
    // little because long wires cover it. A distance-only model makes short multi-LAB chains look nearly
    // free, so the placer spreads deep logic across neighbouring LABs. Carry/share chains and a LUT feeding
    // its own ALM's flip-flop use dedicated connections.
    if (dst_pin == id_CI || dst_pin == id_SHAREIN)
        return 20;
    Loc src_loc = getBelLocation(src_bel);
    Loc dst_loc = getBelLocation(dst_bel);
    int x_diff = std::abs(dst_loc.x - src_loc.x);
    int y_diff = std::abs(dst_loc.y - src_loc.y);
    if (x_diff == 0 && y_diff == 0) {
        // Only LUT i reaches FFs 2i and 2i+1 directly; the other half's FFs need the E/F input.
        if (src_pin == id_COMBOUT && dst_pin == id_DATAIN && getBelType(dst_bel) == id_MISTRAL_FF) {
            const auto &src = bel_data(src_bel).lab_data, &dst = bel_data(dst_bel).lab_data;
            if (src.alm == dst.alm && src.idx == dst.idx / 2)
                return 20;
        }
        return 300;
    }
    return 650 + (y_diff ? 250 : 0) + 35 * x_diff + 100 * y_diff;
}

delay_t Arch::estimateDelay(WireId src, WireId dst) const
{
    int x0 = src.node.x();
    int y0 = src.node.y();
    int x1 = dst.node.x();
    int y1 = dst.node.y();
    int x_diff = std::abs(x1 - x0);
    int y_diff = std::abs(y1 - y0);
    return 75 * x_diff + 200 * y_diff;
}

// Diagnostic: write every routed arc's per-hop delay-table and analogue
// delays after the bitstream has been configured.
void Arch::dump_analogue_arcs(const std::string &path) const
{
    FILE *f = fopen(path.c_str(), "w");
    if (!f)
        log_error("cannot open analogue arc dump '%s'\n", path.c_str());
    fprintf(f, "net\tuser\thop\tsrc_type\tsrc_x\tsrc_y\tdst_type\tdst_x\tdst_y\ttable_ps\trise_ps\tfall_ps\n");
    std::vector<AnalogueHop> hops;
    for (auto &net : nets) {
        const NetInfo *ni = net.second.get();
        if (ni->driver.cell == nullptr || ni->wires.empty() || ni->is_global)
            continue;
        for (auto item : const_cast<NetInfo *>(ni)->users.enumerate()) {
            const PortRef &usr = item.value;
            auto usr_idx = item.index;
            hops.clear();
            DelayQuad d;
            if (!analogue_arc_delay(ni, usr, d, &hops))
                continue;
            for (size_t h = 0; h < hops.size(); h++) {
                WireId s = getPipSrcWire(hops[h].pip), t = getPipDstWire(hops[h].pip);
                auto tn = [&](WireId w) -> const char * {
                    return w.is_nextpnr_created() ? "NPNR" : CycloneV::rnode_type_names[w.node.t()];
                };
                fprintf(f, "%s\t%d\t%zu\t%s\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%d\n", ni->name.c_str(getCtx()),
                        usr_idx.idx(), h, tn(s), s.is_nextpnr_created() ? -1 : int(s.node.x()),
                        s.is_nextpnr_created() ? -1 : int(s.node.y()), tn(t),
                        t.is_nextpnr_created() ? -1 : int(t.node.x()),
                        t.is_nextpnr_created() ? -1 : int(t.node.y()), int(hops[h].table),
                        int(hops[h].rise), int(hops[h].fall));
            }
        }
    }
    fclose(f);
}

NEXTPNR_NAMESPACE_END
