/* Bounded report-guided control decomposition. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "log.h"
#include "json11.hpp"
#include "timing.h"
#include "decomposition_policy.h"
#include "enable_replication_policy.h"
#include "local_remap_pin_policy.h"
#include "remap_report.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
namespace {
const std::array<IdString,6> dpins = {id_A,id_B,id_C,id_D,id_E,id_F};
using Lab = std::pair<int,int>;
int dwidth(IdString type)
{
    if (type==id_MISTRAL_ALUT2) return 2;
    if (type==id_MISTRAL_ALUT3) return 3;
    if (type==id_MISTRAL_ALUT4) return 4;
    if (type==id_MISTRAL_ALUT5) return 5;
    if (type==id_MISTRAL_ALUT6) return 6;
    return 0;
}
IdString dtype(size_t width)
{
    const IdString types[] = {id_MISTRAL_ALUT2,id_MISTRAL_ALUT3,id_MISTRAL_ALUT4,id_MISTRAL_ALUT5,id_MISTRAL_ALUT6};
    NPNR_ASSERT(width>=2 && width<=6);
    return types[width-2];
}
bool dattr(const dict<IdString,Property> &attrs, Context *ctx)
{
    for (const auto &entry : attrs) {
        auto name=entry.first.str(ctx);
        if (name=="keep" || name=="dont_touch") return true;
    }
    return false;
}
bool dtimed(float slack)
{
    return std::isfinite(slack) && slack<float(std::numeric_limits<delay_t>::max());
}
std::map<std::string,int> dholds(TimingAnalyser &timing)
{
    std::map<std::string,int> result;
    for (const auto &path : timing.get_timing_result().min_delay_violations) {
        if (path.segments.empty()) continue;
        int slack=0;
        for (const auto &segment : path.segments) slack+=segment.delay;
        auto end=path.segments.back().to;
        auto key=std::to_string(path.clock_pair.start.clock.index)+":"+
            std::to_string(int(path.clock_pair.start.edge))+":"+std::to_string(path.clock_pair.end.clock.index)+":"+
            std::to_string(int(path.clock_pair.end.edge))+":"+std::to_string(end.first.index)+":"+std::to_string(end.second.index);
        if (!result.count(key)) result[key]=slack;
        else result[key]=std::min(result[key],slack);
    }
    return result;
}
}

bool Arch::remap_decomposed_critical(const std::string &report, int selection)
{
    namespace policy=decomposition_policy;
    Context *ctx=getCtx();
    if (selection < -1) log_error("Invalid control decomposition selection.\n");
    if (fes_any_slot_region_active) log_error("Control decomposition requires ordinary placement.\n");
    for (const auto &entry : nets)
        if (!entry.second->wires.empty()) log_error("Control decomposition requires an unrouted design.\n");
    std::string error;
    auto json=json11::Json::parse(report,error);
    if (!error.empty() || !json["critical_paths"].is_array()) log_error("Invalid control decomposition report.\n");
    auto lab=[&](BelId bel) { auto at=getBelLocation(bel); return Lab(at.x,at.y); };
    auto movable=[&](const CellInfo *cell) {
        return cell && cell->bel!=BelId() && cell->belStrength<=STRENGTH_WEAK &&
            cell->cluster==ClusterId() && !cell->region && !cell->isPseudo() && !dattr(cell->attrs,ctx);
    };
    std::set<Lab> protected_labs;
    std::map<CellInfo *,std::pair<BelId,PlaceStrength>> original_places;
    for (const auto &entry : cells) {
        auto *cell=entry.second.get();
        original_places[cell]={cell->bel,cell->belStrength};
        if (cell->bel!=BelId() && (!movable(cell) || cell->type==id_MISTRAL_MLAB))
            protected_labs.insert(lab(cell->bel));
    }
    auto safe=[&](const CellInfo *cell) { return movable(cell) && !protected_labs.count(lab(cell->bel)); };
    auto table_valid=[&](const CellInfo *cell) {
        int width=dwidth(cell->type);
        if (!width || cell->get_pin_state(id_Q)!=PIN_SIG || cell->params.size()!=1 || !cell->params.count(id_LUT)) return false;
        auto output=cell->getPort(id_Q);
        if (!output || !cell->ports.count(id_Q) || cell->ports.at(id_Q).type!=PORT_OUT ||
            output->driver.cell!=cell || output->driver.port!=id_Q) return false;
        const auto &table=cell->params.at(id_LUT);
        return table.is_fully_def() && table.size()==(1u<<width);
    };
    std::set<NetInfo *> boundary;
    for (const auto &entry : ctx->ports) if (entry.second.net) boundary.insert(entry.second.net);
    auto ordinary=[&](NetInfo *net) {
        if (!net || !net->driver.cell || net->is_global || net->clkconstr || net->region || !net->wires.empty() ||
            net->constant_value!=IdString() || boundary.count(net) || dattr(net->attrs,ctx)) return false;
        auto driver=net->driver.cell;
        if (!driver->ports.count(net->driver.port) || driver->ports.at(net->driver.port).type!=PORT_OUT ||
            driver->getPort(net->driver.port)!=net) return false;
        for (auto user : net->users) {
            int clocks=0;
            auto kind=getPortTimingClass(user.cell,user.port,clocks);
            if (kind==TMG_CLOCK_INPUT) return false;
            if (dwidth(user.cell->type) || user.cell->type==id_MISTRAL_FF) continue;
            if (kind!=TMG_REGISTER_INPUT || clocks<=0 || !movable(user.cell) ||
                getBelPinsForCellPin(user.cell,user.port).empty()) return false;
            for (int i=0;i<clocks;++i) {
                auto info=getPortClockingInfo(user.cell,user.port,i);
                auto clock=user.cell->getPort(info.clock_port);
                if (!clock || !clock->clkconstr || clock->clkconstr->period.minDelay()<=0) return false;
            }
        }
        return true;
    };
    struct Cut {
        std::vector<CellInfo *> nodes;
        PortRef sink;
        double excess;
        size_t distance;
        std::vector<NetInfo *> inputs;
        policy::Result function;
        std::vector<policy::Candidate> definitions;
    };
    std::vector<Cut> cuts;
    using Key=std::tuple<std::vector<IdString>,IdString,IdString>;
    std::set<Key> seen;
    // Expanding a bounded fanin DAG includes off-path siblings. Greedily
    // following only the reported chain misses valid reconvergent cuts.
    for (const auto &path : mistral_remap_report::validate(ctx,json,true)) {
        for (size_t edge=0;edge<path.edges.size();++edge) {
            auto source=path.edges[edge].first, sink=path.edges[edge].second;
            int sink_width=dwidth(sink.cell->type);
            if (source.port!=id_Q || !safe(source.cell) || !table_valid(source.cell) ||
                !sink_width || !safe(sink.cell) || !table_valid(sink.cell) ||
                std::find(dpins.begin(),dpins.begin()+sink_width,sink.port)==dpins.begin()+sink_width) continue;
            std::vector<std::vector<CellInfo *>> frontier{{source.cell}};
            std::set<std::vector<IdString>> expanded;
            size_t states=0;
            while (!frontier.empty() && states++<256) {
                auto selected=std::move(frontier.back()); frontier.pop_back();
                std::sort(selected.begin(),selected.end(),[&](auto *a,auto *b) { return a->name.str(ctx)<b->name.str(ctx); });
                std::vector<IdString> ids;
                for (auto *cell : selected) ids.push_back(cell->name);
                if (!expanded.insert(ids).second) continue;
                if (selected.size()<4) {
                    std::vector<CellInfo *> additions;
                    for (auto *cell : selected) for (int pin=0;pin<dwidth(cell->type);++pin) {
                        auto input=cell->getPort(dpins[pin]);
                        auto *driver=input ? input->driver.cell : nullptr;
                        if (driver && input->driver.port==id_Q && driver!=sink.cell && safe(driver) && table_valid(driver) &&
                            ordinary(input) && std::find(selected.begin(),selected.end(),driver)==selected.end()) additions.push_back(driver);
                    }
                    std::sort(additions.begin(),additions.end(),[&](auto *a,auto *b) { return a->name.str(ctx)<b->name.str(ctx); });
                    additions.erase(std::unique(additions.begin(),additions.end()),additions.end());
                    for (auto it=additions.rbegin();it!=additions.rend();++it) {
                        auto next=selected; next.push_back(*it); frontier.push_back(std::move(next));
                    }
                    continue;
                }
                Key key{ids,sink.cell->name,sink.port};
                if (!seen.insert(key).second) continue;
                std::set<CellInfo *> inside(selected.begin(),selected.end());
                std::map<CellInfo *,int> visits;
                std::vector<CellInfo *> ordered;
                bool eligible=true;
                auto topo=[&](auto &&self,CellInfo *cell)->void {
                    if (visits[cell]==1) { eligible=false; return; }
                    if (visits[cell]==2) return;
                    visits[cell]=1;
                    if (!safe(cell) || !table_valid(cell) || !ordinary(cell->getPort(id_Q))) eligible=false;
                    for (int pin=0;pin<dwidth(cell->type);++pin) {
                        auto input=cell->getPort(dpins[pin]);
                        if (input && inside.count(input->driver.cell)) self(self,input->driver.cell);
                    }
                    visits[cell]=2; ordered.push_back(cell);
                };
                topo(topo,source.cell);
                if (!eligible || ordered.size()!=4 || inside.count(sink.cell)) continue;
                std::map<NetInfo *,size_t> internal;
                for (size_t i=0;i<ordered.size();++i) internal[ordered[i]->getPort(id_Q)]=i;
                std::vector<NetInfo *> inputs;
                for (auto *cell : ordered) for (int pin=0;pin<dwidth(cell->type);++pin) {
                    auto net=cell->getPort(dpins[pin]);
                    auto state=cell->get_pin_state(dpins[pin]);
                    if (state==PIN_0 || state==PIN_1) { if (net) eligible=false; }
                    else if (state==PIN_SIG || state==PIN_INV) {
                        if (!ordinary(net)) eligible=false;
                        else if (!internal.count(net)) inputs.push_back(net);
                    } else eligible=false;
                }
                std::sort(inputs.begin(),inputs.end(),[&](auto *a,auto *b) { return a->name.str(ctx)<b->name.str(ctx); });
                inputs.erase(std::unique(inputs.begin(),inputs.end()),inputs.end());
                if (!eligible || inputs.size()>7) continue;
                std::vector<policy::Node> nodes;
                for (size_t index=0;index<ordered.size();++index) {
                    auto *cell=ordered[index]; policy::Node node;
                    node.mask=uint64_t(cell->params.at(id_LUT).as_int64());
                    for (int pin=0;pin<dwidth(cell->type);++pin) {
                        auto net=cell->getPort(dpins[pin]); auto state=cell->get_pin_state(dpins[pin]);
                        if (state==PIN_0 || state==PIN_1) node.pins.push_back({state==PIN_0 ? policy::ZERO : policy::ONE,false});
                        else if (internal.count(net)) {
                            auto prior=internal.at(net); if (prior>=index) eligible=false;
                            node.pins.push_back({policy::node_source(int(prior)),state==PIN_INV});
                        } else {
                            auto at=std::find(inputs.begin(),inputs.end(),net);
                            if (at==inputs.end()) { eligible=false; break; }
                            node.pins.push_back({int(at-inputs.begin()),state==PIN_INV});
                        }
                    }
                    nodes.push_back(std::move(node));
                }
                if (!eligible) continue;
                auto function=policy::compose(nodes);
                if (!function.valid || function.signals.size()!=7) continue;
                auto definitions=policy::decompose(function);
                if (definitions.empty()) continue;
                cuts.push_back({ordered,sink,path.excess,path.edges.size()-edge,inputs,function,std::move(definitions)});
            }
        }
    }
    std::sort(cuts.begin(),cuts.end(),[&](const Cut &a,const Cut &b) {
        auto identity=[&](const Cut &cut) {
            std::vector<std::string> names;
            for (auto *node : cut.nodes) names.push_back(node->name.str(ctx));
            return names;
        };
        return std::make_tuple(-a.excess,a.distance,a.sink.cell->name.str(ctx),a.sink.port.str(ctx),identity(a)) <
            std::make_tuple(-b.excess,b.distance,b.sink.cell->name.str(ctx),b.sink.port.str(ctx),identity(b));
    });
    if (cuts.size()>16) cuts.resize(16);
    log_info("Decomposition discovery: %zu bounded seven-input cuts.\n",cuts.size());
    int qualified=0;
    for (const auto &cut : cuts) {
        bool eligible=true;
        std::set<CellPortKey> endpoint_keys;
        std::map<NetInfo *,int> visit;
        auto follow=[&](auto &&self,NetInfo *net)->void {
            if (!net) { eligible=false; return; }
            auto found=visit.find(net);
            if (found!=visit.end()) {
                if (found->second==1) eligible=false;
                return;
            }
            if (visit.size()>=64) { eligible=false; return; }
            if (!ordinary(net)) { eligible=false; return; }
            visit[net]=1;
            for (auto user : net->users) {
                int clocks=0;
                auto kind=getPortTimingClass(user.cell,user.port,clocks);
                if (dwidth(user.cell->type) && kind==TMG_COMB_INPUT) {
                    if (!safe(user.cell) || !table_valid(user.cell)) eligible=false;
                    else self(self,user.cell->getPort(id_Q));
                } else if (kind==TMG_REGISTER_INPUT && clocks>0) endpoint_keys.insert(CellPortKey(user));
                else eligible=false;
            }
            visit[net]=2;
        };
        follow(follow,cut.sink.cell->getPort(id_Q));
        if (!eligible || endpoint_keys.empty()) continue;
        TimingAnalyser before(ctx); before.setup(false,false,true);
        auto old_slack=before.get_setup_slack(CellPortKey(cut.sink));
        if (!dtimed(old_slack)) continue;
        auto old_holds=dholds(before);
        std::map<CellPortKey,float> endpoints,boundaries;
        for (auto key : endpoint_keys) {
            auto value=before.get_setup_slack(key);
            if (!dtimed(value)) eligible=false;
            else endpoints[key]=value;
        }
        std::vector<NetInfo *> inputs;
        std::array<delay_t,7> arrivals;
        for (size_t signal=0;signal<cut.function.signals.size();++signal) {
            auto net=cut.inputs.at(cut.function.signals[signal]); inputs.push_back(net);
            if (!before.get_max_arrival(CellPortKey(net->driver),arrivals[signal])) eligible=false;
            for (auto user : net->users) {
                if (dwidth(user.cell->type) || user.cell->type==id_MISTRAL_FF) continue;
                auto key=CellPortKey(user); auto value=before.get_setup_slack(key);
                if (!dtimed(value)) eligible=false;
                else boundaries[key]=value;
            }
        }
        if (!eligible) continue;
        auto max_logic=[&](const policy::Lut &model)->int {
            CellInfo temporary(ctx,cut.nodes.back()->name,dtype(model.signals.size()));
            int maximum=0;
            for (size_t pin=0;pin<model.signals.size();++pin) {
                DelayQuad value;
                if (!getCellDelay(&temporary,dpins[pin],id_Q,value)) return -1;
                maximum=std::max(maximum,int(value.maxDelay()));
            }
            return maximum;
        };
        struct Definition { size_t index; int64_t score; int width,total; };
        std::vector<Definition> definitions;
        for (size_t index=0;index<cut.definitions.size();++index) {
            const auto &model=cut.definitions[index];
            std::array<int64_t,2> encoded{}; int widest=0,total=0; bool timed=true;
            for (size_t code=0;code<model.encoders.size();++code) {
                const auto &encoder=model.encoders[code];
                auto logic=max_logic(encoder); timed &= logic>=0;
                int64_t incoming=std::numeric_limits<int64_t>::lowest();
                for (int signal : encoder.signals) incoming=std::max(incoming,int64_t(arrivals.at(signal)));
                encoded[code]=incoming+std::max(logic,0)+650;
                widest=std::max(widest,int(encoder.signals.size())); total+=encoder.signals.size();
            }
            int64_t incoming=std::numeric_limits<int64_t>::lowest();
            for (int signal : model.root.signals)
                incoming=std::max(incoming,signal<7 ? int64_t(arrivals.at(signal)) : encoded.at(signal-7));
            auto logic=max_logic(model.root); timed &= logic>=0;
            if (timed) definitions.push_back({index,incoming+logic,widest,total});
        }
        std::sort(definitions.begin(),definitions.end(),[](const Definition &a,const Definition &b) {
            return std::tie(a.score,a.width,a.total,a.index)<std::tie(b.score,b.width,b.total,b.index);
        });
        // Complemented encodings with the same supports have the same local
        // max-delay model. Reserve coverage for different physical fanins.
        std::vector<Definition> shortlist;
        std::set<std::pair<std::vector<std::vector<int>>,std::vector<int>>> supports;
        for (const auto &definition : definitions) {
            const auto &model=cut.definitions[definition.index];
            std::vector<std::vector<int>> groups;
            for (const auto &encoder : model.encoders) groups.push_back(encoder.signals);
            std::sort(groups.begin(),groups.end());
            if (!supports.emplace(groups,model.root.signals).second) continue;
            shortlist.push_back(definition); if (shortlist.size()==4) break;
        }
        int timed_cut=0;
        for (const auto &definition : shortlist) {
            const auto &model=cut.definitions[definition.index];
            std::vector<policy::Lut> models=model.encoders; models.push_back(model.root);
            std::vector<IdString> cnames,nnames;
            for (size_t index=0;index<models.size();++index) {
                auto name=cut.nodes.back()->name.str(ctx)+"$decomposition$"+cut.sink.cell->name.str(ctx)+"$"+
                    cut.sink.port.str(ctx)+(index<model.encoders.size() ? "$code"+std::to_string(index) : "$root");
                cnames.push_back(id(name)); nnames.push_back(id(name+"$Q"));
                if (cells.count(cnames.back()) || nets.count(nnames.back()) || net_aliases.count(nnames.back())) eligible=false;
            }
            if (!eligible) continue;
            auto *old_net=cut.sink.cell->getPort(cut.sink.port);
            auto saved_port=cut.sink.cell->ports.at(cut.sink.port);
            std::map<NetInfo *,indexed_store<PortRef>> saved_users;
            saved_users.emplace(old_net,old_net->users);
            for (auto net : inputs) saved_users.emplace(net,net->users);
            decltype(ctx->cells) parked_cells;
            decltype(ctx->nets) parked_nets;
            decltype(ctx->net_aliases) parked_aliases;
            std::vector<CellInfo *> clones;
            std::vector<NetInfo *> outputs;
            std::vector<std::vector<NetInfo *>> clone_inputs;
            bool live=false;
            auto rollback=[&]() {
                if (!live) return;
                live=false;
                for (auto *cell : clones) if (cell->bel!=BelId()) unbindBel(cell->bel);
                cut.sink.cell->disconnectPort(cut.sink.port);
                for (auto *cell : clones)
                    for (const auto &port : cell->ports) cell->disconnectPort(port.first);
                for (auto name : cnames) cells.erase(name);
                for (auto name : nnames) nets.erase(name);
                for (auto &entry : cells) parked_cells.at(entry.first)=std::move(entry.second);
                for (auto &entry : nets) parked_nets.at(entry.first)=std::move(entry.second);
                cells.swap(parked_cells); nets.swap(parked_nets); net_aliases.swap(parked_aliases);
                cut.sink.cell->ports.at(cut.sink.port)=saved_port;
                for (auto &entry : saved_users) std::swap(entry.first->users,entry.second);
                assign_comb_info(cut.sink.cell); update_bel(cut.sink.cell->bel);
                ctx->check();
            };
            // Keep the exact original owner dictionaries while speculative
            // owners are live; removal alone would retain rehash perturbations.
            cells.swap(parked_cells); nets.swap(parked_nets); net_aliases.swap(parked_aliases);
            live=true;
            try {
                for (auto &entry : parked_cells) cells[entry.first]=std::move(entry.second);
                for (auto &entry : parked_nets) nets[entry.first]=std::move(entry.second);
                net_aliases=parked_aliases;
                for (size_t index=0;index<models.size();++index) {
                    auto *cell=ctx->createCell(cnames[index],dtype(models[index].signals.size())); clones.push_back(cell);
                    std::vector<NetInfo *> signals;
                    for (int signal : models[index].signals) signals.push_back(signal<7 ? inputs.at(signal) : outputs.at(signal-7));
                    clone_inputs.push_back(signals);
                    cell->params[id_LUT]=Property(int64_t(models[index].mask),1u<<signals.size());
                    for (size_t pin=0;pin<signals.size();++pin) {
                        cell->addInput(dpins[pin]); cell->connectPort(dpins[pin],signals[pin]);
                        cell->pin_data[dpins[pin]].state=PIN_SIG;
                    }
                    cell->addOutput(id_Q); auto net=ctx->createNet(nnames[index]); outputs.push_back(net);
                    cell->connectPort(id_Q,net); cell->pin_data[id_Q].state=PIN_SIG;
                }
                cut.sink.cell->disconnectPort(cut.sink.port); cut.sink.cell->connectPort(cut.sink.port,outputs.back());
                for (auto *cell : clones) { assign_comb_info(cell); assign_default_pinmap(cell); }
                assign_comb_info(cut.sink.cell); update_bel(cut.sink.cell->bel);
                auto apply_order=[&](size_t cell_index,const std::vector<int> &order) {
                    auto *cell=clones.at(cell_index); uint64_t mask=0;
                    NPNR_ASSERT(local_remap_pin_policy::permute_mask(models.at(cell_index).mask,order,mask));
                    for (size_t pin=0;pin<order.size();++pin) cell->disconnectPort(dpins[pin]);
                    for (size_t pin=0;pin<order.size();++pin)
                        cell->connectPort(dpins[pin],clone_inputs.at(cell_index).at(order[pin]));
                    cell->params[id_LUT]=Property(int64_t(mask),1u<<order.size());
                    assign_comb_info(cell); assign_default_pinmap(cell);
                    if (cell->bel!=BelId()) update_bel(cell->bel);
                };
                // Only the sink and new clone LABs change. Validate every bound
                // neighbour in all affected LABs, including fixed FF halves.
                auto legal=[&]() {
                    std::set<Lab> affected{lab(cut.sink.cell->bel)};
                    for (auto *cell : clones) if (cell->bel!=BelId()) affected.insert(lab(cell->bel));
                    for (auto at : affected) for (auto bel : getBelsByTile(at.first,at.second))
                        if (getBoundBelCell(bel) && !isBelLocationValid(bel)) return false;
                    return true;
                };
                struct Site { BelId bel; int64_t score; std::vector<int> order; };
                auto rank_order=[&](size_t cell_index,const std::array<int64_t,2> &code_arrival)->Site {
                    auto *cell=clones.at(cell_index); size_t count=models.at(cell_index).signals.size();
                    std::vector<std::vector<int>> costs(count,std::vector<int>(count));
                    for (size_t source=0;source<count;++source) for (size_t pin=0;pin<count;++pin) {
                        int signal=models[cell_index].signals[source]; DelayQuad logic;
                        if (!getCellDelay(cell,dpins[pin],id_Q,logic)) return {BelId(),0,{}};
                        int64_t arrival=signal<7 ? int64_t(arrivals.at(signal)) : code_arrival.at(signal-7);
                        int64_t cost=arrival+int64_t(ctx->predictArcDelay(clone_inputs[cell_index][source],{cell,dpins[pin]}))+logic.maxDelay();
                        if (cost<0 || cost>std::numeric_limits<int>::max()) return {BelId(),0,{}};
                        costs[source][pin]=int(cost);
                    }
                    auto ranked=local_remap_pin_policy::orders(costs);
                    if (ranked.empty()) return {BelId(),0,{}};
                    return {cell->bel,ranked.front().score,ranked.front().input_for_pin};
                };
                auto trim=[&](std::vector<Site> sites)->std::vector<Site> {
                    std::sort(sites.begin(),sites.end(),[&](const Site &a,const Site &b) {
                        auto x=getBelLocation(a.bel),y=getBelLocation(b.bel);
                        return std::make_tuple(a.score,x.x,x.y,x.z,a.order)<std::make_tuple(b.score,y.x,y.y,y.z,b.order);
                    });
                    std::map<Lab,int> counts; std::vector<Site> result;
                    for (auto &site : sites) {
                        if (counts[lab(site.bel)]++>=2) continue;
                        result.push_back(std::move(site)); if (result.size()==8) break;
                    }
                    return result;
                };

                auto canonical=[&](size_t index) {
                    std::vector<int> order(models[index].signals.size());
                    for (size_t pin=0;pin<order.size();++pin) order[pin]=int(pin);
                    return order;
                };
                auto score_order=[&](size_t index,const std::vector<int> &order,
                                     const std::array<int64_t,2> &encoded)->int64_t {
                    int64_t score=std::numeric_limits<int64_t>::lowest();
                    for (size_t pin=0;pin<order.size();++pin) {
                        auto source=size_t(order[pin]); int signal=models[index].signals[source]; DelayQuad logic;
                        if (!getCellDelay(clones[index],dpins[pin],id_Q,logic)) return -1;
                        int64_t arrival=signal<7 ? int64_t(arrivals.at(signal)) : encoded.at(signal-7);
                        int64_t value=arrival+int64_t(ctx->predictArcDelay(clone_inputs[index][source],{clones[index],dpins[pin]}))+
                            int64_t(logic.maxDelay());
                        if (value<0 || value>std::numeric_limits<int>::max()) return -1;
                        score=std::max(score,value);
                    }
                    return score;
                };
                auto choose=[&](size_t index,const std::array<int64_t,2> &encoded)->Site {
                    auto site=rank_order(index,encoded);
                    if (site.bel==BelId()) return site;
                    apply_order(index,site.order);
                    if (legal()) return site;
                    // The fastest pin order can violate ALM sharing. Retain the
                    // canonical legal order as an independently scored fallback.
                    site.order=canonical(index); apply_order(index,site.order);
                    if (!legal()) return {BelId(),0,{}};
                    site.score=score_order(index,site.order,encoded);
                    if (site.score<0) return {BelId(),0,{}};
                    return site;
                };
                auto nearby=[&](const std::vector<Lab> &centers) {
                    std::set<Lab> locations;
                    for (auto center : centers) for (int dx=-3;dx<=3;++dx) for (int dy=-3;dy<=3;++dy) {
                        int x=center.first+dx,y=center.second+dy;
                        if (std::abs(dx)+std::abs(dy)>3 || x<0 || y<0 || x>=getGridDimX() || y>=getGridDimY() ||
                            protected_labs.count({x,y})) continue;
                        locations.emplace(x,y);
                    }
                    return locations;
                };
                const size_t root_index=model.encoders.size();
                std::array<int64_t,2> estimated{};
                for (size_t code=0;code<model.encoders.size();++code) {
                    int64_t incoming=std::numeric_limits<int64_t>::lowest();
                    for (int signal : model.encoders[code].signals) incoming=std::max(incoming,int64_t(arrivals.at(signal)));
                    estimated[code]=incoming+max_logic(model.encoders[code])+650;
                }
                std::vector<Site> roots;
                for (auto at : nearby({lab(cut.sink.cell->bel),lab(cut.nodes.back()->bel)}))
                    for (auto bel : getBelsByTile(at.first,at.second)) {
                        if (!checkBelAvail(bel) || !isValidBelForCellType(clones.back()->type,bel)) continue;
                        bindBel(bel,clones.back(),STRENGTH_WEAK);
                        auto site=choose(root_index,estimated);
                        if (site.bel!=BelId()) {
                            site.score+=ctx->predictArcDelay(outputs.back(),cut.sink);
                            roots.push_back(std::move(site));
                        }
                        unbindBel(bel);
                    }
                roots=trim(std::move(roots));
                struct Tuple { Site root; std::vector<Site> encoders; int64_t score; };
                std::vector<Tuple> tuples;
                for (const auto &root : roots) {
                    bindBel(root.bel,clones.back(),STRENGTH_WEAK); apply_order(root_index,root.order);
                    std::vector<std::vector<Site>> code_sites;
                    for (size_t code=0;code<model.encoders.size();++code) {
                        std::vector<int> xs,ys;
                        for (auto *net : clone_inputs[code]) if (net->driver.cell && net->driver.cell->bel!=BelId()) {
                            auto at=getBelLocation(net->driver.cell->bel); xs.push_back(at.x); ys.push_back(at.y);
                        }
                        std::sort(xs.begin(),xs.end()); std::sort(ys.begin(),ys.end());
                        std::vector<Lab> centers{lab(root.bel)};
                        if (!xs.empty()) centers.emplace_back(xs[xs.size()/2],ys[ys.size()/2]);
                        std::vector<Site> sites;
                        for (auto at : nearby(centers)) for (auto bel : getBelsByTile(at.first,at.second)) {
                            if (!checkBelAvail(bel) || !isValidBelForCellType(clones[code]->type,bel)) continue;
                            bindBel(bel,clones[code],STRENGTH_WEAK);
                            auto site=choose(code,{});
                            if (site.bel!=BelId()) {
                                // Include the wire to the root's currently chosen
                                // code pin when shortlisting each encoder.
                                for (size_t pin=0;pin<root.order.size();++pin)
                                    if (models[root_index].signals[root.order[pin]]==int(7+code))
                                        site.score+=ctx->predictArcDelay(outputs[code],{clones.back(),dpins[pin]});
                                sites.push_back(std::move(site));
                            }
                            unbindBel(bel);
                        }
                        code_sites.push_back(trim(std::move(sites)));
                    }
                    std::vector<Site> placed;
                    auto joint=[&](auto &&self,size_t code)->void {
                        if (code<model.encoders.size()) {
                            for (const auto &site : code_sites[code]) {
                                if (!checkBelAvail(site.bel)) continue;
                                bindBel(site.bel,clones[code],STRENGTH_WEAK); apply_order(code,site.order);
                                placed.push_back(site); self(self,code+1); placed.pop_back(); unbindBel(site.bel);
                            }
                            return;
                        }
                        // Recompute the root order before testing joint legality:
                        // the preceding tuple may have left another pin order.
                        std::array<int64_t,2> encoded{};
                        for (size_t i=0;i<placed.size();++i) {
                            encoded[i]=score_order(i,placed[i].order,{});
                            if (encoded[i]<0) return;
                        }
                        auto exact_root=choose(root_index,encoded);
                        if (exact_root.bel==BelId()) return;
                        int64_t score=exact_root.score+ctx->predictArcDelay(outputs.back(),cut.sink);
                        tuples.push_back({std::move(exact_root),placed,score});
                    };
                    joint(joint,0); unbindBel(root.bel);
                }
                auto tuple_key=[&](const Tuple &tuple) {
                    std::vector<std::tuple<int,int,int,std::vector<int>>> positions;
                    auto add=[&](const Site &site) {
                        auto at=getBelLocation(site.bel); positions.emplace_back(at.x,at.y,at.z,site.order);
                    };
                    add(tuple.root); for (const auto &site : tuple.encoders) add(site);
                    return std::make_pair(tuple.score,positions);
                };
                std::sort(tuples.begin(),tuples.end(),[&](const Tuple &a,const Tuple &b) { return tuple_key(a)<tuple_key(b); });
                std::map<std::pair<Lab,std::vector<Lab>>,int> geometry_counts;
                std::set<std::vector<Lab>> ordered_geometries;
                int timed_definition=0;
                for (const auto &tuple : tuples) {
                    if (timed_definition>=4 || timed_cut>=16) break;
                    std::vector<Lab> ordered{lab(tuple.root.bel)},code_labs;
                    for (const auto &site : tuple.encoders) { ordered.push_back(lab(site.bel)); code_labs.push_back(lab(site.bel)); }
                    std::sort(code_labs.begin(),code_labs.end());
                    auto geometry=std::make_pair(lab(tuple.root.bel),code_labs);
                    if (ordered_geometries.count(ordered) || geometry_counts[geometry]>=2) continue;
                    bindBel(tuple.root.bel,clones.back(),STRENGTH_WEAK); apply_order(root_index,tuple.root.order);
                    for (size_t i=0;i<tuple.encoders.size();++i) {
                        bindBel(tuple.encoders[i].bel,clones[i],STRENGTH_WEAK); apply_order(i,tuple.encoders[i].order);
                    }
                    if (!legal()) {
                        for (auto *cell : clones) unbindBel(cell->bel);
                        continue;
                    }
                    // Illegal tuples consume no timing or geometry budget.
                    ordered_geometries.insert(ordered); ++geometry_counts[geometry]; ++timed_definition; ++timed_cut;
                    TimingAnalyser after(ctx); after.setup(false,false,true);
                    auto slack=after.get_setup_slack(CellPortKey(cut.sink));
                    bool improve=dtimed(slack) && slack>=old_slack+250;
                    bool endpoint_ok=true,boundary_ok=true,clocks=true;
                    for (const auto &entry : endpoints) {
                        auto now=after.get_setup_slack(entry.first); endpoint_ok &= dtimed(now) && now>=entry.second;
                    }
                    for (const auto &entry : boundaries) {
                        auto now=after.get_setup_slack(entry.first); boundary_ok &= dtimed(now) && now>=entry.second;
                    }
                    for (const auto &clock : before.get_timing_result().clock_fmax) {
                        const auto &now=after.get_timing_result().clock_fmax;
                        if (!now.count(clock.first) || now.at(clock.first).achieved+1e-4<clock.second.achieved) clocks=false;
                    }
                    bool hold=enable_replication_policy::hold_nonregressing(old_holds,dholds(after));
                    std::string code_bels;
                    for (size_t i=0;i<model.encoders.size();++i) {
                        if (i) code_bels += ";";
                        code_bels += nameOfBel(clones[i]->bel);
                    }
                    log_info("Decomposition trial root=%s sink=%s.%s definition=%zu encoders=%zu root_bel=%s code_bels=%s gain=%.0fps improve=%d endpoints=%d clocks=%d hold=%d boundaries=%d\n",
                        nameOf(cut.nodes.back()),nameOf(cut.sink.cell),cut.sink.port.c_str(ctx),definition.index,model.encoders.size(),
                        nameOfBel(clones.back()->bel),code_bels.c_str(),slack-old_slack,int(improve),int(endpoint_ok),int(clocks),int(hold),int(boundary_ok));
                    if (improve && endpoint_ok && boundary_ok && clocks && hold) {
                        log_info("Decomposition candidate %d: root=%s sink=%s.%s cut=4 inputs=7 encoders=%zu gain=%.0fps.\n",
                            qualified,nameOf(cut.nodes.back()),nameOf(cut.sink.cell),cut.sink.port.c_str(ctx),model.encoders.size(),slack-old_slack);
                        if (selection==qualified++) {
                            for (const auto &entry : original_places) {
                                NPNR_ASSERT(entry.first->bel==entry.second.first && entry.first->belStrength==entry.second.second);
                            }
                            ctx->check(); live=false;
                            log_info("Decomposition applied candidate %d; full routing and signoff still required.\n",selection);
                            return true;
                        }
                    }
                    for (auto *cell : clones) unbindBel(cell->bel);
                }
                log_info("Decomposition probes: root=%s sink=%s.%s definition=%zu timed=%d geometries=%zu\n",
                    nameOf(cut.nodes.back()),nameOf(cut.sink.cell),cut.sink.port.c_str(ctx),definition.index,timed_definition,geometry_counts.size());
                rollback();
            } catch (...) { rollback(); throw; }
        }
    }
    log_info("Decomposition: %d qualified candidates; no candidate applied.\n",qualified);
    return false;
}
NEXTPNR_NAMESPACE_END
