/* Post-place gather for long failing setup hops. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_CRITICAL_GATHER_H
#define MISTRAL_CRITICAL_GATHER_H

#include "nextpnr_namespaces.h"
#include <vector>

NEXTPNR_NAMESPACE_BEGIN

struct Context;

// One constrained clock after static timing. `known` is false when that clock
// disappeared from the later analysis.
struct GatherClockSlack
{
    bool known = false;
    long slack_ps = 0;
};

// Worst setup slack must improve by at least min_gain_ps. A clock that already
// met its constraint must still meet it. A clock that disappears is a reject.
bool critical_gather_accepts(long before_worst_ps, const std::vector<GatherClockSlack> &before, long after_worst_ps,
                             const std::vector<GatherClockSlack> &after, long min_gain_ps);

// HeAP's legaliser can leave a critical cone split by many rows. The refine
// annealer only proposes moves inside a 3-tile radius and stops when wirelength
// stalls, so those hops survive into routing. Slide the movable end of a long
// negative-slack hop toward the other end when the move is legal and the
// acceptance rule above holds. Clocks that already meet setup are left alone.
void gather_critical_paths(Context *ctx);

NEXTPNR_NAMESPACE_END

#endif
