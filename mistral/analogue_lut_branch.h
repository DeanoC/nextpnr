/* Bounded compound LUT/branch proposals. SPDX-License-Identifier: ISC */
#ifndef MISTRAL_ANALOGUE_LUT_BRANCH_H
#define MISTRAL_ANALOGUE_LUT_BRANCH_H
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include "analogue_repair.h"
#include "analogue_routing.h"
#include "analogue_lut_moves.h"
#include "critical_cohort.h"
#include "critical_cohort_route_cost.h"
#include "critical_cohort_model.h"
#include "jsonwrite.h"
#include "routed_placement.h"
#include "timing.h"
NEXTPNR_NAMESPACE_BEGIN
namespace lut_branch_detail {
using J=json11::Json;
struct Edge { WireId wire; PipId pip; PlaceStrength strength; };
using Tree=std::vector<Edge>;
using Routes=std::map<IdString,Tree>;
struct Calibration {
    bool enabled;float prior;dict<PipId,delay_t> observed;std::array<Arch::TypeCalibration,256> types;
    explicit Calibration(Context *ctx):enabled(ctx->pip_delay_calibrated),prior(ctx->pip_delay_prior),
        observed(ctx->pip_delay_observed),types(ctx->pip_type_calibration) {}
    bool matches(Context *ctx) const {
        if(enabled!=ctx->pip_delay_calibrated||prior!=ctx->pip_delay_prior||observed.size()!=ctx->pip_delay_observed.size())return false;
        for(const auto &p:observed){auto q=ctx->pip_delay_observed.find(p.first);if(q==ctx->pip_delay_observed.end()||q->second!=p.second)return false;}
        for(size_t i=0;i<types.size();++i)if(types[i].table_ps!=ctx->pip_type_calibration[i].table_ps||
            types[i].analogue_ps!=ctx->pip_type_calibration[i].analogue_ps||types[i].hops!=ctx->pip_type_calibration[i].hops)return false;
        return true;
    }
};
inline void require(bool value,const char *why) {if(!value) log_error("Compound LUT branch proposal: %s.\n",why);}
inline void write(const std::string &p,const J &v) {std::ofstream f(p);f<<v.dump()<<'\n';require(bool(f),"cannot write diagnostic");}
inline J canonical(const J &value) {
    auto object=value.object_items();
    auto order=[](J::array rows){std::sort(rows.begin(),rows.end(),[](const J&a,const J&b){return a.dump()<b.dump();});return rows;};
    for(auto &v:object) if(v.second.is_array()) {
        auto rows=v.second.array_items();
        for(auto &row:rows) if(row.is_object()&&row["rows"].is_array()) {auto r=row.object_items();r["rows"]=order(row["rows"].array_items());row=r;}
        v.second=order(rows);
    }
    return object;
}
inline Routes routes(Context *ctx) {
    Routes result;
    for(const auto &n:ctx->nets) for(const auto &w:n.second->wires)
        result[n.first].push_back({w.first,w.second.pip,w.second.strength});
    for(const auto &n:ctx->nets) result.emplace(n.first,Tree{});
    return result;
}
inline bool tree_equal(NetInfo *net,const Tree &tree) {
    if(net->wires.size()!=tree.size()) return false;
    for(const auto &e:tree) {auto p=net->wires.find(e.wire);if(p==net->wires.end()||p->second.pip!=e.pip||p->second.strength!=e.strength)return false;}
    return true;
}
inline bool other_routes_equal(Context *ctx,const Routes &old,IdString allowed=IdString()) {
    if(ctx->nets.size()!=old.size())return false;
    for(const auto &n:old) if(n.first!=allowed && !tree_equal(ctx->nets.at(n.first).get(),n.second))return false;
    return true;
}
inline bool bind_tree(Context *ctx,NetInfo *net,const Tree &tree) {
    ctx->ripupNet(net->name);
    for(const auto &e:tree) if(e.pip==PipId()) {
        if(!ctx->checkWireAvail(e.wire))return false;
        ctx->bindWire(e.wire,net,e.strength);
    }
    for(const auto &e:tree) if(e.pip!=PipId()) {
        if(!ctx->checkWireAvail(e.wire)||!ctx->checkPipAvailForNet(e.pip,net))return false;
        ctx->bindPip(e.pip,net,e.strength);
    }
    return true;
}
inline J pins(Context *ctx) {
    J::object result;
    for(const auto &e:ctx->cells) {J::object p;for(const auto &pin:e.second->pin_data) {
        J::array b;for(auto v:pin.second.bel_pins)b.emplace_back(v.str(ctx));
        p[pin.first.str(ctx)]=J::array{int(pin.second.state),b};}
        result[e.first.str(ctx)]=p;}
    return result;
}
inline J labs(Context *ctx) {
    J::array result;
    for(const auto &lab:ctx->labs) {J::array v{int(lab.aclr_used[0]),int(lab.aclr_used[1])};
        for(const auto &a:lab.alms) for(int x:{int(a.l6_mode),int(a.carry_mode),a.clk_ena_idx[0],a.clk_ena_idx[1],a.aclr_idx[0],a.aclr_idx[1]})v.emplace_back(x);
        result.emplace_back(v);}
    return result;
}
inline std::unique_ptr<TimingAnalyser> analyse(Context *ctx) {
    ctx->configure_bitstream(false);
    auto t=std::make_unique<TimingAnalyser>(ctx);t->with_clock_skew=true;t->setup(false,true,true);
    require(t->observations_complete(),"incomplete native timing analysis");
    ctx->timing_result=t->get_timing_result();
    require(ctx->bitstream_configured,"missing actual configured bitstream");
    ctx->timing_result_is_final_analogue=true;
    return t;
}
inline CriticalCohortEndpointRows rows(Context *ctx,TimingAnalyser &timing) {
    CriticalCohortEndpointRows result;
    if(!timing.observations_complete())return result;
    bool unrelated=false;
    for(const auto &cell:ctx->cells) for(const auto &port:cell.second->ports) {
        int count=0;auto kind=ctx->getPortTimingClass(cell.second.get(),port.first,count);
        if(kind!=TMG_REGISTER_INPUT&&kind!=TMG_ENDPOINT)continue;
        std::vector<EndpointClockPairTiming> v;CellPortKey key(cell.first,port.first);
        if(timing.get_endpoint_clock_pair_timings(key,v)&&!v.empty()) {
            for(const auto &r:v)unrelated|=!r.setup_timed;
            result.emplace(key,std::move(v));}
    }
    if(unrelated) {
        TimingAnalyser reference(ctx);reference.with_clock_skew=false;reference.setup(false,false,true);
        if(!reference.observations_complete())return {};
        for(auto &e:result) {std::vector<EndpointClockPairTiming> v;
            if(!reference.get_endpoint_clock_pair_timings(e.first,v))return {};
            for(auto &r:e.second) if(!r.setup_timed) {
                auto p=std::find_if(v.begin(),v.end(),[&](const auto &s){return s.launch==r.launch&&s.capture==r.capture;});
                if(p==v.end())return {};
                r.max_path_delay=p->max_path_delay;r.min_path_delay=p->min_path_delay;}}
    }
    return result;
}
inline J metadata(const J &value) {auto v=canonical(value).object_items();v.erase("endpoints");return v;}
inline bool inventory_safe(const J &before,const J &after) {
    if(metadata(before)!=metadata(after))return false;
    using Key=std::pair<std::string,std::string>;std::map<Key,J> old,now;
    auto collect=[](const J &d,auto &out){
        for(const auto &e:d["endpoints"].array_items())
            if(!out.emplace(Key{e["cell"].string_value(),e["port"].string_value()},e).second)return false;
        return true;
    };
    if(!collect(before,old)||!collect(after,now)||old.size()!=now.size())return false;
    for(const auto &e:old) {
        auto p=now.find(e.first);if(p==now.end())return false;
        auto a=e.second.object_items(),b=p->second.object_items();a.erase("rows");b.erase("rows");if(a!=b)return false;
        std::map<std::string,J>x,y;
        for(const auto &r:e.second["rows"].array_items()) if(!x.emplace(J(J::array{r["launch"],r["capture"]}).dump(),r).second)return false;
        for(const auto &r:p->second["rows"].array_items()) if(!y.emplace(J(J::array{r["launch"],r["capture"]}).dump(),r).second)return false;
        if(x.size()!=y.size())return false;
        for(const auto &r:x) {auto q=y.find(r.first);if(q==y.end())return false;const auto &a=r.second,&b=q->second;
            for(auto k:{"setup_timed","hold_related","setup_window_ps"})if(a[k]!=b[k])return false;
            if(a["setup_timed"].bool_value()) {if(!a["setup_margin_ps"].is_number()||!b["setup_margin_ps"].is_number()||b["setup_margin_ps"].number_value()<a["setup_margin_ps"].number_value())return false;}
            else if(b["max_path_ps"].number_value()>a["max_path_ps"].number_value()||b["min_path_ps"].number_value()<a["min_path_ps"].number_value())return false;
            if(a["hold_related"].bool_value()) {if(!a["hold_margin_ps"].is_number()||!b["hold_margin_ps"].is_number())return false;
                auto v=a["hold_margin_ps"].number_value(),w=b["hold_margin_ps"].number_value();if(v>0?w<=0:w<v)return false;}
        }
    }
    return true;
}
inline std::vector<uint8_t> rbf(Context *ctx,const std::string &path) {
    std::vector<uint8_t> bytes;ctx->cyclonev->rbf_save(bytes);std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());require(bool(f),"cannot write RBF");return bytes;
}
inline void save(Context *ctx,const std::string &path) {
    std::filesystem::create_directories(path+"/native");
    ctx->save_fes_pin_maps(true);ctx->archInfoToAttributes();
    {auto name=path+"/routed.json";std::ofstream f(name);require(write_json_file(f,name,ctx),"cannot save routed graph");}
    {std::ofstream f(path+"/timing.json");ctx->writeJsonReport(f);require(bool(f),"cannot write timing report");}
    write(path+"/native/endpoints.json",routed_placement_endpoints(ctx));
    write(path+"/native/routed-identity.json",routed_placement_snapshot(ctx));
    bool old=ctx->routed_placement_record_calibration;ctx->routed_placement_record_calibration=true;
    try {std::ofstream f(path+"/native/model.json");write_critical_cohort_route_model(ctx,f);}
    catch(...){ctx->routed_placement_record_calibration=old;throw;}
    ctx->routed_placement_record_calibration=old;rbf(ctx,path+"/core.rbf");
}
inline J tree_json(Context *ctx,const Tree &tree) {J::array out;for(const auto &e:tree)out.emplace_back(J::array{ctx->getWireName(e.wire).str(ctx),e.pip==PipId()?"":ctx->getPipName(e.pip).str(ctx),int(e.strength)});return out;}
inline bool supported(Context *ctx,NetInfo *net) {
    if(!analogue_net_is_movable(net)||net->driver.cell->type==id_MISTRAL_M10K)return false;
    for(const auto &u:net->users) {auto p=ctx->analogue_arc_cache.find(&u);int c=0;
        if(p==ctx->analogue_arc_cache.end()||!p->second.ok||ctx->getPortTimingClass(u.cell,u.port,c)==TMG_CLOCK_INPUT)return false;}
    return true;
}
inline std::pair<int64_t,int64_t> score(const J &inventory) {
    int64_t worst=std::numeric_limits<int64_t>::max(),negative=0;
    for(const auto &e:inventory["endpoints"].array_items())for(const auto &r:e["rows"].array_items())if(r["setup_timed"].bool_value()) {
        auto value=int64_t(r["setup_margin_ps"].number_value());worst=std::min(worst,value);negative+=std::min(int64_t(0),value);}
    require(worst!=std::numeric_limits<int64_t>::max(),"no setup rows to score");return {worst,negative};
}
inline std::optional<std::vector<Edge>> lut_input_prefix(Context *ctx, NetInfo *net, PortRef user)
{
    std::vector<WireId> targets;
    for (auto wire : ctx->getNetinfoSinkWires(net, user)) targets.push_back(wire);
    if (targets.size() != 1) return {};
    const auto target = targets.front(), source = ctx->getNetinfoSourceWire(net);
    std::map<WireId, Edge> tree;
    std::map<WireId, int> children;
    for (const auto &binding : net->wires) {
        tree.emplace(binding.first, Edge{binding.first, binding.second.pip, binding.second.strength});
        if (binding.second.pip != PipId()) ++children[ctx->getPipSrcWire(binding.second.pip)];
    }
    std::set<WireId> other_sinks;
    for (const auto &other : net->users) {
        if (other.cell == user.cell && other.port == user.port) continue;
        for (auto wire : ctx->getNetinfoSinkWires(net, other)) other_sinks.insert(wire);
    }
    if (!tree.count(target) || !tree.count(source) || target == source || children[target] || other_sinks.count(target))
        return {};
    WireId cursor = target;
    bool removed = false;
    for (size_t steps = 0; steps < net->wires.size(); ++steps) {
        if (cursor == source || children[cursor] || other_sinks.count(cursor)) break;
        auto found = tree.find(cursor);
        if (found == tree.end() || found->second.pip == PipId()) return {};
        WireId parent = ctx->getPipSrcWire(found->second.pip);
        if (!tree.count(parent)) return {};
        tree.erase(found);
        --children[parent];
        cursor = parent;
        removed = true;
    }
    if (!removed || tree.count(target) || !tree.count(source)) return {};
    for (auto sink : other_sinks) if (!tree.count(sink)) return {};
    std::vector<Edge> result;
    for (const auto &entry : tree) result.push_back(entry.second);
    return result;
}

struct LutOutputForest
{
    std::set<WireId> roots;
    std::map<WireId, PipId> edges;
};

// Rebuild routing from the actual moved source to the original consumers.
// Retain any existing branches below an internal consumer; never retain the
// old driver. The native search validates the complete resulting tree.
inline std::optional<LutOutputForest> lut_output_forest(Context *ctx, NetInfo *net)
{
    const auto source = ctx->getNetinfoSourceWire(net);
    std::map<WireId, std::vector<WireId>> children;
    std::set<WireId> sinks;
    for (const auto &user : net->users)
        for (auto wire : ctx->getNetinfoSinkWires(net, user)) sinks.insert(wire);
    if (!net->wires.count(source) || sinks.empty() || sinks.count(source)) return {};
    for (const auto &wire : net->wires)
        if (wire.second.pip != PipId())
            children[ctx->getPipSrcWire(wire.second.pip)].push_back(wire.first);
    LutOutputForest result;
    std::vector<WireId> pending{source};
    std::set<WireId> seen;
    for (size_t i = 0; i < pending.size(); ++i) {
        auto wire = pending[i];
        if (!seen.insert(wire).second) return {};
        if (wire != source && sinks.count(wire)) {
            result.roots.insert(wire);
            continue;
        }
        for (auto child : children[wire]) pending.push_back(child);
    }
    if (result.roots.empty()) return {};
    for (const auto &wire : net->wires) {
        if (result.roots.count(wire.first)) continue;
        auto cursor = wire.first;
        std::set<WireId> path;
        while (cursor != source && !result.roots.count(cursor)) {
            auto parent = net->wires.find(cursor);
            if (parent == net->wires.end() || parent->second.pip == PipId() || !path.insert(cursor).second)
                return {};
            cursor = ctx->getPipSrcWire(parent->second.pip);
        }
        if (result.roots.count(cursor)) result.edges.emplace(wire.first, wire.second.pip);
    }
    for (auto sink : sinks) if (!result.roots.count(sink) && !result.edges.count(sink)) return {};
    return result;
}



// This function runs only on a fresh full route. Its input is an exact native
// parent contract, never an imported planning checkpoint. An intermediate LUT
// move is not a keeper: the full proposal is compared with the original rows.
// reassign_alm_inputs inserts BUF.Q -> FF.DATAIN after placement. The
// FF cache can still name BUF.A's net, double-counting the routed input.
// Refresh only that exact architecture-owned route-through relationship.
inline J refresh_route_through_datain(Context *ctx) {
    J::array corrected;
    for(const auto &entry:ctx->cells){
        auto *ff=entry.second.get();if(ff->type!=id_MISTRAL_FF||ff->bel==BelId())continue;
        auto *actual=ff->getPort(id_DATAIN);if(ff->ffInfo.datain==actual)continue;
        require(actual&&actual->driver.cell&&critical_cohort_route_through_ff(ctx,actual->driver.cell)==ff,
                "stale FF input is not a declared native route-through");
        auto *buffer=actual->driver.cell;
        require(buffer->getPort(id_A)==ff->ffInfo.datain,"stale FF cache does not match route-through input");
        const auto old=ff->ffInfo;ctx->assign_ff_info(ff);
        require(ff->ffInfo.ctrlset==old.ctrlset&&ff->ffInfo.sdata==old.sdata&&ff->ffInfo.datain==actual,
                "FF cache refresh changed a control or synchronous-data contract");
        ctx->update_bel(ff->bel);
        corrected.push_back(J::object{{"cell",ff->name.str(ctx)},{"route_through",buffer->name.str(ctx)},
            {"old_cached_net",buffer->getPort(id_A)->name.str(ctx)},{"actual_datain_net",actual->name.str(ctx)}});
    }
    return corrected;
}
inline void validate_plan(const J &plan) {
    require(plan.is_object()&&plan.object_items().size()==5&&plan["version"]==J(1)&&
            plan["parent"].is_object()&&plan["route_state"].is_object()&&plan["move"].is_object()&&
            plan["branch_trials"].is_number()&&plan["branch_trials"].number_value()==plan["branch_trials"].int_value()&&
            plan["branch_trials"].int_value()>=0&&plan["branch_trials"].int_value()<=16,"invalid bounded plan");
    const auto move=plan["move"];
    require(move.object_items().size()==4&&move["cell"].is_string()&&move["from_bel"].is_string()&&
            move["to_bel"].is_string()&&move["strength"].is_number(),"invalid direct move identity");
}
template <typename Replay>
inline bool replay_best_candidate_if_unexpired(bool has_candidate, const std::function<bool()> &expired, Replay replay) {
    if (!has_candidate || expired())
        return false;
    replay();
    return true;
}
inline J run(Context *ctx,const J &plan,const std::function<bool()> &expired) {
    validate_plan(plan);const auto move=plan["move"];
    const auto original_native=routed_placement_snapshot(ctx);
    require(original_native==plan["parent"],"plan does not match the complete fresh native parent");
    const auto original_costs=CriticalCohortRouteCosts::snapshot(ctx,original_native);
    require(original_costs==plan["route_state"],"plan does not match fresh routes, reservations and calibration");
    const auto found=ctx->cells.find(ctx->id(move["cell"].string_value()));
    require(found!=ctx->cells.end(),"missing declared LUT");
    auto *cell=found->second.get();const auto home=cell->bel;const auto strength=cell->belStrength;
    const auto target=ctx->getBelByName(IdStringList::parse(ctx,move["to_bel"].string_value()));
    require(move["from_bel"]==J(ctx->getBelName(home).str(ctx))&&move["strength"]==J(int(strength)),"source binding differs");
    require(target!=BelId()&&cell->type.in(id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,id_MISTRAL_ALUT4,id_MISTRAL_ALUT5),
            "invalid ordinary LUT or target identity");
    const auto here=ctx->getBelLocation(home),there=ctx->getBelLocation(target);
    require(here.x==there.x&&here.y==there.y,"proposal requires distinct ALMs in the same native tile");
    const std::string root=ctx->routed_placement_directory+"/lut-branch";
    std::filesystem::create_directories(root+"/before/native");std::filesystem::create_directories(root+"/after/native");
    auto timing=analyse(ctx);const auto original_rows=rows(ctx,*timing);
    const AnalogueClockTiming original_clock(timing->get_timing_result());
    const auto original_inventory=routed_placement_endpoints(ctx),original_pins=pins(ctx),original_controls=labs(ctx);
    const auto original_routes=routes(ctx);const auto source_pins=cell->pin_data;
    Calibration calibration(ctx);const int initial_errors=message_count_by_level[LogLevel::ERROR_MSG];
    save(ctx,root+"/before");const auto original_rbf=rbf(ctx,root+"/before/core.rbf");
    require(routed_placement_snapshot(ctx)==original_native,"baseline capture changed native execution identity");
    auto legality_inventory=[&](){
        J::array invalid;
        for(const auto &p:ctx->cells){
            auto *candidate=p.second.get();const auto bel=candidate->bel;
            if(bel==BelId()||ctx->isBelLocationValid(bel))continue;
            J::object item{{"cell",candidate->name.str(ctx)},{"type",candidate->type.str(ctx)},
                {"bel",ctx->getBelName(bel).str(ctx)}};
            const auto &data=ctx->bel_data(bel);
            if(data.type.in(id_MISTRAL_COMB,id_MISTRAL_MCOMB,id_MISTRAL_FF)){
                const auto lab=data.lab_data.lab;const auto alm=data.lab_data.alm;
                item["alm_legal"]=ctx->is_alm_legal(lab,alm);
                item["lab_inputs_legal"]=ctx->check_lab_input_count(lab);
                item["lab_controls_legal"]=ctx->is_lab_ctrlset_legal(lab);
                item["mlab_groups_legal"]=ctx->check_mlab_groups(lab);
                J::array counts;for(const auto &a:ctx->labs.at(lab).alms)counts.push_back(a.unique_input_count);
                item["lab_input_counts"]=counts;
            }
            invalid.push_back(item);
        }
        return J(invalid);
    };
    const bool eligible_before_refresh=analogue_lut_move_allowed(ctx,cell,target);
    write(root+"/original-legality-before-derived-refresh.json",legality_inventory());
    const auto corrections=refresh_route_through_datain(ctx);write(root+"/derived-cache-refresh.json",corrections);
    auto refreshed=analyse(ctx);
    require(routed_placement_snapshot(ctx)==original_native&&pins(ctx)==original_pins&&labs(ctx)==original_controls&&
            other_routes_equal(ctx,original_routes),"derived-cache refresh changed native graph, pins, controls or routes");
    require(canonical(routed_placement_endpoints(ctx))==canonical(original_inventory)&&
            rbf(ctx,root+"/cache-refresh.rbf")==original_rbf,"derived-cache refresh changed bitstream or complete timing inventory");
    require(CriticalCohortRouteCosts::snapshot(ctx,original_native)==original_costs&&calibration.matches(ctx),
            "derived-cache refresh changed native cost or calibration state");
    save(ctx,root+"/cache-refreshed");
    const auto original_legality=legality_inventory();write(root+"/original-legality.json",original_legality);
    require(original_legality.array_items().empty(),"fresh parent still fails native placement legality after cache refresh; see original-legality.json");
    const bool eligible_after_refresh=analogue_lut_move_allowed(ctx,cell,target);
    write(root+"/eligibility-cache-order.json",J::object{{"before_refresh",eligible_before_refresh},{"after_refresh",eligible_after_refresh}});
    require(eligible_after_refresh,"native ordinary LUT eligibility rejected target after exact cache refresh");
    const auto original_labs=ctx->labs;
    auto restore_original=[&](){
        if(cell->bel!=home){if(cell->bel!=BelId())ctx->unbindBel(cell->bel);ctx->bindBel(home,cell,strength);}
        ctx->labs=original_labs;cell->pin_data=source_pins;
        for(const auto &net:original_routes)ctx->ripupNet(net.first);
        for(const auto &net:original_routes)require(bind_tree(ctx,ctx->nets.at(net.first).get(),net.second),"original route restore failed");
        ctx->bitstream_configured=false;ctx->analogue_cache_valid=false;
    };
    bool accepted=false,seed_built=false;int seed_nodes=0,best_trial=0;J::array trials,choice_records;J seed_inventory;
    auto best_score=score(original_inventory);NetInfo *best_net=nullptr;Tree best_tree;std::vector<uint8_t> best_rbf;
    try {
        if(!expired()){
            auto *output=cell->getPort(id_Q);require(supported(ctx,output),"unsupported moved output");
            auto forest=lut_output_forest(ctx,output);require(bool(forest),"incomplete native output forest");
            std::map<IdString,Tree> prefixes;std::set<IdString> changed{output->name};
            for(const auto &p:cell->ports)if(p.second.type==PORT_IN){
                auto *net=p.second.net;require(net&&supported(ctx,net)&&!changed.count(net->name),"unsupported or repeated moved input");
                auto prefix=lut_input_prefix(ctx,net,{cell,p.first});require(bool(prefix),"moved input lacks exclusive prefix");
                prefixes.emplace(net->name,*prefix);changed.insert(net->name);
            }
            CriticalCohortRouteCosts costs(ctx,original_costs,original_native);
            require(apply_critical_cohort_placement(ctx,{{cell,target}}),"native placement transaction rejected move");
            const auto from=ctx->bel_data(home).lab_data,to=ctx->bel_data(target).lab_data;
            ctx->assign_alm_lut_inputs(from.lab,from.alm);ctx->assign_alm_lut_inputs(to.lab,to.alm);cell->pin_data=source_pins;
            require(pins(ctx)==original_pins&&labs(ctx)==original_controls,"move changed pins or LAB control state");
            const auto moved_legality=legality_inventory();write(root+"/moved-legality.json",moved_legality);
            require(moved_legality.array_items().empty(),"native placement became illegal; see moved-legality.json");
            for(auto name:changed)ctx->ripupNet(name);
            for(const auto &p:prefixes)require(bind_tree(ctx,ctx->nets.at(p.first).get(),p.second),"input prefix binding failed");
            auto allowed=analogue_parent_cell_pips(ctx);
            // Capture the parent's selectors, including the temporarily removed
            // moved trees. Only the declared LUT's new half may add selectors.
            for(const auto &n:original_routes)for(const auto &e:n.second)if(e.pip!=PipId()&&
                (e.wire.is_nextpnr_created()||ctx->getPipSrcWire(e.pip).is_nextpnr_created()))allowed.insert(e.pip);
            const auto source=ctx->getNetinfoSourceWire(output);
            require(source.is_nextpnr_created(),"moved LUT source lacks native virtual identity");
            for(auto pip:ctx->getPipsDownhill(source))allowed.insert(pip);
            for(const auto &p:cell->ports)if(p.second.type==PORT_IN)for(auto w:ctx->getNetinfoSinkWires(p.second.net,{cell,p.first}))
                if(w.is_nextpnr_created())for(auto pip:ctx->getPipsUphill(w))allowed.insert(pip);
            std::map<WireId,IdString> drivers;
            for(const auto &n:ctx->nets)if(n.second->driver.cell){auto w=ctx->getNetinfoSourceWire(n.second.get());if(w!=WireId())drivers.emplace(w,n.first);}
            bool complete=true;int remaining=CriticalCohortRouteCosts::node_limit;
            {AnalogueCellPipScope scope(ctx,allowed);
            std::map<WireId,PipId> path=costs.reconnect_source(ctx,output,source,forest->roots,forest->edges,changed,drivers,remaining);
            complete=!path.empty();Tree tree;
            for(const auto &p:path){auto old=std::find_if(original_routes.at(output->name).begin(),original_routes.at(output->name).end(),
                [&](const Edge&e){return e.wire==p.first&&e.pip==p.second;});
                tree.push_back({p.first,p.second,old==original_routes.at(output->name).end()?STRENGTH_STRONG:old->strength});}
            if(complete)complete=bind_tree(ctx,output,tree);
            for(const auto &p:prefixes){
                if(!complete||expired()){complete=false;break;}
                auto *net=ctx->nets.at(p.first).get();std::set<WireId> targets;
                for(const auto &q:cell->ports)if(q.second.type==PORT_IN&&q.second.net==net)
                    for(auto w:ctx->getNetinfoSinkWires(net,{cell,q.first}))targets.insert(w);
                std::map<WireId,PipId> extension;auto prices=costs.estimate(ctx,net,ctx->getNetinfoSourceWire(net),targets,changed,drivers,remaining,nullptr,&extension);
                if(prices.size()!=targets.size()){complete=false;break;}
                std::map<WireId,Edge> joined;for(const auto &e:p.second)joined.emplace(e.wire,e);
                for(const auto &e:extension){auto old=joined.find(e.first);require(old==joined.end()||old->second.pip==e.second,"input extension changed retained parent");
                    if(old==joined.end())joined.emplace(e.first,Edge{e.first,e.second,STRENGTH_STRONG});}
                Tree input;for(const auto &e:joined)input.push_back(e.second);complete=bind_tree(ctx,net,input);
            }}
            seed_nodes=CriticalCohortRouteCosts::node_limit-remaining;
            if(complete){
                ctx->check();auto seed_timing=analyse(ctx);seed_inventory=routed_placement_endpoints(ctx);
                auto expected=original_native.object_items();auto placement=expected["placement"].object_items();
                placement[cell->name.str(ctx)]=J::array{move["to_bel"],int(strength)};expected["placement"]=placement;
                require(J(expected)==routed_placement_snapshot(ctx),"seed changed undeclared native identity");
                require(pins(ctx)==original_pins&&labs(ctx)==original_controls,"seed changed pin/LAB controls");
                for(const auto &n:original_routes)if(!changed.count(n.first))require(tree_equal(ctx->nets.at(n.first).get(),n.second),"seed changed unrelated tree");
                for(const auto &p:prefixes)for(const auto &e:p.second){auto &net=*ctx->nets.at(p.first);auto q=net.wires.find(e.wire);
                    require(q!=net.wires.end()&&q->second.pip==e.pip&&q->second.strength==e.strength,"seed lost protected input prefix");}
                seed_built=true;save(ctx,root+"/seed");const auto seed_rbf=rbf(ctx,root+"/seed/core.rbf");
                const auto seed_routes=routes(ctx);const auto seed_native=routed_placement_snapshot(ctx);
                const auto state=CriticalCohortRouteCosts::snapshot(ctx,seed_native);
                for(const char *key:{"calibrated","prior","observed","types","reservations"})require(state[key]==original_costs[key],"seed changed immutable cost history");
                CriticalCohortRouteCosts branch_costs(ctx,state,seed_native);
                struct Choice{NetInfo *net;WireId target;std::set<WireId>sinks;PipId avoid;};std::vector<Choice> choices;
                std::set<std::tuple<IdString,WireId,PipId>> seen;
                std::vector<CriticalPath> paths;
                std::map<std::string,CriticalPath> ordered;
                for(const auto &p:seed_timing->get_timing_result().clock_paths)ordered.emplace(p.first.str(ctx),p.second);
                for(const auto &p:ordered)paths.push_back(p.second);
                for(const auto &p:seed_timing->get_timing_result().xclock_paths)paths.push_back(p);
                for(const auto &p:paths)for(const auto &s:p.segments){
                    if(s.type!=CriticalPath::Segment::Type::ROUTING)continue;
                    auto found=ctx->cells.find(s.to.first);if(found==ctx->cells.end())continue;auto *sink=found->second.get();
                    if(!sink->type.in(id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,id_MISTRAL_ALUT4,id_MISTRAL_ALUT5,id_MISTRAL_ALUT6))continue;
                    auto *net=sink->getPort(s.to.second);
                    if(!net||!supported(ctx,net)||net->clkconstr||net->region||net->constant_value!=IdString()||
                       net->attrs.count(ctx->id("keep"))||net->attrs.count(ctx->id("dont_touch"))||
                       sink->ports.at(s.to.second).type!=PORT_IN||net->driver.cell->name!=s.from.first||net->driver.port!=s.from.second)continue;
                    std::set<WireId>sinks;for(const auto &u:net->users)for(auto w:ctx->getNetinfoSinkWires(net,u))sinks.insert(w);
                    const auto wires=ctx->getNetinfoSinkWires(net,{sink,s.to.second});if(wires.size()!=1)continue;
                    for(auto avoid:branch_costs.branch_alternatives(ctx,net->name,wires[0],sinks))
                        if(seen.emplace(net->name,wires[0],avoid).second)choices.push_back({net,wires[0],sinks,avoid});
                }
                for(size_t i=0;i<choices.size();++i){const auto &q=choices[i];choice_records.emplace_back(J::object{{"choice_index",int(i+1)},
                    {"net",q.net->name.str(ctx)},{"target",ctx->getWireName(q.target).str(ctx)},{"avoid",ctx->getPipName(q.avoid).str(ctx)}});}
                const auto branch_allowed=analogue_parent_cell_pips(ctx);
                {AnalogueCellPipScope scope(ctx,branch_allowed);
                for(size_t i=0;i<choices.size()&&int(i)<plan["branch_trials"].int_value()&&!expired();++i){
                    const auto &q=choices[i];const std::string dir=root+"/trials/"+std::to_string(i+1);
                    std::filesystem::create_directories(dir+"/native");J::object info=choice_records[i].object_items();
                    const auto old=seed_routes.at(q.net->name);int budget=CriticalCohortRouteCosts::node_limit;J::object trace;std::map<WireId,PipId> proposed;
                    auto prices=branch_costs.repair_branch(ctx,q.net,ctx->getNetinfoSourceWire(q.net),q.sinks,q.target,q.avoid,{q.net->name},{},budget,&trace,&proposed);
                    info["cost_nodes"]=CriticalCohortRouteCosts::node_limit-budget;info["complete_prices"]=prices.size()==q.sinks.size();
                    write(dir+"/search.json",trace);
                    if(prices.size()!=q.sinks.size()){info["decision"]="incomplete bounded native search";write(dir+"/result.json",info);trials.emplace_back(info);continue;}
                    // Compute the exclusive terminal prefix from architecture wires.
                    std::map<WireId,Edge> parent;std::map<WireId,int> children;
                    for(const auto &e:old){parent.emplace(e.wire,e);if(e.pip!=PipId())++children[ctx->getPipSrcWire(e.pip)];}
                    auto kept=parent;auto cursor=q.target;const auto source=ctx->getNetinfoSourceWire(q.net);
                    require(children[cursor]==0&&cursor!=source,"branch terminal is not exclusive");
                    while(cursor!=source){auto pip=kept.at(cursor).pip;require(pip!=PipId(),"branch source missing");auto up=ctx->getPipSrcWire(pip);
                        kept.erase(cursor);--children[up];if(up==source||children[up]||q.sinks.count(up))break;cursor=up;}
                    bool exact=true;for(const auto &e:kept)exact&=proposed.count(e.first)&&proposed.at(e.first)==e.second.pip;
                    Tree candidate;for(const auto &e:proposed)candidate.push_back({e.first,e.second,parent.count(e.first)?parent.at(e.first).strength:parent.at(q.target).strength});
                    std::vector<std::pair<WireId,PipId>> x,y;for(const auto&e:old)x.emplace_back(e.wire,e.pip);for(const auto&e:candidate)y.emplace_back(e.wire,e.pip);
                    exact&=analogue_cell_connections(ctx,x)==analogue_cell_connections(ctx,y);
                    info["parent_prefix_and_cell_connections_exact"]=exact;write(dir+"/proposed-tree.json",tree_json(ctx,candidate));
                    if(!exact){info["decision"]="protected parent prefix or selectors differ";write(dir+"/result.json",info);trials.emplace_back(info);continue;}
                    const bool bound=bind_tree(ctx,q.net,candidate);bool safe=false;J inventory;std::vector<uint8_t> candidate_rbf;
                    if(bound){ctx->check();auto after=analyse(ctx);inventory=routed_placement_endpoints(ctx);
                        require(routed_placement_snapshot(ctx)==seed_native&&pins(ctx)==original_pins&&labs(ctx)==original_controls&&
                            other_routes_equal(ctx,seed_routes,q.net->name),"branch changed protected native state");
                        safe=critical_cohort_proposal_rows_safe(original_rows,rows(ctx,*after))&&
                            !AnalogueClockTiming(after->get_timing_result()).worse_than(original_clock)&&inventory_safe(original_inventory,inventory);
                        write(dir+"/native/endpoints.json",inventory);write(dir+"/native/routed-identity.json",routed_placement_snapshot(ctx));
                        candidate_rbf=rbf(ctx,dir+"/core.rbf");
                    }
                    info["bound"]=bound;info["original_native_allrow_clock_hold_20ps_guard"]=safe;
                    if(safe&&score(inventory)>best_score){best_score=score(inventory);best_net=q.net;best_tree=candidate;best_rbf=candidate_rbf;best_trial=int(i+1);}
                    info["decision"]=safe?"global original guard passed":"rejected";write(dir+"/result.json",info);trials.emplace_back(info);
                    require(bind_tree(ctx,q.net,old),"seed branch restore failed");auto restored=analyse(ctx);
                    require(routed_placement_snapshot(ctx)==seed_native&&pins(ctx)==original_pins&&labs(ctx)==original_controls&&other_routes_equal(ctx,seed_routes),"branch rollback changed native state");
                    require(canonical(routed_placement_endpoints(ctx))==canonical(seed_inventory)&&rbf(ctx,dir+"/rollback.rbf")==seed_rbf,"branch rollback changed seed bitstream or rows");
                }
                accepted=replay_best_candidate_if_unexpired(best_net!=nullptr,expired,[&]{
                    require(bind_tree(ctx,best_net,best_tree),"selected branch replay failed");auto selected=analyse(ctx);
                    require(critical_cohort_proposal_rows_safe(original_rows,rows(ctx,*selected))&&
                        !AnalogueClockTiming(selected->get_timing_result()).worse_than(original_clock)&&inventory_safe(original_inventory,routed_placement_endpoints(ctx)),"selected candidate failed original final guard");
                    require(routed_placement_snapshot(ctx)==seed_native&&pins(ctx)==original_pins&&labs(ctx)==original_controls&&other_routes_equal(ctx,seed_routes,best_net->name),"selected proposal changed protected native state");
                    require(rbf(ctx,root+"/selected-replay.rbf")==best_rbf,"selected bitstream did not replay exactly");
                });
            }
            }
        }
        if(!accepted){restore_original();auto restored=analyse(ctx);
            require(routed_placement_snapshot(ctx)==original_native&&pins(ctx)==original_pins&&labs(ctx)==original_controls&&other_routes_equal(ctx,original_routes),"original native rollback mismatch");
            require(canonical(routed_placement_endpoints(ctx))==canonical(original_inventory)&&rbf(ctx,root+"/rollback.rbf")==original_rbf,"original bitstream/timing rollback mismatch");}
        require(calibration.matches(ctx)&&message_count_by_level[LogLevel::ERROR_MSG]==initial_errors,"proposal changed calibration or emitted errors");
        const auto final_state=CriticalCohortRouteCosts::snapshot(ctx,routed_placement_snapshot(ctx));
        for(const char *key:{"calibrated","prior","observed","types","reservations"})require(final_state[key]==original_costs[key],"proposal changed immutable cost history");
        auto final_timing=analyse(ctx);save(ctx,root+"/after");const auto inventory=routed_placement_endpoints(ctx);
        J receipt=J::object{{"version",1},{"parent",original_native},{"target",routed_placement_snapshot(ctx)},
            {"accepted",accepted},{"seed_built",seed_built},{"seed_cost_nodes",seed_nodes},{"branch_budget",plan["branch_trials"]},
            {"selected_trial",best_trial},{"choices",choice_records},{"trials",trials},
            {"before_wns_ps",double(score(original_inventory).first)},{"after_wns_ps",double(score(inventory).first)},
            {"before_tns_ps",double(score(original_inventory).second)},{"after_tns_ps",double(score(inventory).second)},
            {"original_native_allrow_clock_hold_20ps_guard",accepted},{"moves",accepted?J(J::array{move}):J(J::array{})},
            {"original_native_parent_and_cost_state_exact",true},{"intermediate_seed_retained",false},
            {"derived_ff_datain_caches_refreshed",int(corrections.array_items().size())},{"cache_refresh_bitstream_and_allrows_exact",true}};
        if(accepted){const auto native_receipt=J::object{{"version",1},{"parent",original_native},{"target",routed_placement_snapshot(ctx)},{"moves",J::array{move}}};
            check_post_route_lut_moves(native_receipt,routed_placement_snapshot(ctx));write(root+"/after/native/post-route-lut-moves.json",native_receipt);}
        write(root+"/result.json",receipt);
        log_info("Compound LUT/branch proposal: %zu branch trials, selected %d, WNS %.0f -> %.0f ps, %s.\n",trials.size(),best_trial,
            receipt["before_wns_ps"].number_value(),receipt["after_wns_ps"].number_value(),accepted?"original guard passed":"original restored");
        return receipt;
    } catch(...){restore_original();throw;}
}
}
NEXTPNR_NAMESPACE_END
#endif
