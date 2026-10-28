/* Bounded native routing-cost estimates for placement proposals. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_CRITICAL_COHORT_ROUTE_COST_H
#define MISTRAL_CRITICAL_COHORT_ROUTE_COST_H
#include <map>
#include <optional>
#include <set>
#include <functional>
#include <memory>
#include "nextpnr.h"
#include "json11.hpp"

NEXTPNR_NAMESPACE_BEGIN
struct CriticalCohortBranchRepair
{
    IdString net;
    WireId target;
    PipId avoid;
};
class CriticalCohortRouteCosts
{
  public:
    static constexpr int node_limit = 200000;
    static json11::Json snapshot(Context *ctx, const json11::Json &parent);
    CriticalCohortRouteCosts(Context *ctx, const json11::Json &source, const json11::Json &parent);
    std::optional<delay_t> reference(IdString net, WireId source, const std::vector<WireId> &sinks) const;
    // Price every physical sink with bidirectional Dijkstra. The budget counts
    // settled vertices across every sink and net in one placement trial.
    // Partial results are diagnostics; missing sinks make a proposal ineligible.
    std::map<WireId, delay_t> estimate(Context *ctx, NetInfo *net, WireId source, const std::set<WireId> &sinks,
        const std::set<IdString> &released, const std::map<WireId, IdString> &drivers, int &budget,
        json11::Json::object *trace = nullptr, std::map<WireId, PipId> *tree_out = nullptr) const;
    // Detour an exclusive sink branch while retaining every other parent edge.
    // Each alternative excludes a distinct parent pip and uses the same bounded
    // native search. The returned costs cover the complete fanout, not only target.
    std::vector<PipId> branch_alternatives(Context *ctx, IdString net, WireId target,
                                          const std::set<WireId> &sinks) const;
    std::map<WireId, delay_t> repair_branch(Context *ctx, NetInfo *net, WireId source,
        const std::set<WireId> &sinks, WireId target, PipId avoid,
        const std::set<IdString> &released, const std::map<WireId, IdString> &drivers, int &budget,
        json11::Json::object *trace, std::map<WireId, PipId> *tree_out) const;
    // Connect the actual moved driver to declared roots of its parent's
    // downstream forest. Retained edges are exact native parent edges. This
    // only searches; the caller must bind, check legality and run full STA.
    std::map<WireId, PipId> reconnect_source(Context *ctx, NetInfo *net, WireId source,
        const std::set<WireId> &roots, const std::map<WireId, PipId> &forest,
        const std::set<IdString> &released, const std::map<WireId, IdString> &drivers,
        int &budget, json11::Json::object *trace = nullptr) const;
    const std::map<WireId, PipId> &parent_tree(IdString net) const { return trees.at(net); }
    void with_analogue(Context *ctx, const std::set<IdString> &released,
        const std::map<IdString, std::map<WireId, PipId>> &replacement, const std::function<void()> &inspect) const;
    std::optional<Arch::AnalogueArc> analogue(Context *ctx, NetInfo *net, PortRef sink,
                                             const std::map<WireId, PipId> &tree) const;

  private:
    bool calibrated = false;
    float prior = 1.0f;
    dict<PipId, delay_t> observed;
    std::array<Arch::TypeCalibration, 256> types{};
    std::map<WireId, IdString> occupied;
    std::map<IdString, WireId> sources;
    std::map<IdString, std::map<WireId, delay_t>> references;
    mutable dict<PipId, delay_t> costs;
    std::map<IdString, std::map<WireId, PipId>> trees;
    std::map<IdString, std::vector<PipId>> background;
    std::map<WireId, WireId> reservations;
    mutable std::unique_ptr<mistral::CycloneV> analogue_model;
    delay_t cost(Context *ctx, PipId pip) const;
    bool branch_stub(Context *ctx, IdString net, WireId target, const std::set<WireId> &sinks,
        std::map<WireId, PipId> &retained, std::vector<PipId> &removed) const;
    std::map<WireId, delay_t> search(Context *ctx, NetInfo *net, WireId source, const std::set<WireId> &sinks,
        const std::set<IdString> &released, const std::map<WireId, IdString> &drivers, int &budget,
        json11::Json::object *trace, std::map<WireId, PipId> *tree_out,
        const std::map<WireId, PipId> &retained, PipId avoid,
        const std::set<WireId> &barriers = {}) const;
};
// Programs the ordinary native data-path mux writer on a scoped model. It does
// not bind routing, configure clocks, run STA or mark a bitstream complete.
void configure_critical_cohort_analogue(Context *ctx, const pool<PipId> &pips);
NEXTPNR_NAMESPACE_END
#endif
