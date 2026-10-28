/* Validated route preservation for placement feedback. SPDX-License-Identifier: ISC */
#include "routed_placement.h"
#include <algorithm>
#include <functional>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include "log.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
using J = json11::Json;
void require(bool condition, const char *message)
{
    if (!condition) log_error("Invalid routed placement routes: %s.\n", message);
}
J properties(Context *ctx, const dict<IdString, Property> &values)
{
    J::object result;
    for (const auto &entry : values)
        result[entry.first.str(ctx)] = J::array{entry.second.is_string, entry.second.str};
    return result;
}
J attributes(Context *ctx, const dict<IdString, Property> &values)
{
    J::object result;
    for (const auto &entry : values) {
        auto name = entry.first.str(ctx);
        if (name == "ROUTING" || name == "BEL" || name == "NEXTPNR_BEL" || name == "BEL_STRENGTH" ||
            name.find("FES_PINMAP_") == 0) continue;
        result[name] = J::array{entry.second.is_string, entry.second.str};
    }
    return result;
}
J physical_cell(Context *ctx, CellInfo *cell)
{
    J::object ports;
    for (const auto &entry : cell->ports) {
        J::array pins;
        auto data = cell->pin_data.find(entry.first);
        if (data != cell->pin_data.end())
            for (auto pin : data->second.bel_pins) pins.push_back(pin.str(ctx));
        ports[entry.first.str(ctx)] = J::array{int(entry.second.type),
            entry.second.net ? entry.second.net->name.str(ctx) : "", int(cell->get_pin_state(entry.first)), pins};
    }
    return J::object{{"name", cell->name.str(ctx)}, {"type", cell->type.str(ctx)},
                     {"params", properties(ctx, cell->params)}, {"attributes", attributes(ctx, cell->attrs)}, {"ports", ports},
                     {"bel", cell->bel == BelId() ? "" : ctx->getBelName(cell->bel).str(ctx)},
                     {"strength", int(cell->belStrength)}};
}
bool data_net(Context *ctx, NetInfo *net)
{
    if (net->is_global || net->clkconstr || !net->driver.cell || net->users.empty() ||
        net->driver.cell->bel == BelId()) return false;
    int clocks = 0;
    auto klass = ctx->getPortTimingClass(net->driver.cell, net->driver.port, clocks);
    if (klass != TMG_COMB_OUTPUT && klass != TMG_REGISTER_OUTPUT) return false;
    for (const auto &user : net->users) {
        if (!user.cell || user.cell->bel == BelId()) return false;
        auto sink = ctx->getPortTimingClass(user.cell, user.port, clocks);
        if (sink != TMG_COMB_INPUT && sink != TMG_REGISTER_INPUT && sink != TMG_ENDPOINT) return false;
    }
    return true;
}
J physical_net(Context *ctx, NetInfo *net)
{
    J::array users, sinks;
    for (const auto &user : net->users) {
        users.push_back(J::array{physical_cell(ctx, user.cell), user.port.str(ctx)});
        for (auto wire : ctx->getNetinfoSinkWires(net, user)) {
            require(wire != WireId(), "missing physical sink");
            sinks.push_back(ctx->getWireName(wire).str(ctx));
        }
    }
    auto order = [](const J &a, const J &b) { return a.dump() < b.dump(); };
    std::sort(users.begin(), users.end(), order);
    std::sort(sinks.begin(), sinks.end(), order);
    WireId source = ctx->getNetinfoSourceWire(net);
    require(source != WireId(), "missing physical source");
    return J::object{{"name", net->name.str(ctx)}, {"attributes", attributes(ctx, net->attrs)},
                     {"driver", physical_cell(ctx, net->driver.cell)},
                     {"driver_port", net->driver.port.str(ctx)}, {"users", users},
                     {"source", ctx->getWireName(source).str(ctx)}, {"sinks", sinks}};
}
J route_wires(Context *ctx, NetInfo *net)
{
    J::array wires;
    for (const auto &binding : net->wires)
        wires.push_back(J::array{ctx->getWireName(binding.first).str(ctx),
            binding.second.pip == PipId() ? "" : ctx->getPipName(binding.second.pip).str(ctx)});
    std::sort(wires.begin(), wires.end(), [](const J &a, const J &b) { return a.dump() < b.dump(); });
    return wires;
}
struct Tree
{
    NetInfo *net = nullptr;
    std::map<WireId, PipId> wires;
    std::vector<std::pair<int, WireId>> ordered;
};
Tree parse_tree(Context *ctx, const J &entry, std::set<WireId> &all_wires)
{
    require(entry.is_object() && entry["physical"].is_object() && entry["wires"].is_array(), "missing tree fields");
    Tree result;
    for (const auto &binding : entry["wires"].array_items()) {
        require(binding.is_array() && binding.array_items().size() == 2 &&
                binding[0].is_string() && binding[1].is_string(), "invalid wire binding");
        WireId wire = ctx->getWireByName(IdStringList::parse(ctx, binding[0].string_value()));
        require(wire != WireId() && ctx->wires.count(wire) && result.wires.count(wire) == 0 && all_wires.insert(wire).second,
                "unknown or duplicate wire");
        PipId pip;
        if (!binding[1].string_value().empty()) {
            pip = ctx->getPipByName(IdStringList::parse(ctx, binding[1].string_value()));
            require(pip != PipId() && ctx->getPipDstWire(pip) == wire &&
                    ctx->wires.count(ctx->getPipSrcWire(pip)), "invalid driving pip");
            bool exists = false;
            for (auto candidate : ctx->getPipsDownhill(ctx->getPipSrcWire(pip)))
                exists |= candidate == pip;
            require(exists, "pip is not an architecture edge");
        }
        result.wires.emplace(wire, pip);
    }
    require(!result.wires.empty(), "empty route tree");
    const auto &physical = entry["physical"];
    require(physical["source"].is_string() && physical["sinks"].is_array(), "missing physical terminals");
    WireId source = ctx->getWireByName(IdStringList::parse(ctx, physical["source"].string_value()));
    std::map<WireId, int> depth;
    for (const auto &start : result.wires) {
        std::vector<WireId> path;
        std::set<WireId> seen;
        WireId cursor = start.first;
        while (depth.count(cursor) == 0) {
            require(result.wires.count(cursor) && seen.insert(cursor).second, "disconnected or cyclic tree");
            PipId pip = result.wires.at(cursor);
            if (pip == PipId()) {
                require(cursor == source, "route has a different source");
                depth[cursor] = 0;
                break;
            }
            path.push_back(cursor);
            cursor = ctx->getPipSrcWire(pip);
        }
        while (!path.empty()) {
            WireId next = path.back();
            path.pop_back();
            depth[next] = depth.at(cursor) + 1;
            cursor = next;
        }
    }
    std::set<WireId> sinks;
    for (const auto &sink : physical["sinks"].array_items()) {
        require(sink.is_string(), "invalid physical sink");
        WireId wire = ctx->getWireByName(IdStringList::parse(ctx, sink.string_value()));
        require(result.wires.count(wire), "route omits a physical sink");
        sinks.insert(wire);
    }
    std::set<WireId> non_leaves;
    for (const auto &binding : result.wires)
        if (binding.second != PipId()) non_leaves.insert(ctx->getPipSrcWire(binding.second));
    for (const auto &binding : result.wires)
        if (!non_leaves.count(binding.first)) require(sinks.count(binding.first), "route has an undeclared leaf");
    for (const auto &entry : depth) result.ordered.emplace_back(entry.second, entry.first);
    std::sort(result.ordered.begin(), result.ordered.end());
    return result;
}
std::map<std::string, Tree> validated_trees(Context *ctx, const J &witness, const J &parent)
{
    require(witness["version"] == J(1) && witness["parent"] == parent && witness["nets"].is_object(),
            "source does not match the immediate parent");
    std::map<std::string, Tree> result;
    std::set<WireId> all_wires;
    for (const auto &entry : witness["nets"].object_items()) {
        require(parent["design"]["nets"].object_items().count(entry.first) &&
                entry.second["physical"]["name"] == J(entry.first), "unknown original net");
        Tree tree = parse_tree(ctx, entry.second, all_wires);
        auto found = ctx->nets.find(ctx->id(entry.first));
        require(found != ctx->nets.end(), "original net disappeared");
        tree.net = found->second.get();
        result.emplace(entry.first, std::move(tree));
    }
    return result;
}

J encode_wires(Context *ctx, const Tree &tree)
{
    J::array result;
    for (const auto &wire : tree.wires)
        result.push_back(J::array{ctx->getWireName(wire.first).str(ctx),
            wire.second == PipId() ? "" : ctx->getPipName(wire.second).str(ctx)});
    std::sort(result.begin(), result.end(), [](const J &a, const J &b) { return a.dump() < b.dump(); });
    return result;
}

// This is a separate, explicit prefix contract. Whole-tree preservation keeps
// its exact original contract. Only declared sink BEL moves may differ here.
bool stationary_branches(Context *ctx, const Tree &source, const J &record, const J &physical,
                         const J &parent, const J &target, Tree &prefix, J &proof)
{
    const auto &old = record["physical"];
    for (const char *key : {"name", "attributes", "driver", "driver_port", "source"})
        if (old[key] != physical[key]) return false;
    if (!old["users"].is_array() || !physical["users"].is_array()) return false;
    using User = std::pair<std::string, std::string>;
    std::map<User, J> before, after;
    auto users = [](const J &rows, std::map<User, J> &result) {
        for (const auto &row : rows.array_items()) {
            if (!row.is_array() || row.array_items().size() != 2 || !row[0].is_object() ||
                !row[0]["name"].is_string() || !row[1].is_string() ||
                !result.emplace(User(row[0]["name"].string_value(), row[1].string_value()), row).second)
                return false;
        }
        return true;
    };
    if (!users(old["users"], before) || !users(physical["users"], after) || before.size() != after.size()) return false;
    std::set<User> stationary;
    bool moved = false;
    for (const auto &entry : before) {
        auto found = after.find(entry.first);
        if (found == after.end()) return false;
        if (entry.second == found->second) {
            stationary.insert(entry.first);
            continue;
        }
        auto a = entry.second[0].object_items(), b = found->second[0].object_items();
        const auto &name = entry.first.first;
        if (a["bel"] == b["bel"] || a["bel"] != parent["placement"][name][0] ||
            b["bel"] != target["placement"][name][0]) return false;
        a.erase("bel"); b.erase("bel");
        if (a != b) return false; // Truth, pin maps, state, controls and strength remain exact.
        moved = true;
    }
    if (!moved || stationary.empty()) return false;
    std::set<WireId> sinks, kept;
    std::set<std::string> old_sinks;
    for (const auto &wire : old["sinks"].array_items()) old_sinks.insert(wire.string_value());
    for (const auto &user : source.net->users) {
        if (!stationary.count(User(user.cell->name.str(ctx), user.port.str(ctx)))) continue;
        for (auto wire : ctx->getNetinfoSinkWires(source.net, user)) {
            if (!old_sinks.count(ctx->getWireName(wire).str(ctx)) || !source.wires.count(wire)) return false;
            sinks.insert(wire);
            for (WireId cursor = wire;;) {
                kept.insert(cursor);
                PipId pip = source.wires.at(cursor);
                if (pip == PipId()) break;
                cursor = ctx->getPipSrcWire(pip);
            }
        }
    }
    if (sinks.empty()) return false;
    prefix.net = source.net;
    for (const auto &wire : source.wires)
        if (kept.count(wire.first)) prefix.wires.emplace(wire);
    for (const auto &wire : source.ordered)
        if (kept.count(wire.second)) prefix.ordered.push_back(wire);
    J::array endpoints;
    for (auto wire : sinks) endpoints.push_back(ctx->getWireName(wire).str(ctx));
    std::sort(endpoints.begin(), endpoints.end(), [](const J &a, const J &b) { return a.dump() < b.dump(); });
    proof = J::object{{"source", record}, {"physical", physical}, {"sinks", endpoints},
                      {"wires", encode_wires(ctx, prefix)}};
    return true;
}
} // namespace

json11::Json snapshot_routed_placement_routes(Context *ctx, const json11::Json &parent)
{
    J::object nets;
    for (const auto &entry : ctx->nets) {
        auto *net = entry.second.get();
        // Generated helper nets remain the architecture's responsibility.
        if (!parent["design"]["nets"].object_items().count(net->name.str(ctx)) ||
            !data_net(ctx, net) || net->wires.empty()) continue;
        nets[net->name.str(ctx)] = J::object{{"physical", physical_net(ctx, net)}, {"wires", route_wires(ctx, net)}};
    }
    return J::object{{"version", 1}, {"parent", parent}, {"nets", nets}};
}

json11::Json apply_routed_placement_routes(Context *ctx, const json11::Json &witness, const json11::Json &parent,
                                          const json11::Json &handoff)
{
    auto parsed = validated_trees(ctx, witness, parent);
    std::vector<Tree> trees;
    J::array preserved, changed, blocked, proposed;
    J::object proposed_witness;
    J::object branch_witness;
    std::string target_error;
    auto target = J::parse(ctx->routed_placement_current, target_error);
    if (ctx->routed_placement_preserve_branches)
        require(target_error.empty() && target.is_object(), "missing prepared branch target");
    if (!handoff.is_null()) {
        std::string error;
        auto target = J::parse(ctx->routed_placement_current, error);
        require(ctx->routed_placement_preserve_routes && error.empty() && target.is_object() &&
                handoff.is_object() && handoff.object_items().size() == 3 && handoff["version"] == J(1) &&
                handoff["target"] == target && handoff["nets"].is_object(), "stale proposed route target");
        std::set<WireId> proposed_wires;
        for (const auto &entry : handoff["nets"].object_items()) {
            require(parsed.count(entry.first) && entry.second.is_object() && entry.second.object_items().size() == 3,
                    "unknown proposed data net");
            auto *net = ctx->nets.at(ctx->id(entry.first)).get();
            require(data_net(ctx, net), "proposed route is not a prepared data net");
            auto physical = physical_net(ctx, net);
            const auto &request = entry.second;
            require(request["source"].is_string() && request["source"] == physical["source"] &&
                    request["sinks"].is_array() && request["wires"].is_array(), "proposed physical source changed");
            std::set<std::string> expected, actual;
            for (const auto &sink : request["sinks"].array_items()) {
                require(sink.is_string() && expected.insert(sink.string_value()).second,
                        "duplicate or invalid proposed sink");
            }
            for (const auto &sink : physical["sinks"].array_items()) actual.insert(sink.string_value());
            require(expected == actual && !expected.empty(), "proposed physical fanout changed");
            auto encoded = request["wires"].array_items();
            std::sort(encoded.begin(), encoded.end(), [](const J &a, const J &b) { return a.dump() < b.dump(); });
            J record = J::object{{"physical", physical}, {"wires", encoded}};
            auto tree = parse_tree(ctx, record, proposed_wires);
            tree.net = net;
            for (const auto &binding : net->wires) {
                auto saved = tree.wires.find(binding.first);
                require(saved != tree.wires.end() && saved->second == binding.second.pip,
                        "proposed route omits a native prefix");
            }
            for (const auto &binding : tree.wires) {
                auto *owner = ctx->getBoundWireNet(binding.first);
                require(owner == nullptr || owner == net, "proposed route conflicts with a native owner");
                if (binding.second != PipId() && !ctx->checkPipAvailForNet(binding.second, net)) {
                    const char *reason = ctx->pip_rejection_reason(binding.second, net);
                    if (!ctx->routed_placement_directory.empty()) {
                        auto path = std::filesystem::path(ctx->routed_placement_directory) / "route-handoff-failure.json";
                        std::ofstream out(path);
                        out << J(J::object{{"net", entry.first}, {"pip", ctx->getPipName(binding.second).str(ctx)},
                            {"reason", reason ? reason : "pip_owned"}}).dump();
                        require(bool(out), "cannot write handoff failure diagnostic");
                    }
                    log_error("Invalid proposed route for '%s': pip %s is unavailable (%s).\n", entry.first.c_str(),
                              ctx->getPipName(binding.second).str(ctx).c_str(), reason ? reason : "pip_owned");
                }
            }
            proposed.push_back(entry.first);
            proposed_witness[entry.first] = record;
            trees.push_back(std::move(tree));
        }
    }
    for (auto &entry : parsed) {
        if (proposed_witness.count(entry.first)) continue;
        Tree tree = std::move(entry.second);
        NetInfo *net = tree.net;
        J branch;
        const bool complete = data_net(ctx, net) && physical_net(ctx, net) == witness["nets"][entry.first]["physical"];
        if (!complete) {
            changed.push_back(entry.first);
            Tree prefix;
            if (!ctx->routed_placement_preserve_branches || !data_net(ctx, net) ||
                !stationary_branches(ctx, tree, witness["nets"][entry.first], physical_net(ctx, net),
                                     parent, target, prefix, branch)) continue;
            tree = std::move(prefix);
        }
        bool available = true;
        for (const auto &binding : net->wires) {
            auto saved = tree.wires.find(binding.first);
            available &= saved != tree.wires.end() && saved->second == binding.second.pip;
        }
        for (const auto &binding : tree.wires) {
            auto *owner = ctx->getBoundWireNet(binding.first);
            available &= owner == nullptr || owner == net;
            if (binding.second != PipId())
                available &= ctx->checkPipAvailForNet(binding.second, net);
        }
        if (!available) {
            blocked.push_back(entry.first);
            continue;
        }
        trees.push_back(std::move(tree));
        if (complete) preserved.push_back(entry.first);
        else branch_witness[entry.first] = branch;
    }
    // Validate the complete union before modifying any bindings. A proposed
    // replacement must never displace a retained tree or another proposal.
    std::map<WireId, IdString> owners;
    for (const auto &tree : trees)
        for (const auto &wire : tree.wires) {
            auto owner = owners.emplace(wire.first, tree.net->name);
            require(owner.second || owner.first->second == tree.net->name, "proposed route conflicts with a retained tree");
        }
    struct Transaction
    {
        Context *ctx;
        struct Saved { NetInfo *net; WireId wire; PlaceStrength strength; bool added; };
        std::vector<Saved> bindings;
        bool committed = false;
        ~Transaction()
        {
            if (committed) return;
            for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
                if (it->added) ctx->unbindWire(it->wire);
                else it->net->wires.at(it->wire).strength = it->strength;
            }
        }
    } transaction{ctx};
    size_t bindings = 0;
    for (const auto &tree : trees) bindings += tree.wires.size();
    transaction.bindings.reserve(bindings);
    for (const auto &tree : trees)
        for (const auto &item : tree.ordered) {
            WireId wire = item.second;
            auto old = tree.net->wires.find(wire);
            if (old != tree.net->wires.end()) {
                transaction.bindings.push_back({tree.net, wire, old->second.strength, false});
                old->second.strength = std::max(old->second.strength, STRENGTH_USER);
            } else {
                PipId pip = tree.wires.at(wire);
                if (pip == PipId()) ctx->bindWire(wire, tree.net, STRENGTH_USER);
                else ctx->bindPip(pip, tree.net, STRENGTH_USER);
                transaction.bindings.push_back({tree.net, wire, STRENGTH_USER, true});
            }
        }
    transaction.committed = true;
    log_info("Routed placement routes: preserved=%zu changed=%zu blocked=%zu.\n",
             preserved.size(), changed.size(), blocked.size());
    J::object result{{"version", 1}, {"preserved", preserved}, {"changed", changed}, {"blocked", blocked}};
    if (ctx->routed_placement_preserve_branches) {
        result["branches"] = J::object{{"version", 1}, {"parent", parent}, {"target", target}, {"nets", branch_witness}};
        log_info("Routed placement branches: protected %zu stationary fanouts.\n", branch_witness.size());
    }
    if (!handoff.is_null()) {
        result["proposed"] = proposed;
        result["handoff"] = J::object{{"version", 1}, {"target", handoff["target"]}, {"nets", proposed_witness}};
        log_info("Routed placement handoff: protected %zu proposed data trees.\n", proposed.size());
    }
    return result;
}

void check_routed_placement_branches(Context *ctx, const J &witness, const J &parent, const J &target)
{
    require(witness["version"] == J(1) && witness["parent"] == parent && witness["target"] == target &&
            witness["nets"].is_object(), "stale branch-preservation contract");
    J::object originals;
    for (const auto &entry : witness["nets"].object_items()) originals[entry.first] = entry.second["source"];
    auto trees = validated_trees(ctx, J::object{{"version", 1}, {"parent", parent}, {"nets", originals}}, parent);
    std::set<WireId> final_wires;
    for (const auto &entry : witness["nets"].object_items()) {
        auto *net = trees.at(entry.first).net;
        require(data_net(ctx, net), "preserved branch is no longer a data net");
        auto physical = physical_net(ctx, net);
        // The prefix is only part of a net. Explicitly validate its completed
        // target tree as well, including every reconnected physical sink.
        parse_tree(ctx, J::object{{"physical", physical}, {"wires", route_wires(ctx, net)}}, final_wires);
        Tree prefix;
        J expected;
        require(stationary_branches(ctx, trees.at(entry.first), entry.second["source"], physical,
                                    parent, target, prefix, expected) && expected == entry.second,
                "preserved branch physical contract changed");
        for (const auto &wire : prefix.wires) {
            auto actual = net->wires.find(wire.first);
            require(actual != net->wires.end() && actual->second.pip == wire.second &&
                    actual->second.strength >= STRENGTH_USER && ctx->getBoundWireNet(wire.first) == net,
                    "protected branch edge changed after routing");
        }
    }
}

std::map<WireId, IdString> routed_placement_route_reservations(Context *ctx, const json11::Json &witness,
                                                            const json11::Json &parent)
{
    auto trees = validated_trees(ctx, witness, parent);
    std::map<WireId, IdString> result;
    for (const auto &entry : trees)
        for (const auto &wire : entry.second.wires)
            result.emplace(wire.first, entry.second.net->name);
    return result;
}

void check_routed_placement_routes(Context *ctx, const json11::Json &witness, const json11::Json &preserved)
{
    require(preserved.is_array(), "missing preservation inventory");
    for (const auto &name : preserved.array_items()) {
        require(name.is_string(), "invalid preservation inventory");
        auto found = ctx->nets.find(ctx->id(name.string_value()));
        require(found != ctx->nets.end(), "preserved net disappeared");
        auto *net = found->second.get();
        auto saved = witness["nets"][name.string_value()];
        auto physical = physical_net(ctx, net), wires = route_wires(ctx, net);
        if (physical != saved["physical"] || wires != saved["wires"]) {
            if (!ctx->routed_placement_directory.empty()) {
                auto path = std::filesystem::path(ctx->routed_placement_directory) / "route-preservation-failure.json";
                require(!std::filesystem::exists(path), "preservation failure output already exists");
                std::ofstream out(path);
                out << J(J::object{{"net", name}, {"physical_matches", physical == saved["physical"]},
                    {"tree_matches", wires == saved["wires"]}, {"expected_physical", saved["physical"]},
                    {"actual_physical", physical}, {"expected_wires", saved["wires"]}, {"actual_wires", wires}}).dump();
                require(bool(out), "failed to record preservation failure");
            }
            log_error("Preserved physical tree '%s' changed during routing (physical=%d tree=%d).\n",
                      name.string_value().c_str(), int(physical == saved["physical"]), int(wires == saved["wires"]));
        }
    }
}

NEXTPNR_NAMESPACE_END
