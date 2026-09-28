#ifndef MISTRAL_READY_REUSE_H
#define MISTRAL_READY_REUSE_H
#include "nextpnr.h"
NEXTPNR_NAMESPACE_BEGIN
bool ready_reuse_equivalent(const CellInfo *original, const CellInfo *replica);
bool ready_reuse_clock_valid(const CellInfo *ff, const NetInfo *clock);
void ready_reuse_move(Context *ctx, CellInfo *ff, NetInfo *replica_net);
void diagnostic_ready_reuse(Context *ctx, const char *prefix);
void diagnostic_ready_reuse_timing(Context *ctx, const char *prefix);
NEXTPNR_NAMESPACE_END
#endif
