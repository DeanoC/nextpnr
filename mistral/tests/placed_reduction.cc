#include "gtest/gtest.h"
#include "log.h"
#include "nextpnr.h"
#include "placed_reduction_policy.h"
#include "reduction_balance_plan.h"
#include "timing.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

USING_NEXTPNR_NAMESPACE

NEXTPNR_NAMESPACE_BEGIN
bool placed_reduction(Context *, const std::string &, int, int, int minimum_branch_gain_ps = 250);
void diagnostic_placed_reduction(Context *, const char *);
NEXTPNR_NAMESPACE_END

namespace {
const IdString cube_pins[] = {id_A, id_B, id_C, id_D, id_E, id_F};

struct PlacedReductionLog {
    std::ostringstream stream;
    PlacedReductionLog() { log_streams.emplace_back(&stream, LogLevel::INFO_MSG); }
    ~PlacedReductionLog() { log_streams.pop_back(); }
};

// Save actual indexed_store slots, not merely the number of consumers. A
// rewrite can preserve its truth table while perturbing unrelated placement
// through a changed consumer traversal order.
struct CubeSnapshot {
    struct Cell {
        CellInfo *identity;
        IdString type;
        BelId bel;
        PlaceStrength strength;
        std::map<IdString, Property> params;
        std::map<IdString, Property> attrs;
        std::map<IdString, PortInfo> ports;
        std::map<IdString, ArchPinInfo> pins;
    };
    struct Net {
        NetInfo *identity;
        PortRef driver;
        std::map<store_index<PortRef>, PortRef> users;
        indexed_store<PortRef> user_store;
    };
    std::map<IdString, Cell> cells;
    std::map<IdString, Net> nets;
    std::map<IdString, IdString> aliases;
    std::vector<IdString> cell_order, net_order, alias_order;

    explicit CubeSnapshot(Context *ctx)
    {
        for (const auto &entry : ctx->net_aliases) {
            aliases.emplace(entry.first, entry.second);
            alias_order.push_back(entry.first);
        }
        for (const auto &entry : ctx->cells) {
            cell_order.push_back(entry.first);
            auto *c = entry.second.get();
            Cell saved{c, c->type, c->bel, c->belStrength, {}, {}, {}, {}};
            for (const auto &value : c->params) saved.params.emplace(value.first, value.second);
            for (const auto &value : c->attrs) saved.attrs.emplace(value.first, value.second);
            for (const auto &value : c->ports) saved.ports.emplace(value.first, value.second);
            for (const auto &value : c->pin_data) saved.pins.emplace(value.first, value.second);
            cells.emplace(entry.first, std::move(saved));
        }
        for (const auto &entry : ctx->nets) {
            net_order.push_back(entry.first);
            auto *n = entry.second.get();
            Net saved{n, n->driver, {}, n->users};
            for (const auto user : n->users.enumerate()) saved.users.emplace(user.index, user.value);
            nets.emplace(entry.first, std::move(saved));
        }
    }

    void expect_exact(Context *ctx) const
    {
        ASSERT_EQ(ctx->cells.size(), cells.size());
        ASSERT_EQ(ctx->nets.size(), nets.size());
        ASSERT_EQ(ctx->net_aliases.size(), aliases.size());
        std::vector<IdString> actual_aliases;
        for (const auto &entry : ctx->net_aliases) actual_aliases.push_back(entry.first);
        EXPECT_EQ(actual_aliases, alias_order);
        for (const auto &entry : aliases) {
            ASSERT_TRUE(ctx->net_aliases.count(entry.first));
            EXPECT_EQ(ctx->net_aliases.at(entry.first), entry.second);
            EXPECT_EQ(ctx->getNetByAlias(entry.first), nets.at(entry.second).identity);
        }
        std::vector<IdString> actual_cells, actual_nets;
        for (const auto &entry : ctx->cells) actual_cells.push_back(entry.first);
        for (const auto &entry : ctx->nets) actual_nets.push_back(entry.first);
        EXPECT_EQ(actual_cells, cell_order);
        EXPECT_EQ(actual_nets, net_order);
        for (const auto &entry : cells) {
            SCOPED_TRACE(entry.first.str(ctx));
            ASSERT_TRUE(ctx->cells.count(entry.first));
            auto *cell = ctx->cells.at(entry.first).get();
            const auto &saved = entry.second;
            EXPECT_EQ(cell, saved.identity);
            EXPECT_EQ(cell->type, saved.type);
            EXPECT_EQ(cell->bel, saved.bel);
            EXPECT_EQ(cell->belStrength, saved.strength);
            ASSERT_EQ(cell->params.size(), saved.params.size());
            for (const auto &param : saved.params) {
                ASSERT_TRUE(cell->params.count(param.first));
                EXPECT_EQ(cell->params.at(param.first), param.second);
            }
            ASSERT_EQ(cell->attrs.size(), saved.attrs.size());
            for (const auto &attr : saved.attrs) {
                ASSERT_TRUE(cell->attrs.count(attr.first));
                EXPECT_EQ(cell->attrs.at(attr.first), attr.second);
            }
            ASSERT_EQ(cell->ports.size(), saved.ports.size());
            for (const auto &port : saved.ports) {
                ASSERT_TRUE(cell->ports.count(port.first));
                const auto &now = cell->ports.at(port.first);
                EXPECT_EQ(now.name, port.second.name);
                EXPECT_EQ(now.net, port.second.net);
                EXPECT_EQ(now.type, port.second.type);
                EXPECT_EQ(now.user_idx, port.second.user_idx);
            }
            ASSERT_EQ(cell->pin_data.size(), saved.pins.size());
            for (const auto &pin : saved.pins) {
                ASSERT_TRUE(cell->pin_data.count(pin.first));
                EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
                EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
            }
        }
        for (const auto &entry : nets) {
            SCOPED_TRACE(entry.first.str(ctx));
            ASSERT_TRUE(ctx->nets.count(entry.first));
            auto *net = ctx->nets.at(entry.first).get();
            const auto &saved = entry.second;
            EXPECT_EQ(net, saved.identity);
            EXPECT_EQ(net->driver.cell, saved.driver.cell);
            EXPECT_EQ(net->driver.port, saved.driver.port);
            ASSERT_EQ(net->users.entries(), saved.users.size());
            for (const auto &user : saved.users) {
                ASSERT_TRUE(net->users.count(user.first));
                EXPECT_EQ(net->users.at(user.first).cell, user.second.cell);
                EXPECT_EQ(net->users.at(user.first).port, user.second.port);
            }
            auto actual = net->users, expected = saved.user_store;
            ASSERT_EQ(actual.capacity(), expected.capacity());
            // Probing copies also detects a reordered free list after rollback.
            for (size_t probe = 0; probe < size_t(saved.user_store.capacity()) + 8; ++probe)
                EXPECT_EQ(actual.add(PortRef{}), expected.add(PortRef{}));
        }
    }

    void expect_slots_and_fixed_cells(Context *ctx, const std::vector<CellInfo *> &cone) const
    {
        ASSERT_EQ(ctx->cells.size(), cells.size());
        ASSERT_EQ(ctx->nets.size(), nets.size());
        std::vector<IdString> actual_cells, actual_nets;
        for (const auto &entry : ctx->cells) actual_cells.push_back(entry.first);
        for (const auto &entry : ctx->nets) actual_nets.push_back(entry.first);
        EXPECT_EQ(actual_cells, cell_order);
        EXPECT_EQ(actual_nets, net_order);
        for (const auto &entry : cells) {
            auto *cell = ctx->cells.at(entry.first).get();
            EXPECT_EQ(cell, entry.second.identity);
            if (std::find(cone.begin(), cone.end(), cell) != cone.end()) continue;
            EXPECT_EQ(cell->type, entry.second.type) << entry.first.str(ctx);
            EXPECT_EQ(cell->bel, entry.second.bel) << entry.first.str(ctx);
            EXPECT_EQ(cell->belStrength, entry.second.strength) << entry.first.str(ctx);
            ASSERT_EQ(cell->params.size(), entry.second.params.size());
            for (const auto &param : entry.second.params) EXPECT_EQ(cell->params.at(param.first), param.second);
            ASSERT_EQ(cell->attrs.size(), entry.second.attrs.size());
            for (const auto &attr : entry.second.attrs) EXPECT_EQ(cell->attrs.at(attr.first), attr.second);
            ASSERT_EQ(cell->pin_data.size(), entry.second.pins.size());
            for (const auto &pin : entry.second.pins) {
                ASSERT_TRUE(cell->pin_data.count(pin.first));
                EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
                EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
            }
            ASSERT_EQ(cell->ports.size(), entry.second.ports.size());
            for (const auto &port : entry.second.ports) {
                ASSERT_TRUE(cell->ports.count(port.first));
                EXPECT_EQ(cell->ports.at(port.first).name, port.second.name);
                EXPECT_EQ(cell->ports.at(port.first).type, port.second.type);
                EXPECT_EQ(cell->ports.at(port.first).net, port.second.net);
                EXPECT_EQ(cell->ports.at(port.first).user_idx, port.second.user_idx);
            }
        }
        for (const auto &entry : nets) {
            auto *net = ctx->nets.at(entry.first).get();
            EXPECT_EQ(net, entry.second.identity);
            EXPECT_EQ(net->driver.cell, entry.second.driver.cell);
            EXPECT_EQ(net->driver.port, entry.second.driver.port);
            ASSERT_EQ(net->users.entries(), entry.second.users.size());
            for (const auto &user : entry.second.users) {
                ASSERT_TRUE(net->users.count(user.first));
                const auto &now = net->users.at(user.first);
                EXPECT_EQ(now.cell->getPort(now.port), net);
                EXPECT_EQ(now.cell->ports.at(now.port).user_idx, user.first);
                if (std::find(cone.begin(), cone.end(), user.second.cell) == cone.end()) {
                    EXPECT_EQ(now.cell, user.second.cell);
                    EXPECT_EQ(now.port, user.second.port);
                }
            }
            auto actual = net->users, expected = entry.second.user_store;
            ASSERT_EQ(actual.capacity(), expected.capacity());
            for (size_t probe = 0; probe < size_t(entry.second.user_store.capacity()) + 8; ++probe)
                EXPECT_EQ(actual.add(PortRef{}), expected.add(PortRef{}));
        }
    }
};

class PlacedReductionTest : public ::testing::Test {
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *clock, *output;
    std::vector<NetInfo *> inputs;
    CellInfo *leaf, *middle, *root, *sink, *unrelated;

    void SetUp() override
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->settings[ctx->id("target_freq")] = 130e6;
        clock = ctx->createNet(ctx->id("generic_fixture_clock"));
        clock->is_global = true;
        clock->clkconstr = std::make_unique<ClockConstraint>();
        clock->clkconstr->period = DelayPair(7692);
        clock->clkconstr->high = clock->clkconstr->low = DelayPair(3846);
        auto *clock_driver = ctx->createCell(ctx->id("generic_clock_source"), id_MISTRAL_CLKBUF);
        clock_driver->addOutput(id_Q);
        clock_driver->connectPort(id_Q, clock);
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        std::vector<CellInfo *> source_ffs;
        for (int bit = 0; bit < 11; ++bit) {
            auto *source = ff(ctx->idf("generic_source_%d", bit), nullptr);
            source_ffs.push_back(source);
            inputs.push_back(source->getPort(id_Q));
        }
        // Active-low intermediate outputs and inverted external pins exercise
        // polarity composition as well as the physically shorter two levels.
        leaf = lut("generic_first", id_MISTRAL_ALUT4, 0x7fff,
                   {inputs[0], inputs[1], inputs[2], inputs[3]});
        leaf->pin_data[id_B].state = PIN_INV;
        middle = lut("generic_middle", id_MISTRAL_ALUT5, 0xffff7fffULL,
                     {inputs[4], inputs[5], inputs[6], inputs[7], leaf->getPort(id_Q)});
        middle->pin_data[id_C].state = PIN_INV;
        root = lut("generic_result", id_MISTRAL_ALUT4, 0x80,
                   {inputs[8], inputs[9], inputs[10], middle->getPort(id_Q)});
        root->pin_data[id_B].state = PIN_INV;
        output = root->getPort(id_Q);
        sink = ff(ctx->id("generic_ena_endpoint"), output);
        unrelated = ff(ctx->id("generic_unrelated_register"), nullptr);
        ctx->assignArchInfo();
        bool clock_bound = false;
        for (auto bel : ctx->getBels())
            if (ctx->checkBelAvail(bel) && ctx->isValidBelForCellType(clock_driver->type, bel)) {
                ctx->bindBel(bel, clock_driver, STRENGTH_LOCKED);
                clock_bound = true;
                break;
            }
        ASSERT_TRUE(clock_bound);
        for (auto *source : source_ffs) ASSERT_NO_FATAL_FAILURE(place(source, 25, 28));
        ASSERT_NO_FATAL_FAILURE(place(leaf, 25, 10));
        ASSERT_NO_FATAL_FAILURE(place(middle, 25, 40));
        ASSERT_NO_FATAL_FAILURE(place(root, 25, 30));
        ASSERT_NO_FATAL_FAILURE(place(sink, 25, 31));
        ASSERT_NO_FATAL_FAILURE(place(unrelated, 30, 20, STRENGTH_STRONG));
        assert_legal();
        ctx->check();
    }

    CellInfo *ff(IdString name, NetInfo *ena)
    {
        auto *cell = ctx->createCell(name, id_MISTRAL_FF);
        for (IdString pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN})
            cell->addInput(pin);
        cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        cell->connectPort(id_CLK, clock);
        if (ena) {
            cell->connectPort(id_ENA, ena);
            cell->pin_data[id_ENA].state = PIN_SIG;
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx->createNet(ctx->idf("%s$q", name.c_str(ctx.get()))));
        // Existing self-feedback is an unrelated consumer of every source net.
        cell->connectPort(id_DATAIN, cell->getPort(id_Q));
        return cell;
    }

    CellInfo *lut(const char *name, IdString type, uint64_t mask, const std::vector<NetInfo *> &ins)
    {
        auto *cell = ctx->createCell(ctx->id(name), type);
        cell->params[id_LUT] = Property(int64_t(mask), 1 << ins.size());
        for (size_t pin = 0; pin < ins.size(); ++pin) {
            cell->addInput(cube_pins[pin]);
            cell->connectPort(cube_pins[pin], ins[pin]);
            cell->pin_data[cube_pins[pin]].state = PIN_SIG;
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx->createNet(ctx->idf("%s$q", name)));
        return cell;
    }

    void place(CellInfo *cell, int x, int y, PlaceStrength strength = STRENGTH_WEAK)
    {
        for (auto bel : ctx->getBelsByTile(x, y)) {
            if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(cell->type, bel)) continue;
            ctx->bindBel(bel, cell, strength);
            if (ctx->isBelLocationValid(bel)) return;
            ctx->unbindBel(bel);
        }
        FAIL() << "No legal fixture BEL for " << cell->name.str(ctx.get()) << " at " << x << "," << y;
    }

    void assert_legal() const
    {
        for (const auto &entry : ctx->cells) {
            ASSERT_NE(entry.second->bel, BelId()) << entry.first.str(ctx.get());
            EXPECT_TRUE(ctx->isBelLocationValid(entry.second->bel)) << entry.first.str(ctx.get());
        }
    }

    bool evaluate(NetInfo *net, unsigned row) const
    {
        for (size_t bit = 0; bit < inputs.size(); ++bit)
            if (net == inputs[bit]) return (row >> bit) & 1;
        auto *cell = net->driver.cell;
        unsigned address = 0;
        for (size_t pin = 0; pin < cell->ports.size() - 1; ++pin) {
            bool value = evaluate(cell->getPort(cube_pins[pin]), row);
            if (cell->get_pin_state(cube_pins[pin]) == PIN_INV) value = !value;
            address |= unsigned(value) << pin;
        }
        return (uint64_t(cell->params.at(id_LUT).as_int64()) >> address) & 1;
    }
};

class TwelveReductionTest : public PlacedReductionTest {
  protected:
    void SetUp() override
    {
        PlacedReductionTest::SetUp();
        auto *extra_source = ff(ctx->id("generic_source_11"), nullptr);
        inputs.push_back(extra_source->getPort(id_Q));
        for (auto *cell : {leaf, middle, root}) {
            ctx->unbindBel(cell->bel);
            for (IdString pin : cube_pins)
                if (cell->ports.count(pin)) {
                    cell->disconnectPort(pin);
                    cell->ports.erase(pin);
                    cell->pin_data.erase(pin);
                }
        }
        // Extend the existing active-low chain by one final literal. Both
        // new six-input leaves must be used after balancing; the public root
        // and all source/sink registers keep their original identities.
        auto rebuild = [&](CellInfo *cell, IdString type, uint64_t mask, const std::vector<NetInfo *> &ins) {
            cell->type = type;
            cell->params[id_LUT] = Property(int64_t(mask), 1 << ins.size());
            for (size_t pin = 0; pin < ins.size(); ++pin) {
                cell->addInput(cube_pins[pin]);
                cell->connectPort(cube_pins[pin], ins[pin]);
                cell->pin_data[cube_pins[pin]].state = PIN_SIG;
            }
        };
        rebuild(leaf, id_MISTRAL_ALUT4, 0x7fff, {inputs[0], inputs[1], inputs[2], inputs[3]});
        rebuild(middle, id_MISTRAL_ALUT5, 0xffff7fffULL,
                {inputs[4], inputs[5], inputs[6], inputs[7], leaf->getPort(id_Q)});
        rebuild(root, id_MISTRAL_ALUT5, 0x8000,
                {inputs[8], inputs[9], inputs[10], inputs[11], middle->getPort(id_Q)});
        leaf->pin_data[id_B].state = PIN_INV;
        middle->pin_data[id_C].state = PIN_INV;
        root->pin_data[id_B].state = PIN_INV;
        auto holes = [&](NetInfo *net) {
            auto a = net->users.add(PortRef{}), b = net->users.add(PortRef{}), c = net->users.add(PortRef{});
            net->users.remove(a); net->users.remove(c); net->users.remove(b);
        };
        for (auto *net : inputs) holes(net);
        for (auto *cell : {leaf, middle, root}) holes(cell->getPort(id_Q));
        const auto public_result = ctx->id("twelve_public_result"), public_literal = ctx->id("twelve_public_literal");
        ctx->ports[public_result] = PortInfo{public_result, output, PORT_OUT, {}};
        ctx->ports[public_literal] = PortInfo{public_literal, inputs[11], PORT_OUT, {}};
        ctx->assignArchInfo();
        ASSERT_NO_FATAL_FAILURE(place(extra_source, 25, 29));
        ASSERT_NO_FATAL_FAILURE(place(leaf, 25, 10));
        ASSERT_NO_FATAL_FAILURE(place(middle, 25, 40));
        ASSERT_NO_FATAL_FAILURE(place(root, 25, 30));
        assert_legal();
        ctx->check();
    }
};

// A test-side transaction exposes real timing on the future graph, then
// restores the source before calling the production search. It neither uses
// its shortlist nor reproduces its score function.
struct NarrowTimingProbe {
    struct Saved {
        CellInfo *cell;
        IdString type;
        decltype(CellInfo::ports) ports;
        decltype(CellInfo::params) params;
        decltype(CellInfo::pin_data) pins;
        BelId bel;
        PlaceStrength strength;
    };
    Context *ctx;
    const ReductionBalancePlan &plan;
    std::vector<Saved> saved;
    std::map<NetInfo *, indexed_store<PortRef>> users;
    std::map<NetInfo *, PortRef> drivers;

    NarrowTimingProbe(Context *ctx, const ReductionBalancePlan &plan) : ctx(ctx), plan(plan)
    {
        NPNR_ASSERT(plan.cells.size() == 3 && plan.retired.empty());
        for (auto *cell : plan.cells)
            saved.push_back({cell, cell->type, cell->ports, cell->params, cell->pin_data,
                             cell->bel, cell->belStrength});
        for (const auto &entry : ctx->nets) {
            users.emplace(entry.second.get(), entry.second->users);
            drivers.emplace(entry.second.get(), entry.second->driver);
        }
        for (auto *cell : plan.cells) ctx->unbindBel(cell->bel);
        rewrite_reduction(ctx, plan);
        ctx->assignArchInfo();
        for (const auto &cell : saved)
            if (cell.cell == plan.root) ctx->bindBel(cell.bel, plan.root, cell.strength);
    }

    void unbind_leaves()
    {
        for (auto *cell : plan.leaves)
            if (cell->bel != BelId()) ctx->unbindBel(cell->bel);
    }

    bool bind_pair(BelId a, BelId b)
    {
        unbind_leaves();
        if (a == b || !ctx->checkBelAvail(a) || !ctx->checkBelAvail(b)) return false;
        ctx->bindBel(a, plan.leaves[0], STRENGTH_WEAK);
        ctx->bindBel(b, plan.leaves[1], STRENGTH_WEAK);
        for (const auto &entry : ctx->cells)
            if (entry.second->bel != BelId() && !ctx->isBelLocationValid(entry.second->bel)) return false;
        return true;
    }

    ~NarrowTimingProbe()
    {
        unbind_leaves();
        ctx->unbindBel(plan.root->bel);
        for (const auto &cell : saved) {
            cell.cell->type = cell.type;
            cell.cell->ports = cell.ports;
            cell.cell->params = cell.params;
            cell.cell->pin_data = cell.pins;
        }
        for (auto &entry : users) std::swap(entry.first->users, entry.second);
        for (const auto &entry : drivers) entry.first->driver = entry.second;
        ctx->assignArchInfo();
        for (const auto &cell : saved) ctx->bindBel(cell.bel, cell.cell, cell.strength);
    }
};

class NarrowSearchTest : public TwelveReductionTest {
  protected:
    using Lab = std::pair<int, int>;
    static constexpr int compact_x = 25, compact_y = 28;
    CellInfo *preserved_buf = nullptr;
    std::vector<CellInfo *> blockers;

    Lab lab(BelId bel) const
    {
        const auto at = ctx->getBelLocation(bel);
        return {at.x, at.y};
    }

    std::string ff_tile_diagnostic(CellInfo *source, const std::vector<Lab> &tiles)
    {
        NPNR_ASSERT(source->bel == BelId());
        std::ostringstream result;
        for (const auto &tile : tiles) {
            const auto bels = ctx->getBelsByTile(tile.first, tile.second);
            unsigned ff_bels = 0, valid_type = 0, free = 0, legal = 0;
            for (auto bel : bels) {
                if (ctx->getBelType(bel) == id_MISTRAL_FF) ++ff_bels;
                if (!ctx->isValidBelForCellType(source->type, bel)) continue;
                ++valid_type;
                if (!ctx->checkBelAvail(bel)) continue;
                ++free;
                ctx->bindBel(bel, source, STRENGTH_WEAK);
                if (ctx->isBelLocationValid(bel)) ++legal;
                ctx->unbindBel(bel);
            }
            result << "Ranking fixture FF tile " << tile.first << ',' << tile.second << ": bels=" << bels.size()
                   << " ff=" << ff_bels << " valid_type=" << valid_type << " free=" << free
                   << " legal=" << legal << '\n';
        }
        return result.str();
    }

    bool empty_alm(BelId bel) const
    {
        const auto at = ctx->getBelLocation(bel);
        for (const auto &entry : ctx->cells) {
            if (entry.second->bel == BelId()) continue;
            const auto other = ctx->getBelLocation(entry.second->bel);
            if (other.x == at.x && other.y == at.y && other.z / 6 == at.z / 6) return false;
        }
        return true;
    }

    void place_empty_alm(CellInfo *cell)
    {
        for (auto bel : ctx->getBelsByTile(compact_x, compact_y)) {
            if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(cell->type, bel) || !empty_alm(bel)) continue;
            ctx->bindBel(bel, cell, STRENGTH_WEAK);
            if (ctx->isBelLocationValid(bel)) return;
            ctx->unbindBel(bel);
        }
        FAIL() << "No completely empty ALM for " << cell->name.str(ctx.get());
    }

    CellInfo *constant_blocker()
    {
        const std::string name = "generic_search_blocker_" + std::to_string(blockers.size());
        auto *cell = lut(name.c_str(), id_MISTRAL_ALUT2, 0, {inputs[0], inputs[1]});
        for (IdString pin : {id_A, id_B}) {
            cell->disconnectPort(pin);
            cell->pin_data[pin].state = PIN_0;
        }
        blockers.push_back(cell);
        ctx->assign_comb_info(cell);
        return cell;
    }

    void compact_with_blocked_upgrades(unsigned spare_alms)
    {
        for (auto *cell : {leaf, middle, root}) ctx->unbindBel(cell->bel);
        auto *extra = inputs[11]->driver.cell;
        ctx->unbindBel(extra->bel);
        ASSERT_NO_FATAL_FAILURE(place(extra, compact_x, compact_y));
        ASSERT_NO_FATAL_FAILURE(place(leaf, compact_x, compact_y));
        ASSERT_NO_FATAL_FAILURE(place(middle, compact_x, compact_y));
        ASSERT_NO_FATAL_FAILURE(place(root, compact_x, compact_y));
        ctx->unbindBel(leaf->bel);
        ctx->unbindBel(middle->bel);
        ASSERT_NO_FATAL_FAILURE(place_empty_alm(leaf));
        ASSERT_NO_FATAL_FAILURE(place_empty_alm(middle));
        for (auto *original : {leaf, middle}) {
            const auto at = ctx->getBelLocation(original->bel);
            auto *blocker = constant_blocker();
            bool placed = false;
            for (auto bel : ctx->getBelsByTile(at.x, at.y)) {
                if (ctx->getBelLocation(bel).z != (at.z ^ 1)) continue;
                ASSERT_TRUE(ctx->checkBelAvail(bel));
                ASSERT_TRUE(ctx->isValidBelForCellType(blocker->type, bel));
                ctx->bindBel(bel, blocker, STRENGTH_WEAK);
                ASSERT_TRUE(ctx->isBelLocationValid(bel));
                placed = true;
                break;
            }
            ASSERT_TRUE(placed);
        }
        // Reserve genuinely FF-empty ALMs. Constant, weak unrelated LUTs
        // block every other COMB-empty ALM without consuming extra LAB inputs
        // or making the LAB protected. The two old child sites cannot upgrade.
        std::set<int> reserved;
        for (auto bel : ctx->getBelsByTile(compact_x, compact_y)) {
            if (!ctx->isValidBelForCellType(id_MISTRAL_ALUT6, bel) || !empty_alm(bel)) continue;
            reserved.insert(ctx->getBelLocation(bel).z / 6);
            if (reserved.size() == spare_alms) break;
        }
        ASSERT_EQ(reserved.size(), spare_alms);
        for (int alm = 0; alm < 10; ++alm) {
            if (reserved.count(alm)) continue;
            bool occupied = false;
            for (auto bel : ctx->getBelsByTile(compact_x, compact_y))
                if (ctx->getBelLocation(bel).z / 6 == alm &&
                    ctx->isValidBelForCellType(id_MISTRAL_ALUT2, bel) && !ctx->checkBelAvail(bel)) occupied = true;
            if (occupied) continue;
            auto *blocker = constant_blocker();
            bool placed = false;
            for (auto bel : ctx->getBelsByTile(compact_x, compact_y)) {
                if (ctx->getBelLocation(bel).z / 6 != alm || !ctx->checkBelAvail(bel) ||
                    !ctx->isValidBelForCellType(blocker->type, bel)) continue;
                ctx->bindBel(bel, blocker, STRENGTH_WEAK);
                if (ctx->isBelLocationValid(bel)) { placed = true; break; }
                ctx->unbindBel(bel);
            }
            ASSERT_TRUE(placed) << "Cannot block unrelated ALM " << alm;
        }
        preserved_buf = ctx->createCell(ctx->id("generic_preserved_buffer"), id_MISTRAL_BUF);
        preserved_buf->addInput(id_A);
        preserved_buf->connectPort(id_A, unrelated->getPort(id_Q));
        preserved_buf->addOutput(id_Q);
        preserved_buf->connectPort(id_Q, ctx->createNet(ctx->id("generic_preserved_buffer$q")));
        ctx->assignArchInfo();
        // BUF is a late route-through cell, intentionally excluded from the
        // ordinary placer type filter. Follow the backend's direct COMB bind
        // and still require full physical legality and complete pin state.
        for (auto bel : ctx->getBelsByTile(30, 21)) {
            if (!ctx->checkBelAvail(bel) || !ctx->getBelType(bel).in(id_MISTRAL_COMB, id_MISTRAL_MCOMB)) continue;
            ctx->bindBel(bel, preserved_buf, STRENGTH_WEAK);
            if (ctx->isBelLocationValid(bel)) break;
            ctx->unbindBel(bel);
        }
        ASSERT_NE(preserved_buf->bel, BelId());
        assert_legal();
        ctx->check();
    }

    std::vector<BelId> representatives(NarrowTimingProbe &probe, CellInfo *child)
    {
        probe.unbind_leaves();
        std::vector<BelId> sites;
        const std::vector<Lab> labs{{compact_x, compact_y}, {compact_x - 1, compact_y},
                                  {compact_x + 1, compact_y}, {compact_x, compact_y - 1},
                                  {compact_x, compact_y + 1}};
        for (auto tile : labs) {
            std::set<int> alms;
            for (auto bel : ctx->getBelsByTile(tile.first, tile.second)) {
                const auto at = ctx->getBelLocation(bel);
                if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(child->type, bel) || alms.count(at.z / 6)) continue;
                ctx->bindBel(bel, child, STRENGTH_WEAK);
                const bool legal = ctx->isBelLocationValid(bel);
                ctx->unbindBel(bel);
                if (!legal) continue;
                alms.insert(at.z / 6);
                sites.push_back(bel);
                if (alms.size() == 2) break;
            }
        }
        EXPECT_LE(sites.size(), 12u);
        return sites;
    }

    std::pair<BelId, BelId> first_logged_pair(const std::string &log)
    {
        const std::regex pattern("Placed reduction trial root=" + root->name.str(ctx.get()) +
                                 " leaves=([^, ]+),([^ ]+) ");
        std::smatch match;
        if (!std::regex_search(log, match, pattern)) {
            ADD_FAILURE() << "No actual timed tuple in log:\n" << log;
            return {};
        }
        std::pair<BelId, BelId> result;
        for (auto bel : ctx->getBels()) {
            if (match[1].str() == ctx->nameOfBel(bel)) result.first = bel;
            if (match[2].str() == ctx->nameOfBel(bel)) result.second = bel;
        }
        return result;
    }
};
}

TEST(PlacedReductionDiagnostic, DisabledDoesNotInternIdentifiers)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    Context ctx(args);
    auto before = ctx.id("placed-reduction-disabled-before");
    diagnostic_placed_reduction(&ctx, nullptr);
    diagnostic_placed_reduction(&ctx, "");
    auto after = ctx.id("placed-reduction-disabled-after");
    EXPECT_EQ(after.index, before.index + 1);
    EXPECT_TRUE(ctx.cells.empty());
    EXPECT_TRUE(ctx.nets.empty());
}

TEST_F(PlacedReductionTest, ListOnlyRestoresEveryLogicalAndPhysicalProperty)
{
    CubeSnapshot before(ctx.get());
    EXPECT_FALSE(placed_reduction(ctx.get(), root->name.str(ctx.get()), 2, -1));
    before.expect_exact(ctx.get());
    assert_legal();
    ctx->check();
}

TEST_F(PlacedReductionTest, AppliesEquivalentTwoLevelsWithFixedRegistersAndConsumerSlots)
{
    CubeSnapshot before(ctx.get());
    std::vector<bool> values;
    const unsigned required = ((1u << 11) - 1) ^ (1u << 1) ^ (1u << 6) ^ (1u << 9);
    for (unsigned row = 0; row < (1u << 11); ++row) {
        values.push_back(evaluate(output, row));
        ASSERT_EQ(values.back(), row == required);
    }
    TimingAnalyser timing_before(ctx.get());
    timing_before.setup(false, false, true);
    ASSERT_TRUE(timing_before.get_timing_result().min_delay_violations.empty());
    auto slack = timing_before.get_setup_slack(CellPortKey(sink->name, id_ENA));
    auto root_bel = root->bel;
    auto root_strength = root->belStrength;
    ASSERT_TRUE(placed_reduction(ctx.get(), root->name.str(ctx.get()), 2, 0));
    EXPECT_EQ(root->type, id_MISTRAL_ALUT2);
    EXPECT_EQ(root->bel, root_bel);
    EXPECT_EQ(root->belStrength, root_strength);
    EXPECT_EQ(root->getPort(id_Q), output);
    before.expect_slots_and_fixed_cells(ctx.get(), {leaf, middle, root});
    for (unsigned row = 0; row < (1u << 11); ++row)
        EXPECT_EQ(evaluate(output, row), values[row]) << "assignment " << row;
    TimingAnalyser timing_after(ctx.get());
    timing_after.setup(false, false, true);
    EXPECT_TRUE(timing_after.get_timing_result().min_delay_violations.empty());
    EXPECT_GE(timing_after.get_setup_slack(CellPortKey(sink->name, id_ENA)) - slack, 250);
    const auto &fmax_after = timing_after.get_timing_result().clock_fmax;
    for (const auto &entry : timing_before.get_timing_result().clock_fmax) {
        ASSERT_TRUE(fmax_after.count(entry.first));
        EXPECT_GE(fmax_after.at(entry.first).achieved + 0.001f, entry.second.achieved);
    }
    assert_legal();
    ctx->check();
}

TEST_F(TwelveReductionTest, ListingRestoresEveryCellNetPinAndIndexedFreeList)
{
    ReductionBalancePlan plan;
    ASSERT_TRUE(plan_reduction(ctx.get(), root->name.str(ctx.get()), true, plan));
    ASSERT_EQ(plan.literals.size(), 12u);
    ASSERT_EQ(plan.cells.size(), 3u);
    CubeSnapshot before(ctx.get());
    EXPECT_FALSE(placed_reduction(ctx.get(), root->name.str(ctx.get()), 2, -1));
    before.expect_exact(ctx.get());
    EXPECT_EQ(ctx->ports.at(ctx->id("twelve_public_result")).net, output);
    EXPECT_EQ(ctx->ports.at(ctx->id("twelve_public_literal")).net, inputs[11]);
    assert_legal();
    ctx->check();
}

TEST_F(TwelveReductionTest, SelectedRewriteIsExhaustiveAndPreservesEveryRegisterAndPublicRoot)
{
    const unsigned required = ((1u << 12) - 1) ^ (1u << 1) ^ (1u << 6) ^ (1u << 9);
    for (unsigned row = 0; row < (1u << 12); ++row)
        ASSERT_EQ(evaluate(output, row), row == required) << "before assignment " << row;
    CubeSnapshot before(ctx.get());
    TimingAnalyser timing_before(ctx.get());
    timing_before.setup(false, false, true);
    ASSERT_TRUE(timing_before.get_timing_result().min_delay_violations.empty());
    const auto root_port = CellPortKey(root->name, id_Q), sink_port = CellPortKey(sink->name, id_ENA);
    const auto branch_slack = timing_before.get_setup_slack(root_port);
    const auto endpoint_slack = timing_before.get_setup_slack(sink_port);
    ASSERT_TRUE(placed_reduction(ctx.get(), root->name.str(ctx.get()), 2, 0));
    EXPECT_EQ(root->type, id_MISTRAL_ALUT2);
    EXPECT_EQ(leaf->type, id_MISTRAL_ALUT6);
    EXPECT_EQ(middle->type, id_MISTRAL_ALUT6);
    EXPECT_EQ(root->bel, before.cells.at(root->name).bel);
    EXPECT_EQ(root->belStrength, before.cells.at(root->name).strength);
    EXPECT_EQ(root->getPort(id_Q), output);
    EXPECT_EQ(ctx->ports.at(ctx->id("twelve_public_result")).net, output);
    EXPECT_EQ(ctx->ports.at(ctx->id("twelve_public_literal")).net, inputs[11]);
    before.expect_slots_and_fixed_cells(ctx.get(), {leaf, middle, root});
    for (const auto &entry : before.cells) {
        const auto &saved = entry.second;
        if (saved.type != id_MISTRAL_FF) continue;
        auto *cell = ctx->cells.at(entry.first).get();
        EXPECT_EQ(cell->type, saved.type);
        ASSERT_EQ(cell->params.size(), saved.params.size());
        for (const auto &param : saved.params) EXPECT_EQ(cell->params.at(param.first), param.second);
        ASSERT_EQ(cell->pin_data.size(), saved.pins.size());
        for (const auto &pin : saved.pins) {
            ASSERT_TRUE(cell->pin_data.count(pin.first));
            EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
            EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
        }
    }
    for (unsigned row = 0; row < (1u << 12); ++row)
        EXPECT_EQ(evaluate(output, row), row == required) << "after assignment " << row;
    TimingAnalyser timing_after(ctx.get());
    timing_after.setup(false, false, true);
    EXPECT_GE(timing_after.get_setup_slack(root_port) - branch_slack, 250);
    EXPECT_GE(timing_after.get_setup_slack(sink_port), endpoint_slack);
    EXPECT_TRUE(timing_after.get_timing_result().min_delay_violations.empty());
    const auto &fmax_after = timing_after.get_timing_result().clock_fmax;
    for (const auto &entry : timing_before.get_timing_result().clock_fmax) {
        ASSERT_TRUE(fmax_after.count(entry.first));
        EXPECT_GE(fmax_after.at(entry.first).achieved + 0.001f, entry.second.achieved);
    }
    assert_legal();
    ctx->check();
}

TEST_F(TwelveReductionTest, PositiveSubDefaultGainNeedsItsExplicitMinimum)
{
    // Compact the generic cone, including all twelve launch registers, into
    // one LAB. With real architecture arcs the original critical chain has
    // 512 + (300 + 97) + (300 + 97) ps after its first input route; the
    // balanced chain has 605 + 300 + 400 ps. That leaves a genuine but tiny
    // positive opportunity. Any trial outside this LAB pays extra routing.
    for (auto *cell : {leaf, middle, root}) ctx->unbindBel(cell->bel);
    auto *extra_source = inputs[11]->driver.cell;
    ctx->unbindBel(extra_source->bel);
    ASSERT_NO_FATAL_FAILURE(place(extra_source, 25, 28));
    ASSERT_NO_FATAL_FAILURE(place(leaf, 25, 28));
    ASSERT_NO_FATAL_FAILURE(place(middle, 25, 28));
    ASSERT_NO_FATAL_FAILURE(place(root, 25, 28));
    // Keep an original child site in each of two entirely empty ALMs. The
    // per-LAB shortlist retains those original sites. Merely taking the two
    // first free halves can choose the same ALM; two ALUT6s cannot share its
    // 64 LUT bits, even when both sites are individually legal.
    ctx->unbindBel(leaf->bel);
    ctx->unbindBel(middle->bel);
    auto place_in_empty_alm = [&](CellInfo *cell) {
        for (auto bel : ctx->getBelsByTile(25, 28)) {
            if (!ctx->checkBelAvail(bel) || !ctx->isValidBelForCellType(cell->type, bel)) continue;
            const auto at = ctx->getBelLocation(bel);
            bool empty = true;
            for (const auto &entry : ctx->cells) {
                if (entry.second->bel == BelId()) continue;
                const auto other = ctx->getBelLocation(entry.second->bel);
                if (other.x == at.x && other.y == at.y && other.z / 6 == at.z / 6) empty = false;
            }
            if (!empty) continue;
            ctx->bindBel(bel, cell, STRENGTH_WEAK);
            if (ctx->isBelLocationValid(bel)) return true;
            ctx->unbindBel(bel);
        }
        return false;
    };
    ASSERT_TRUE(place_in_empty_alm(leaf));
    ASSERT_TRUE(place_in_empty_alm(middle));
    EXPECT_NE(ctx->getBelLocation(leaf->bel).z / 6, ctx->getBelLocation(middle->bel).z / 6);
    assert_legal();
    ctx->check();
    CubeSnapshot before(ctx.get());
    TimingAnalyser timing_before(ctx.get());
    timing_before.setup(false, false, true);
    ASSERT_TRUE(timing_before.get_timing_result().min_delay_violations.empty());
    const auto root_port = CellPortKey(root->name, id_Q), sink_port = CellPortKey(sink->name, id_ENA);
    const auto branch_slack = timing_before.get_setup_slack(root_port);
    const auto endpoint_slack = timing_before.get_setup_slack(sink_port);
    std::vector<bool> values;
    for (unsigned row = 0; row < (1u << 12); ++row) values.push_back(evaluate(output, row));

    // The old three-field diagnostic and explicit default must both reject
    // and restore exactly. A lower positive threshold changes admission only.
    const auto diagnostic = root->name.str(ctx.get()) + " 1 0";
    EXPECT_THROW(diagnostic_placed_reduction(ctx.get(), diagnostic.c_str()), log_execution_error_exception);
    before.expect_exact(ctx.get());
    EXPECT_THROW(diagnostic_placed_reduction(ctx.get(), (diagnostic + " 250").c_str()),
                 log_execution_error_exception);
    before.expect_exact(ctx.get());
    PlacedReductionLog evidence;
    for (auto *cell : {clock->driver.cell, leaf, middle, root, extra_source, unrelated}) {
        auto at = ctx->getBelLocation(cell->bel);
        evidence.stream << "Fixture " << cell->name.str(ctx.get()) << " @ " << at.x << ',' << at.y << ',' << at.z
                        << " strength=" << int(cell->belStrength) << '\n';
    }
    ASSERT_NO_THROW(diagnostic_placed_reduction(ctx.get(), (diagnostic + " 1").c_str())) << evidence.stream.str();
    ASSERT_EQ(root->type, id_MISTRAL_ALUT2);
    EXPECT_EQ(leaf->type, id_MISTRAL_ALUT6);
    EXPECT_EQ(middle->type, id_MISTRAL_ALUT6);
    EXPECT_EQ(root->bel, before.cells.at(root->name).bel);
    EXPECT_EQ(root->getPort(id_Q), output);
    before.expect_slots_and_fixed_cells(ctx.get(), {leaf, middle, root});
    for (const auto &entry : before.cells) {
        if (entry.second.type != id_MISTRAL_FF) continue;
        auto *cell = ctx->cells.at(entry.first).get();
        ASSERT_EQ(cell->pin_data.size(), entry.second.pins.size());
        for (const auto &pin : entry.second.pins) {
            ASSERT_TRUE(cell->pin_data.count(pin.first));
            EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
            EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
        }
    }
    for (unsigned row = 0; row < (1u << 12); ++row)
        EXPECT_EQ(evaluate(output, row), values[row]) << "assignment " << row;
    TimingAnalyser timing_after(ctx.get());
    timing_after.setup(false, false, true);
    const auto branch_gain = timing_after.get_setup_slack(root_port) - branch_slack;
    EXPECT_GE(branch_gain, 1);
    EXPECT_LT(branch_gain, 250);
    EXPECT_GE(timing_after.get_setup_slack(sink_port), endpoint_slack);
    EXPECT_TRUE(timing_after.get_timing_result().min_delay_violations.empty());
    for (const auto &clock_fmax : timing_before.get_timing_result().clock_fmax) {
        const auto &after = timing_after.get_timing_result().clock_fmax;
        ASSERT_TRUE(after.count(clock_fmax.first));
        EXPECT_GE(after.at(clock_fmax.first).achieved + 0.001f, clock_fmax.second.achieved);
    }
    EXPECT_EQ(ctx->ports.at(ctx->id("twelve_public_result")).net, output);
    EXPECT_EQ(ctx->ports.at(ctx->id("twelve_public_literal")).net, inputs[11]);
    assert_legal();
    ctx->check();
}

TEST_F(NarrowSearchTest, BlockedOriginalUpgradesStillReachTwoSameLabAlms)
{
    ASSERT_NO_FATAL_FAILURE(compact_with_blocked_upgrades(2));
    ReductionBalancePlan plan;
    ASSERT_TRUE(plan_reduction(ctx.get(), root->name.str(ctx.get()), true, plan));
    ASSERT_EQ(plan.literals.size(), 12u);
    CubeSnapshot before(ctx.get());
    {
        NarrowTimingProbe probe(ctx.get(), plan);
        const auto a = representatives(probe, plan.leaves[0]);
        const auto b = representatives(probe, plan.leaves[1]);
        const Lab home{compact_x, compact_y};
        ASSERT_EQ(std::count_if(a.begin(), a.end(), [&](BelId bel) { return lab(bel) == home; }), 2);
        ASSERT_EQ(std::count_if(b.begin(), b.end(), [&](BelId bel) { return lab(bel) == home; }), 2);
        const auto first = *std::find_if(a.begin(), a.end(), [&](BelId bel) { return lab(bel) == home; });
        const auto second = *std::find_if(b.begin(), b.end(), [&](BelId bel) {
            return lab(bel) == home && ctx->getBelLocation(bel).z / 6 != ctx->getBelLocation(first).z / 6;
        });
        ASSERT_TRUE(probe.bind_pair(first, second));
        EXPECT_FALSE(probe.bind_pair(before.cells.at(plan.leaves[0]->name).bel, second));
        EXPECT_FALSE(probe.bind_pair(first, before.cells.at(plan.leaves[1]->name).bel));
    }
    before.expect_exact(ctx.get());

    // Both original sites are blocked for their future ALUT6s. The obsolete
    // quota counted the two halves of the first empty ALM and never reached
    // the next jointly legal same-LAB pair. That loses this real 1 ps gain.
    std::string listing;
    {
        PlacedReductionLog evidence;
        EXPECT_FALSE(placed_reduction(ctx.get(), root->name.str(ctx.get()), 1, -1, 1));
        listing = evidence.stream.str();
    }
    before.expect_exact(ctx.get());
    const auto first = first_logged_pair(listing);
    ASSERT_NE(first.first, BelId()) << listing;
    ASSERT_NE(first.second, BelId()) << listing;
    EXPECT_EQ(lab(first.first), (Lab{compact_x, compact_y}));
    EXPECT_EQ(lab(first.second), (Lab{compact_x, compact_y}));
    EXPECT_NE(ctx->getBelLocation(first.first).z / 6, ctx->getBelLocation(first.second).z / 6);
    ASSERT_NE(listing.find("Placed reduction candidate 0,"), std::string::npos) << listing;

    TimingAnalyser timing_before(ctx.get());
    timing_before.setup(false, false, true);
    ASSERT_TRUE(timing_before.get_timing_result().min_delay_violations.empty());
    const auto old_branch = timing_before.get_setup_slack(CellPortKey(root->name, id_Q));
    const auto old_endpoint = timing_before.get_setup_slack(CellPortKey(sink->name, id_ENA));
    const unsigned required = ((1u << 12) - 1) ^ (1u << 1) ^ (1u << 6) ^ (1u << 9);
    for (unsigned row = 0; row < 4096; ++row) ASSERT_EQ(evaluate(output, row), row == required);
    ASSERT_TRUE(placed_reduction(ctx.get(), root->name.str(ctx.get()), 1, 0, 1));
    EXPECT_EQ(plan.leaves[0]->bel, first.first);
    EXPECT_EQ(plan.leaves[1]->bel, first.second);
    EXPECT_EQ(root->bel, before.cells.at(root->name).bel);
    before.expect_slots_and_fixed_cells(ctx.get(), {leaf, middle, root});
    ASSERT_EQ(preserved_buf->type, id_MISTRAL_BUF);
    for (unsigned row = 0; row < 4096; ++row) EXPECT_EQ(evaluate(output, row), row == required) << row;
    EXPECT_EQ(ctx->ports.at(ctx->id("twelve_public_result")).net, output);
    EXPECT_EQ(ctx->ports.at(ctx->id("twelve_public_literal")).net, inputs[11]);
    TimingAnalyser timing_after(ctx.get());
    timing_after.setup(false, false, true);
    const auto gain = timing_after.get_setup_slack(CellPortKey(root->name, id_Q)) - old_branch;
    EXPECT_GE(gain, 1);
    EXPECT_LT(gain, 250);
    EXPECT_GE(timing_after.get_setup_slack(CellPortKey(sink->name, id_ENA)), old_endpoint);
    EXPECT_TRUE(timing_after.get_timing_result().min_delay_violations.empty());
    for (const auto &clock_fmax : timing_before.get_timing_result().clock_fmax) {
        const auto &after = timing_after.get_timing_result().clock_fmax;
        ASSERT_TRUE(after.count(clock_fmax.first));
        EXPECT_GE(after.at(clock_fmax.first).achieved + 0.001f, clock_fmax.second.achieved);
    }
    assert_legal();
    ctx->check();
}

TEST_F(NarrowSearchTest, FutureAlut2ArcsChooseTheActualFasterFirstTuple)
{
    ASSERT_NO_FATAL_FAILURE(compact_with_blocked_upgrades(1));
    ReductionBalancePlan plan;
    ASSERT_TRUE(plan_reduction(ctx.get(), root->name.str(ctx.get()), true, plan));
    ASSERT_EQ(plan.literals.size(), 12u);
    // Derive the future second group's D literal from the actual canonical
    // policy order. Its diagonal launch site makes the two objectives disagree.
    auto *diagonal_source = plan.literals.at(6 + 3).first->driver.cell;
    ASSERT_EQ(diagonal_source->type, id_MISTRAL_FF);
    ctx->unbindBel(diagonal_source->bel);
    // sx120f has an M10K column at x26; use its adjacent real LAB at
    // x24 for the mirrored diagonal. Keep the bounded actual grid/legality
    // diagnostic so a missing tile differs visibly from a rejected FF.
    const auto tile_diagnostic = ff_tile_diagnostic(diagonal_source,
        {{compact_x - 1, compact_y + 1}, {compact_x + 1, compact_y + 1},
         {compact_x, compact_y + 1}, {compact_x + 2, compact_y + 1}});
    std::fputs(tile_diagnostic.c_str(), stderr);
    ASSERT_NO_FATAL_FAILURE(place(diagonal_source, compact_x - 1, compact_y + 1)) << tile_diagnostic;
    // Long original stages ensure the genuinely faster first tuple also
    // qualifies with the unchanged default margin and safety guards.
    ctx->unbindBel(leaf->bel);
    ctx->unbindBel(middle->bel);
    ASSERT_NO_FATAL_FAILURE(place(leaf, compact_x, 10));
    ASSERT_NO_FATAL_FAILURE(place(middle, compact_x, 40));
    assert_legal();
    ctx->check();
    ASSERT_TRUE(plan_reduction(ctx.get(), root->name.str(ctx.get()), true, plan));
    CellInfo future_root(ctx.get(), root->name, id_MISTRAL_ALUT2);
    DelayQuad a_arc, b_arc, original_a, original_b;
    ASSERT_TRUE(ctx->getCellDelay(&future_root, id_A, id_Q, a_arc));
    ASSERT_TRUE(ctx->getCellDelay(&future_root, id_B, id_Q, b_arc));
    ASSERT_TRUE(ctx->getCellDelay(root, id_A, id_Q, original_a));
    ASSERT_TRUE(ctx->getCellDelay(root, id_B, id_Q, original_b));
    EXPECT_EQ(a_arc.maxDelay(), 400);
    EXPECT_EQ(b_arc.maxDelay(), 97);
    EXPECT_NE(a_arc.maxDelay(), original_a.maxDelay());
    EXPECT_NE(b_arc.maxDelay(), original_b.maxDelay());
    struct Measurement { BelId a, b; delay_t leaf_only, root_arrival; };
    std::vector<Measurement> measurements;
    CubeSnapshot before(ctx.get());
    {
        NarrowTimingProbe probe(ctx.get(), plan);
        const auto a_sites = representatives(probe, plan.leaves[0]);
        const auto b_sites = representatives(probe, plan.leaves[1]);
        const Lab home{compact_x, compact_y};
        ASSERT_EQ(std::count_if(a_sites.begin(), a_sites.end(), [&](BelId bel) { return lab(bel) == home; }), 1);
        ASSERT_EQ(std::count_if(b_sites.begin(), b_sites.end(), [&](BelId bel) { return lab(bel) == home; }), 1);
        std::set<std::pair<Lab, Lab>> measured;
        for (auto a : a_sites) {
            for (auto b : b_sites) {
                const auto geometry = std::make_pair(lab(a), lab(b));
                if (measured.count(geometry) || !probe.bind_pair(a, b)) continue;
                measured.insert(geometry);
                TimingAnalyser timing(ctx.get());
                timing.setup(false, false, true);
                delay_t at_a, at_b, at_root;
                ASSERT_TRUE(timing.get_max_arrival(CellPortKey(plan.leaves[0]->name, id_Q), at_a));
                ASSERT_TRUE(timing.get_max_arrival(CellPortKey(plan.leaves[1]->name, id_Q), at_b));
                ASSERT_TRUE(timing.get_max_arrival(CellPortKey(root->name, id_Q), at_root));
                const auto without_root = std::max(
                    at_a + ctx->predictArcDelay(plan.leaves[0]->getPort(id_Q), {root, id_A}),
                    at_b + ctx->predictArcDelay(plan.leaves[1]->getPort(id_Q), {root, id_B}));
                measurements.push_back({a, b, without_root, at_root});
            }
        }
        ASSERT_LE(measurements.size(), 25u);
        const unsigned required = ((1u << 12) - 1) ^ (1u << 1) ^ (1u << 6) ^ (1u << 9);
        for (unsigned row = 0; row < 4096; ++row) EXPECT_EQ(evaluate(output, row), row == required) << row;
    }
    before.expect_exact(ctx.get());
    ASSERT_FALSE(measurements.empty());
    const auto fastest = std::min_element(measurements.begin(), measurements.end(),
        [](const Measurement &a, const Measurement &b) { return a.root_arrival < b.root_arrival; });
    const auto old_best = std::min_element(measurements.begin(), measurements.end(),
        [](const Measurement &a, const Measurement &b) { return a.leaf_only < b.leaf_only; });
    // The five root-neighborhood LABs contain a measured rank reversal, not
    // merely the two delay constants. The logged production prefix below
    // independently checks its choice despite the additional far-old-site LABs.
    for (const auto &row : measurements) {
        if (row.leaf_only == old_best->leaf_only) {
            EXPECT_GE(row.root_arrival - fastest->root_arrival, 80);
        }
    }
    EXPECT_EQ(lab(fastest->a), (Lab{compact_x, compact_y}));
    EXPECT_EQ(lab(fastest->b), (Lab{compact_x - 1, compact_y}));
    std::string listing;
    {
        PlacedReductionLog evidence;
        EXPECT_FALSE(placed_reduction(ctx.get(), root->name.str(ctx.get()), 1, -1));
        listing = evidence.stream.str();
    }
    before.expect_exact(ctx.get());
    const auto actual = first_logged_pair(listing);
    ASSERT_NE(actual.first, BelId()) << listing;
    ASSERT_NE(actual.second, BelId()) << listing;
    EXPECT_EQ(lab(actual.first), lab(fastest->a)) << listing;
    EXPECT_EQ(lab(actual.second), lab(fastest->b)) << listing;
    ASSERT_NE(listing.find("Placed reduction candidate 0,"), std::string::npos) << listing;
    ASSERT_TRUE(placed_reduction(ctx.get(), root->name.str(ctx.get()), 1, 0));
    EXPECT_EQ(plan.leaves[0]->bel, actual.first);
    EXPECT_EQ(plan.leaves[1]->bel, actual.second);
    EXPECT_EQ(root->bel, before.cells.at(root->name).bel);
    before.expect_slots_and_fixed_cells(ctx.get(), {leaf, middle, root});
    const unsigned required = ((1u << 12) - 1) ^ (1u << 1) ^ (1u << 6) ^ (1u << 9);
    for (unsigned row = 0; row < 4096; ++row) EXPECT_EQ(evaluate(output, row), row == required) << row;
    assert_legal();
    ctx->check();
}

TEST_F(TwelveReductionTest, ExplicitDefaultMinimumRetainsQualifiedRewriteAndRollback)
{
    CubeSnapshot before(ctx.get());
    const auto listing = root->name.str(ctx.get()) + " 2 -1 250";
    ASSERT_NO_THROW(diagnostic_placed_reduction(ctx.get(), listing.c_str()));
    before.expect_exact(ctx.get());
    TimingAnalyser timing_before(ctx.get());
    timing_before.setup(false, false, true);
    const auto branch_slack = timing_before.get_setup_slack(CellPortKey(root->name, id_Q));
    const auto selected = root->name.str(ctx.get()) + " 2 0 250";
    ASSERT_NO_THROW(diagnostic_placed_reduction(ctx.get(), selected.c_str()));
    EXPECT_EQ(root->type, id_MISTRAL_ALUT2);
    before.expect_slots_and_fixed_cells(ctx.get(), {leaf, middle, root});
    const unsigned required = ((1u << 12) - 1) ^ (1u << 1) ^ (1u << 6) ^ (1u << 9);
    for (unsigned row = 0; row < (1u << 12); ++row)
        EXPECT_EQ(evaluate(output, row), row == required) << "assignment " << row;
    TimingAnalyser timing_after(ctx.get());
    timing_after.setup(false, false, true);
    EXPECT_GE(timing_after.get_setup_slack(CellPortKey(root->name, id_Q)) - branch_slack, 250);
    assert_legal();
    ctx->check();
}

TEST_F(PlacedReductionTest, OptionalMinimumPrevalidatesEveryStageBeforeMutation)
{
    const auto root_name = root->name.str(ctx.get());
    const auto first = root_name + " 2 0\n";
    for (const auto &invalid : {"0", "-1", "2147483648", "999999999999999999999999999", "200x", "word",
                                "250 extra"}) {
        SCOPED_TRACE(invalid);
        const auto bad_stage = root_name + " 2 0 " + invalid;
        CubeSnapshot before(ctx.get());
        // A valid selected first stage would mutate this fixture. Parsing a
        // malformed second minimum must fail before that first stage runs.
        EXPECT_THROW(diagnostic_placed_reduction(ctx.get(), (first + bad_stage).c_str()),
                     log_execution_error_exception);
        before.expect_exact(ctx.get());
        EXPECT_THROW(diagnostic_placed_reduction(ctx.get(), bad_stage.c_str()), log_execution_error_exception);
        before.expect_exact(ctx.get());
    }
    for (int minimum : {0, -1}) {
        CubeSnapshot before(ctx.get());
        EXPECT_THROW(placed_reduction(ctx.get(), root_name, 2, 0, minimum), log_execution_error_exception);
        before.expect_exact(ctx.get());
    }
    CubeSnapshot before(ctx.get());
    const auto largest_positive = root_name + " 2 -1 " + std::to_string(std::numeric_limits<int>::max());
    ASSERT_NO_THROW(diagnostic_placed_reduction(ctx.get(), largest_positive.c_str()));
    before.expect_exact(ctx.get());
    assert_legal();
    ctx->check();
}

TEST_F(PlacedReductionTest, ProtectedConeCannotBeRewrittenOrMoved)
{
    leaf->attrs[ctx->id("keep")] = 1;
    CubeSnapshot leaf_protected(ctx.get());
    EXPECT_FALSE(placed_reduction(ctx.get(), root->name.str(ctx.get()), 2, 0));
    leaf_protected.expect_exact(ctx.get());
    leaf->attrs.erase(ctx->id("keep"));
    middle->attrs[ctx->id("dont_touch")] = 1;
    CubeSnapshot middle_protected(ctx.get());
    EXPECT_FALSE(placed_reduction(ctx.get(), root->name.str(ctx.get()), 2, 0));
    middle_protected.expect_exact(ctx.get());
    assert_legal();
    ctx->check();
}

TEST_F(PlacedReductionTest, ObservableIntermediateRejectsBothPlacedAndUnplacedPlans)
{
    const auto boundary_name = ctx->id("externally_observable_intermediate");
    const std::string root_name = root->name.str(ctx.get());
    const std::vector<CellInfo *> cone{leaf, middle, root};
    for (auto *intermediate : {leaf, middle}) {
        SCOPED_TRACE(intermediate->name.str(ctx.get()));
        ctx->ports[boundary_name] = PortInfo{boundary_name, intermediate->getPort(id_Q), PORT_OUT, {}};
        // A top-level port is absent from NetInfo::users. The net therefore
        // still looks private to the cone, although its value is observable.
        ASSERT_EQ(intermediate->getPort(id_Q)->users.entries(), 1);
        CubeSnapshot placed_before(ctx.get());
        ReductionBalancePlan placed_plan;
        EXPECT_FALSE(plan_reduction(ctx.get(), root_name, true, placed_plan));
        EXPECT_FALSE(placed_reduction(ctx.get(), root_name, 2, 0));
        placed_before.expect_exact(ctx.get());
        ASSERT_EQ(ctx->ports.size(), 1u);
        EXPECT_EQ(ctx->ports.at(boundary_name).net, intermediate->getPort(id_Q));
        EXPECT_EQ(ctx->ports.at(boundary_name).type, PORT_OUT);

        std::vector<std::pair<BelId, PlaceStrength>> placements;
        for (auto *cell : cone) {
            placements.emplace_back(cell->bel, cell->belStrength);
            ctx->unbindBel(cell->bel);
        }
        CubeSnapshot unplaced_before(ctx.get());
        ReductionBalancePlan unplaced_plan;
        EXPECT_FALSE(plan_reduction(ctx.get(), root_name, false, unplaced_plan));
        unplaced_before.expect_exact(ctx.get());
        ASSERT_EQ(ctx->ports.size(), 1u);
        EXPECT_EQ(ctx->ports.at(boundary_name).net, intermediate->getPort(id_Q));
        for (size_t i = 0; i < cone.size(); ++i)
            ctx->bindBel(placements[i].first, cone[i], placements[i].second);
        ctx->ports.erase(boundary_name);
    }
    assert_legal();
    ctx->check();
}

TEST_F(PlacedReductionTest, PreservedLiteralAndRootMayRemainExternallyObservable)
{
    const auto input_boundary = ctx->id("externally_observed_literal");
    const auto output_boundary = ctx->id("externally_observed_result");
    ctx->ports[input_boundary] = PortInfo{input_boundary, inputs[6], PORT_OUT, {}};
    ctx->ports[output_boundary] = PortInfo{output_boundary, output, PORT_OUT, {}};
    CubeSnapshot before(ctx.get());
    ReductionBalancePlan plan;
    ASSERT_TRUE(plan_reduction(ctx.get(), root->name.str(ctx.get()), true, plan));
    ASSERT_TRUE(placed_reduction(ctx.get(), root->name.str(ctx.get()), 2, 0));
    ASSERT_EQ(ctx->ports.size(), 2u);
    EXPECT_EQ(ctx->ports.at(input_boundary).name, input_boundary);
    EXPECT_EQ(ctx->ports.at(input_boundary).net, inputs[6]);
    EXPECT_EQ(ctx->ports.at(input_boundary).type, PORT_OUT);
    EXPECT_EQ(ctx->ports.at(output_boundary).name, output_boundary);
    EXPECT_EQ(ctx->ports.at(output_boundary).net, output);
    EXPECT_EQ(ctx->ports.at(output_boundary).type, PORT_OUT);
    before.expect_slots_and_fixed_cells(ctx.get(), {leaf, middle, root});
    const unsigned required = ((1u << 11) - 1) ^ (1u << 1) ^ (1u << 6) ^ (1u << 9);
    for (unsigned row = 0; row < (1u << 11); ++row)
        EXPECT_EQ(evaluate(output, row), row == required) << "assignment " << row;
    assert_legal();
    ctx->check();
}

TEST_F(PlacedReductionTest, ImprovesRootBranchWhileIndependentSlowBranchStillLimitsEndpoint)
{
    auto *slow_source = ff(ctx->id("independent_slow_registered_source"), nullptr);
    auto *slow_first = lut("independent_slow_first", id_MISTRAL_ALUT2, 0xa,
                           {slow_source->getPort(id_Q), slow_source->getPort(id_Q)});
    auto *slow_second = lut("independent_slow_second", id_MISTRAL_ALUT2, 0xa,
                            {slow_first->getPort(id_Q), slow_first->getPort(id_Q)});
    auto *slow_third = lut("independent_slow_third", id_MISTRAL_ALUT2, 0xa,
                           {slow_second->getPort(id_Q), slow_second->getPort(id_Q)});
    auto *merge = lut("independent_branch_merge", id_MISTRAL_ALUT2, 0x8,
                      {output, slow_third->getPort(id_Q)});
    sink->disconnectPort(id_ENA);
    sink->connectPort(id_ENA, merge->getPort(id_Q));
    ctx->assignArchInfo();
    // Long alternating physical distances make the independent registered
    // branch dominate the shared endpoint. Both branches use the same clock,
    // edge and constraint; no asynchronous or unconstrained path masks it.
    ASSERT_NO_FATAL_FAILURE(place(slow_source, 25, 10));
    ASSERT_NO_FATAL_FAILURE(place(slow_first, 25, 40));
    ASSERT_NO_FATAL_FAILURE(place(slow_second, 25, 10));
    ASSERT_NO_FATAL_FAILURE(place(slow_third, 25, 40));
    ASSERT_NO_FATAL_FAILURE(place(merge, 25, 31));
    assert_legal();
    CubeSnapshot before(ctx.get());
    TimingAnalyser timing_before(ctx.get());
    timing_before.setup(false, false, true);
    const auto root_port = CellPortKey(root->name, id_Q);
    const auto sink_port = CellPortKey(sink->name, id_ENA);
    const auto root_slack = timing_before.get_setup_slack(root_port);
    const auto endpoint_slack = timing_before.get_setup_slack(sink_port);
    ASSERT_GT(root_slack, endpoint_slack + 250);
    ASSERT_TRUE(timing_before.get_timing_result().min_delay_violations.empty());
    const auto root_bel = root->bel;
    ASSERT_TRUE(placed_reduction(ctx.get(), root->name.str(ctx.get()), 2, 0));
    EXPECT_EQ(root->bel, root_bel);
    EXPECT_EQ(root->getPort(id_Q), output);
    before.expect_slots_and_fixed_cells(ctx.get(), {leaf, middle, root});
    const unsigned required = ((1u << 11) - 1) ^ (1u << 1) ^ (1u << 6) ^ (1u << 9);
    for (unsigned row = 0; row < (1u << 11); ++row)
        EXPECT_EQ(evaluate(output, row), row == required) << "assignment " << row;
    TimingAnalyser timing_after(ctx.get());
    timing_after.setup(false, false, true);
    EXPECT_GE(timing_after.get_setup_slack(root_port) - root_slack, 250);
    EXPECT_GE(timing_after.get_setup_slack(sink_port), endpoint_slack);
    EXPECT_EQ(timing_after.get_setup_slack(sink_port), endpoint_slack);
    EXPECT_TRUE(timing_after.get_timing_result().min_delay_violations.empty());
    const auto &fmax_after = timing_after.get_timing_result().clock_fmax;
    for (const auto &entry : timing_before.get_timing_result().clock_fmax) {
        ASSERT_TRUE(fmax_after.count(entry.first));
        EXPECT_GE(fmax_after.at(entry.first).achieved + 0.001f, entry.second.achieved);
    }
    assert_legal();
    ctx->check();
}

namespace {
class TwentyFourReductionTest : public PlacedReductionTest {
  protected:
    std::vector<NetInfo *> wide_inputs;
    std::vector<CellInfo *> wide_cone, wide_sinks;
    CellInfo *wide_root = nullptr;
    NetInfo *wide_output = nullptr;
    unsigned required = 0;

    CellInfo *cube(const std::string &name, const std::vector<NetInfo *> &ins,
                   const std::vector<bool> &values, bool positive, int inverted_pin)
    {
        unsigned row = 0;
        for (size_t i = 0; i < values.size(); ++i)
            row |= unsigned(values[i] ^ (int(i) == inverted_pin)) << i;
        uint64_t mask = uint64_t(1) << row;
        if (!positive) mask ^= (uint64_t(1) << (1u << ins.size())) - 1;
        const IdString types[] = {id_MISTRAL_ALUT2, id_MISTRAL_ALUT3, id_MISTRAL_ALUT4,
                                  id_MISTRAL_ALUT5, id_MISTRAL_ALUT6};
        auto *cell = lut(name.c_str(), types[ins.size() - 2], mask, ins);
        if (inverted_pin >= 0) cell->pin_data[cube_pins[inverted_pin]].state = PIN_INV;
        return cell;
    }

    void SetUp() override
    {
        PlacedReductionTest::SetUp();
        std::vector<CellInfo *> sources;
        for (int i = 0; i < 24; ++i) {
            auto *source = ff(ctx->idf("wide_source_%02d", i), nullptr);
            sources.push_back(source);
            wide_inputs.push_back(source->getPort(id_Q));
            if (i % 3 != 1) required |= 1u << i;
        }
        std::vector<NetInfo *> branches;
        std::vector<bool> branch_values;
        for (int branch = 0; branch < 3; ++branch) {
            std::vector<NetInfo *> first_inputs, second_inputs;
            std::vector<bool> first_values, second_values;
            for (int i = 0; i < 4; ++i) {
                first_inputs.push_back(wide_inputs[8 * branch + i]);
                first_values.push_back((required >> (8 * branch + i)) & 1);
                second_inputs.push_back(wide_inputs[8 * branch + 4 + i]);
                second_values.push_back((required >> (8 * branch + 4 + i)) & 1);
            }
            const bool first_positive = branch == 1, second_positive = branch != 1;
            auto *first = cube("wide_branch_" + std::to_string(branch) + "_first", first_inputs,
                               first_values, first_positive, 1);
            second_inputs.push_back(first->getPort(id_Q));
            second_values.push_back(first_positive);
            auto *second = cube("wide_branch_" + std::to_string(branch) + "_middle", second_inputs,
                                second_values, second_positive, 2);
            wide_cone.push_back(first);
            wide_cone.push_back(second);
            branches.push_back(second->getPort(id_Q));
            branch_values.push_back(second_positive);
        }
        wide_root = cube("wide_result", branches, branch_values, true, 1);
        wide_cone.push_back(wide_root);
        wide_output = wide_root->getPort(id_Q);
        // Exercise multiple aliases, including map entries absent from the
        // net's optional alias list, for both retired and surviving outputs.
        for (auto *cell : wide_cone) {
            auto *net = cell->getPort(id_Q);
            auto alias = ctx->id(net->name.str(ctx.get()) + "_alias");
            net->aliases.push_back(alias);
            ctx->net_aliases[alias] = net->name;
            ctx->net_aliases[ctx->id(net->name.str(ctx.get()) + "_extra_alias")] = net->name;
        }
        ctx->net_aliases[ctx->id("wide_literal_alias")] = wide_inputs[7]->name;
        for (int i = 0; i < 9; ++i)
            wide_sinks.push_back(ff(ctx->idf("wide_endpoint_%d", i), wide_output));
        const auto public_root = ctx->id("wide_public_result"), public_literal = ctx->id("wide_public_literal");
        ctx->ports[public_root] = PortInfo{public_root, wide_output, PORT_OUT, {}};
        ctx->ports[public_literal] = PortInfo{public_literal, wide_inputs[7], PORT_OUT, {}};
        // Include real inactive slots with a nontrivial allocation order.
        for (auto *net : wide_inputs) add_holes(net);
        for (auto *cell : wide_cone) add_holes(cell->getPort(id_Q));
        ctx->assignArchInfo();
        for (size_t i = 0; i < sources.size(); ++i)
            ASSERT_NO_FATAL_FAILURE(place(sources[i], 28, i < 12 ? 28 : 29));
        for (int branch = 0; branch < 3; ++branch) {
            ASSERT_NO_FATAL_FAILURE(place(wide_cone[2 * branch], 28, 10 + branch));
            ASSERT_NO_FATAL_FAILURE(place(wide_cone[2 * branch + 1], 28, 40 + branch));
        }
        ASSERT_NO_FATAL_FAILURE(place(wide_root, 28, 30));
        for (auto *endpoint : wide_sinks) ASSERT_NO_FATAL_FAILURE(place(endpoint, 28, 31));
        assert_legal();
        ctx->check();
    }

    void add_holes(NetInfo *net)
    {
        auto a = net->users.add(PortRef{}), b = net->users.add(PortRef{}), c = net->users.add(PortRef{});
        net->users.remove(a);
        net->users.remove(c);
        net->users.remove(b);
    }

    void unplace_wide()
    {
        for (auto *cell : wide_cone) ctx->unbindBel(cell->bel);
    }

    bool wide_evaluate(NetInfo *net, unsigned row) const
    {
        auto found = std::find(wide_inputs.begin(), wide_inputs.end(), net);
        if (found != wide_inputs.end()) return (row >> (found - wide_inputs.begin())) & 1;
        auto *cell = net->driver.cell;
        unsigned address = 0;
        for (int pin = 0; pin < 6 && cell->ports.count(cube_pins[pin]); ++pin) {
            bool value = wide_evaluate(cell->getPort(cube_pins[pin]), row);
            if (cell->get_pin_state(cube_pins[pin]) == PIN_INV) value = !value;
            address |= unsigned(value) << pin;
        }
        return (uint64_t(cell->params.at(id_LUT).as_int64()) >> address) & 1;
    }

    void expect_external_cells(const CubeSnapshot &before) const
    {
        for (const auto &entry : before.cells) {
            const auto &saved = entry.second;
            if (std::find(wide_cone.begin(), wide_cone.end(), saved.identity) != wide_cone.end()) continue;
            ASSERT_TRUE(ctx->cells.count(entry.first));
            auto *cell = ctx->cells.at(entry.first).get();
            EXPECT_EQ(cell, saved.identity);
            EXPECT_EQ(cell->type, saved.type);
            EXPECT_EQ(cell->bel, saved.bel);
            EXPECT_EQ(cell->belStrength, saved.strength);
            ASSERT_EQ(cell->params.size(), saved.params.size());
            for (const auto &param : saved.params) EXPECT_EQ(cell->params.at(param.first), param.second);
            ASSERT_EQ(cell->ports.size(), saved.ports.size());
            for (const auto &port : saved.ports) {
                EXPECT_EQ(cell->ports.at(port.first).net, port.second.net);
                EXPECT_EQ(cell->ports.at(port.first).user_idx, port.second.user_idx);
            }
            ASSERT_EQ(cell->pin_data.size(), saved.pins.size());
            for (const auto &pin : saved.pins) {
                EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
                EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
            }
        }
    }

    void expect_balanced_cube(const ReductionBalancePlan &plan)
    {
        ASSERT_EQ(plan.leaves.size(), 4u);
        ASSERT_EQ(wide_root->type, id_MISTRAL_ALUT4);
        EXPECT_EQ(wide_root->getPort(id_Q), wide_output);
        for (auto *child : plan.leaves) {
            ASSERT_EQ(child->type, id_MISTRAL_ALUT6);
            for (unsigned row = 0; row < 64; ++row) {
                bool expected = true;
                for (int pin = 0; pin < 6; ++pin) {
                    auto at = std::find(wide_inputs.begin(), wide_inputs.end(), child->getPort(cube_pins[pin]));
                    ASSERT_NE(at, wide_inputs.end());
                    expected &= bool((row >> pin) & 1) == bool((required >> (at - wide_inputs.begin())) & 1);
                    EXPECT_EQ(child->get_pin_state(cube_pins[pin]), PIN_SIG);
                }
                EXPECT_EQ(bool((uint64_t(child->params.at(id_LUT).as_int64()) >> row) & 1), expected);
            }
        }
        for (unsigned row = 0; row < 16; ++row)
            EXPECT_EQ(bool((uint64_t(wide_root->params.at(id_LUT).as_int64()) >> row) & 1), row == 15);
        EXPECT_TRUE(wide_evaluate(wide_output, required));
        for (int bit = 0; bit < 24; ++bit) EXPECT_FALSE(wide_evaluate(wide_output, required ^ (1u << bit)));
    }

    void expect_retired_aliases(const CubeSnapshot &before, const std::vector<IdString> &retired_nets)
    {
        std::vector<IdString> removed;
        for (const auto &entry : before.aliases) {
            if (std::find(retired_nets.begin(), retired_nets.end(), entry.second) != retired_nets.end()) {
                ASSERT_FALSE(ctx->net_aliases.count(entry.first));
                EXPECT_EQ(ctx->getNetByAlias(entry.first), nullptr);
                removed.push_back(entry.first);
            } else {
                ASSERT_TRUE(ctx->net_aliases.count(entry.first));
                EXPECT_EQ(ctx->net_aliases.at(entry.first), entry.second);
                EXPECT_EQ(ctx->getNetByAlias(entry.first), before.nets.at(entry.second).identity);
            }
        }
        EXPECT_EQ(ctx->net_aliases.size() + removed.size(), before.aliases.size());
        ASSERT_EQ(removed.size(), 3 * retired_nets.size());
        // Both canonical net names and secondary alias names become reusable.
        for (auto name : removed) {
            auto *net = ctx->createNet(name);
            EXPECT_EQ(ctx->getNetByAlias(name), net);
        }
        ctx->check();
    }
};
}

TEST_F(TwentyFourReductionTest, RecognizesMixedPolaritiesWithoutMutation)
{
    CubeSnapshot before(ctx.get());
    ReductionBalancePlan plan;
    ASSERT_TRUE(plan_reduction(ctx.get(), wide_root->name.str(ctx.get()), true, plan));
    ASSERT_EQ(plan.cells.size(), 7u);
    ASSERT_EQ(plan.literals.size(), 24u);
    ASSERT_EQ(plan.leaves.size(), 4u);
    ASSERT_EQ(plan.retired.size(), 2u);
    ASSERT_EQ(plan.retired_nets.size(), 2u);
    for (const auto &literal : plan.literals) {
        auto at = std::find(wide_inputs.begin(), wide_inputs.end(), literal.first);
        ASSERT_NE(at, wide_inputs.end());
        EXPECT_EQ(literal.second, bool((required >> (at - wide_inputs.begin())) & 1));
    }
    EXPECT_TRUE(wide_evaluate(wide_output, required));
    for (int bit = 0; bit < 24; ++bit) EXPECT_FALSE(wide_evaluate(wide_output, required ^ (1u << bit)));
    before.expect_exact(ctx.get());
}

TEST_F(TwentyFourReductionTest, UnplacedRewriteRetiresOnlyTwoPrivateCellsAndNets)
{
    unplace_wide();
    CubeSnapshot before(ctx.get());
    ReductionBalancePlan plan;
    ASSERT_TRUE(plan_reduction(ctx.get(), wide_root->name.str(ctx.get()), false, plan));
    std::vector<IdString> retired_cells, retired_nets;
    for (auto *cell : plan.retired) retired_cells.push_back(cell->name);
    for (auto *net : plan.retired_nets) retired_nets.push_back(net->name);
    ASSERT_TRUE(ctx->balance_reduction(wide_root->name.str(ctx.get())));
    EXPECT_EQ(ctx->cells.size() + 2, before.cells.size());
    EXPECT_EQ(ctx->nets.size() + 2, before.nets.size());
    for (auto name : retired_cells) EXPECT_FALSE(ctx->cells.count(name));
    for (auto name : retired_nets) EXPECT_FALSE(ctx->nets.count(name));
    expect_external_cells(before);
    for (auto *net : wide_inputs) {
        const auto &saved = before.nets.at(net->name);
        EXPECT_EQ(net->driver.cell, saved.driver.cell);
        EXPECT_EQ(net->driver.port, saved.driver.port);
        EXPECT_EQ(net->users.capacity(), saved.user_store.capacity());
        EXPECT_EQ(net->users.entries(), saved.user_store.entries());
        auto actual = net->users, expected = saved.user_store;
        for (auto user : expected.enumerate()) ASSERT_TRUE(net->users.count(user.index));
        for (size_t probe = 0; probe < size_t(saved.user_store.capacity()) + 8; ++probe)
            EXPECT_EQ(actual.add(PortRef{}), expected.add(PortRef{}));
    }
    EXPECT_EQ(wide_output->users.entries(), 9);
    EXPECT_EQ(ctx->ports.at(ctx->id("wide_public_result")).net, wide_output);
    EXPECT_EQ(ctx->ports.at(ctx->id("wide_public_literal")).net, wide_inputs[7]);
    expect_balanced_cube(plan);
    ctx->check();
    expect_retired_aliases(before, retired_nets);
}

TEST_F(TwentyFourReductionTest, ListingAndMissingOrdinalRestoreExactGraphAndFreeLists)
{
    for (int selection : {-1, 100000}) {
        CubeSnapshot before(ctx.get());
        EXPECT_FALSE(placed_reduction(ctx.get(), wide_root->name.str(ctx.get()), 2, selection));
        before.expect_exact(ctx.get());
        assert_legal();
        ctx->check();
    }
}

TEST_F(TwentyFourReductionTest, PlacedRewritePreservesPublicRootAndExternalRegisters)
{
    CubeSnapshot before(ctx.get());
    ReductionBalancePlan plan;
    ASSERT_TRUE(plan_reduction(ctx.get(), wide_root->name.str(ctx.get()), true, plan));
    std::vector<IdString> retired_nets;
    for (auto *net : plan.retired_nets) retired_nets.push_back(net->name);
    auto root_bel = wide_root->bel;
    ASSERT_TRUE(placed_reduction(ctx.get(), wide_root->name.str(ctx.get()), 2, 0));
    EXPECT_EQ(wide_root->bel, root_bel);
    EXPECT_EQ(ctx->cells.size() + 2, before.cells.size());
    EXPECT_EQ(ctx->nets.size() + 2, before.nets.size());
    expect_external_cells(before);
    EXPECT_EQ(wide_output->users.entries(), 9);
    for (const auto &user : before.nets.at(wide_output->name).users) {
        ASSERT_TRUE(wide_output->users.count(user.first));
        EXPECT_EQ(wide_output->users.at(user.first).cell, user.second.cell);
        EXPECT_EQ(wide_output->users.at(user.first).port, user.second.port);
    }
    expect_balanced_cube(plan);
    assert_legal();
    ctx->check();
    expect_retired_aliases(before, retired_nets);
}

TEST_F(TwentyFourReductionTest, RejectsRepeatedLiteralReconvergenceCycleAndNonCubeExactly)
{
    for (int mutation = 0; mutation < 4; ++mutation) {
        auto *cell = mutation == 1 || mutation == 3 ? wide_root : wide_cone[0];
        auto pin = mutation == 1 ? id_B : id_A;
        auto *original = cell->getPort(pin);
        auto mask = cell->params.at(id_LUT);
        if (mutation == 3) cell->params[id_LUT] = Property(3, 8);
        else {
            cell->disconnectPort(pin);
            cell->connectPort(pin, mutation == 0 ? wide_inputs[1] : mutation == 1 ? wide_root->getPort(id_A) : wide_output);
        }
        CubeSnapshot before(ctx.get());
        ReductionBalancePlan plan;
        EXPECT_FALSE(plan_reduction(ctx.get(), wide_root->name.str(ctx.get()), true, plan));
        before.expect_exact(ctx.get());
        cell->params[id_LUT] = mask;
        if (mutation != 3) { cell->disconnectPort(pin); cell->connectPort(pin, original); }
    }
}

TEST_F(TwentyFourReductionTest, RejectsPrivateSideUsersBoundaryAndProtectedObjects)
{
    auto *private_net = wide_cone[0]->getPort(id_Q);
    for (int mutation = 0; mutation < 5; ++mutation) {
        const auto boundary = ctx->id("private_boundary"), keep = ctx->id("keep");
        if (mutation == 0) unrelated->connectPort(id_ENA, private_net);
        if (mutation == 1) ctx->ports[boundary] = PortInfo{boundary, private_net, PORT_OUT, {}};
        if (mutation == 2) wide_cone[0]->attrs[keep] = 1;
        if (mutation == 3) private_net->attrs[keep] = 1;
        if (mutation == 4) wide_inputs[0]->is_global = true;
        CubeSnapshot before(ctx.get());
        ReductionBalancePlan plan;
        EXPECT_FALSE(plan_reduction(ctx.get(), wide_root->name.str(ctx.get()), true, plan));
        before.expect_exact(ctx.get());
        if (mutation == 0) unrelated->disconnectPort(id_ENA);
        if (mutation == 1) ctx->ports.erase(boundary);
        wide_cone[0]->attrs.erase(keep);
        private_net->attrs.erase(keep);
        wide_inputs[0]->is_global = false;
    }
}

TEST_F(TwentyFourReductionTest, MultilineDiagnosticPrevalidatesAllStepsBeforeMutation)
{
    const auto first = root->name.str(ctx.get()) + " 2 0\n";
    for (const auto &bad : {"missing-fields", "wide_result 0 0", "wide_result 7 0", "wide_result 2 -2", "wide_result 2 0 extra"}) {
        CubeSnapshot before(ctx.get());
        EXPECT_THROW(diagnostic_placed_reduction(ctx.get(), (first + bad).c_str()), log_execution_error_exception);
        before.expect_exact(ctx.get());
    }
    for (const auto &spec : {root->name.str(ctx.get()) + " 2 -1\nwide_result 2 0",
                            first + first + first + first + first + first + first + first + "wide_result 2 0"}) {
        CubeSnapshot before(ctx.get());
        EXPECT_THROW(diagnostic_placed_reduction(ctx.get(), spec.c_str()), log_execution_error_exception);
        before.expect_exact(ctx.get());
    }
}

TEST_F(TwentyFourReductionTest, DownstreamReconvergentDagPreservesAllNineEndpoints)
{
    auto *a = lut("wide_downstream_a", id_MISTRAL_ALUT2, 0xa, {wide_output, wide_inputs[0]});
    auto *b = lut("wide_downstream_b", id_MISTRAL_ALUT2, 0xa, {wide_output, wide_inputs[1]});
    auto *merge = lut("wide_downstream_merge", id_MISTRAL_ALUT2, 0x8,
                      {a->getPort(id_Q), b->getPort(id_Q)});
    wide_sinks[0]->disconnectPort(id_ENA);
    wide_sinks[0]->connectPort(id_ENA, merge->getPort(id_Q));
    ctx->assignArchInfo();
    ASSERT_NO_FATAL_FAILURE(place(a, 28, 31));
    ASSERT_NO_FATAL_FAILURE(place(b, 28, 31));
    ASSERT_NO_FATAL_FAILURE(place(merge, 28, 31));
    CubeSnapshot before(ctx.get());
    ASSERT_TRUE(placed_reduction(ctx.get(), wide_root->name.str(ctx.get()), 2, 0));
    expect_external_cells(before);
    EXPECT_EQ(wide_sinks[0]->getPort(id_ENA), merge->getPort(id_Q));
    for (size_t i = 1; i < wide_sinks.size(); ++i) EXPECT_EQ(wide_sinks[i]->getPort(id_ENA), wide_output);
    ctx->check();
}

TEST_F(TwentyFourReductionTest, DownstreamCycleRejectsBeforeRewriting)
{
    auto *a = lut("wide_cycle_a", id_MISTRAL_ALUT2, 0xa, {wide_output, wide_inputs[0]});
    auto *b = lut("wide_cycle_b", id_MISTRAL_ALUT2, 0xa, {a->getPort(id_Q), wide_inputs[1]});
    a->disconnectPort(id_B);
    a->connectPort(id_B, b->getPort(id_Q));
    ctx->assignArchInfo();
    ASSERT_NO_FATAL_FAILURE(place(a, 28, 31));
    ASSERT_NO_FATAL_FAILURE(place(b, 28, 31));
    CubeSnapshot before(ctx.get());
    EXPECT_FALSE(placed_reduction(ctx.get(), wide_root->name.str(ctx.get()), 2, 0));
    before.expect_exact(ctx.get());
    ctx->check();
}

TEST_F(TwentyFourReductionTest, MultilineListingRetainsAcceptedElevenStep)
{
    const auto root_bel = wide_root->bel;
    const auto root_type = wide_root->type;
    const auto root_mask = wide_root->params.at(id_LUT);
    std::vector<std::pair<IdString, CubeSnapshot::Cell>> saved;
    CubeSnapshot before(ctx.get());
    for (auto *cell : wide_cone) saved.emplace_back(cell->name, before.cells.at(cell->name));
    const auto spec = root->name.str(ctx.get()) + " 2 0\n" + wide_root->name.str(ctx.get()) + " 2 -1";
    ASSERT_NO_THROW(diagnostic_placed_reduction(ctx.get(), spec.c_str()));
    EXPECT_EQ(root->type, id_MISTRAL_ALUT2);
    EXPECT_EQ(wide_root->type, root_type);
    EXPECT_EQ(wide_root->params.at(id_LUT), root_mask);
    EXPECT_EQ(wide_root->bel, root_bel);
    for (const auto &entry : saved) {
        auto *cell = ctx->cells.at(entry.first).get();
        EXPECT_EQ(cell, entry.second.identity);
        EXPECT_EQ(cell->type, entry.second.type);
        EXPECT_EQ(cell->bel, entry.second.bel);
        for (const auto &port : entry.second.ports) {
            EXPECT_EQ(cell->ports.at(port.first).net, port.second.net);
            EXPECT_EQ(cell->ports.at(port.first).user_idx, port.second.user_idx);
        }
        for (const auto &pin : entry.second.pins) {
            EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
            EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
        }
    }
    ctx->check();
}

TEST_F(TwentyFourReductionTest, MinimumIsLocalToItsStageAndFinalListingStillRollsBack)
{
    CubeSnapshot before(ctx.get());
    // Parsing the final very high threshold must not replace the first
    // stage's threshold. The second eligible cone is probed and restored.
    const auto spec = root->name.str(ctx.get()) + " 2 0 250\n" + wide_root->name.str(ctx.get()) + " 2 -1 " +
                      std::to_string(std::numeric_limits<int>::max());
    ASSERT_NO_THROW(diagnostic_placed_reduction(ctx.get(), spec.c_str()));
    ASSERT_EQ(root->type, id_MISTRAL_ALUT2);
    for (auto *cell : wide_cone) {
        const auto &saved = before.cells.at(cell->name);
        EXPECT_EQ(cell, saved.identity);
        EXPECT_EQ(cell->type, saved.type);
        EXPECT_EQ(cell->bel, saved.bel);
        EXPECT_EQ(cell->belStrength, saved.strength);
        ASSERT_EQ(cell->params.size(), saved.params.size());
        for (const auto &param : saved.params) EXPECT_EQ(cell->params.at(param.first), param.second);
        ASSERT_EQ(cell->ports.size(), saved.ports.size());
        for (const auto &port : saved.ports) {
            EXPECT_EQ(cell->ports.at(port.first).net, port.second.net);
            EXPECT_EQ(cell->ports.at(port.first).user_idx, port.second.user_idx);
        }
        ASSERT_EQ(cell->pin_data.size(), saved.pins.size());
        for (const auto &pin : saved.pins) {
            EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
            EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
        }
    }
    for (auto *net : wide_inputs) {
        const auto &saved = before.nets.at(net->name);
        auto actual = net->users, expected = saved.user_store;
        ASSERT_EQ(actual.capacity(), expected.capacity());
        ASSERT_EQ(actual.entries(), expected.entries());
        for (auto user : expected.enumerate()) {
            ASSERT_TRUE(net->users.count(user.index));
            EXPECT_EQ(net->users.at(user.index).cell, user.value.cell);
            EXPECT_EQ(net->users.at(user.index).port, user.value.port);
        }
        for (size_t probe = 0; probe < size_t(saved.user_store.capacity()) + 8; ++probe)
            EXPECT_EQ(actual.add(PortRef{}), expected.add(PortRef{}));
    }
    EXPECT_EQ(wide_root->getPort(id_Q), wide_output);
    EXPECT_EQ(ctx->ports.at(ctx->id("wide_public_result")).net, wide_output);
    assert_legal();
    ctx->check();
}

namespace {
using ProbeLabs = std::array<placed_reduction_policy::Lab, 4>;

ProbeLabs probe_geometry(int category)
{
    // Categories identify disjoint toy geometries independently of the
    // policy's sorted-key implementation.
    return {{{4 * category, 0}, {4 * category + 1, 1}, {4 * category + 2, 2}, {4 * category + 3, 3}}};
}
}

TEST(PlacedReductionProbePolicy, AdversarialPermutationPrefixDoesNotHideMeasuredImprovement)
{
    struct Trial { ProbeLabs labs; int measured_gain; };
    std::vector<Trial> candidates;
    auto crowded = probe_geometry(0);
    do { candidates.push_back({crowded, 19}); } while (std::next_permutation(crowded.begin(), crowded.end()));
    ASSERT_EQ(candidates.size(), 24u);
    const auto improvement = probe_geometry(1);
    candidates.push_back({improvement, 300});
    placed_reduction_policy::WideProbeBudget budget;
    bool found_improvement = false;
    for (const auto &trial : candidates) {
        if (budget.exhausted()) break;
        if (!budget.eligible(trial.labs)) continue;
        ASSERT_TRUE(budget.admit_legal_ordered_tuple(trial.labs));
        if (trial.measured_gain >= 250) {
            EXPECT_EQ(trial.labs, improvement);
            found_improvement = true;
            break;
        }
    }
    // A raw sixteen-tuple prefix contains no useful measurement. The spatial
    // frontier must make the later geometry observable with budget remaining.
    EXPECT_TRUE(found_improvement);
    EXPECT_EQ(budget.timed_count(), 3);
    EXPECT_EQ(budget.geometry_count(), 2u);
    EXPECT_FALSE(budget.exhausted());
}

TEST(PlacedReductionProbePolicy, SixteenMeasurementsCoverAvailableGeometries)
{
    struct Trial { ProbeLabs labs; int category; };
    std::vector<Trial> candidates;
    auto prefix = probe_geometry(0);
    do { candidates.push_back({prefix, 0}); } while (std::next_permutation(prefix.begin(), prefix.end()));
    for (int category = 1; category < 8; ++category) {
        auto labs = probe_geometry(category);
        candidates.push_back({labs, category});
        std::reverse(labs.begin(), labs.end());
        candidates.push_back({labs, category});
    }
    placed_reduction_policy::WideProbeBudget budget;
    std::map<int, int> category_measurements;
    std::set<ProbeLabs> measured;
    for (const auto &trial : candidates) {
        if (budget.exhausted()) break;
        if (!budget.eligible(trial.labs)) continue;
        ASSERT_TRUE(budget.admit_legal_ordered_tuple(trial.labs));
        EXPECT_TRUE(measured.insert(trial.labs).second);
        ++category_measurements[trial.category];
    }
    EXPECT_TRUE(budget.exhausted());
    EXPECT_EQ(budget.timed_count(), 16);
    EXPECT_EQ(measured.size(), 16u);
    EXPECT_EQ(budget.geometry_count(), 8u);
    ASSERT_EQ(category_measurements.size(), 8u);
    for (int category = 0; category < 8; ++category) EXPECT_EQ(category_measurements.at(category), 2);
    EXPECT_FALSE(budget.eligible(probe_geometry(8)));
    EXPECT_FALSE(budget.admit_legal_ordered_tuple(probe_geometry(8)));
    EXPECT_EQ(budget.timed_count(), 16);
}

TEST(PlacedReductionProbePolicy, RepeatedLabsPreserveMultiplicityAndAssignmentAlternatives)
{
    const placed_reduction_policy::Lab a{1, 1}, b{2, 2}, c{3, 3};
    ProbeLabs twice_a{{a, a, b, c}}, twice_b{{a, b, b, c}};
    placed_reduction_policy::WideProbeBudget budget;
    for (const auto &initial : {twice_a, twice_b}) {
        auto labs = initial;
        int admitted = 0;
        do {
            if (budget.admit_legal_ordered_tuple(labs)) ++admitted;
        } while (std::next_permutation(labs.begin(), labs.end()));
        EXPECT_EQ(admitted, 2);
    }
    // Both have the same set of distinct LABs. Collapsing multiplicities would
    // incorrectly prevent the second geometry from receiving any probes.
    EXPECT_EQ(budget.geometry_count(), 2u);
    EXPECT_EQ(budget.timed_count(), 4);
    EXPECT_FALSE(budget.eligible(twice_a));
    EXPECT_FALSE(budget.eligible(twice_b));
}

TEST(PlacedReductionProbePolicy, LegalityFailuresAndDuplicatesDoNotConsumeMeasurements)
{
    const auto labs = probe_geometry(0);
    placed_reduction_policy::WideProbeBudget budget;
    // Several physical BEL variants can share one ordered LAB tuple. Failed
    // legality probes only query eligibility and leave a later legal variant usable.
    for (int illegal_variant = 0; illegal_variant < 20; ++illegal_variant) EXPECT_TRUE(budget.eligible(labs));
    EXPECT_EQ(budget.timed_count(), 0);
    EXPECT_EQ(budget.geometry_count(), 0u);
    ASSERT_TRUE(budget.admit_legal_ordered_tuple(labs));
    for (int duplicate = 0; duplicate < 20; ++duplicate) {
        EXPECT_FALSE(budget.eligible(labs));
        EXPECT_FALSE(budget.admit_legal_ordered_tuple(labs));
    }
    EXPECT_EQ(budget.timed_count(), 1);
    auto alternative = labs;
    std::swap(alternative[0], alternative[1]);
    ASSERT_TRUE(budget.admit_legal_ordered_tuple(alternative));
    EXPECT_EQ(budget.timed_count(), 2);
    auto third = labs;
    std::swap(third[0], third[2]);
    EXPECT_FALSE(budget.admit_legal_ordered_tuple(third));
    EXPECT_EQ(budget.timed_count(), 2);
}

TEST(PlacedReductionProbePolicy, DeterministicFrontierForIdenticalRankedInputs)
{
    std::vector<ProbeLabs> candidates;
    for (int category = 0; category < 12; ++category) {
        auto labs = probe_geometry(category);
        candidates.push_back(labs);
        std::swap(labs[0], labs[1]);
        candidates.push_back(labs);
        candidates.push_back(labs);
        std::swap(labs[1], labs[2]);
        candidates.push_back(labs);
    }
    placed_reduction_policy::WideProbeBudget first, second;
    std::vector<ProbeLabs> first_frontier, second_frontier;
    for (const auto &labs : candidates) {
        const bool first_eligible = first.eligible(labs), second_eligible = second.eligible(labs);
        EXPECT_EQ(first_eligible, second_eligible);
        if (first.admit_legal_ordered_tuple(labs)) first_frontier.push_back(labs);
        if (second.admit_legal_ordered_tuple(labs)) second_frontier.push_back(labs);
    }
    EXPECT_EQ(first_frontier, second_frontier);
    EXPECT_EQ(first_frontier.size(), 16u);
    EXPECT_EQ(first.timed_count(), second.timed_count());
    EXPECT_EQ(first.geometry_count(), second.geometry_count());
}
