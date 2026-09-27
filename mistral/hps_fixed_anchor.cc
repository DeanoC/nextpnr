/* Diagnostic fixed FPGA2SDRAM anchor during analytical HeAP only.
 * Restore the ordinary WEAK binding contract before unchanged SA refinement.
 * SPDX-License-Identifier: ISC
 */
#include "nextpnr.h"
#include "placer_heap.h"
#include "log.h"
#include <fstream>
#include <map>
#include <memory>
#include <string>

NEXTPNR_NAMESPACE_BEGIN
void configure_hps_fixed_anchor(Context *ctx, PlacerHeapCfg &cfg, const char *prefix)
{
    if (!prefix || !*prefix) return; // No identifiers or callbacks allocated while disabled.
    CellInfo *hps=nullptr;
    for (const auto &entry:ctx->cells) {
        auto cell=entry.second.get();
        if (cell->type!=id_cyclonev_hps_interface_fpga2sdram) continue;
        if (hps) log_error("HPS fixed anchor diagnostic requires exactly one FPGA2SDRAM cell.\n");
        hps=cell;
    }
    if (!hps) log_error("HPS fixed anchor diagnostic has no FPGA2SDRAM cell.\n");
    bool bel_constraint=false;
    for (const auto &attr:hps->attrs) if (attr.first.str(ctx)=="BEL") bel_constraint=true;
    if (hps->bel!=BelId() || hps->cluster!=ClusterId() || hps->region || hps->isPseudo() || bel_constraint)
        log_error("HPS fixed anchor diagnostic requires an initially unbound, unclustered, unconstrained cell.\n");
    BelId bel;
    for (auto b:ctx->getBels()) {
        if (!ctx->isValidBelForCellType(hps->type,b)) continue;
        if (bel!=BelId()) log_error("HPS fixed anchor diagnostic requires a unique compatible BEL.\n");
        bel=b;
    }
    if (bel==BelId() || !ctx->checkBelAvail(bel)) log_error("HPS fixed anchor diagnostic BEL is unavailable.\n");
    NPNR_ASSERT(!cfg.diagnostic_cell && !cfg.observe_diagnostic_cell && !cfg.before_refine);
    auto trace=std::make_shared<std::ofstream>(std::string(prefix)+".phases.tsv");
    *trace << "phase\tcell\tbel\tanalytical_x\tanalytical_y\tin_place_cells\trows_known\trow_assigned\tlocked\tstrength\n";
    trace->flush(); if (!*trace) log_error("Cannot write HPS anchor phase trace.\n");
    const Loc physical=ctx->getBelLocation(bel);
    const std::string path(prefix);
    cfg.ioBufTypes.insert(hps->type);
    cfg.diagnostic_cell=hps;
    cfg.observe_diagnostic_cell=[ctx,hps,bel,physical,path,trace](const char *phase,Loc xy,bool in_place,bool rows_known,bool row_assigned,bool locked) {
        NPNR_ASSERT(locked && !in_place && (!rows_known || !row_assigned));
        NPNR_ASSERT(xy.x==physical.x && xy.y==physical.y);
        NPNR_ASSERT(hps->bel==bel && ctx->getBoundBelCell(bel)==hps);
        const std::string p(phase);
        const bool weak=p=="refine_ready" || p=="after_refine";
        NPNR_ASSERT(hps->belStrength==(weak?STRENGTH_WEAK:STRENGTH_STRONG));
        *trace << phase << '\t' << hps->name.str(ctx) << '\t' << ctx->nameOfBel(bel) << '\t'
               << xy.x << '\t' << xy.y << '\t' << in_place << '\t' << rows_known << '\t';
        if (rows_known) *trace << row_assigned; else *trace << "unknown";
        *trace << '\t' << locked << '\t' << (weak?"WEAK":"STRONG") << '\n';
        trace->flush(); if (!*trace) log_error("Cannot write HPS anchor phase trace.\n");
        if (p=="before_refine" || p=="after_refine") {
            std::map<std::string,std::string> placements;
            for (const auto &entry:ctx->cells)
                if (entry.second->bel!=BelId()) placements[entry.first.str(ctx)]=ctx->nameOfBel(entry.second->bel);
            std::ofstream out(path+"."+p+".bels.tsv"); out << "cell\tbel\n";
            for (const auto &entry:placements) out << entry.first << '\t' << entry.second << '\n';
            out.close(); if (!out) log_error("Cannot write HPS anchor phase placements.\n");
        }
    };
    cfg.before_refine=[ctx,hps,bel]() {
        NPNR_ASSERT(hps->bel==bel && ctx->getBoundBelCell(bel)==hps && hps->belStrength==STRENGTH_STRONG);
        // Binding and arch information stay intact; only restore the baseline strength.
        hps->belStrength=STRENGTH_WEAK;
        NPNR_ASSERT(ctx->isBelLocationValid(bel));
    };
    std::ofstream scope(path+".scope.json");
    scope << "{\n  \"mode\": \"fixed-HPS-analytical-anchor\",\n"
             "  \"mechanism\": \"HeAP ioBufTypes seed locking\",\n"
             "  \"before_SA\": \"restore WEAK binding strength\",\n"
             "  \"unchanged\": [\"SA algorithm\", \"predictDelay\", \"timing model\"]\n}\n";
    scope.close(); if (!scope) log_error("Cannot write HPS anchor scope record.\n");
    log_info("HPS fixed anchor: analytical HeAP locked to %s; WEAK strength restored before SA.\n",ctx->nameOfBel(bel));
}
NEXTPNR_NAMESPACE_END
