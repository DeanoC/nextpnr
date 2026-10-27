/* Measured placement feedback. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_ROUTED_PLACEMENT_H
#define MISTRAL_ROUTED_PLACEMENT_H
#include <map>
#include "nextpnr.h"
#include "json11.hpp"

NEXTPNR_NAMESPACE_BEGIN

json11::Json routed_placement_snapshot(Context *ctx);
json11::Json routed_placement_endpoints(Context *ctx);
// Validate a native, explicitly declared post-route LUT-only placement change.
// Registered endpoints, graph, constraints, strengths and all other BELs remain exact.
void check_post_route_lut_moves(const json11::Json &receipt, const json11::Json &final);
json11::Json make_routed_placement_plan(const json11::Json &origin, const json11::Json &parent,
                                      const json11::Json &target, delay_t before, delay_t after,
                                      const json11::Json &routing = json11::Json());
bool apply_routed_placement_plan(Context *ctx, const json11::Json &plan, const json11::Json &origin,
                                 bool check_only = false);
json11::Json make_routed_placement_group_plan(const json11::Json &origin, const json11::Json &parent,
                                             const json11::Json &target);
void prepare_routed_placement(Context *ctx);
void finish_routed_placement(Context *ctx, bool timing_pass);
json11::Json snapshot_routed_placement_routes(Context *ctx, const json11::Json &parent);
json11::Json apply_routed_placement_routes(Context *ctx, const json11::Json &witness, const json11::Json &parent,
                                          const json11::Json &handoff = json11::Json());
void preserve_routed_placement_routes(Context *ctx);
void check_routed_placement_routes(Context *ctx, const json11::Json &witness, const json11::Json &preserved);
void check_routed_placement_branches(Context *ctx, const json11::Json &witness,
                                    const json11::Json &parent, const json11::Json &target);
std::map<WireId, IdString> routed_placement_route_reservations(Context *ctx, const json11::Json &witness,
                                                            const json11::Json &parent);

NEXTPNR_NAMESPACE_END
#endif
