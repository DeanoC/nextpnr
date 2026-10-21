/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  Deano Calver
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <map>
#include <string>
#include <vector>

#include "gpurouter.h"
#include "gtest/gtest.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

namespace {

// Each routed wire with the wire that drives it ("" for the source)
using Tree = std::map<std::string, std::string>;

// One net with three sinks, routed in one launch. Without timing every tree
// wire starts at cost 0, so each sink attaches at its cheapest hop:
//
//     src -1-> p -5-> x -1-> y -1-> z -1-> t
//                     ^             |
//                     +------1------+
//
// The sinks are y, x and t, in that order. The first arc reaches y through
// x, so the second arc's sink is already on the tree. Searched as a fresh
// destination, x would come back as y -> z -> x, a second parent. The third
// arc reaches t through z, which must then be a wire the tree really has.
struct SharedSinkDesign
{
    Context ctx{ArchArgs{}};
    NetInfo *net = nullptr;

    SharedSinkDesign()
    {
        ctx.settings[ctx.id("timing_driven")] = false;
        auto wire = [&](const char *name) { return ctx.addWire(IdStringList(ctx.id(name)), ctx.id("W"), 0, 0); };
        const WireId src = wire("src"), p = wire("p"), x = wire("x"), y = wire("y"), z = wire("z"), t = wire("t");
        auto pip = [&](WireId from, WireId to, delay_t delay) {
            const std::string name = std::string(ctx.nameOfWire(from)) + "." + ctx.nameOfWire(to);
            ctx.addPip(IdStringList(ctx.id(name)), ctx.id("P"), from, to, delay, Loc(0, 0, 0));
        };
        pip(src, p, 1);
        pip(p, x, 5);
        pip(x, y, 1);
        pip(y, z, 1);
        pip(z, x, 1);
        pip(z, t, 1);

        const IdString type = ctx.id("B"), in = ctx.id("I"), out = ctx.id("O");
        net = ctx.createNet(ctx.id("n"));
        std::vector<std::pair<BelId, CellInfo *>> placement;
        auto cell = [&](const char *name, int zpos) {
            BelId bel = ctx.addBel(IdStringList(ctx.id(name)), type, Loc(0, 0, zpos), false, false);
            placement.emplace_back(bel, ctx.createCell(ctx.id(name), type));
            return placement.back();
        };
        auto drv = cell("drv", 0);
        ctx.addBelOutput(drv.first, out, src);
        drv.second->addOutput(out);
        drv.second->connectPort(out, net);
        int zpos = 1;
        for (WireId sink : {y, x, t}) {
            auto usr = cell((std::string("to_") + ctx.nameOfWire(sink)).c_str(), zpos++);
            ctx.addBelInput(usr.first, in, sink);
            usr.second->addInput(in);
            usr.second->connectPort(in, net);
        }
        ctx.assignArchInfo();
        for (auto &pl : placement)
            ctx.bindBel(pl.first, pl.second, STRENGTH_USER);
    }

    // On the CPU reference backend, which never opens a GPU
    bool route()
    {
        GpuRouterCfg cfg(&ctx);
        cfg.cpu_backend = true;
        cfg.gpu_required = false;
        return gpurouter(&ctx, cfg);
    }

    Tree tree()
    {
        Tree parents;
        for (auto &w : net->wires)
            parents[ctx.nameOfWire(w.first)] =
                    w.second.pip == PipId() ? std::string() : ctx.nameOfWire(ctx.getPipSrcWire(w.second.pip));
        return parents;
    }
};

const Tree expected_tree = {{"src", ""}, {"p", "src"}, {"x", "p"}, {"y", "x"}, {"z", "y"}, {"t", "z"}};

} // namespace

// The device search is checked against the same contract in
// GpuApproximateSearch.SinkReachedByEarlierArcAddsNoPathOnDevice.
TEST(GpuRouterSharedSink, LaterArcAttachesOnlyToRoutedWires)
{
    SharedSinkDesign design;
    ASSERT_TRUE(design.route());
    EXPECT_EQ(design.tree(), expected_tree);
}
