/* Throwaway paired one-cell relocation/composition experiment; not an optimizer.
 * SPDX-License-Identifier: ISC
 */
#include "nextpnr.h"
#include "jsonwrite.h"
#include "log.h"
#include <fstream>
#include <limits>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

NEXTPNR_NAMESPACE_BEGIN
void diagnostic_placed_composition(Context *ctx, const char *prefix, const char *mode)
{
    // No identifier allocation or context mutation when the experiment is off.
    if (!prefix || !*prefix) return;
    if (!mode || (std::strcmp(mode,"compose") && std::strcmp(mode,"relocate")))
        log_error("Placed composition requires MODE=compose or MODE=relocate.\n");
    bool compose = std::strcmp(mode,"compose") == 0;
    auto cell = [&](const char *name) { return ctx->cells.at(ctx->id(name)).get(); };
    CellInfo *consumer = cell("hps_ddr.port1.slot_free_MISTRAL_ALUT3_B");
    CellInfo *producer = cell("hps_ddr.port1.slot_free_MISTRAL_ALUT2_Q");
    CellInfo *replica = cell("hps_ddr.port1.slot_free_MISTRAL_ALUT3_B$enable_replica");
    NPNR_ASSERT(consumer->type == id_MISTRAL_ALUT3 && consumer->params.at(id_LUT).as_int64() == 0x32);
    NPNR_ASSERT(producer->type == id_MISTRAL_ALUT2 && producer->params.at(id_LUT).as_int64() == 0xb);
    NPNR_ASSERT(replica->type == id_MISTRAL_ALUT3 && replica->params.at(id_LUT).as_int64() == 0x32);
    NPNR_ASSERT(consumer->ports.size() == 4 && producer->ports.size() == 3 && replica->ports.size() == 4);
    NPNR_ASSERT(consumer->cluster == ClusterId() && !consumer->region && consumer->belStrength == STRENGTH_WEAK);
    NPNR_ASSERT(ctx->nameOfBel(consumer->bel) == std::string("MISTRAL_COMB.24.20.36"));
    NPNR_ASSERT(ctx->nameOfBel(producer->bel) == std::string("MISTRAL_COMB.30.26.48"));
    NPNR_ASSERT(ctx->nameOfBel(replica->bel) == std::string("MISTRAL_COMB.37.25.0"));
    for (auto c : {consumer, producer, replica})
        for (const auto &p : c->ports) NPNR_ASSERT(c->get_pin_state(p.first) == PIN_SIG);
    NetInfo *read = consumer->getPort(id_A), *slot = producer->getPort(id_Q), *write = consumer->getPort(id_C);
    NetInfo *ready = producer->getPort(id_A), *valid = producer->getPort(id_B), *output = consumer->getPort(id_Q);
    NPNR_ASSERT(read && slot && write && ready && valid && output);
    NPNR_ASSERT(consumer->getPort(id_B) == slot);
    for (auto pin : {id_A, id_B, id_C}) NPNR_ASSERT(replica->getPort(pin) == consumer->getPort(pin));
    NPNR_ASSERT(read->driver.cell == cell("hps_ddr.port1.enter_read_MISTRAL_ALUT5_Q") && read->driver.port == id_Q);
    NPNR_ASSERT(write->driver.cell == cell("hps_ddr.port1.enter_write_MISTRAL_ALUT2_Q") && write->driver.port == id_Q);
    NPNR_ASSERT(valid->driver.cell == cell("hps_ddr.f2sdram_cmd_valid_1_MISTRAL_ALUT2_Q") && valid->driver.port == id_Q);
    NPNR_ASSERT(ready->driver.cell == cell("hps_ddr.f2sdram") && ready->driver.port == ctx->id("cmd_ready_1"));
    NPNR_ASSERT(ready->driver.cell->type == id_cyclonev_hps_interface_fpga2sdram);
    NPNR_ASSERT(std::set<NetInfo *>({read,ready,valid,write}).size() == 4);
    NPNR_ASSERT(output->users.entries() == 107 && slot->users.entries() == 7 && replica->getPort(id_Q)->users.entries() == 4);
    for (auto n : {output, replica->getPort(id_Q)})
        for (auto user : n->users) NPNR_ASSERT(user.cell->type == id_MISTRAL_FF && user.port == id_ENA);
    for (const auto &n : ctx->nets) NPNR_ASSERT(n.second->wires.empty());

    auto snapshot = [&](const char *suffix) {
        ctx->archInfoToAttributes();
        std::string filename = std::string(prefix) + suffix;
        std::ofstream stream(filename);
        if (!stream || !write_json_file(stream, filename, ctx)) log_error("Cannot write composition snapshot '%s'.\n", filename.c_str());
        stream.close(); if (!stream) log_error("Cannot finish composition snapshot.\n");
        std::map<std::pair<std::string,std::string>,int> pins;
        for (auto &entry : ctx->cells) {
            auto c = entry.second.get();
            for (auto &p : c->ports) pins[{c->name.str(ctx),p.first.str(ctx)}] = int(c->get_pin_state(p.first));
            for (auto &p : c->pin_data) pins[{c->name.str(ctx),p.first.str(ctx)}] = int(c->get_pin_state(p.first));
        }
        std::ofstream sidecar(filename + ".pins.tsv"); sidecar << "cell\tport\tstate\n";
        for (auto &p : pins) sidecar << p.first.first << '\t' << p.first.second << '\t' << p.second << '\n';
        sidecar.close(); if (!sidecar) log_error("Cannot finish composition pin evidence.\n");
    };
    snapshot(".before.json");
    struct Saved {
        IdString type; BelId bel; PlaceStrength strength;
        dict<IdString,Property> params, attrs;
        std::map<IdString,std::tuple<NetInfo *,PortType,CellPinState,int>> ports;
        std::map<IdString,CellPinState> pins;
    };
    std::map<IdString,Saved> saved;
    for (auto &entry : ctx->cells) {
        auto c = entry.second.get(); auto &s = saved[entry.first];
        s.type=c->type; s.bel=c->bel; s.strength=c->belStrength; s.params=c->params; s.attrs=c->attrs;
        for (auto &p : c->ports) s.ports[p.first] = {p.second.net,p.second.type,c->get_pin_state(p.first),p.second.user_idx.idx()};
        for (auto &p : c->pin_data) s.pins[p.first] = c->get_pin_state(p.first);
    }
    auto net_count = ctx->nets.size();
    auto ready_users = ready->users.entries(), valid_users = valid->users.entries();
    const unsigned pmask = producer->params.at(id_LUT).as_int64(), cmask = consumer->params.at(id_LUT).as_int64();
    unsigned composed = 0;
    for (unsigned row = 0; row < 16; ++row) {
        unsigned a=row&1, b=(row>>1)&1, c=(row>>2)&1, d=(row>>3)&1;
        unsigned intermediate = (pmask >> (b | (c << 1))) & 1;
        unsigned result = (cmask >> (a | (intermediate << 1) | (d << 2))) & 1;
        // Independent Boolean oracle for the asserted original masks.
        NPNR_ASSERT(result == unsigned(!(b || !c) && (a || d)));
        composed |= result << row;
    }
    NPNR_ASSERT(composed == 0x3020);
    BelId bel = consumer->bel; PlaceStrength strength = consumer->belStrength;
    auto original_ports = consumer->ports;
    auto original_pin_data = consumer->pin_data;
    auto original_ready_users = ready->users, original_valid_users = valid->users;
    auto original_slot_users = slot->users, original_write_users = write->users;
    ctx->unbindBel(bel);
    for (auto p : {id_B,id_C}) consumer->disconnectPort(p);
    consumer->type = id_MISTRAL_ALUT4;
    consumer->params[id_LUT] = Property(composed,16);
    consumer->addInput(id_D);
    consumer->connectPort(id_B,ready);
    consumer->connectPort(id_C,valid); consumer->connectPort(id_D,write);
    consumer->pin_data[id_D].state = PIN_SIG;
    ctx->assignArchInfo();
    // Both modes perform exactly the same ALUT4 search and identifier activity.
    // The original LAB is overfull after composition in the measured design;
    // never relax its 42-input legality bound or displace another cell.
    auto mapped = [&](const CellInfo *c, IdString pin) {
        auto pins=ctx->getBelPinsForCellPin(c,pin);
        return pins.begin()!=pins.end();
    };
    for (auto p : {id_A,id_B,id_C,id_D}) {
        auto net=consumer->getPort(p);
        NPNR_ASSERT(net && net->driver.cell && net->driver.cell->bel != BelId());
        NPNR_ASSERT(mapped(net->driver.cell,net->driver.port) && mapped(consumer,p));
    }
    auto original_loc = ctx->getBelLocation(bel);
    std::set<std::pair<int,int>> protected_labs;
    for (const auto &entry : ctx->cells) {
        auto c = entry.second.get();
        if (c == consumer || c->bel == BelId()) continue;
        if (c->type == id_MISTRAL_MLAB || c->belStrength > STRENGTH_WEAK || c->cluster != ClusterId() ||
            c->region || c->attrs.count(ctx->id("keep")) || c->attrs.count(ctx->id("dont_touch"))) {
            auto l = ctx->getBelLocation(c->bel); protected_labs.emplace(l.x,l.y);
        }
    }
    BelId selected;
    std::tuple<int,int,std::string> best{4,std::numeric_limits<int>::max(),""};
    for (int dx=-3; dx<=3; ++dx) for (int dy=-3; dy<=3; ++dy) {
        int distance=std::abs(dx)+std::abs(dy);
        if (!distance || distance>3) continue;
        int x=original_loc.x+dx, y=original_loc.y+dy;
        if (protected_labs.count({x,y})) continue;
        for (auto trial : ctx->getBelsByTile(x,y)) {
            if (!ctx->checkBelAvail(trial) || !ctx->isValidBelForCellType(id_MISTRAL_ALUT4,trial)) continue;
            ctx->bindBel(trial,consumer,strength);
            if (ctx->isBelLocationValid(trial)) {
                int inputs=0, outputs=0;
                for (auto p : {id_A,id_B,id_C,id_D}) inputs=std::max(inputs,int(ctx->predictArcDelay(consumer->getPort(p),{consumer,p})));
                for (auto user : output->users) outputs=std::max(outputs,int(ctx->predictArcDelay(output,user)));
                auto score=std::make_tuple(distance,inputs+outputs,std::string(ctx->nameOfBel(trial)));
                if (score<best) { best=score; selected=trial; }
            }
            ctx->unbindBel(trial);
        }
    }
    if (selected == BelId()) log_error("No legal ALUT4 site within radius3 for placed composition.\n");
    if (!compose) {
        for (auto p : {id_B,id_C,id_D}) consumer->disconnectPort(p);
        consumer->ports = original_ports; consumer->pin_data = original_pin_data;
        // Restore the indexed stores too: the relocation control retains both
        // active PortRef indices and free-slot ordering from the baseline.
        ready->users = std::move(original_ready_users); valid->users = std::move(original_valid_users);
        slot->users = std::move(original_slot_users); write->users = std::move(original_write_users);
        consumer->type=id_MISTRAL_ALUT3; consumer->params=saved.at(consumer->name).params;
        ctx->assignArchInfo();
    }
    ctx->bindBel(selected,consumer,strength);
    ctx->assignArchInfo();
    NPNR_ASSERT(ctx->nets.size() == net_count && ctx->cells.size() == saved.size());
    for (auto &entry : saved) {
        auto c = ctx->cells.at(entry.first).get(); auto &s = entry.second;
        NPNR_ASSERT(c->bel == (c == consumer ? selected : s.bel) && c->belStrength == s.strength);
        if (c->bel != BelId() && !ctx->isBelLocationValid(c->bel))
            log_error("Placed composition leaves illegal cell %s at %s.\n",c->name.c_str(ctx),ctx->nameOfBel(c->bel));
        NPNR_ASSERT(c->attrs.size() == s.attrs.size());
        for (auto &a : s.attrs) NPNR_ASSERT(c->attrs.at(a.first) == a.second);
        if (c == consumer && compose) continue;
        NPNR_ASSERT(c->type == s.type && c->params.size() == s.params.size() && c->ports.size() == s.ports.size());
        for (auto &p : s.params) NPNR_ASSERT(c->params.at(p.first) == p.second);
        for (auto &p : s.ports) {
            auto &actual = c->ports.at(p.first);
            NPNR_ASSERT(std::make_tuple(actual.net,actual.type,c->get_pin_state(p.first),actual.user_idx.idx()) == p.second);
        }
        for (auto &p : s.pins) NPNR_ASSERT(c->get_pin_state(p.first) == p.second);
    }
    for (const auto &p : consumer->ports) NPNR_ASSERT(consumer->get_pin_state(p.first) == PIN_SIG);
    NPNR_ASSERT(consumer->getPort(id_Q) == output && output->users.entries() == 107);
    NPNR_ASSERT(slot->users.entries() == (compose ? 6 : 7) && replica->getPort(id_Q)->users.entries() == 4);
    NPNR_ASSERT(ready->users.entries() == ready_users + int(compose) && valid->users.entries() == valid_users + int(compose));
    ctx->check(); snapshot(".after.json");
    std::ofstream record(std::string(prefix)+".experiment.tsv");
    record << "mode\tcell\told_bel\tnew_bel\tmask\tdistance\tpredicted_max_input_output_ps\n";
    record << mode << '\t' << consumer->name.str(ctx) << '\t' << ctx->nameOfBel(bel) << '\t'
           << ctx->nameOfBel(selected) << '\t' << consumer->params.at(id_LUT).as_int64() << '\t'
           << std::get<0>(best) << '\t' << std::get<1>(best) << '\n';
    record.close(); if (!record) log_error("Cannot finish composition experiment record.\n");
    log_info("Diagnostic placed composition mode=%s: %s %s mask=0x%04x %s -> %s; "
             "107 ENA users, producer retained with %zu users, replica retained with 4 users.\n",
             mode, consumer->name.c_str(ctx), consumer->type.c_str(ctx),
             unsigned(consumer->params.at(id_LUT).as_int64()), ctx->nameOfBel(bel), ctx->nameOfBel(selected), size_t(slot->users.entries()));
}
NEXTPNR_NAMESPACE_END
