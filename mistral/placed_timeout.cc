/* Fixture-specific placement-preserving timeout rewrite. Diagnostic only.
 * Failure aborts before routing; this is not a production transactional pass.
 * SPDX-License-Identifier: ISC
 */
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include "jsonwrite.h"
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
struct TimeoutInput
{
    const char *cell;
    bool added;
};
struct TimeoutCell
{
    const char *name;
    int width;
    uint64_t mask;
    int root, channel;
    std::vector<TimeoutInput> inputs;
};
struct TimeoutRoot
{
    const char *name;
    int width;
    uint64_t mask;
    int channel;
};
#include "placed_timeout_payload.inc"
} // namespace
void diagnostic_placed_timeout(Context *ctx, const char *prefix)
{
    if (!prefix || !*prefix)
        return; // No IDs or mutations in the default path.
    auto pin = [&](int i) { return ctx->id(std::string(1, 'A' + i)); };
    auto type = [&](int width) { return ctx->id("MISTRAL_ALUT" + std::to_string(width)); };
    std::set<IdString> removed;
    std::vector<NetInfo *> root_nets;
    std::array<std::pair<int, int>, 3> centers{};
    for (const auto &r : timeout_roots) {
        auto c = ctx->cells.at(ctx->id(r.name)).get();
        NPNR_ASSERT(c->type == type(r.width) && uint64_t(c->params.at(id_LUT).as_int64()) == r.mask);
        NPNR_ASSERT(c->bel != BelId() && c->belStrength == STRENGTH_WEAK && c->cluster == ClusterId() && !c->region);
        NPNR_ASSERT(c->ports.size() == size_t(r.width + 1));
        for (auto &p : c->ports)
            NPNR_ASSERT(c->get_pin_state(p.first) == PIN_SIG);
        auto q = c->getPort(id_Q);
        NPNR_ASSERT(q && q->driver.cell == c && q->driver.port == id_Q);
        root_nets.push_back(q);
        removed.insert(c->name);
        auto l = ctx->getBelLocation(c->bel);
        centers[r.channel].first += l.x;
        centers[r.channel].second += l.y;
    }
    for (auto &p : centers) {
        p.first /= 2;
        p.second /= 2;
    }
    for (auto &e : ctx->nets)
        NPNR_ASSERT(e.second->wires.empty());
    for (const auto &p : timeout_cells) {
        NPNR_ASSERT(!ctx->cells.count(ctx->id(p.name)));
        auto netname = ctx->idf("%s$placed_timeout_q", p.name);
        NPNR_ASSERT(!ctx->nets.count(netname) && !ctx->net_aliases.count(netname));
        for (auto &input : p.inputs)
            if (!input.added) {
                auto c = ctx->cells.at(ctx->id(input.cell)).get();
                NPNR_ASSERT(c->type == id_MISTRAL_FF && c->bel != BelId() && c->get_pin_state(id_Q) == PIN_SIG &&
                            c->getPort(id_Q));
            }
    }
    auto snapshot = [&](const char *suffix) {
        ctx->archInfoToAttributes();
        std::string path = std::string(prefix) + suffix;
        std::ofstream file(path);
        if (!file || !write_json_file(file, path, ctx))
            log_error("Cannot write timeout snapshot.\n");
        file.close();
        if (!file)
            log_error("Cannot finish timeout snapshot.\n");
        std::map<std::pair<std::string, std::string>, int> states;
        for (auto &e : ctx->cells) {
            auto c = e.second.get();
            for (auto &p : c->ports)
                states[{c->name.str(ctx), p.first.str(ctx)}] = int(c->get_pin_state(p.first));
            for (auto &p : c->pin_data)
                states[{c->name.str(ctx), p.first.str(ctx)}] = int(c->get_pin_state(p.first));
        }
        std::ofstream pins(path + ".pins.tsv");
        pins << "cell\tport\tstate\n";
        for (auto &p : states)
            pins << p.first.first << '\t' << p.first.second << '\t' << p.second << '\n';
        pins.close();
        if (!pins)
            log_error("Cannot finish timeout pin states.\n");
    };
    snapshot(".before.json");
    struct Saved
    {
        IdString type;
        BelId bel;
        PlaceStrength strength;
        dict<IdString, Property> params, attrs;
        std::map<IdString, std::tuple<NetInfo *, PortType, CellPinState, int>> ports;
        std::map<IdString, CellPinState> pins;
    };
    std::map<IdString, Saved> saved;
    std::set<std::pair<int, int>> protected_labs;
    auto keep = ctx->id("keep"), dont_touch = ctx->id("dont_touch");
    for (auto &e : ctx->cells) {
        auto c = e.second.get();
        if (removed.count(e.first))
            continue;
        auto &s = saved[e.first];
        s.type = c->type;
        s.bel = c->bel;
        s.strength = c->belStrength;
        s.params = c->params;
        s.attrs = c->attrs;
        for (auto &p : c->ports)
            s.ports[p.first] = {p.second.net, p.second.type, c->get_pin_state(p.first), p.second.user_idx.idx()};
        for (auto &p : c->pin_data)
            s.pins[p.first] = c->get_pin_state(p.first);
        if (c->bel != BelId() &&
            (c->type == id_MISTRAL_MLAB || c->belStrength > STRENGTH_WEAK || c->cluster != ClusterId() || c->region ||
             c->attrs.count(keep) || c->attrs.count(dont_touch))) {
            auto l = ctx->getBelLocation(c->bel);
            protected_labs.emplace(l.x, l.y);
        }
    }
    for (auto name : removed) {
        auto c = ctx->cells.at(name).get();
        ctx->unbindBel(c->bel);
        std::vector<IdString> ports;
        for (auto &p : c->ports)
            ports.push_back(p.first);
        for (auto p : ports)
            c->disconnectPort(p);
        ctx->cells.erase(name);
    }
    std::map<std::string, CellInfo *> additions;
    for (const auto &p : timeout_cells) {
        auto c = ctx->createCell(ctx->id(p.name), type(p.width));
        additions[p.name] = c;
        c->params[id_LUT] = Property(int64_t(p.mask), 1 << p.width);
        for (int i = 0; i < p.width; ++i) {
            c->addInput(pin(i));
            c->pin_data[pin(i)].state = PIN_SIG;
        }
        c->addOutput(id_Q);
        c->pin_data[id_Q].state = PIN_SIG;
        auto net = p.root < 0 ? ctx->createNet(ctx->idf("%s$placed_timeout_q", p.name)) : root_nets.at(p.root);
        c->connectPort(id_Q, net);
    }
    for (const auto &p : timeout_cells) {
        auto c = additions.at(p.name);
        for (int i = 0; i < p.width; ++i) {
            const auto &ref = p.inputs.at(i);
            auto source = ref.added ? additions.at(ref.cell) : ctx->cells.at(ctx->id(ref.cell)).get();
            c->connectPort(pin(i), source->getPort(id_Q));
        }
    }
    ctx->assignArchInfo();
    std::ofstream sites(std::string(prefix) + ".sites.tsv");
    sites << "cell\tbel\tchannel\tcenter_x\tcenter_y\tdistance\tpredicted_input_output_ps\tbound_inputs\tbound_"
             "outputs\n";
    // Only the new logic is placed. A topological order makes all new input
    // drivers available; future new output endpoints are excluded explicitly.
    for (const auto &p : timeout_cells) {
        auto c = additions.at(p.name);
        auto center = centers.at(p.channel);
        BelId best_bel;
        std::tuple<int, int, std::string> best{std::numeric_limits<int>::max(), 99, ""};
        int best_inputs = 0, best_outputs = 0;
        for (auto trial : ctx->getBels()) {
            auto l = ctx->getBelLocation(trial);
            int dist = std::abs(l.x - center.first) + std::abs(l.y - center.second);
            if (dist > 6 || protected_labs.count({l.x, l.y}) || !ctx->checkBelAvail(trial) ||
                !ctx->isValidBelForCellType(c->type, trial))
                continue;
            ctx->bindBel(trial, c, STRENGTH_WEAK);
            if (ctx->isBelLocationValid(trial)) {
                int input = 0, output = 0, ni = 0, no = 0;
                for (int i = 0; i < p.width; ++i) {
                    auto net = c->getPort(pin(i));
                    NPNR_ASSERT(net && net->driver.cell && net->driver.cell->bel != BelId());
                    input = std::max(input, int(ctx->predictArcDelay(net, {c, pin(i)})));
                    ++ni;
                }
                auto q = c->getPort(id_Q);
                for (auto u : q->users)
                    if (u.cell->bel != BelId()) {
                        output = std::max(output, int(ctx->predictArcDelay(q, u)));
                        ++no;
                    }
                auto score = std::make_tuple(input + output, dist, std::string(ctx->nameOfBel(trial)));
                if (score < best) {
                    best = score;
                    best_bel = trial;
                    best_inputs = ni;
                    best_outputs = no;
                }
            }
            ctx->unbindBel(trial);
        }
        if (best_bel == BelId())
            log_error("Timeout diagnostic cannot legally place %s within radius6; abort before routing.\n", p.name);
        ctx->bindBel(best_bel, c, STRENGTH_WEAK);
        sites << p.name << '\t' << ctx->nameOfBel(best_bel) << '\t' << p.channel << '\t' << center.first << '\t'
              << center.second << '\t' << std::get<1>(best) << '\t' << std::get<0>(best) << '\t' << best_inputs << '\t'
              << best_outputs << '\n';
    }
    sites.close();
    if (!sites)
        log_error("Cannot finish timeout site evidence.\n");
    ctx->assignArchInfo();
    for (auto &e : saved) {
        auto c = ctx->cells.at(e.first).get();
        auto &s = e.second;
        NPNR_ASSERT(c->type == s.type && c->bel == s.bel && c->belStrength == s.strength &&
                    c->params.size() == s.params.size() && c->attrs.size() == s.attrs.size() &&
                    c->ports.size() == s.ports.size());
        for (auto &p : s.params)
            NPNR_ASSERT(c->params.at(p.first) == p.second);
        for (auto &a : s.attrs)
            NPNR_ASSERT(c->attrs.at(a.first) == a.second);
        for (auto &p : s.ports) {
            auto &v = c->ports.at(p.first);
            NPNR_ASSERT(std::make_tuple(v.net, v.type, c->get_pin_state(p.first), v.user_idx.idx()) == p.second);
        }
        for (auto &p : s.pins)
            NPNR_ASSERT(c->get_pin_state(p.first) == p.second);
    }
    for (auto &e : ctx->cells)
        if (e.second->bel != BelId() && !ctx->isBelLocationValid(e.second->bel))
            log_error("Timeout diagnostic leaves illegal BEL: %s.\n", e.first.c_str(ctx));
    ctx->check();
    snapshot(".after.json");
    std::ofstream manifest(std::string(prefix) + ".manifest.json");
    manifest << "{\"payload_sha256\":\"" << payload_sha
             << "\",\"removed\":6,\"added\":41,\"preserved\":" << saved.size()
             << ",\"search_radius\":6,\"strategy\":\"topological; fixed channel root midpoint; bound input/output "
                "predicted max sum; no displacement\"}\n";
    manifest.close();
    if (!manifest)
        log_error("Cannot finish timeout manifest.\n");
    log_info("Placed timeout diagnostic: 6 roots replaced, 41 new LUTs, %zu surviving cells unchanged.\n",
             saved.size());
}
NEXTPNR_NAMESPACE_END
