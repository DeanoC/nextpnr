/* Diagnostic HPS logical-pin offsets for analytical HeAP only.
 * SA refinement and all timing predictors remain unchanged.
 * SPDX-License-Identifier: ISC
 */
#include "nextpnr.h"
#include "placer_heap.h"
#include "log.h"
#include <fstream>
#include <map>
#include <sstream>

NEXTPNR_NAMESPACE_BEGIN
void configure_hps_pin_geometry(Context *ctx, PlacerHeapCfg &cfg, const char *prefix)
{
    if (!prefix || !*prefix) return; // No identifier allocation in disabled mode.
    BelId hps_bel;
    for (auto bel : ctx->getBels()) {
        if (ctx->getBelType(bel) != id_cyclonev_hps_interface_fpga2sdram) continue;
        NPNR_ASSERT(hps_bel == BelId());
        hps_bel=bel;
    }
    NPNR_ASSERT(hps_bel != BelId());
    const Loc base=ctx->getBelLocation(hps_bel);
    dict<std::pair<IdString,IdString>,Loc> offsets;
    std::map<std::pair<std::string,std::string>,std::string> rows;
    int applied=0, considered=0;
    for (const auto &entry : ctx->cells) {
        const CellInfo *cell=entry.second.get();
        if (cell->type != id_cyclonev_hps_interface_fpga2sdram) continue;
        for (const auto &port_entry : cell->ports) {
            ++considered;
            IdString logical=port_entry.first;
            auto state=cell->get_pin_state(logical);
            const auto &pins=ctx->getBelPinsForCellPin(cell,logical);
            IdString physical=pins.size()==1 ? pins.front() : IdString();
            WireId wire=physical==IdString() ? WireId() : ctx->getBelPinWire(hps_bel,physical);
            Loc offset(0,0,0);
            int clocks=0;
            std::string reason;
            if (state==PIN_0 || state==PIN_1) reason="folded_constant";
            else if (!port_entry.second.net) reason="unconnected";
            else if (port_entry.second.net->constant_value != IdString() ||
                     (port_entry.second.net->driver.cell && port_entry.second.net->driver.cell->type==id_MISTRAL_CONST)) reason="constant_net";
            else if (ctx->getPortTimingClass(cell,logical,clocks)==TMG_CLOCK_INPUT) reason="clock";
            else if (port_entry.second.type==PORT_OUT && port_entry.second.net->users.empty()) reason="unused_output";
            else if (pins.empty()) reason="unmapped";
            else if (pins.size()!=1) reason="multiple_physical_pins";
            else if (wire==WireId()) reason="missing_wire";
            else if (wire.node.t()!=mistral::CycloneV::GIN && wire.node.t()!=mistral::CycloneV::GOUT) reason="non_fabric_wire";
            else {
                offset=Loc(int(wire.node.x())-base.x,int(wire.node.y())-base.y,0);
                offsets[{cell->name,logical}]=offset;
                ++applied;
            }
            std::ostringstream row;
            row << cell->name.str(ctx) << '\t' << logical.str(ctx) << '\t'
                << (port_entry.second.type==PORT_IN ? "input" : port_entry.second.type==PORT_OUT ? "output" : "inout") << '\t'
                << (port_entry.second.net ? port_entry.second.net->name.str(ctx) : "") << '\t' << int(state) << '\t'
                << ctx->nameOfBel(hps_bel) << '\t' << base.x << '\t' << base.y << '\t'
                << (physical==IdString() ? "" : physical.str(ctx)) << '\t'
                << (wire==WireId() ? "" : std::string(ctx->nameOfWire(wire))) << '\t';
            if (wire!=WireId()) row << wire.node.x() << '\t' << wire.node.y(); else row << '\t';
            row << '\t' << offset.x << '\t' << offset.y << '\t' << (reason.empty() ? "apply" : "skip") << '\t' << reason << '\n';
            rows[{cell->name.str(ctx),logical.str(ctx)}]=row.str();
        }
    }
    if (applied==0) log_error("HPS pin geometry diagnostic has no applicable connected fabric pins.\n");
    // The physical BEL is unique and fixed; its offsets remain relative to
    // the analytical cell variable even while that variable is being solved.
    // The cache is immutable and safe for HeAP's concurrent X/Y equation build.
    cfg.get_port_offset=[offsets=std::move(offsets)](const PortRef &port) {
        if (!port.cell) return Loc(0,0,0);
        auto found=offsets.find({port.cell->name,port.port});
        return found==offsets.end() ? Loc(0,0,0) : found->second;
    };
    std::ofstream manifest(std::string(prefix)+".pins.tsv");
    manifest << "cell\tlogical_pin\tdirection\tnet\tpin_state\tbel\tbel_x\tbel_y\tphysical_pin\twire\twire_x\twire_y\toffset_x\toffset_y\tstatus\treason\n";
    for (const auto &row : rows) manifest << row.second;
    manifest.close(); if (!manifest) log_error("Cannot write HPS geometry manifest.\n");
    std::ofstream scope(std::string(prefix)+".scope.json");
    scope << "{\n  \"mode\": \"analytical-heap-only\",\n  \"considered_ports\": " << considered
          << ",\n  \"applied_ports\": " << applied
          << ",\n  \"changed\": [\"HeAP bounds and weights\", \"HeAP equation RHS\", \"HeAP HPWL\"],\n"
             "  \"unchanged\": [\"SA refinement\", \"predictDelay\", \"timing model\", \"legal BEL coordinates\"]\n}\n";
    scope.close(); if (!scope) log_error("Cannot write HPS geometry scope record.\n");
    log_info("HPS pin geometry: analytical HeAP only, %d/%d ports use physical offsets; SA and timing prediction unchanged.\n",applied,considered);
}
NEXTPNR_NAMESPACE_END
