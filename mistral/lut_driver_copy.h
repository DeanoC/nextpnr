/* Optional report-guided one-edge LUT driver copies. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_LUT_DRIVER_COPY_H
#define MISTRAL_LUT_DRIVER_COPY_H
#include "nextpnr_namespaces.h"
NEXTPNR_NAMESPACE_BEGIN
struct Context;
// A listing from an earlier stage cannot feed a later driver-copy stage.
// Called while preloading CLI state and again before placement.
void prevalidate_lut_driver_copy_prefix(Context *ctx);
// Every earlier listing must be final before a composed-copy plan starts.
void prevalidate_lut_pair_copy_plan_prefix(Context *ctx);
// A post-plan follows reductions and decomposition, so their listings must be final too.
void prevalidate_local_remap_post_prefix(Context *ctx);
NEXTPNR_NAMESPACE_END
#endif
