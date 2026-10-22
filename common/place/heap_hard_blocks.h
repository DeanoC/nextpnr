#ifndef HEAP_HARD_BLOCKS_H
#define HEAP_HARD_BLOCKS_H
#include "nextpnr.h"
#include "placer_heap.h"
#include "timing.h"

NEXTPNR_NAMESPACE_BEGIN
struct HeapHardBlockLocation {
    double x, y;
    bool movable;
};
struct HeapHardBlockAssignment {
    CellInfo *cell;
    BelId bel;
};
// A bounded, heuristic-only proposal. No Context mutations, RNG or ID creation.
// Every movable member of the declared bucket participates, or the bucket falls back.
bool plan_heap_hard_blocks(Context *ctx, const PlacerHeapCfg &cfg, const TimingAnalyser &timing,
                          const dict<IdString, HeapHardBlockLocation> &locations,
                          std::vector<HeapHardBlockAssignment> &assignment, std::string &reason);
// Called after ordinary strict-legalizer unbinding; failure restores that exact
// unbound state. All bindings are weak and use the native legality API.
bool bind_heap_hard_blocks(Context *ctx, const PlacerHeapCfg &cfg,
                           const std::vector<HeapHardBlockAssignment> &assignment, std::string &reason);
NEXTPNR_NAMESPACE_END
#endif
