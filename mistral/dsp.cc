// SPDX-License-Identifier: ISC
#include "nextpnr.h"

#include "dsp.h"

NEXTPNR_NAMESPACE_BEGIN

void Arch::create_dsp(int x, int y)
{
    for (const auto &lane : mistral_dsp_lanes) {
        BelId bel = add_bel(x, y, id_MISTRAL_MUL9X9, id_MISTRAL_MUL9X9);
        for (int bit = 0; bit < 9; ++bit) {
            add_bel_pin(bel, idf("A[%d]", bit), PORT_IN,
                        get_port(CycloneV::DSP, x, y, lane.a_group, CycloneV::DATAIN, bit));
            add_bel_pin(bel, idf("B[%d]", bit), PORT_IN,
                        get_port(CycloneV::DSP, x, y, lane.b_group, CycloneV::DATAIN, bit));
        }
        for (int bit = 0; bit < 18; ++bit)
            add_bel_pin(bel, idf("Y[%d]", bit), PORT_OUT,
                        get_port(CycloneV::DSP, x, y, -1, CycloneV::RESULT, lane.result_offset + bit));
    }
}

NEXTPNR_NAMESPACE_END
