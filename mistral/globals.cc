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
    // Import every physical FPLL with its output counters (bel pins C0..C8),
    // every dedicated GPIO -> CLKIN reference edge and every counter ->
    // clock-buffer edge that Mistral describes.  The packer maps logical
    // outclk ports onto counters (see setup_plls in pack.cc).
    //
    // Creation order is deliberate: the original C6/C7/C5/C8 wires and edges
    // come first, in their original order, so designs that do not use the
    // new resources keep identical wire numbering, routing and bitstreams.
    // Additional counters and the vertical clock-mux lanes are appended.
    const auto links = cyclonev->get_all_p2p();
    auto is_ref_link = [&](const CycloneV::pnode_coords &src, const CycloneV::pnode_coords &dst,
                           CycloneV::xycoords pos) {
        return dst.bt() == CycloneV::FPLL && dst.p() == pos && dst.pt() == CycloneV::CLKIN && dst.pi() >= 0 &&
               dst.pi() <= 3 && src.bt() == CycloneV::GPIO;
    };
    auto counter_link = [&](const CycloneV::pnode_coords &src, const CycloneV::pnode_coords &dst,
                            CycloneV::xycoords pos, CycloneV::block_type_t cmux) {
        return src.bt() == CycloneV::FPLL && src.p() == pos && src.pt() == CycloneV::PLLCOUT && src.pi() >= 0 &&
               src.pi() <= 8 && dst.bt() == cmux && dst.pt() == CycloneV::PLLIN && dst.pi() >= 0 && dst.pi() <= 15;
    };
    struct Site
    {
        BelId bel;
        WireId ref;
        std::array<WireId, 9> counters;
    };
    std::vector<Site> sites;
    // New reference edges leave existing GPIO wires; like new counter edges
    // into the fabric lane they are hidden from nets without a PLL.
    auto add_ref = [&](const Site &site, const CycloneV::pnode_coords &src, const CycloneV::pnode_coords &dst,
                       bool private_edge = false) {
        WireId pad = get_port(CycloneV::GPIO, src.x(), src.y(), src.bi(), CycloneV::DATAIN, 0);
        // Quartus selects dedicated input CLKIN(k) with CLKIN_0_SRC = 4 + k
        // (clk_0..clk_3), checked for k = 0..3.
        PipId pip = add_pip(pad, site.ref);
        pll_ref_select[pip] = 4 + dst.pi();
        if (private_edge)
            pll_private_ref_pips.insert(pip);
    };
    // private_fabric_lane: HG lane 2 also accepts fabric clocks and is
    // searched by the fabric router.  Edges added after the original ones
    // into that lane are hidden from non-PLL nets (checkPipAvailForNet) so
    // they cannot perturb routing of designs that do not use them.
    auto add_counter = [&](Site &site, const CycloneV::pnode_coords &src, const CycloneV::pnode_coords &dst,
                           bool vertical, bool private_fabric_lane = false) {
        int counter = src.pi();
        if (site.counters[counter] == WireId()) {
            Loc l = getBelLocation(site.bel);
            site.counters[counter] = add_wire(l.x, l.y, idf("FPLL_C%d", counter));
            add_bel_pin(site.bel, idf("C%d", counter), PORT_OUT, site.counters[counter]);
        }
        for (BelId clock : getBelsByTile(dst.x(), dst.y())) {
            if (getBelType(clock) != (vertical ? id_MISTRAL_CLKENA_PLL : id_MISTRAL_CLKENA))
                continue;
            PipId pip = add_pip(site.counters[counter], getBelPinWire(clock, id_A));
            pll_clock_select[pip] = 8 + dst.pi();
            if (private_fabric_lane && bel_data(clock).block_index == 2)
                pll_private_pips.insert(pip);
            pll_clock_bels[site.bel][counter].push_back(clock);
        }
    };
    const std::array<int, 4> original{6, 7, 5, 8};
    for (auto pos : cyclonev->fpll_get_pos()) {
        int x = pos.x(), y = pos.y();
        Site site;
        site.bel = add_bel(x, y, id_altera_pll, id_altera_pll);
        site.ref = add_wire(x, y, id("FPLL_REFCLK"));
        for (int k : original)
            site.counters[k] = add_wire(x, y, idf("FPLL_C%d", k));
        add_bel_pin(site.bel, id_refclk, PORT_IN, site.ref);
        for (int k : original)
            add_bel_pin(site.bel, idf("C%d", k), PORT_OUT, site.counters[k]);
        add_bel_pin(site.bel, id_rst, PORT_IN, get_port(CycloneV::FPLL, x, y, -1, CycloneV::NRESET0));
        add_bel_pin(site.bel, id_locked, PORT_OUT, get_port(CycloneV::FPLL, x, y, -1, CycloneV::LOCK0));
        for (auto link : links) {
            auto src = link.first, dst = link.second;
            if (is_ref_link(src, dst, pos) && (dst.pi() == 0 || (x == 0 && y == 31 && dst.pi() == 2)))
                add_ref(site, src, dst);
            if (counter_link(src, dst, pos, CycloneV::CMUXHG) && src.pi() >= 5)
                add_counter(site, src, dst, false);
        }
        sites.push_back(site);
    }
    // Remaining dedicated reference inputs and counters C0..C4 on the
    // horizontal muxes.
    for (auto &site : sites) {
        CycloneV::xycoords pos(getBelLocation(site.bel).x, getBelLocation(site.bel).y);
        for (auto link : links) {
            auto src = link.first, dst = link.second;
            if (is_ref_link(src, dst, pos) && !(dst.pi() == 0 || (pos.x() == 0 && pos.y() == 31 && dst.pi() == 2)))
                add_ref(site, src, dst, /*private_edge=*/true);
            if (counter_link(src, dst, pos, CycloneV::CMUXHG) && src.pi() < 5)
                add_counter(site, src, dst, false, /*private_fabric_lane=*/true);
        }
    }
    // Vertical global clock muxes: PLL-dedicated lanes only (block_index
    // 4..7, no fabric input).  They use their own bel type so that the
    // placer's clock-buffer bucket, and hence placement of designs that do
    // not use them, is unchanged; only the PLL packer binds them.
    for (auto pos : cyclonev->cmuxv_get_pos()) {
        int x = pos.x(), y = pos.y();
        for (int z = 0; z < 4; ++z) {
            if (!has_port(CycloneV::CMUXVG, x, y, z, CycloneV::CLKOUT))
                continue;
            BelId bel = add_bel(x, y, idf("VCLKBUF[%d]", z), id_MISTRAL_CLKENA_PLL);
            WireId input = add_wire(x, y, idf("VCLKBUF%d_INPUT", z));
            add_bel_pin(bel, id_A, PORT_IN, input);
            add_bel_pin(bel, id_Q, PORT_OUT, get_port(CycloneV::CMUXVG, x, y, z, CycloneV::CLKOUT));
            add_bel_pin(bel, id_ENA, PORT_IN, get_port(CycloneV::CMUXVG, x, y, z, CycloneV::ENABLE));
            add_bel_pin(bel, id_ENAOUT, PORT_OUT, get_port(CycloneV::CMUXVG, x, y, z, CycloneV::SYN_EN));
            bel_data(bel).block_index = 4 + z;
        }
    }
    for (auto &site : sites) {
        CycloneV::xycoords pos(getBelLocation(site.bel).x, getBelLocation(site.bel).y);
        for (auto link : links)
            if (counter_link(link.first, link.second, pos, CycloneV::CMUXVG))
                add_counter(site, link.first, link.second, true);
    }
}

WireId Arch::pll_output_wire(const CellInfo *pll, IdString port) const
{
    if (pll == nullptr || pll->bel == BelId())
        return WireId();
    auto found = pll->pin_data.find(port);
    if (found == pll->pin_data.end() || found->second.bel_pins.size() != 1)
        return WireId();
    return getBelPinWire(pll->bel, found->second.bel_pins.front());
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
        auto src_type = pip.src.t();
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
