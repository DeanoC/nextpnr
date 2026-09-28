/* Opt-in, fixture-specific post-placement feedback-state copy diagnostic.
 * SPDX-License-Identifier: ISC
 */
#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "jsonwrite.h"
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

CellInfo *feedback_state_copy_into_lab(Context *ctx, CellInfo *source, const std::vector<PortRef> &users,
                                       const char *suffix)
{
    if (!source || source->type != id_MISTRAL_FF || source->bel == BelId() || !suffix || !*suffix || users.empty())
        return nullptr;
    NetInfo *old_net = source->getPort(id_Q);
    if (!old_net || old_net->driver.cell != source || old_net->driver.port != id_Q)
        return nullptr;
    for (auto user : users)
        if (!user.cell || user.cell->getPort(user.port) != old_net || user.cell == source)
            return nullptr;
    auto clone_name = ctx->idf("%s%s", source->name.c_str(ctx), suffix);
    auto net_name = ctx->idf("%s$Q", clone_name.c_str(ctx));
    if (ctx->cells.count(clone_name) || ctx->nets.count(net_name) || ctx->net_aliases.count(net_name))
        return nullptr;

    std::map<IdString, BelId> placed;
    for (auto &entry : ctx->cells)
        if (entry.second->bel != BelId())
            placed.emplace(entry.first, entry.second->bel);
    CellInfo *clone = ctx->createCell(clone_name, source->type);
    clone->params = source->params;
    clone->attrs = source->attrs;
    for (auto attr : {"NEXTPNR_BEL", "BEL_STRENGTH", "FES_PINMAP_V1"})
        clone->attrs.erase(ctx->id(attr));
    clone->pin_data = source->pin_data;
    for (auto &port : source->ports)
        if (port.second.type == PORT_IN)
            source->copyPortTo(port.first, clone, port.first);
    NetInfo *new_net = ctx->createNet(net_name);
    clone->addOutput(id_Q);
    clone->connectPort(id_Q, new_net);
    ctx->assignArchInfo();

    auto origin = ctx->getBelLocation(source->bel);
    std::vector<BelId> sites;
    for (auto bel : ctx->getBelsByTile(origin.x, origin.y))
        if (ctx->getBelType(bel) == id_MISTRAL_FF && ctx->checkBelAvail(bel) &&
            ctx->isValidBelForCellType(clone->type, bel))
            sites.push_back(bel);
    std::sort(sites.begin(), sites.end(), [&](BelId a, BelId b) {
        int az = ctx->getBelLocation(a).z, bz = ctx->getBelLocation(b).z;
        return std::make_pair(std::abs(az - origin.z), az) < std::make_pair(std::abs(bz - origin.z), bz);
    });

    BelId chosen;
    for (auto bel : sites) {
        ctx->bindBel(bel, clone, STRENGTH_WEAK);
        bool legal = ctx->isBelLocationValid(bel);
        for (auto &entry : placed)
            if (ctx->getBelLocation(entry.second).x == origin.x &&
                ctx->getBelLocation(entry.second).y == origin.y && !ctx->isBelLocationValid(entry.second)) {
                legal = false;
                break;
            }
        ctx->unbindBel(bel);
        if (legal) {
            chosen = bel;
            break;
        }
    }
    if (chosen == BelId()) {
        std::vector<IdString> ports;
        for (auto &port : clone->ports)
            ports.push_back(port.first);
        for (auto port : ports)
            clone->disconnectPort(port);
        ctx->cells.erase(clone_name);
        ctx->nets.erase(net_name);
        ctx->assignArchInfo();
        return nullptr;
    }
    for (auto user : users) {
        user.cell->disconnectPort(user.port);
        user.cell->connectPort(user.port, new_net);
    }
    ctx->bindBel(chosen, clone, STRENGTH_WEAK);
    ctx->assignArchInfo();
    for (auto &entry : placed)
        if (ctx->cells.at(entry.first)->bel != entry.second)
            log_error("Feedback-state copy moved existing cell %s.\n", entry.first.c_str(ctx));
    for (auto &entry : ctx->cells)
        if (entry.second->bel != BelId() && !ctx->isBelLocationValid(entry.second->bel))
            log_error("Feedback-state copy leaves illegal cell %s.\n", entry.first.c_str(ctx));
    return clone;
}

void diagnostic_feedback_state_copy(Context *ctx, const char *prefix)
{
    if (!prefix || !*prefix)
        return;
    struct Rule {
        const char *source;
        const char *first_cell;
        IdString first_pin;
        const char *second_cell;
        IdString second_pin;
    };
    const Rule rules[] = {
        {"hps_ddr.port1.draining_MISTRAL_FF_Q", "ddr1_test.write_MISTRAL_ALUT5_C", id_B,
         "hps_ddr.port1.enter_read_MISTRAL_ALUT5_Q", id_B},
        {"hps_ddr.port1.finishing_MISTRAL_FF_Q", "ddr1_test.write_MISTRAL_ALUT5_C", id_E,
         "hps_ddr.port1.enter_read_MISTRAL_ALUT5_Q", id_D},
    };
    auto snapshot = [&](const char *suffix) {
        ctx->archInfoToAttributes();
        std::string path = std::string(prefix) + suffix;
        std::ofstream json(path);
        if (!json || !write_json_file(json, path, ctx))
            log_error("Cannot write feedback-state copy snapshot %s.\n", path.c_str());
        json.close();
        std::ofstream pins(path + ".pins.tsv");
        pins << "cell\tport\tstate\n";
        for (auto &entry : ctx->cells) {
            auto cell = entry.second.get();
            for (auto &port : cell->ports)
                pins << entry.first.str(ctx) << '\t' << port.first.str(ctx) << '\t'
                     << int(cell->get_pin_state(port.first)) << '\n';
        }
        pins.close();
        if (!pins)
            log_error("Cannot finish feedback-state copy pin snapshot.\n");
    };
    snapshot(".before.json");
    for (const auto &rule : rules) {
        auto source = ctx->cells.at(ctx->id(rule.source)).get();
        auto first = ctx->cells.at(ctx->id(rule.first_cell)).get();
        auto second = ctx->cells.at(ctx->id(rule.second_cell)).get();
        if (!feedback_state_copy_into_lab(ctx, source, {{first, rule.first_pin}, {second, rule.second_pin}},
                                          "$local_state_copy"))
            log_error("No legal same-LAB feedback-state copy for %s.\n", rule.source);
    }
    snapshot(".after.json");
    log_info("Feedback-state copy diagnostic: two same-LAB FF copies, four LUT inputs rewired.\n");
}

NEXTPNR_NAMESPACE_END
