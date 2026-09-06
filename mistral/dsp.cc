// SPDX-License-Identifier: ISC
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

void Arch::create_dsp(int x, int y)
{
    // Reserve the entire physical DSP for lane 0. The three 9x9 lanes share
    // mode, signedness and register controls; independent packing is not yet
    // supported. See Mistral docs/cyclonev_details.rst, DSP input mapping.
    BelId bel = add_bel(x, y, id_MISTRAL_MUL9X9, id_MISTRAL_MUL9X9);
    for (int bit = 0; bit < 9; ++bit) {
        add_bel_pin(bel, idf("A[%d]", bit), PORT_IN, get_port(CycloneV::DSP, x, y, 0, CycloneV::DATAIN, bit));
        add_bel_pin(bel, idf("B[%d]", bit), PORT_IN, get_port(CycloneV::DSP, x, y, 2, CycloneV::DATAIN, bit));
    }
    for (int bit = 0; bit < 18; ++bit)
        add_bel_pin(bel, idf("Y[%d]", bit), PORT_OUT, get_port(CycloneV::DSP, x, y, -1, CycloneV::RESULT, bit));
}

NEXTPNR_NAMESPACE_END
