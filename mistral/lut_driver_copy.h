/* Optional report-guided one-edge LUT driver copies. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LUT_DRIVER_COPY_H
#define MISTRAL_LUT_DRIVER_COPY_H
#include "nextpnr_namespaces.h"
NEXTPNR_NAMESPACE_BEGIN
struct Context;
// A listing from an earlier stage cannot feed a later driver-copy stage.
// Called while preloading CLI state and again before placement.
void prevalidate_lut_driver_copy_prefix(Context *ctx);
NEXTPNR_NAMESPACE_END
#endif
