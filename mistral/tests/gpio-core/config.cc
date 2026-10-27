#include "cyclonev.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>

using C = mistral::CycloneV;

int main(int argc, char **argv)
{
    if (argc != 2)
        return 1;
    std::unique_ptr<C> cv(C::get_model("5CSEBA6U23I7"));
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
    if (data.empty())
        return 2;
    cv->rbf_load(data.data(), data.size());
    std::string name;
    while (std::cin >> name) {
        const auto *pin = cv->pin_find_name(name);
        if (!pin)
            return 3;
        const C::xycoords pos{uint32_t(pin->pad & 0x3fff)};
        const int8_t bi = pin->pad >> 14;
        const auto lane = cv->p2p_to(C::pnode_coords{C::GPIO, pos, C::PNONE, bi, -1});
        if (!lane)
            return 4;
        std::cout << name << '\t' << pos.x() << '\t' << pos.y() << '\t' << int(bi)
                  << '\t' << lane.to_string();
        for (auto port : {C::CLKIN, C::CLKOUT, C::DATAIN, C::DATAOUT, C::OEIN}) {
            const int count = port == C::DATAIN ? 5 : port == C::DATAOUT ? 4 : 2;
            for (int i = 0; i < count; ++i) {
                auto identity = C::pnode_coords{C::GPIO, pos, port, bi, int8_t(i)};
                auto ri = cv->pnode_to_rnode(identity);
                if (ri == 0xffffffff) {
                    auto peer = cv->p2p_to(identity);
                    if (!peer) {
                        const auto peers = cv->p2p_from(identity);
                        if (peers.size() == 1)
                            peer = peers.front();
                    }
                    const auto bypass = cv->hmc_get_bypass(peer);
                    if (bypass) {
                        identity = bypass;
                        ri = cv->pnode_to_rnode(identity);
                    }
                }
                if (ri != 0xffffffff) {
                    std::cout << '\t' << C::port_type_names[port] << i << '=' << cv->ri2rc(ri).to_string();
                    std::cout << '\t' << C::port_type_names[port] << i << "_PNODE=" << identity.to_string();
                }
            }
        }
        for (auto mux : {C::RB_T1_SEL_IREG_CFF_DELAY, C::SET_T3_FOR_CDATA0IN, C::SET_T3_FOR_CDATA1IN,
                         C::RB_T9_SEL_OREG_DFF_DELAY, C::RB_T9_SEL_EREG_CFF_DELAY,
                         C::INPUT_REG0_SEL, C::INPUT_REG1_SEL, C::INPUT_REG2_SEL, C::INPUT_REG3_SEL,
                         C::INPUT_REG4_SEL, C::OUTREG_OUTPUT_SEL, C::OUTREG_MODE_SEL, C::OEREG_OUTPUT_SEL,
                         C::OEREG_HR_CLK_EN, C::OEREG_CLK_INV, C::RBOE_LVL_FR_CLK_EN,
                         C::RB_FIFO_WCLK_EN, C::RB_FIFO_WCLK_INV, C::INPUT_PATH_CE_IN,
                         C::CE_OUTREG_TIEOFF_EN, C::CE_OEREG_TIEOFF_EN, C::USE_CLR_INREG_EN,
                         C::USE_CLR_OUTREG_EN, C::OEREG_ACLR_EN}) {
            C::bmux_setting_t setting;
            if (!cv->bmux_get(C::DQS16, lane.p(), mux, lane.bi(), setting))
                return 5;
            std::cout << '\t' << C::bmux_type_names[mux] << '=';
            if (setting.type == C::MT_MUX) {
                std::cout << C::bmux_type_names[setting.s];
            } else if (setting.type == C::MT_RAM) {
                if (setting.r.size() > 4)
                    return 6;
                uint32_t value = 0;
                for (size_t i = 0; i < setting.r.size(); ++i)
                    value |= uint32_t(setting.r[i]) << (8 * i);
                std::cout << value;
            } else {
                std::cout << setting.s;
            }
        }
        std::cout << '\n';
    }
}
