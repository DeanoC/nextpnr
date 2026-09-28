#ifndef MISTRAL_READY_SHORTEST_BACKEND_H
#define MISTRAL_READY_SHORTEST_BACKEND_H
#include "gpurouter.h"
#include "nextpnr.h"
NEXTPNR_NAMESPACE_BEGIN
std::vector<GpuRouteTree> ready_shortest_candidate(Context *ctx, NetInfo *target, WireId source, WireId sink,
                                                   const char *prefix);
NEXTPNR_NAMESPACE_END
#endif
