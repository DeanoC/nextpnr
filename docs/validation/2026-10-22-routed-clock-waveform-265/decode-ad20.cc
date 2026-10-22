#include "cyclonev.h"
#include <fstream>
#include <iterator>
#include <cstdio>
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    using C = mistral::CycloneV;
    auto m = C::get_model("5CSEBA6U23I7");
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    if (data.empty()) return 2;
    m->rbf_load(data.data(), data.size());
    auto pin = m->pin_find_name("AD20");
    auto pos = C::xycoords(pin->pad & 0x3fff);
    int8_t bi = pin->pad >> 14;
    auto lane = m->p2p_to(C::pnode_coords{C::GPIO, pos, C::PNONE, bi, -1});
    auto dump = [&](C::block_type_t block, C::xycoords xy, int8_t index, C::bmux_type_t mux) {
        C::bmux_setting_t s;
        if (!m->bmux_get(block, xy, mux, index, s)) return false;
        printf("%s\t%u\t%d\ttype=%d", C::bmux_type_names[mux], s.s, s.def, s.type);
        for (auto b : s.r) printf("\t%02x", b);
        printf("\n");
        return true;
    };
    for (auto mux : {C::RB_T9_SEL_OREG_DFF_DELAY, C::RB_T9_SEL_EREG_CFF_DELAY,
                    C::OUTREG_OUTPUT_SEL, C::OUTREG_MODE_SEL, C::OEREG_OUTPUT_SEL,
                    C::RBOE_LVL_FR_CLK_EN, C::INPUT_REG4_SEL})
        if (!dump(C::DQS16, lane.p(), lane.bi(), mux)) return 3;
    for (auto mux : {C::DRIVE_STRENGTH, C::SLEW_RATE_SLOW, C::IOCSR_STD,
                    C::USE_WEAK_PULLUP, C::USE_BUS_HOLD, C::USE_PCI_DIODE_CLAMP})
        if (!dump(C::GPIO, pos, bi, mux)) return 4;
    for (int8_t word : {0, 1}) {
        C::pnode_coords p{C::GPIO, pos, C::DATAOUT, bi, word};
        auto ri = m->pnode_to_rnode(p);
        if (ri == 0xffffffff) {
            p = m->hmc_get_bypass(m->p2p_to(p));
            ri = m->pnode_to_rnode(p);
        }
        bool found = false;
        for (const auto &inv : m->inv_get()) if (inv.node == ri) {
            printf("DATAOUT%d_INVERSION\t%d\t%d\t%s\n", word, inv.value, inv.def, p.to_string().c_str());
            found = true;
        }
        if (!found) return 5;
    }
}
