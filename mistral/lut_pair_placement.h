/* Optional original-LUT pair relocation or composed enable copy. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LUT_PAIR_PLACEMENT_H
#define MISTRAL_LUT_PAIR_PLACEMENT_H
#include "nextpnr_namespaces.h"
NEXTPNR_NAMESPACE_BEGIN
struct Context;
// Earlier listings cannot precede this pass, and its listing must be final.
void prevalidate_lut_pair_prefix(Context *ctx);
NEXTPNR_NAMESPACE_END
#endif
