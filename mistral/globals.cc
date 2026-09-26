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

#include <cstdio>
#include <queue>

NEXTPNR_NAMESPACE_BEGIN

void Arch::create_clkbuf(int x, int y)
{
    for (int z : {2, 3, 1, 0}) {
        // Subblock 2 accepts fabric routing. Other subblocks are reserved
        // for dedicated PLL outputs; every lane can select any PLLIN0..15.
        BelId bel = add_bel(x, y, idf("CLKBUF[%d]", z), id_MISTRAL_CLKENA);
        WireId input = add_wire(x, y, idf("CLKBUF%d_INPUT", z));
        if (z == 2)
            add_pip(get_port(CycloneV::CMUXHG, x, y, -1, CycloneV::CLKIN, z), input);
        add_bel_pin(bel, id_A, PORT_IN, input);
        add_bel_pin(bel, id_Q, PORT_OUT, get_port(CycloneV::CMUXHG, x, y, z, CycloneV::CLKOUT));
        add_bel_pin(bel, id_ENA, PORT_IN, get_port(CycloneV::CMUXHG, x, y, z, CycloneV::ENABLE));
        add_bel_pin(bel, id_ENAOUT, PORT_OUT, get_port(CycloneV::CMUXHG, x, y, z, CycloneV::SYN_EN));
        bel_data(bel).block_index = z;
    }
}

bool Arch::is_clkbuf_cell(IdString cell_type) const { return cell_type.in(id_MISTRAL_CLKENA, id_MISTRAL_CLKBUF); }

void Arch::create_plls()
{
    // The supported profiles use C6 and optionally C7/C5/C8. Import physical FPLL sites
    // and their dedicated edges from Mistral, rather than fabric substitutes.
    const auto links = cyclonev->get_all_p2p();
    for (auto pos : cyclonev->fpll_get_pos()) {
        int x = CycloneV::pos2x(pos), y = CycloneV::pos2y(pos);
        BelId bel = add_bel(x, y, id_altera_pll, id_altera_pll);
        WireId ref = add_wire(x, y, id("FPLL_REFCLK"));
        WireId out = add_wire(x, y, id("FPLL_C6"));
        WireId out1 = add_wire(x, y, id("FPLL_C7"));
        WireId out2 = add_wire(x, y, id("FPLL_C5"));
        WireId out3 = add_wire(x, y, id("FPLL_C8"));
        add_bel_pin(bel, id_refclk, PORT_IN, ref);
        add_bel_pin(bel, id_outclk, PORT_OUT, out);
        add_bel_pin(bel, id("outclk[1]"), PORT_OUT, out1);
        add_bel_pin(bel, id("outclk[2]"), PORT_OUT, out2);
        add_bel_pin(bel, id("outclk[3]"), PORT_OUT, out3);
        add_bel_pin(bel, id_rst, PORT_IN, get_port(CycloneV::FPLL, x, y, -1, CycloneV::NRESET0));
        add_bel_pin(bel, id_locked, PORT_OUT, get_port(CycloneV::FPLL, x, y, -1, CycloneV::LOCK0));
        for (auto link : links) {
            auto src = link.first, dst = link.second;
            if (CycloneV::pn2bt(dst) == CycloneV::FPLL && CycloneV::pn2p(dst) == pos &&
                CycloneV::pn2pt(dst) == CycloneV::CLKIN &&
                (CycloneV::pn2pi(dst) == 0 || (x == 0 && y == 31 && CycloneV::pn2pi(dst) == 2)) &&
                CycloneV::pn2bt(src) == CycloneV::GPIO) {
                WireId pad = get_port(CycloneV::GPIO, CycloneV::pn2x(src), CycloneV::pn2y(src),
                                      CycloneV::pn2bi(src), CycloneV::DATAIN, 0);
                // Quartus-checked V11 routes: CLKIN0 uses 4; CLKIN2 at (0,31) uses 6.
                pll_ref_select[add_pip(pad, ref)] = CycloneV::pn2pi(dst) == 0 ? 4 : 6;
            }
            if (CycloneV::pn2bt(src) != CycloneV::FPLL || CycloneV::pn2p(src) != pos ||
                CycloneV::pn2pt(src) != CycloneV::PLLCOUT || (CycloneV::pn2pi(src) < 5 || CycloneV::pn2pi(src) > 8) ||
                CycloneV::pn2bt(dst) != CycloneV::CMUXHG || CycloneV::pn2pt(dst) != CycloneV::PLLIN ||
                CycloneV::pn2pi(dst) < 0 || CycloneV::pn2pi(dst) > 15)
                continue;
            for (BelId clock : getBelsByTile(CycloneV::pn2x(dst), CycloneV::pn2y(dst))) {
                if (getBelType(clock) != id_MISTRAL_CLKENA)
                    continue;
                int counter = CycloneV::pn2pi(src);
                WireId source = counter == 6 ? out : (counter == 7 ? out1 : (counter == 5 ? out2 : out3));
                pll_clock_select[add_pip(source, getBelPinWire(clock, id_A))] = 8 + CycloneV::pn2pi(dst);
                int output = counter == 6 ? 0 : (counter == 7 ? 1 : (counter == 5 ? 2 : 3));
                pll_clock_bels[bel][output].push_back(clock);
            }
        }
    }
}

void Arch::create_hps_mpu_general_purpose(int x, int y)
{
    BelId gp_bel =
            add_bel(x, y, id_cyclonev_hps_interface_mpu_general_purpose, id_cyclonev_hps_interface_mpu_general_purpose);
    for (int i = 0; i < 32; i++) {
        add_bel_pin(gp_bel, idf("gp_in[%d]", i), PORT_IN,
                    get_port(CycloneV::HPS_MPU_GENERAL_PURPOSE, x, y, -1, CycloneV::GP_IN, i));
        add_bel_pin(gp_bel, idf("gp_out[%d]", i), PORT_OUT,
                    get_port(CycloneV::HPS_MPU_GENERAL_PURPOSE, x, y, -1, CycloneV::GP_OUT, i));
    }
}

void Arch::create_hps_peripheral_i2c(int x, int y)
{
    BelId i2c_bel = add_bel(x, y, id_cyclonev_hps_interface_peripheral_i2c,
                            id_cyclonev_hps_interface_peripheral_i2c);
    add_bel_pin(i2c_bel, id("scl"), PORT_IN,
                get_port(CycloneV::HPS_PERIPHERAL_I2C, x, y, -1, CycloneV::SCL));
    add_bel_pin(i2c_bel, id("sda"), PORT_IN,
                get_port(CycloneV::HPS_PERIPHERAL_I2C, x, y, -1, CycloneV::SDA));
    add_bel_pin(i2c_bel, id("out_clk"), PORT_OUT,
                get_port(CycloneV::HPS_PERIPHERAL_I2C, x, y, -1, CycloneV::OUT_CLK));
    add_bel_pin(i2c_bel, id("out_data"), PORT_OUT,
                get_port(CycloneV::HPS_PERIPHERAL_I2C, x, y, -1, CycloneV::OUT_DATA));
}

void Arch::create_hps_fpga2sdram(int x, int y)
{
    BelId bel = add_bel(x, y, id_cyclonev_hps_interface_fpga2sdram, id_cyclonev_hps_interface_fpga2sdram);
    auto add_bits = [&](const char *name, PortType dir, int bi, CycloneV::port_type_t port, int count) {
        for (int bit = 0; bit < count; bit++)
            add_bel_pin(bel, idf("%s[%d]", name, bit), dir, get_port(CycloneV::HPS_FPGA2SDRAM, x, y, bi, port, bit));
    };
    auto add_bit = [&](const char *name, PortType dir, int bi, CycloneV::port_type_t port) {
        add_bel_pin(bel, id(name), dir, get_port(CycloneV::HPS_FPGA2SDRAM, x, y, bi, port));
    };
    add_bits("cfg_axi_mm_select", PORT_IN, -1, CycloneV::CFG_AXI_MM_SELECT, 6);
    add_bits("cfg_cport_rfifo_map", PORT_IN, -1, CycloneV::CFG_CPORT_RFIFO_MAP, 18);
    add_bits("cfg_cport_type", PORT_IN, -1, CycloneV::CFG_CPORT_TYPE, 12);
    add_bits("cfg_cport_wfifo_map", PORT_IN, -1, CycloneV::CFG_CPORT_WFIFO_MAP, 18);
    add_bits("cfg_port_width", PORT_IN, -1, CycloneV::CFG_PORT_WIDTH, 12);
    add_bits("cfg_rfifo_cport_map", PORT_IN, -1, CycloneV::CFG_RFIFO_CPORT_MAP, 16);
    add_bits("cfg_wfifo_cport_map", PORT_IN, -1, CycloneV::CFG_WFIFO_CPORT_MAP, 16);
    char name[32];
    for (int port = 0; port < 6; port++) {
        std::snprintf(name, sizeof name, "cmd_data_%d", port);
        add_bits(name, PORT_IN, port, CycloneV::CMD_DATA, 60);
        std::snprintf(name, sizeof name, "cmd_port_clk_%d", port);
        add_bit(name, PORT_IN, port, CycloneV::CMD_PORT_CLK);
        std::snprintf(name, sizeof name, "cmd_ready_%d", port);
        add_bit(name, PORT_OUT, port, CycloneV::CMD_READY);
        std::snprintf(name, sizeof name, "cmd_valid_%d", port);
        add_bit(name, PORT_IN, port, CycloneV::CMD_VALID);
        std::snprintf(name, sizeof name, "wrack_data_%d", port);
        add_bits(name, PORT_OUT, port, CycloneV::WRACK_DATA, 10);
        std::snprintf(name, sizeof name, "wrack_ready_%d", port);
        add_bit(name, PORT_IN, port, CycloneV::WRACK_READY);
        std::snprintf(name, sizeof name, "wrack_valid_%d", port);
        add_bit(name, PORT_OUT, port, CycloneV::WRACK_VALID);
    }
    for (int port = 0; port < 4; port++) {
        std::snprintf(name, sizeof name, "rd_clk_%d", port);
        add_bit(name, PORT_IN, port, CycloneV::RD_CLK);
        std::snprintf(name, sizeof name, "rd_data_%d", port);
        add_bits(name, PORT_OUT, port, CycloneV::RD_DATA, 80);
        std::snprintf(name, sizeof name, "rd_ready_%d", port);
        add_bit(name, PORT_IN, port, CycloneV::RD_READY);
        std::snprintf(name, sizeof name, "rd_valid_%d", port);
        add_bit(name, PORT_OUT, port, CycloneV::RD_VALID);
        std::snprintf(name, sizeof name, "wr_clk_%d", port);
        add_bit(name, PORT_IN, port, CycloneV::WR_CLK);
        std::snprintf(name, sizeof name, "wr_data_%d", port);
        add_bits(name, PORT_IN, port, CycloneV::WR_DATA, 90);
        std::snprintf(name, sizeof name, "wr_ready_%d", port);
        add_bit(name, PORT_OUT, port, CycloneV::WR_READY);
        std::snprintf(name, sizeof name, "wr_valid_%d", port);
        add_bit(name, PORT_IN, port, CycloneV::WR_VALID);
    }
}

void Arch::create_control(int x, int y)
{
    BelId oscillator_bel = add_bel(x, y, id_cyclonev_oscillator, id_cyclonev_oscillator);
    add_bel_pin(oscillator_bel, id_oscena, PORT_IN, get_port(CycloneV::CTRL, x, y, -1, CycloneV::OSC_ENA, -1));
    add_bel_pin(oscillator_bel, id_clkout, PORT_OUT, get_port(CycloneV::CTRL, x, y, -1, CycloneV::CLK_OUT, -1));
    add_bel_pin(oscillator_bel, id_clkout1, PORT_OUT, get_port(CycloneV::CTRL, x, y, -1, CycloneV::CLK_OUT1, -1));
}

struct MistralGlobalRouter
{
    Context *ctx;

    MistralGlobalRouter(Context *ctx) : ctx(ctx) {};

    // When routing globals; we allow global->local for some tricky cases but never local->local
    bool global_pip_filter(PipId pip) const
    {
        auto src_type = CycloneV::rn2t(pip.src);
        return src_type != CycloneV::H14 && src_type != CycloneV::H6 && src_type != CycloneV::H3 &&
               src_type != CycloneV::V12 && src_type != CycloneV::V2 && src_type != CycloneV::V4 &&
               src_type != CycloneV::WM;
    }

    // Dedicated backwards BFS routing for global networks
    template <typename Tfilt>
    bool backwards_bfs_route(NetInfo *net, store_index<PortRef> user_idx, int iter_limit, bool strict, Tfilt pip_filter)
    {
        // Queue of wires to visit
        std::queue<WireId> visit;
        // Wire -> upstream pip
        dict<WireId, PipId> backtrace;

        // Lookup source and destination wires
        WireId src = ctx->getNetinfoSourceWire(net);
        WireId dst = ctx->getNetinfoSinkWire(net, net->users.at(user_idx), 0);

        if (src == WireId())
            log_error("Net '%s' has an invalid source port %s.%s\n", ctx->nameOf(net), ctx->nameOf(net->driver.cell),
                      ctx->nameOf(net->driver.port));

        if (dst == WireId())
            log_error("Net '%s' has an invalid sink port %s.%s\n", ctx->nameOf(net),
                      ctx->nameOf(net->users.at(user_idx).cell), ctx->nameOf(net->users.at(user_idx).port));

        if (ctx->getBoundWireNet(src) != net)
            ctx->bindWire(src, net, STRENGTH_LOCKED);

        if (src == dst) {
            // Nothing more to do
            return true;
        }

        visit.push(dst);
        backtrace[dst] = PipId();

        int iter = 0;

        while (!visit.empty() && (iter++ < iter_limit)) {
            WireId cursor = visit.front();
            visit.pop();
            // Search uphill pips
            for (PipId pip : ctx->getPipsUphill(cursor)) {
                // Skip pip if unavailable, and not because it's already used for this net
                if (!ctx->checkPipAvail(pip) && ctx->getBoundPipNet(pip) != net)
                    continue;
                WireId prev = ctx->getPipSrcWire(pip);
                // Ditto for the upstream wire
                if (!ctx->checkWireAvail(prev) && ctx->getBoundWireNet(prev) != net)
                    continue;
                // Skip already visited wires
                if (backtrace.count(prev))
                    continue;
                // Apply our custom pip filter
                if (!pip_filter(pip))
                    continue;
                // Add to the queue
                visit.push(prev);
                backtrace[prev] = pip;
                // Check if we are done yet
                if (prev == src)
                    goto done;
            }
            if (false) {
            done:
                break;
            }
        }

        if (backtrace.count(src)) {
            WireId cursor = src;
            std::vector<PipId> pips;
            // Create a list of pips on the routed path
            while (true) {
                PipId pip = backtrace.at(cursor);
                if (pip == PipId())
                    break;
                pips.push_back(pip);
                cursor = ctx->getPipDstWire(pip);
            }
            // Reverse that list
            std::reverse(pips.begin(), pips.end());
            // Bind pips until we hit already-bound routing
            for (PipId pip : pips) {
                WireId dst = ctx->getPipDstWire(pip);
                if (ctx->getBoundWireNet(dst) == net)
                    break;
                ctx->bindPip(pip, net, STRENGTH_LOCKED);
            }
            return true;
        } else {
            if (strict)
                log_error("Failed to route net '%s' from %s to %s using dedicated routing.\n", ctx->nameOf(net),
                          ctx->nameOfWire(src), ctx->nameOfWire(dst));
            return false;
        }
    }

    bool is_relaxed_sink(const PortRef &sink) const
    {
        // Cases where global clocks are driving fabric
        if (sink.cell->type == id_MISTRAL_FF && sink.port != id_CLK)
            return true;
        return false;
    }

    void route_clk_net(NetInfo *net)
    {
        for (auto usr : net->users.enumerate())
            backwards_bfs_route(net, usr.index, 1000000, true,
                                [&](PipId pip) { return (is_relaxed_sink(usr.value) || global_pip_filter(pip)); });
        log_info("    routed net '%s' using global resources\n", ctx->nameOf(net));
    }

    void operator()()
    {
        log_info("Routing globals...\n");
        for (auto &net : ctx->nets) {
            NetInfo *ni = net.second.get();
            CellInfo *drv = ni->driver.cell;
            if (drv == nullptr)
                continue;
            // ENAOUT is a fabric status signal, not a global clock.
            if (drv->type.in(id_MISTRAL_CLKENA, id_MISTRAL_CLKBUF) && ni->driver.port == id_Q) {
                route_clk_net(ni);
                continue;
            }
        }
    }
};

void Arch::route_globals()
{
    MistralGlobalRouter router(getCtx());
    router();
}

NEXTPNR_NAMESPACE_END
