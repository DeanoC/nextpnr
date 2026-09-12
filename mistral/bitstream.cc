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
#include "dsp.h"
#include "pll.h"
#include "timing.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {

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

struct MistralBitgen
{
    MistralBitgen(Context *ctx) : ctx(ctx), cv(ctx->cyclonev) {};
    Context *ctx;
    CycloneV *cv;

    using rnode_t = CycloneV::rnode_t;
    using pnode_t = CycloneV::pnode_t;
    using pos_t = CycloneV::pos_t;
    using block_type_t = CycloneV::block_type_t;
    using port_type_t = CycloneV::port_type_t;

    void set_lab_clock_inversion(block_type_t block, pos_t pos, CycloneV::bmux_type_t mux)
    {
#ifndef MISTRAL_CORRECT_LAB_CLOCK_MUXES
        log_error("Inverted LAB/MLAB clocks require Mistral with corrected CLKx_INV/CLKx_SEL tables. "
                  "Rebuild nextpnr against the corrected Mistral revision.\n");
#endif
        NPNR_ASSERT(cv->bmux_b_set(block, pos, mux, 0, true));
    }

    rnode_t find_rnode(block_type_t bt, pos_t pos, port_type_t port, int bi = -1, int pi = -1) const
    {
        auto pn1 = CycloneV::pnode(bt, pos, port, bi, pi);
        auto rn1 = cv->pnode_to_rnode(pn1);
        if (rn1)
            return rn1;

        if (bt == CycloneV::GPIO) {
            auto pn2 = cv->p2p_to(pn1);
            if (!pn2) {
                auto pnv = cv->p2p_from(pn1);
                if (!pnv.empty())
                    pn2 = pnv[0];
            }
            auto pn3 = cv->hmc_get_bypass(pn2);
            auto rn2 = cv->pnode_to_rnode(pn3);
            return rn2;
        }

        return 0;
    }

    void options()
    {
        if (!ctx->setting<bool>("compress_rbf", false)) {
            cv->opt_b_set(CycloneV::COMPRESSION_DIS, true);
            cv->opt_r_set(CycloneV::OPT_B, 0xffffff40adffffffULL);
        } else
            cv->opt_r_set(CycloneV::OPT_B, 0xffffff402dffffffULL);
    }

    void write_routing()
    {
        for (auto &net : ctx->nets) {
            NetInfo *ni = net.second.get();
            for (auto &wire : ni->wires) {
                PipId pip = wire.second.pip;
                if (pip == PipId())
                    continue;
                WireId src = ctx->getPipSrcWire(pip), dst = ctx->getPipDstWire(pip);
                // Only write out routes that are entirely in the Mistral domain. Everything else is dealt with
                // specially
                if (src.is_nextpnr_created() || dst.is_nextpnr_created())
                    continue;
                cv->rnode_link(src.node, dst.node);
            }
        }
    }

    void write_dsp_block(int x, int y)
    {
        auto pos = CycloneV::xy2pos(x, y);

        struct DspBinding
        {
            CellInfo *cell;
            std::vector<int> a_groups;
            std::vector<int> b_groups;
            std::vector<int> z_groups;
            std::vector<int> c_groups;
            std::vector<int> d_groups;
        };

        // A DSP tile has several logical BELs. Discover the bound cells from
        // the tile rather than relying on cell-map iteration order, then
        // configure the shared mode/sign controls once.
        std::vector<BelId> dsp_bels;
        for (BelId bel : ctx->getBelsByTile(x, y)) {
            if (ctx->getBelType(bel).in(id_MISTRAL_MUL9X9, id_MISTRAL_MUL18X18, id_MISTRAL_MUL27X27,
                                        id_MISTRAL_MUL18X19, id_MISTRAL_MUL18X19_COMBINED) &&
                ctx->getBoundBelCell(bel) != nullptr)
                dsp_bels.push_back(bel);
        }
        if (dsp_bels.empty())
            return;

        std::sort(dsp_bels.begin(), dsp_bels.end(), [](BelId a, BelId b) { return a.z < b.z; });
        IdString mode_type = ctx->getBelType(dsp_bels.front());
        std::vector<DspBinding> bindings;
        CellInfo *first = ctx->getBoundBelCell(dsp_bels.front());
        NPNR_ASSERT(first != nullptr);
        bool preadder = dsp_bool_param(first->params, id_PREADDER_EN);
        for (BelId bel : dsp_bels) {
            if (ctx->getBelType(bel) != mode_type)
                log_error("DSP tile %s contains incompatible multiplier modes (%s and %s).\n", ctx->nameOfBel(bel),
                          ctx->nameOf(mode_type), ctx->nameOf(ctx->getBelType(bel)));
            CellInfo *cell = ctx->getBoundBelCell(bel);
            if (cell == nullptr)
                continue;
            if (mode_type == id_MISTRAL_MUL9X9) {
                NPNR_ASSERT(bel.z >= 0 && bel.z < int(mistral_dsp_lanes.size()));
                const auto &lane = mistral_dsp_lanes.at(bel.z);
                bindings.push_back({cell,
                                    {lane.a_group},
                                    {preadder ? mistral_dsp_preadder_y_groups.at(bel.z) : lane.b_group},
                                    preadder ? std::vector<int>{mistral_dsp_preadder_z_groups.at(bel.z)}
                                              : std::vector<int>{},
                                    {},
                                    {}});
            } else if (mode_type == id_MISTRAL_MUL18X18) {
                bindings.push_back({cell,
                                    {mistral_dsp_18x18_a_groups.begin(), mistral_dsp_18x18_a_groups.end()},
                                    {mistral_dsp_18x18_b_groups.begin(), mistral_dsp_18x18_b_groups.end()},
                                    {},
                                    {mistral_dsp_18x18_c_groups.begin(), mistral_dsp_18x18_c_groups.end()},
                                    {}});
            } else if (mode_type == id_MISTRAL_MUL27X27) {
                bindings.push_back({cell,
                                    {mistral_dsp_27x27_a_groups.begin(), mistral_dsp_27x27_a_groups.end()},
                                    {mistral_dsp_27x27_b_groups.begin(), mistral_dsp_27x27_b_groups.end()},
                                    {},
                                    {},
                                    {}});
            } else if (mode_type.in(id_MISTRAL_MUL18X19, id_MISTRAL_MUL18X19_COMBINED)) {
                bindings.push_back({cell,
                                    {mistral_dsp_18x19_a_groups.begin(), mistral_dsp_18x19_a_groups.end()},
                                    {mistral_dsp_18x19_b_groups.begin(), mistral_dsp_18x19_b_groups.end()},
                                    {},
                                    {mistral_dsp_18x19_c_groups.begin(), mistral_dsp_18x19_c_groups.end()},
                                    {mistral_dsp_18x19_d_groups.begin(), mistral_dsp_18x19_d_groups.end()}});
            }
        }
        if (bindings.empty())
            return;

        CycloneV::bmux_type_t mode;
        if (mode_type == id_MISTRAL_MUL9X9)
            mode = CycloneV::M9X9;
        else if (mode_type == id_MISTRAL_MUL18X18)
            mode = CycloneV::M18X18P36;
        else if (mode_type == id_MISTRAL_MUL27X27)
            mode = CycloneV::M27X27;
        else if (mode_type == id_MISTRAL_MUL18X19)
            mode = CycloneV::M18X19;
        else if (mode_type == id_MISTRAL_MUL18X19_COMBINED)
            mode = CycloneV::M18X19_COMBINED;
        else
            NPNR_ASSERT_FALSE("unreachable DSP mode");
        NPNR_ASSERT(cv->bmux_m_set(CycloneV::DSP, pos, CycloneV::MODE, 0, mode));
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::AX_SIGNED, 0,
                                  dsp_bool_param(bindings.front().cell->params, id_A_SIGNED, true)));
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::AY_SIGNED, 0,
                                  dsp_bool_param(bindings.front().cell->params, id_B_SIGNED, true)));
        if (mode_type.in(id_MISTRAL_MUL18X19, id_MISTRAL_MUL18X19_COMBINED)) {
            NPNR_ASSERT(cv->bmux_b_set(
                    CycloneV::DSP, pos, CycloneV::BX_SIGNED, 0,
                    dsp_bool_param(bindings.front().cell->params, id_C_SIGNED,
                                   dsp_bool_param(bindings.front().cell->params, id_A_SIGNED, true))));
            NPNR_ASSERT(cv->bmux_b_set(
                    CycloneV::DSP, pos, CycloneV::BY_SIGNED, 0,
                    dsp_bool_param(bindings.front().cell->params, id_D_SIGNED,
                                   dsp_bool_param(bindings.front().cell->params, id_B_SIGNED, true))));
        }

        auto set_reg = [&](CycloneV::bmux_type_t mux, IdString key) {
            NPNR_ASSERT(cv->bmux_m_set(CycloneV::DSP, pos, mux, 0,
                                      dsp_reg_param(first->params, key) ? CycloneV::REG : CycloneV::BYPASS));
        };
        set_reg(CycloneV::INREG_CTRL_AX, id_INREG_CTRL_AX);
        set_reg(CycloneV::INREG_CTRL_AY, id_INREG_CTRL_AY);
        set_reg(CycloneV::INREG_CTRL_AZ, id_INREG_CTRL_AZ);
        set_reg(CycloneV::INREG_CTRL_BX, id_INREG_CTRL_BX);
        set_reg(CycloneV::INREG_CTRL_BY, id_INREG_CTRL_BY);
        set_reg(CycloneV::INREG_CTRL_BZ, id_INREG_CTRL_BZ);
        set_reg(CycloneV::OREG_CTRL, id_OREG_CTRL);

        // DSP control inputs are active-high at the tile boundary. Keep the
        // packed PIN_0/PIN_1/PIN_INV state and translate it into the DSP's
        // inversion/force bits instead of leaving a disconnected control at
        // its silicon default.
        auto control_state = [&](IdString port, CellPinState default_state) {
            auto it = first->pin_data.find(port);
            if (it != first->pin_data.end() && it->second.state != PIN_SIG)
                return it->second.state;
            if (first->getPort(port) == nullptr)
                return default_state;
            return PIN_SIG;
        };
        auto control_inverted = [&](IdString port, CellPinState default_state) {
            CellPinState state = control_state(port, default_state);
            return state == PIN_0 || state == PIN_INV;
        };
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::ACC_INV, 0,
                                  control_inverted(id_ACCUMULATE, PIN_0)));
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::PRELOAD_INV, 0,
                                  control_inverted(id_LOADCONST, PIN_0)));
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::SUB_INV, 0,
                                  control_inverted(id_SUB, PIN_0)));
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::DEC_INV, 0,
                                  control_inverted(id_NEGATE, PIN_0)));

        auto uses_fabric_pin = [&](IdString port, const char *pin) {
            if (first->getPort(port) == nullptr)
                return true;
            const auto &pins = first->pin_data.at(port).bel_pins;
            return !pins.empty() && pins.front() == ctx->id(pin);
        };
        // Unrouted fabric clear is low (unlike the arithmetic/data inputs).
        // Select it for constants and keep the unused second clear inactive.
        CellPinState aclr_state = control_state(id_ACLR, PIN_0);
        NPNR_ASSERT(cv->bmux_n_set(CycloneV::DSP, pos, CycloneV::ACLR0_SEL, 0,
                                  uses_fabric_pin(id_ACLR, "ACLR_FABRIC") ? 2 : 0));
        NPNR_ASSERT(cv->bmux_n_set(CycloneV::DSP, pos, CycloneV::ACLR1_SEL, 0, 3));
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::ACLR0_INV, 0,
                                  aclr_state == PIN_1 || aclr_state == PIN_INV));
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::ACLR1_INV, 0, false));

        NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::PREADDER_EN, 0, preadder));
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::PREADDER_SUB, 0,
                                  dsp_bool_param(first->params, id_PREADDER_SUB)));
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::CASCADE_EN, 0,
                                  dsp_bool_param(first->params, id_CASCADE_EN)));
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::CASCADE_1ST_EN, 0,
                                  dsp_bool_param(first->params, id_CASCADE_1ST_EN)));
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::CHAIN_OUTPUT_EN, 0,
                                  dsp_bool_param(first->params, id_CHAIN_OUTPUT_EN)));

        bool any_register = dsp_reg_param(first->params, id_INREG_CTRL_AX) ||
                            dsp_reg_param(first->params, id_INREG_CTRL_AY) ||
                            dsp_reg_param(first->params, id_INREG_CTRL_AZ) ||
                            dsp_reg_param(first->params, id_INREG_CTRL_BX) ||
                            dsp_reg_param(first->params, id_INREG_CTRL_BY) ||
                            dsp_reg_param(first->params, id_INREG_CTRL_BZ) || dsp_reg_param(first->params, id_OREG_CTRL);
        if (any_register) {
            if (first->getPort(id_CLK) == nullptr && first->get_pin_state(id_CLK) != PIN_0 &&
                first->get_pin_state(id_CLK) != PIN_1)
                log_error("DSP cell '%s' enables a register without a CLK port.\n", ctx->nameOf(first));
            CellPinState ena_state = control_state(id_ENA, PIN_1);
            bool ena_inverted = ena_state == PIN_0 || ena_state == PIN_INV;
            bool ena_forced = first->getPort(id_ENA) == nullptr && ena_state != PIN_0;
            NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::ENABLE0_INV, 0, ena_inverted));
            NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::ENABLE0_FORCE, 0,
                                      ena_forced));

            CellPinState clk_state = control_state(id_CLK, PIN_SIG);
            NPNR_ASSERT(cv->bmux_b_set(CycloneV::DSP, pos, CycloneV::CLK0_INV, 0,
                                      clk_state == PIN_1 || clk_state == PIN_INV));
        }

        NPNR_ASSERT(cv->bmux_n_set(CycloneV::DSP, pos, CycloneV::CLK0_SEL, 0,
                                  uses_fabric_pin(id_CLK, "CLK_FABRIC") ? 3 : 0));
        NPNR_ASSERT(cv->bmux_n_set(CycloneV::DSP, pos, CycloneV::CLK1_SEL, 0, 4));
        NPNR_ASSERT(cv->bmux_n_set(CycloneV::DSP, pos, CycloneV::CLK2_SEL, 0, 5));

        // Unrouted DSP inputs are 1; invert unused inputs to keep them at 0.
        for (int group = 0; group < 12; ++group) {
            unsigned inv = 0x1ff;
            for (const auto &binding : bindings) {
                auto apply = [&](const std::vector<int> &groups, char port_name) {
                    for (size_t slice = 0; slice < groups.size(); ++slice) {
                        if (groups.at(slice) != group)
                            continue;
                        for (int bit = 0; bit < 9; ++bit) {
                            IdString port = ctx->idf("%c[%d]", port_name, int(slice * 9 + bit));
                            auto state = binding.cell->get_pin_state(port);
                            bool invert = state == PIN_0 || state == PIN_INV ||
                                          (state == PIN_SIG && binding.cell->getPort(port) == nullptr);
                            if (!invert)
                                inv &= ~(1u << bit);
                        }
                    }
                };
                apply(binding.a_groups, 'A');
                apply(binding.b_groups, 'B');
                apply(binding.z_groups, 'Z');
                apply(binding.c_groups, 'C');
                apply(binding.d_groups, 'D');
            }
            NPNR_ASSERT(cv->bmux_r_set(CycloneV::DSP, pos, CycloneV::DATA_INV, group, inv));
        }
    }

    void write_io_cell(CellInfo *ci, int x, int y, int bi)
    {
        bool is_output = (ci->type.in(id_MISTRAL_OB, id_MISTRAL_DDROUT, id_MISTRAL_SDROUT, id_MISTRAL_DDRBIDIR) ||
                          (ci->type == id_MISTRAL_IO && ci->getPort(id_OE) != nullptr));
        bool is_input = (ci->type.in(id_MISTRAL_IB, id_MISTRAL_SDRIN, id_MISTRAL_DDRIN, id_MISTRAL_DDRBIDIR) ||
                         (ci->type == id_MISTRAL_IO && ci->getPort(id_O) != nullptr));
        auto pos = CycloneV::xy2pos(x, y);
        // TODO: configurable pull, IO standard, etc
        cv->bmux_b_set(CycloneV::GPIO, pos, CycloneV::USE_WEAK_PULLUP, bi, false);
        if (is_output) {
            cv->bmux_m_set(CycloneV::GPIO, pos, CycloneV::DRIVE_STRENGTH, bi, CycloneV::V3P3_LVTTL_16MA_LVCMOS_2MA);
            // DIS turns off the pad's input buffer.  Keep the database input
            // default when a bidirectional cell consumes O so external pad
            // state still reaches the fabric or an HPS peripheral.
            if (!is_input)
                cv->bmux_m_set(CycloneV::GPIO, pos, CycloneV::IOCSR_STD, bi, CycloneV::DIS);

            // Output gpios must also bypass things in the associated dqs
            auto dqs = cv->p2p_to(CycloneV::pnode(CycloneV::GPIO, pos, CycloneV::PNONE, bi, -1));
            if (dqs && ci->type.in(id_MISTRAL_DDROUT, id_MISTRAL_DDRBIDIR)) {
                auto dp = CycloneV::pn2p(dqs);
                int lane = CycloneV::pn2bi(dqs);
                NPNR_ASSERT(cv->bmux_m_set(CycloneV::DQS16, dp, CycloneV::OUTREG_MODE_SEL, lane, CycloneV::DDR));
                NPNR_ASSERT(cv->bmux_m_set(CycloneV::DQS16, dp, CycloneV::OUTREG_OUTPUT_SEL, lane, CycloneV::SEL_SDR_DELAY));
                NPNR_ASSERT(cv->bmux_b_set(CycloneV::DQS16, dp, CycloneV::RBOE_LVL_FR_CLK_EN, lane, true));
                if (ci->getPort(id_D_H) != nullptr) {
                    // Fabric data is already driven on the two dedicated
                    // lanes; leave both data paths non-inverted.
                    NPNR_ASSERT(cv->inv_set(find_rnode(CycloneV::GPIO, pos, CycloneV::DATAOUT, bi, 0), false));
                    NPNR_ASSERT(cv->inv_set(find_rnode(CycloneV::GPIO, pos, CycloneV::DATAOUT, bi, 1), false));
                } else {
                    bool high = bool_or_default(ci->params, id_DDR_HIGH, true);
                    cv->inv_set(find_rnode(CycloneV::GPIO, pos, CycloneV::DATAOUT, bi, 0), !high);
                    cv->inv_set(find_rnode(CycloneV::GPIO, pos, CycloneV::DATAOUT, bi, 1), high);
                }
            } else if (dqs && ci->type == id_MISTRAL_SDROUT) {
                auto dp = CycloneV::pn2p(dqs);
                int lane = CycloneV::pn2bi(dqs);
                NPNR_ASSERT(cv->bmux_m_set(CycloneV::DQS16, dp, CycloneV::OUTREG_OUTPUT_SEL, lane, CycloneV::SEL_SDR));
                NPNR_ASSERT(cv->bmux_b_set(CycloneV::DQS16, dp, CycloneV::OEREG_HR_CLK_EN, lane, true));
                NPNR_ASSERT(cv->bmux_b_set(CycloneV::DQS16, dp, CycloneV::RBOE_LVL_FR_CLK_EN, lane, true));
                NPNR_ASSERT(cv->bmux_r_set(CycloneV::DQS16, dp, CycloneV::RB_T9_SEL_EREG_CFF_DELAY, lane, 0x1f));
                NPNR_ASSERT(cv->bmux_r_set(CycloneV::DQS16, dp, CycloneV::RB_T9_SEL_OREG_DFF_DELAY, lane, 0x1f));
            } else if (dqs) {
                cv->bmux_m_set(CycloneV::DQS16, CycloneV::pn2p(dqs), CycloneV::INPUT_REG4_SEL, CycloneV::pn2bi(dqs),
                               CycloneV::SEL_LOCKED_DPA);
                cv->bmux_r_set(CycloneV::DQS16, CycloneV::pn2p(dqs), CycloneV::RB_T9_SEL_EREG_CFF_DELAY,
                               CycloneV::pn2bi(dqs), 0x1f);
            }
        }
        if (ci->type.in(id_MISTRAL_SDRIN, id_MISTRAL_DDRIN, id_MISTRAL_DDRBIDIR)) {
            auto dqs = cv->p2p_to(CycloneV::pnode(CycloneV::GPIO, pos, CycloneV::PNONE, bi, -1));
            NPNR_ASSERT(dqs);
            auto dp = CycloneV::pn2p(dqs);
            int lane = CycloneV::pn2bi(dqs);
            NPNR_ASSERT(cv->bmux_b_set(CycloneV::DQS16, dp, CycloneV::RB_FIFO_WCLK_EN, lane, true));
            NPNR_ASSERT(cv->bmux_b_set(CycloneV::DQS16, dp, CycloneV::RB_FIFO_WCLK_INV, lane, true));
        }
        // There seem to be two mirrored OEIN inversion bits for constant OE for inputs/outputs. This might be to
        // prevent a single bitflip from turning inputs to outputs and messing up other devices on the boards, notably
        // ECP5 does similar. OEIN.0 inverted for outputs; OEIN.1 for inputs
        cv->inv_set(find_rnode(CycloneV::GPIO, pos, CycloneV::OEIN, bi, 0), is_output);
        cv->inv_set(find_rnode(CycloneV::GPIO, pos, CycloneV::OEIN, bi, 1), !is_output);
    }

    void write_clkbuf_cell(CellInfo *ci, int x, int y, int bi)
    {
        auto pos = CycloneV::xy2pos(x, y);
        auto net = ci->getPort(id_A);
        auto input = ctx->getBelPinWire(ci->bel, id_A);
        int select = 0x1b;
        if (net && net->wires.count(input)) {
            auto pip = net->wires.at(input).pip;
            if (ctx->pll_clock_select.count(pip))
                select = ctx->pll_clock_select.at(pip);
        }
        NPNR_ASSERT(cv->bmux_r_set(CycloneV::CMUXHG, pos, CycloneV::INPUT_SEL, bi, select));
        cv->bmux_m_set(CycloneV::CMUXHG, pos, CycloneV::TESTSYN_ENOUT_SELECT, bi, CycloneV::PRE_SYNENB);
        if (ci->type == id_MISTRAL_CLKENA) {
            std::string register_mode = str_or_default(ci->params, ctx->id("ena_register_mode"), "falling edge");
            NPNR_ASSERT(cv->bmux_m_set(CycloneV::CMUXHG, pos, CycloneV::ENABLE_REGISTER_MODE, bi,
                                      register_mode == "double register" ? CycloneV::REG2_ENOUT : CycloneV::REG1_ENOUT));
            NPNR_ASSERT(cv->bmux_n_set(CycloneV::CMUXHG, pos, CycloneV::ENABLE_REGISTER_POWER_UP, bi,
                                      str_or_default(ci->params, ctx->id("ena_register_power_up"), "high") == "low" ? 0 : 1));
        }
    }

    void write_pll_cell(CellInfo *ci, int x, int y)
    {
        auto pos = CycloneV::xy2pos(x, y);
        auto raw = [&](CycloneV::bmux_type_t mux, uint64_t value, int index = 0) {
            NPNR_ASSERT(cv->bmux_r_set(CycloneV::FPLL, pos, mux, index, value));
        };
        auto flag = [&](CycloneV::bmux_type_t mux, bool value) {
            NPNR_ASSERT(cv->bmux_b_set(CycloneV::FPLL, pos, mux, 0, value));
        };
        auto ref = ci->getPort(id_refclk);
        auto pip = ref->wires.at(ctx->getBelPinWire(ci->bel, id_refclk)).pip;
        raw(CycloneV::CLKIN_0_SRC, ctx->pll_ref_select.at(pip));
        int reference_mhz = mistral_pll::parse_mhz(ci->params.at(ctx->id("reference_clock_frequency")).as_string());
        int duty0 = int_or_default(ci->params, ctx->id("duty_cycle0"), 50);
        int duty1 = int_or_default(ci->params, ctx->id("duty_cycle1"), 50);
        auto config = mistral_pll::select_hz(mistral_pll::parse_output_hz(
                ci->params.at(ctx->id("output_clock_frequency0")).as_string()), reference_mhz, duty0);
        bool fractional = str_or_default(ci->params, ctx->id("fractional_vco_multiplier"), "false") == "true";
        if (fractional && int_or_default(ci->params, ctx->id("number_of_clocks"), 1) == 1) {
            config = mistral_pll::select_fractional(mistral_pll::parse_output_hz(
                    ci->params.at(ctx->id("output_clock_frequency0")).as_string()), reference_mhz);
        }
        int c1 = 0;
        if (int_or_default(ci->params, ctx->id("number_of_clocks"), 1) >= 2) {
            auto hz0 = mistral_pll::parse_output_hz(ci->params.at(ctx->id("output_clock_frequency0")).as_string());
            auto hz1 = mistral_pll::parse_output_hz(ci->params.at(ctx->id("output_clock_frequency1")).as_string());
            auto dual = fractional ? mistral_pll::select_fractional_dual(hz0, hz1, reference_mhz) :
                                     mistral_pll::select_dual_hz(hz0, hz1, reference_mhz, duty0, duty1);
            NPNR_ASSERT(dual);
            config = dual->feedback;
            c1 = dual->c1;
        }
        int clocks = int_or_default(ci->params, ctx->id("number_of_clocks"), 1);
        std::optional<mistral_pll::MultiConfig> multi;
        std::array<int, 4> duties{duty0, duty1, 50, 50};
        if (clocks >= 3) {
            std::array<int64_t, 4> hz{};
            for (int i = 0; i < clocks; ++i)
                hz[i] = mistral_pll::parse_output_hz(ci->params.at(ctx->idf("output_clock_frequency%d", i)).as_string());
            for (int i = 2; i < clocks; ++i)
                duties[i] = int_or_default(ci->params, ctx->idf("duty_cycle%d", i), 50);
            multi = mistral_pll::select_multi_hz(hz, clocks, reference_mhz, duties);
            NPNR_ASSERT(multi);
            config = multi->feedback;
            c1 = multi->counters[1];
        }
        NPNR_ASSERT(config);
        if (c1) {
            auto counts = mistral_pll::duty_counts(c1, duty1);
            NPNR_ASSERT(counts);
            raw(CycloneV::DPRIO0_CNT_HI_DIV, counts->high, 7);
            raw(CycloneV::DPRIO0_CNT_LO_DIV, counts->low, 7);
            auto phase = mistral_pll::select_phase(str_or_default(ci->params, ctx->id("phase_shift1"), "0 ps"),
                    mistral_pll::parse_output_hz(ci->params.at(ctx->id("output_clock_frequency0")).as_string()));
            NPNR_ASSERT(phase);
            if (phase->shift_ps) {
                raw(CycloneV::CNT_PRESET, phase->c_preset, 7);
                raw(CycloneV::CNT_PH_MUX_PRESET, phase->c_phase_preset, 7);
            }
            NPNR_ASSERT(cv->bmux_b_set(CycloneV::FPLL, pos, CycloneV::DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN,
                                      7, counts->odd));
            raw(CycloneV::CNT_IN_SRC, 0, 7);
            flag(CycloneV::C7_COUT_EN, true);
        }
        for (int i = 2; i < clocks; ++i) {
            int counter = i == 2 ? 5 : 8;
            auto counts = mistral_pll::duty_counts(multi->counters[i], duties[i]);
            NPNR_ASSERT(counts);
            raw(CycloneV::DPRIO0_CNT_HI_DIV, counts->high, counter);
            raw(CycloneV::DPRIO0_CNT_LO_DIV, counts->low, counter);
            NPNR_ASSERT(cv->bmux_b_set(CycloneV::FPLL, pos, CycloneV::DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN,
                                      counter, counts->odd));
            auto phase = mistral_pll::select_phase(
                    str_or_default(ci->params, ctx->idf("phase_shift%d", i), "0 ps"),
                    mistral_pll::parse_output_hz(ci->params.at(ctx->id("output_clock_frequency0")).as_string()));
            NPNR_ASSERT(phase);
            if (phase->shift_ps) {
                raw(CycloneV::CNT_PRESET, phase->c_preset, counter);
                raw(CycloneV::CNT_PH_MUX_PRESET, phase->c_phase_preset, counter);
            }
            raw(CycloneV::CNT_IN_SRC, 0, counter);
            flag(i == 2 ? CycloneV::C5_COUT_EN : CycloneV::C8_COUT_EN, true);
        }
        raw(CycloneV::M_CNT_HI_DIV_SETTING, (config->m + 1) / 2);
        raw(CycloneV::M_CNT_LO_DIV_SETTING, config->m / 2);
        // Fractional profiles may use an odd integer part of M. Quartus
        // enables the even-duty correction for those divide values; keep the
        // established even-M profiles at their default without emitting a
        // redundant zero.
        if (fractional && (config->m & 1))
            flag(CycloneV::M_CNT_ODD_DIV_DUTY_EN, true);
        raw(CycloneV::N_CNT_HI_DIV_SETTING, fractional ? 0 : (config->n + 1) / 2);
        raw(CycloneV::N_CNT_LO_DIV_SETTING, fractional ? 0 : config->n / 2);
        if (fractional) {
            flag(CycloneV::N_CNT_BYPASS_EN, true);
            raw(CycloneV::DSM_OUT_SEL, 1);
        }
        // N has no duty-cycle correction, including the checked odd N=5.
        auto counts = mistral_pll::duty_counts(config->c, duty0);
        NPNR_ASSERT(counts);
        raw(CycloneV::DPRIO0_CNT_HI_DIV, counts->high, 6);
        raw(CycloneV::DPRIO0_CNT_LO_DIV, counts->low, 6);
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::FPLL, pos, CycloneV::DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN,
                                  6, counts->odd));
        raw(CycloneV::M_CNT_LO_PRESET_SETTING, config->m_low_preset);
        raw(CycloneV::M_CNT_PH_MUX_PRESET_SETTING, config->m_phase_preset);
        raw(CycloneV::CNT_IN_SRC, 0, 6);
        raw(CycloneV::FBCLK_MUX_2, 1);
        raw(CycloneV::VCO_DIV, 0);
        raw(CycloneV::TCLK_SEL, 0);
        raw(CycloneV::BWCTRL, config->bandwidth);
        raw(CycloneV::CP_CURRENT, config->charge_pump);
        raw(CycloneV::FRACTIONAL_DIVISION_SETTING, config->fraction);
        raw(CycloneV::LOCK_FILTER_CFG_SETTING, 0x19);
        raw(CycloneV::UNLOCK_FILTER_CFG_SETTING, 2);
        flag(CycloneV::CTRL_OVERRIDE_SETTING, false);
        flag(CycloneV::NREVERT_INVERT, true);
        flag(CycloneV::C6_COUT_EN, true);
        flag(CycloneV::VCO0PH_EN, true);
        for (auto mux : {CycloneV::VCO_PH0_EN, CycloneV::VCO_PH1_EN, CycloneV::VCO_PH2_EN, CycloneV::VCO_PH3_EN,
                         CycloneV::VCO_PH4_EN, CycloneV::VCO_PH5_EN, CycloneV::VCO_PH6_EN, CycloneV::VCO_PH7_EN})
            flag(mux, true);
        flag(CycloneV::FPLL_ENABLE, true);
        // Quartus uses the default (non-inverted) routing bit for active-high
        // fabric rst. Only the unconnected, folded-low case needs inversion.
        NPNR_ASSERT(cv->inv_set(find_rnode(CycloneV::FPLL, pos, CycloneV::NRESET0),
                               ci->getPort(id_rst) == nullptr));
        // The fixed 5CSEBA6U23I7/V11 profile also requires the unused
        // auxiliary bandgap at (0,73) powered down. This is outside the
        // selected FPLL's PRAM: omitting it gives no lock and no output on
        // hardware despite identical settings at (0,14). See the PLL test.
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::FPLL, CycloneV::xy2pos(0, 73),
                                  CycloneV::PL_AUX_BG_POWERDOWN, 0, true));
    }

    void write_m10k_cell(CellInfo *ci, int x, int y, int bi)
    {
        auto pos = CycloneV::xy2pos(x, y);

        // Notes:
        // DATA_FLOW_THRU is probably transparent reads.

        auto dbits = ci->params.at(id_CFG_DBITS).as_int64();

        bool tdp = bool_or_default(ci->params, id_CFG_TDP, false);
        bool mixed = bool_or_default(ci->params, id_CFG_MIXED_WIDTH, false);
        int rdbits = mixed ? int_or_default(ci->params, id_CFG_RD_DBITS, dbits) : dbits;
        bool output_reg_a = bool_or_default(ci->params, id_CFG_OUT_REG_A, false);
        bool output_reg_b = bool_or_default(ci->params, id_CFG_OUT_REG_B, false);
        // A 40-bit SDP B result is physically split across the A and B
        // output halves. A partial registration would give the logical
        // result two different latencies, so any request registers both
        // halves.
        if (!tdp && rdbits == 40 && (output_reg_a || output_reg_b)) {
            output_reg_a = true;
            output_reg_b = true;
        }
        // Quartus clears data flow-through when either mixed port spans 40 bits.
        bool wide_mixed = mixed && (dbits == 40 || rdbits == 40);
        cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::A_DATA_FLOW_THRU, bi, !wide_mixed);
        cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::A_DATA_WIDTH, bi, dbits);
        cv->bmux_m_set(CycloneV::M10K, pos, CycloneV::A_FAST_WRITE, bi,
                       !mixed && dbits == 40 ? CycloneV::SLOW : CycloneV::FAST);
        cv->bmux_m_set(CycloneV::M10K, pos, CycloneV::A_OUTPUT_SEL, bi,
                       output_reg_a ? CycloneV::REG : CycloneV::ASYNC);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::A_SA_WREN_DELAY, bi, 1);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::A_SAEN_DELAY, bi, 2);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::A_WL_DELAY, bi, 2);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::A_WR_TIMER_PULSE, bi, 0x0b);

        cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::B_DATA_FLOW_THRU, bi, !wide_mixed);
        cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::B_DATA_WIDTH, bi, rdbits);
        cv->bmux_m_set(CycloneV::M10K, pos, CycloneV::B_FAST_WRITE, bi,
                       !mixed && dbits == 40 ? CycloneV::SLOW : CycloneV::FAST);
        cv->bmux_m_set(CycloneV::M10K, pos, CycloneV::B_OUTPUT_SEL, bi,
                       output_reg_b ? CycloneV::REG : CycloneV::ASYNC);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::B_SA_WREN_DELAY, bi, 1);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::B_SAEN_DELAY, bi, 2);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::B_WL_DELAY, bi, 2);
        cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::B_WR_TIMER_PULSE, bi, 0x0b);

        // A hard-tied unused read clock is folded by the packer and has no
        // TCLK route. In that case retain the single-clock M10K selector
        // defaults instead of programming the bottom clock mux to an absent
        // CLKIN[1] source. A live CLK2 still selects the independent clock
        // path below.
        bool dual_clock = bool_or_default(ci->params, id_CFG_DUAL_CLOCK, false) &&
                          ci->getPort(id_CLK2) != nullptr;
        bool byte_enable = bool_or_default(ci->params, id_CFG_BYTE_ENABLE, false);
        bool async_read = bool_or_default(ci->params, id_CFG_ASYNC_READ, false);

        // The two M10K clear inputs are shared by the address and output
        // clear paths.  The logical primitive names match the physical
        // ACLR[0:1] pins, while the clear muxes select which source feeds the
        // top and bottom halves.  PIN_0 leaves the corresponding output clear
        // disabled, PIN_1 enables it from the M10K's default-high input,
        // PIN_INV preserves the logical inversion, and PIN_SIG is routed
        // normally.
        auto aclr_state = [&](IdString port) {
            auto it = ci->pin_data.find(port);
            return it == ci->pin_data.end() ? PIN_0 : it->second.state;
        };
        CellPinState aclr0 = aclr_state(id_ACLR0);
        CellPinState aclr1 = aclr_state(id_ACLR1);
        // The M10K has separate address-clear and output-clear enables. The
        // mapped logical ports describe output clears; Cyclone V ignores
        // address clears on the input-register modes used by these cells.
        // Keep the physical clear source selectors aligned with the two
        // logical inputs, and only turn on an output-clear register when its
        // control is actually active. This preserves the existing async
        // output path for cells that have no reset behavior.
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::TOP_CLR_INV, bi,
                                  aclr0 == PIN_INV));
        NPNR_ASSERT(cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::BOT_CLR_INV, bi,
                                  aclr1 == PIN_INV));
        // A 40-bit SDP read spans both physical output halves, so the top
        // output register participates in the clear path even though the
        // logical cell is not true-dual-port.  Its source is ACLR0 (the
        // default TOP_ADDCLR_SEL=0); TOP_OUTCLR_SEL=1 selects the output
        // clear path rather than the address path.
        if ((tdp || rdbits == 40) && aclr0 != PIN_0) {
            if (rdbits == 40)
                NPNR_ASSERT(cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::TOP_OUTCLR_SEL, bi, 1));
            NPNR_ASSERT(cv->bmux_m_set(CycloneV::M10K, pos, CycloneV::A_OUTCLR_EN, bi, CycloneV::REG));
            NPNR_ASSERT(cv->bmux_m_set(CycloneV::M10K, pos, CycloneV::A_OUTPUT_SEL, bi, CycloneV::REG));
        }
        if (aclr1 != PIN_0) {
            NPNR_ASSERT(cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_1_OUTCLR_SEL, bi, 1));
            NPNR_ASSERT(cv->bmux_m_set(CycloneV::M10K, pos, CycloneV::B_OUTCLR_EN, bi, CycloneV::REG));
            NPNR_ASSERT(cv->bmux_m_set(CycloneV::M10K, pos, CycloneV::B_OUTPUT_SEL, bi, CycloneV::REG));
        }
        auto clock_state = [&](IdString port) {
            auto it = ci->pin_data.find(port);
            return it == ci->pin_data.end() ? PIN_SIG : it->second.state;
        };
        CellPinState clk1_state = clock_state(id_CLK1);
        CellPinState clk2_state = clock_state(id_CLK2);
        cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::TOP_CLK_SEL, bi, 1);
        cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::TOP_CLK_INV, bi, clk1_state == PIN_INV);
        if (dual_clock) {
            // Quartus SDP input-clock mode: write CLKIN.0, read CLKIN.1.
            // In 40-bit mode both data input halves use the write clock.
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_CLK_SEL, bi, 1);
            // B1EN holds the read address/core through ENABLE.0. RDEN.0
            // belongs to the write-side port and cannot hold a CLK2 read.
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_CORECLK_SEL, bi, 1);
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_INCLK_SEL, bi, 1);
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_1_CORECLK_SEL, bi, 1);
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_1_INCLK_SEL, bi,
                           dbits == 40 || wide_mixed ? 0 : 1);
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_1_OUTCLK_SEL, bi, 1);
        }
        if (async_read) {
            // Quartus's flow-through simple-dual configurations select the
            // bottom clock tree and program all three selectors for the
            // second data half.  The logical write clock is fanned out to
            // both CLKIN sinks by setup_m10ks(); without these settings the
            // combinational B port can remain on the unused/default branch.
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_CLK_SEL, bi, 1);
            // The packer materialises the omitted logical B1EN as a
            // constant-high route on ENABLE[0]. Select that core/input path
            // explicitly; relying on the site's default can leave a
            // flow-through read disabled on some M10K locations.
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_CORECLK_SEL, bi, 1);
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_INCLK_SEL, bi, 1);
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_1_CORECLK_SEL, bi, 1);
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_1_INCLK_SEL, bi, 1);
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_1_OUTCLK_SEL, bi, 1);
        }
        if (byte_enable || mixed || tdp) {
            // Byte-enabled SDP, mixed-width SDP and TDP select the write core
            // enable lane. Async reads use the explicit constant-high
            // ENABLE[0] route selected above; synchronous modes retain the
            // same core/input selector values used by the existing mapping.
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_CORECLK_SEL, bi, 1);
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_INCLK_SEL, bi, 1);
            cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::TOP_W_INV, bi, false);
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::TOP_W_SEL, bi, 0);
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::TOP_CE0_SEL, bi, 1);
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::TOP_CORECLK_SEL, bi, 1);
        } else {
            cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::TOP_W_INV, bi, dbits != 40);
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::TOP_W_SEL, bi, dbits != 40);
        }
        // The legacy unused bottom clock is inverted in narrow modes. CLK2
        // is a real rising-edge read clock and must not inherit that inversion.
        cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::BOT_CLK_INV, bi,
                       dual_clock ? clk2_state == PIN_INV : (async_read ? false : dbits != 40));
        cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_W_SEL, bi, byte_enable || mixed || tdp ? 0 : dbits != 40);

        if (tdp) {
            // Quartus BIDIR_DUAL_PORT with NEW_DATA_NO_NBE_READ on both ports.
            // CFG_RDW_MODE_A/B and CFG_RDW_MODE_MIXED are normalized by the
            // packer. Cyclone V has no independent collision mux, so the
            // accepted DONT_CARE contract intentionally uses these same
            // physical write-through settings.
            cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::TOP_INCLK_SEL, bi, 1);
            cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::BOT_W_INV, bi, false);
            if (mixed && dbits != rdbits) {
                // These selectors match setup_tdp_m10k's mixed-width routes.
                cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::TOP_CE0_SEL, bi, 0);
                cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_CE0_SEL, bi, 1);
                cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::TOP_W_SEL, bi, dbits == 20 ? 1 : 0);
                cv->bmux_n_set(CycloneV::M10K, pos, CycloneV::BOT_W_SEL, bi, dbits == 20 ? 1 : 0);
            }
        }
        cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::TRUE_DUAL_PORT, bi, tdp);

        cv->bmux_b_set(CycloneV::M10K, pos, CycloneV::DISABLE_UNUSED, bi, 0);

        auto permute_init = [](int64_t init) -> int64_t {
            const int permutation[40] = {0, 20, 10, 30, 1, 21, 11, 31, 2, 22, 12, 32, 3, 23, 13, 33, 4, 24, 14, 34,
                                         5, 25, 15, 35, 6, 26, 16, 36, 7, 27, 17, 37, 8, 28, 18, 38, 9, 29, 19, 39};

            int64_t output = 0;
            for (int bit = 0; bit < 40; bit++)
                output |= ((init >> permutation[bit]) & 1) << bit;
            return ~output; // RAM init is inverted.
        };

        Property init;
        if (ci->params.count(id_INIT) == 0) {
            init = Property{0, 10240};
        } else {
            init = ci->params.at(id_INIT);
        }
        for (int bi = 0; bi < 256; bi++)
            cv->bmux_r_set(CycloneV::M10K, pos, CycloneV::RAM, bi, permute_init(init.extract(bi * 40, 40).as_int64()));
    }

    void write_cells()
    {
        for (auto &cell : ctx->cells) {
            CellInfo *ci = cell.second.get();
            Loc loc = ctx->getBelLocation(ci->bel);
            int bi = ctx->bel_data(ci->bel).block_index;
            if (ctx->is_io_cell(ci->type))
                write_io_cell(ci, loc.x, loc.y, bi);
            else if (ctx->is_clkbuf_cell(ci->type))
                write_clkbuf_cell(ci, loc.x, loc.y, bi);
            else if (ci->type == id_MISTRAL_M10K)
                write_m10k_cell(ci, loc.x, loc.y, bi);
            else if (ci->type == id_altera_pll)
                write_pll_cell(ci, loc.x, loc.y);
        }
        for (auto dsp_pos : cv->dsp_get_pos())
            write_dsp_block(CycloneV::pos2x(dsp_pos), CycloneV::pos2y(dsp_pos));
    }

    bool write_alm(uint32_t lab, uint8_t alm)
    {
        auto &alm_data = ctx->labs.at(lab).alms.at(alm);
        auto block_type = ctx->labs.at(lab).is_mlab ? CycloneV::MLAB : CycloneV::LAB;

        std::array<CellInfo *, 2> luts{ctx->getBoundBelCell(alm_data.lut_bels[0]),
                                       ctx->getBoundBelCell(alm_data.lut_bels[1])};
        std::array<CellInfo *, 4> ffs{
                ctx->getBoundBelCell(alm_data.ff_bels[0]), ctx->getBoundBelCell(alm_data.ff_bels[1]),
                ctx->getBoundBelCell(alm_data.ff_bels[2]), ctx->getBoundBelCell(alm_data.ff_bels[3])};
        // Skip empty ALMs
        if (std::all_of(luts.begin(), luts.end(), [](CellInfo *c) { return !c; }) &&
            std::all_of(ffs.begin(), ffs.end(), [](CellInfo *c) { return !c; }))
            return false;

        bool is_lutram =
                (luts[0] && luts[0]->combInfo.mlab_group != -1) || (luts[1] && luts[1]->combInfo.mlab_group != -1);

        auto pos = alm_data.lut_bels[0].pos;
        if (is_lutram) {
            for (int i = 0; i < 10; i++) {
                // Many MLAB settings apply to the whole LAB, not just the ALM
                cv->bmux_m_set(block_type, pos, CycloneV::TMODE, i, CycloneV::RAM);
                cv->bmux_m_set(block_type, pos, CycloneV::BMODE, i, CycloneV::RAM);
                cv->bmux_n_set(block_type, pos, CycloneV::T_FEEDBACK_SEL, i, 1);
            }
            cv->bmux_r_set(block_type, pos, CycloneV::LUT_MASK, alm, ctx->compute_mlab_mask(lab, alm));
            cv->bmux_b_set(block_type, pos, CycloneV::BPKREG1, alm, true);
            cv->bmux_b_set(block_type, pos, CycloneV::TPKREG0, alm, true);
            cv->bmux_m_set(block_type, pos, CycloneV::MCRG_VOLTAGE, 0, CycloneV::VCCL);
            cv->bmux_b_set(block_type, pos, CycloneV::RAM_DIS, 0, false);
            cv->bmux_b_set(block_type, pos, CycloneV::WRITE_EN, 0, true);
            cv->bmux_n_set(block_type, pos, CycloneV::WRITE_PULSE_LENGTH, 0, 650); // picoseconds, presumably
            // TODO: understand how these enables really work
            cv->bmux_b_set(block_type, pos, CycloneV::EN2_EN, 0, false);
            cv->bmux_b_set(block_type, pos, CycloneV::SCLR_DIS, 0, true);
        } else {
            // Combinational mode - TODO: flop feedback and more modes...
            cv->bmux_m_set(block_type, pos, CycloneV::TMODE, alm, alm_data.l6_mode ? CycloneV::C_E : CycloneV::E_0);
            cv->bmux_m_set(block_type, pos, CycloneV::BMODE, alm, alm_data.l6_mode ? CycloneV::D_E : CycloneV::E_1);
            // LUT function
            cv->bmux_r_set(block_type, pos, CycloneV::LUT_MASK, alm, ctx->compute_lut_mask(lab, alm));
        }
        // DFF/LUT output selection
        const std::array<CycloneV::bmux_type_t, 6> mux_settings{CycloneV::TDFF0, CycloneV::TDFF1, CycloneV::TDFF1L,
                                                                CycloneV::BDFF0, CycloneV::BDFF1, CycloneV::BDFF1L};
        const std::array<CycloneV::port_type_t, 6> mux_port{CycloneV::FFT0, CycloneV::FFT1, CycloneV::FFT1L,
                                                            CycloneV::FFB0, CycloneV::FFB1, CycloneV::FFB1L};
        for (int i = 0; i < 6; i++) {
            if (ctx->wires_connected(alm_data.comb_out[i / 3], ctx->get_port(block_type, CycloneV::pos2x(pos),
                                                                             CycloneV::pos2y(pos), alm, mux_port[i])))
                cv->bmux_m_set(block_type, pos, mux_settings[i], alm, CycloneV::NLUT);
        }

        bool is_carry = (luts[0] && luts[0]->combInfo.is_carry) || (luts[1] && luts[1]->combInfo.is_carry);
        if (is_carry)
            cv->bmux_m_set(block_type, pos, CycloneV::ARITH_SEL, alm, CycloneV::ADDER);
        // The carry in/out enable bits
        if (is_carry && alm == 0 && !luts[0]->combInfo.carry_start)
            cv->bmux_b_set(block_type, pos, CycloneV::TTO_DIS, 0, true);
        if (is_carry && alm == 5)
            cv->bmux_b_set(block_type, pos, CycloneV::BTO_DIS, 0, true);
        // Flipflop configuration
        const std::array<CycloneV::bmux_type_t, 2> ef_sel{CycloneV::TEF_SEL, CycloneV::BEF_SEL};
        // This isn't a typo; the *PKREG* bits really are mirrored.
        const std::array<CycloneV::bmux_type_t, 4> pkreg{CycloneV::TPKREG1, CycloneV::TPKREG0, CycloneV::BPKREG1,
                                                         CycloneV::BPKREG0};

        const std::array<CycloneV::bmux_type_t, 2> clk_sel{CycloneV::TCLK_SEL, CycloneV::BCLK_SEL},
                clr_sel{CycloneV::TCLR_SEL, CycloneV::BCLR_SEL}, sclr_dis{CycloneV::TSCLR_DIS, CycloneV::BSCLR_DIS},
                sload_en{CycloneV::TSLOAD_EN, CycloneV::BSLOAD_EN};

        const std::array<CycloneV::bmux_type_t, 3> clk_choice{CycloneV::CLK0, CycloneV::CLK1, CycloneV::CLK2};

        const std::array<CycloneV::bmux_type_t, 3> clk_inv{CycloneV::CLK0_INV, CycloneV::CLK1_INV, CycloneV::CLK2_INV},
                en_en{CycloneV::EN0_EN, CycloneV::EN1_EN, CycloneV::EN2_EN},
                en_ninv{CycloneV::EN0_NINV, CycloneV::EN1_NINV, CycloneV::EN2_NINV};
        const std::array<CycloneV::bmux_type_t, 2> aclr_inv{CycloneV::ACLR0_INV, CycloneV::ACLR1_INV};

        for (int i = 0; i < 2; i++) {
            // EF selection mux
            if (ctx->wires_connected(ctx->getBelPinWire(alm_data.lut_bels[i], i ? id_F0 : id_F1), alm_data.sel_ef[i]))
                cv->bmux_m_set(block_type, pos, ef_sel[i], alm, CycloneV::bmux_type_t::F);
        }

        for (int i = 0; i < 4; i++) {
            CellInfo *ff = ffs[i];
            if (!ff)
                continue;
            // PKREG (input selection)
            if (ctx->wires_connected(alm_data.sel_ef[i / 2], alm_data.ff_in[i]))
                cv->bmux_b_set(block_type, pos, pkreg[i], alm, true);
            // Control set
            // CLK+ENA
            int ce_idx = alm_data.clk_ena_idx[i / 2];
            cv->bmux_m_set(block_type, pos, clk_sel[i / 2], alm, clk_choice[ce_idx]);
            if (ff->ffInfo.ctrlset.clk.inverted)
                set_lab_clock_inversion(block_type, pos, clk_inv[ce_idx]);
            if (ff->getPort(id_ENA) != nullptr) { // not using ffInfo.ctrlset, this has a fake net always to
                                                  // ensure different constants don't collide
                cv->bmux_b_set(block_type, pos, en_en[ce_idx], 0, true);
                cv->bmux_b_set(block_type, pos, en_ninv[ce_idx], 0, ff->ffInfo.ctrlset.ena.inverted);
            } else {
                cv->bmux_b_set(block_type, pos, en_en[ce_idx], 0, false);
            }
            // ACLR
            int aclr_idx = alm_data.aclr_idx[i / 2];
            cv->bmux_b_set(block_type, pos, clr_sel[i / 2], alm, aclr_idx == 1);
            if (ff->ffInfo.ctrlset.aclr.inverted)
                cv->bmux_b_set(block_type, pos, aclr_inv[aclr_idx], 0, true);
            // SCLR
            if (ff->ffInfo.ctrlset.sclr.net != nullptr) {
                cv->bmux_b_set(block_type, pos, CycloneV::SCLR_INV, 0, ff->ffInfo.ctrlset.sclr.inverted);
                cv->bmux_b_set(block_type, pos, CycloneV::SCLR_DIS, 0, false);
            } else {
                cv->bmux_b_set(block_type, pos, sclr_dis[i / 2], alm, true);
            }
            // SLOAD
            if (ff->ffInfo.ctrlset.sload.net != nullptr) {
                cv->bmux_b_set(block_type, pos, sload_en[i / 2], alm, true);
                if (ff->ffInfo.ctrlset.sload.net->name == ctx->id("$PACKER_GND_NET")) {
                    // force-disabled LOAD (see workaround in assign_ff_info)
                    cv->bmux_b_set(block_type, pos, CycloneV::SLOAD_EN, 0, false);
                }
                cv->bmux_b_set(block_type, pos, CycloneV::SLOAD_INV, 0, ff->ffInfo.ctrlset.sload.inverted);
            }
        }
        if (is_lutram) {
            for (int i = 0; i < 2; i++) {
                CellInfo *lut = luts[i];
                if (!lut || lut->combInfo.mlab_group == -1)
                    continue;
                int ce_idx = alm_data.clk_ena_idx[1];
                cv->bmux_m_set(block_type, pos, clk_sel[1], alm, clk_choice[ce_idx]);
                if (lut->combInfo.wclk.inverted)
                    set_lab_clock_inversion(block_type, pos, clk_inv[ce_idx]);
                if (lut->getPort(id_A1EN) != nullptr) {
                    cv->bmux_b_set(block_type, pos, en_en[ce_idx], 0, true);
                    cv->bmux_b_set(block_type, pos, en_ninv[ce_idx], 0, lut->combInfo.we.inverted);
                } else {
                    cv->bmux_b_set(block_type, pos, en_en[ce_idx], 0, false);
                }
                // TODO: understand what these are doing
                cv->bmux_b_set(block_type, pos, sclr_dis[0], alm, true);
                cv->bmux_b_set(block_type, pos, sclr_dis[1], alm, true);
            }
        }
        return true;
    }

    void write_ff_routing(uint32_t lab)
    {
        auto &lab_data = ctx->labs.at(lab);
        auto pos = lab_data.alms.at(0).lut_bels[0].pos;
        auto block_type = ctx->labs.at(lab).is_mlab ? CycloneV::MLAB : CycloneV::LAB;

        const std::array<CycloneV::bmux_type_t, 2> aclr_inp{CycloneV::ACLR0_SEL, CycloneV::ACLR1_SEL};
        for (int i = 0; i < 2; i++) {
            // Quartus seems to set unused ACLRs to ACLR0
            if (lab_data.aclr_used[i])
                cv->bmux_m_set(block_type, pos, aclr_inp[i], 0, (i == 1) ? CycloneV::DIN2 : CycloneV::DIN3);
            else if (i == 0)
                cv->bmux_m_set(block_type, pos, aclr_inp[i], 0, CycloneV::ACLR0);
        }
        for (int i = 0; i < 3; i++) {
            // Check for fabric->clock routing
            if (ctx->wires_connected(
                        ctx->get_port(block_type, CycloneV::pos2x(pos), CycloneV::pos2y(pos), -1, CycloneV::DATAIN, 0),
                        lab_data.clk_wires[i]))
                cv->bmux_m_set(block_type, pos, CycloneV::CLKA_SEL, 0, CycloneV::DIN0);
        }
    }

    void write_labs()
    {
        for (size_t lab = 0; lab < ctx->labs.size(); lab++) {
            bool used = false;
            for (uint8_t alm = 0; alm < 10; alm++)
                used |= write_alm(lab, alm);
            if (used)
                write_ff_routing(lab);
        }
    }

    void run()
    {
        cv->clear();
        options();
        write_routing();
        write_cells();
        write_labs();
        ctx->bitstream_configured = true;
    }
};
} // namespace

void Arch::build_bitstream()
{
    MistralBitgen gen(getCtx());
    gen.run();

    // This is a hack to run timing analysis yet again after the bitstream is
    // configured in Mistral, because the analogue simulator won't work until
    // it has a bitstream in the library.
    //
    // A better solution would be to move a lot of this bitstream code to
    // {un,}bind{Bel, Pip} and friends, but we're not there yet.
    log_info("Running signoff timing analysis...\n");

    timing_analysis(getCtx(), true, true, true, true, true);
}

NEXTPNR_NAMESPACE_END
