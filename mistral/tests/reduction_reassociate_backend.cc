/* Context proof, packing legality and exact transaction rollback. SPDX-License-Identifier: ISC */
#include "nextpnr.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <array>
#include <functional>
#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

USING_NEXTPNR_NAMESPACE
NEXTPNR_NAMESPACE_BEGIN
bool reassociate_producer(Context *, const std::string &, IdString, std::string *);
bool reassociate_producer_trial(Context *, const std::string &, IdString, const std::function<bool()> &, std::string *);
NEXTPNR_NAMESPACE_END

namespace {
const IdString pins[] = {id_A, id_B, id_C, id_D, id_E, id_F};
struct Snapshot {
    struct Cell {
        CellInfo *identity;
        IdString type;
        BelId bel;
        PlaceStrength strength;
        ClusterId cluster;
        decltype(CellInfo::attrs) attrs;
        decltype(CellInfo::params) params;
        decltype(CellInfo::ports) ports;
        ArchCellInfo architecture;
    };
    struct Net {
        NetInfo *identity;
        PortRef driver;
        indexed_store<PortRef> users;
    };
    std::map<IdString, Cell> cells;
    std::map<IdString, Net> nets;
    std::vector<IdString> cell_order, net_order;
    std::vector<LABInfo> labs;
    explicit Snapshot(Context *ctx) : labs(ctx->labs)
    {
        for (const auto &entry : ctx->cells) {
            auto *cell = entry.second.get();
            cell_order.push_back(entry.first);
            cells.emplace(entry.first, Cell{cell, cell->type, cell->bel, cell->belStrength, cell->cluster, cell->attrs, cell->params,
                                            cell->ports, *static_cast<ArchCellInfo *>(cell)});
        }
        for (const auto &entry : ctx->nets) {
            auto *net = entry.second.get();
            net_order.push_back(entry.first);
            nets.emplace(entry.first, Net{net, net->driver, net->users});
        }
    }
    void expect_exact(Context *ctx) const
    {
        ASSERT_EQ(ctx->cells.size(), cells.size());
        ASSERT_EQ(ctx->nets.size(), nets.size());
        std::vector<IdString> current_cells, current_nets;
        for (const auto &entry : ctx->cells) current_cells.push_back(entry.first);
        for (const auto &entry : ctx->nets) current_nets.push_back(entry.first);
        EXPECT_EQ(current_cells, cell_order);
        EXPECT_EQ(current_nets, net_order);
        for (const auto &entry : cells) {
            auto *cell = ctx->cells.at(entry.first).get();
            const auto &old = entry.second;
            SCOPED_TRACE(entry.first.str(ctx));
            EXPECT_EQ(cell, old.identity);
            EXPECT_EQ(cell->type, old.type);
            EXPECT_EQ(cell->bel, old.bel);
            EXPECT_EQ(cell->belStrength, old.strength);
            if (old.bel != BelId()) { EXPECT_EQ(ctx->getBoundBelCell(old.bel), cell); }
            EXPECT_EQ(cell->attrs, old.attrs);
            EXPECT_EQ(cell->params, old.params);
            ASSERT_EQ(cell->ports.size(), old.ports.size());
            for (const auto &p : old.ports) {
                ASSERT_TRUE(cell->ports.count(p.first));
                const auto &now = cell->ports.at(p.first);
                EXPECT_EQ(now.name, p.second.name);
                EXPECT_EQ(now.type, p.second.type);
                EXPECT_EQ(now.net, p.second.net);
                EXPECT_EQ(now.user_idx, p.second.user_idx);
            }
            const auto &arch = old.architecture;
            ASSERT_EQ(cell->pin_data.size(), arch.pin_data.size());
            for (const auto &pin : arch.pin_data) {
                ASSERT_TRUE(cell->pin_data.count(pin.first));
                EXPECT_EQ(cell->pin_data.at(pin.first).state, pin.second.state);
                EXPECT_EQ(cell->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
            }
            EXPECT_EQ(cell->cluster, old.cluster);
            EXPECT_EQ(cell->constr_x, arch.constr_x);
            EXPECT_EQ(cell->constr_y, arch.constr_y);
            EXPECT_EQ(cell->constr_z, arch.constr_z);
            EXPECT_EQ(cell->constr_abs_z, arch.constr_abs_z);
            EXPECT_EQ(cell->constr_children, arch.constr_children);
            if (ctx->is_comb_cell(cell->type)) {
                for (int i = 0; i < cell->combInfo.lut_input_count; ++i)
                    EXPECT_EQ(cell->combInfo.lut_in[i], arch.combInfo.lut_in[i]);
                EXPECT_EQ(cell->combInfo.comb_out, arch.combInfo.comb_out);
                EXPECT_EQ(cell->combInfo.lut_input_count, arch.combInfo.lut_input_count);
                EXPECT_EQ(cell->combInfo.used_lut_input_count, arch.combInfo.used_lut_input_count);
                EXPECT_EQ(cell->combInfo.lut_bits_count, arch.combInfo.lut_bits_count);
                EXPECT_EQ(cell->combInfo.chain_shared_input_count, arch.combInfo.chain_shared_input_count);
                EXPECT_EQ(cell->combInfo.is_carry, arch.combInfo.is_carry);
                EXPECT_EQ(cell->combInfo.is_shared, arch.combInfo.is_shared);
                EXPECT_EQ(cell->combInfo.is_extended, arch.combInfo.is_extended);
                EXPECT_EQ(cell->combInfo.carry_start, arch.combInfo.carry_start);
                EXPECT_EQ(cell->combInfo.carry_end, arch.combInfo.carry_end);
                EXPECT_EQ(cell->combInfo.mlab_group, arch.combInfo.mlab_group);
            }
        }
        for (const auto &entry : nets) {
            auto *net = ctx->nets.at(entry.first).get();
            const auto &old = entry.second;
            EXPECT_EQ(net, old.identity);
            EXPECT_EQ(net->driver.cell, old.driver.cell);
            EXPECT_EQ(net->driver.port, old.driver.port);
            ASSERT_EQ(net->users.entries(), old.users.entries());
            auto stored_users = old.users;
            for (auto user : stored_users.enumerate()) {
                ASSERT_TRUE(net->users.count(user.index));
                EXPECT_EQ(net->users.at(user.index).cell, user.value.cell);
                EXPECT_EQ(net->users.at(user.index).port, user.value.port);
            }
            auto actual = net->users, expected = old.users;
            ASSERT_EQ(actual.capacity(), expected.capacity());
            // Include holes/free-list order, not only active user identities.
            for (int i = 0; i < old.users.capacity() + 8; ++i)
                EXPECT_EQ(actual.add(PortRef{}), expected.add(PortRef{}));
        }
        ASSERT_EQ(ctx->labs.size(), labs.size());
        for (size_t lab = 0; lab < labs.size(); ++lab) {
            const auto &now = ctx->labs[lab], &old = labs[lab];
            EXPECT_EQ(now.is_mlab, old.is_mlab);
            EXPECT_EQ(now.clk_wires, old.clk_wires);
            EXPECT_EQ(now.ena_wires, old.ena_wires);
            EXPECT_EQ(now.aclr_wires, old.aclr_wires);
            EXPECT_EQ(now.sclr_wire, old.sclr_wire);
            EXPECT_EQ(now.sload_wire, old.sload_wire);
            EXPECT_EQ(now.aclr_used, old.aclr_used);
            for (size_t alm = 0; alm < old.alms.size(); ++alm) {
                const auto &a = now.alms[alm], &b = old.alms[alm];
                EXPECT_EQ(a.comb_out, b.comb_out);
                EXPECT_EQ(a.sel_clk, b.sel_clk);
                EXPECT_EQ(a.sel_ena, b.sel_ena);
                EXPECT_EQ(a.sel_aclr, b.sel_aclr);
                EXPECT_EQ(a.sel_ef, b.sel_ef);
                EXPECT_EQ(a.ff_in, b.ff_in);
                EXPECT_EQ(a.ff_out, b.ff_out);
                EXPECT_EQ(a.lut_bels, b.lut_bels);
                EXPECT_EQ(a.ff_bels, b.ff_bels);
                EXPECT_EQ(a.l6_mode, b.l6_mode);
                EXPECT_EQ(a.carry_mode, b.carry_mode);
                EXPECT_EQ(a.clk_ena_idx, b.clk_ena_idx);
                EXPECT_EQ(a.aclr_idx, b.aclr_idx);
                EXPECT_EQ(a.unique_input_count, b.unique_input_count);
            }
        }
    }
};

class ReductionReassociateBackend : public ::testing::Test {
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *clock;
    std::vector<NetInfo *> inputs;
    CellInfo *producer, *consumer, *mate, *unrelated;
    std::string reason;
    void SetUp() override
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        ctx->attrs[id_step] = std::string("place");
        ctx->settings[ctx->id("target_freq")] = 1e8;
        clock = ctx->createNet(ctx->id("fixture_clock"));
        clock->is_global = true;
        ctx->createNet(ctx->id("$PACKER_GND_NET"));
        ctx->createNet(ctx->id("$PACKER_VCC_NET"));
        for (int i = 0; i < 10; ++i) {
            auto *source = ff(ctx->idf("source%d", i), nullptr, Loc(25, 28, 6 * i + 2));
            inputs.push_back(source->getPort(id_Q));
        }
        producer = lut("producer", id_MISTRAL_ALUT6, uint64_t(1) << 32,
                       {inputs[0], inputs[1], inputs[2], inputs[3], inputs[4], inputs[5]}, Loc(25, 29, 0));
        consumer = lut("consumer", id_MISTRAL_ALUT5, uint64_t(1) << 24,
                       {inputs[6], inputs[7], inputs[8], inputs[9], producer->getPort(id_Q)}, Loc(25, 30, 0));
        mate = lut("consumer_mate", id_MISTRAL_ALUT2, 8, {inputs[0], inputs[1]}, Loc(25, 30, 1));
        unrelated = lut("unrelated", id_MISTRAL_ALUT2, 6, {inputs[0], inputs[1]}, Loc(30, 20, 0));
        ff(ctx->id("capture"), consumer->getPort(id_Q), Loc(25, 31, 2));
        const auto port = ctx->id("public_consumer_output");
        ctx->ports[port] = PortInfo{port, consumer->getPort(id_Q), PORT_OUT, {}};
        for (auto *net : inputs) {
            auto a = net->users.add(PortRef{}), b = net->users.add(PortRef{}), c = net->users.add(PortRef{});
            net->users.remove(a); net->users.remove(c); net->users.remove(b);
        }
        ctx->assignArchInfo();
        for (auto *cell : {producer, consumer, unrelated}) {
            const auto data = ctx->bel_data(cell->bel).lab_data;
            ctx->assign_alm_lut_inputs(data.lab, data.alm);
        }
        ctx->check();
        for (const auto &cell : ctx->cells) ASSERT_TRUE(ctx->isBelLocationValid(cell.second->bel));
    }
    CellInfo *ff(IdString name, NetInfo *data, Loc where)
    {
        auto *cell = ctx->createCell(name, id_MISTRAL_FF);
        for (IdString pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN}) cell->addInput(pin);
        cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        cell->connectPort(id_CLK, clock);
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx->createNet(ctx->idf("%s$q", name.c_str(ctx.get()))));
        cell->connectPort(id_DATAIN, data ? data : cell->getPort(id_Q));
        ctx->assign_ff_info(cell);
        ctx->assign_default_pinmap(cell);
        ctx->bindBel(ctx->getBelByLocation(where), cell, STRENGTH_WEAK);
        return cell;
    }
    CellInfo *lut(const char *name, IdString type, uint64_t mask, const std::vector<NetInfo *> &nets, Loc where)
    {
        auto *cell = ctx->createCell(ctx->id(name), type);
        cell->params[id_LUT] = Property(int64_t(mask), 1u << nets.size());
        for (size_t pin = 0; pin < nets.size(); ++pin) {
            cell->addInput(pins[pin]);
            cell->connectPort(pins[pin], nets[pin]);
            cell->pin_data[pins[pin]].state = PIN_SIG;
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx->createNet(ctx->idf("%s$q", name)));
        ctx->assign_comb_info(cell);
        ctx->assign_default_pinmap(cell);
        ctx->bindBel(ctx->getBelByLocation(where), cell, STRENGTH_WEAK);
        return cell;
    }
    bool evaluate(const NetInfo *net, unsigned assignment) const
    {
        auto found = std::find(inputs.begin(), inputs.end(), net);
        if (found != inputs.end()) return (assignment >> size_t(found - inputs.begin())) & 1u;
        const auto *cell = net->driver.cell;
        unsigned row = 0;
        for (int pin = 0; pin < cell->combInfo.lut_input_count; ++pin) {
            bool value = evaluate(cell->getPort(pins[pin]), assignment);
            if (cell->get_pin_state(pins[pin]) == PIN_INV) value = !value;
            row |= unsigned(value) << pin;
        }
        return (uint64_t(cell->params.at(id_LUT).as_int64()) >> row) & 1u;
    }
    std::vector<bool> truth() const
    {
        std::vector<bool> result;
        for (unsigned assignment = 0; assignment < (1u << inputs.size()); ++assignment)
            result.push_back(evaluate(consumer->getPort(id_Q), assignment));
        return result;
    }
    bool run(const std::function<bool()> &accept = [] { return true; })
    {
        return reassociate_producer_trial(ctx.get(), "producer", id_A, accept, &reason);
    }
    void rejects_without_mutation()
    {
        Snapshot before(ctx.get());
        EXPECT_FALSE(run()) << reason;
        EXPECT_FALSE(reason.empty());
        before.expect_exact(ctx.get());
    }
};

TEST_F(ReductionReassociateBackend, PreservesCompositionAndSlotsAndRelocatesExpandedConsumer)
{
    const auto expected = truth();
    const auto producer_place = producer->bel, consumer_place = consumer->bel, mate_place = mate->bel;
    const auto producer_slot = producer->ports.at(id_A).user_idx;
    const auto unrelated_slot = unrelated->ports.at(id_A).user_idx;
    const auto unrelated_pins = unrelated->pin_data;
    const auto old_inputs = producer->ports;
    const auto old_output = consumer->getPort(id_Q);
    ASSERT_TRUE(run()) << reason;
    EXPECT_EQ(producer->type, id_MISTRAL_ALUT5);
    EXPECT_EQ(consumer->type, id_MISTRAL_ALUT6);
    EXPECT_EQ(producer->params.at(id_LUT).as_int64(), int64_t(1) << 16);
    EXPECT_EQ(consumer->params.at(id_LUT).as_int64(), int64_t(1) << 24);
    EXPECT_EQ(producer->bel, producer_place);
    EXPECT_NE(consumer->bel, consumer_place);
    EXPECT_EQ(mate->bel, mate_place);
    EXPECT_EQ(consumer->getPort(id_Q), old_output);
    EXPECT_EQ(consumer->getPort(id_F), inputs[0]);
    EXPECT_EQ(consumer->ports.at(id_F).user_idx, producer_slot);
    EXPECT_EQ(unrelated->ports.at(id_A).user_idx, unrelated_slot);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(producer->getPort(pins[i]), inputs[i + 1]);
        EXPECT_EQ(producer->ports.at(pins[i]).user_idx, old_inputs.at(pins[i + 1]).user_idx);
    }
    for (const auto &pin : unrelated_pins) {
        EXPECT_EQ(unrelated->pin_data.at(pin.first).state, pin.second.state);
        EXPECT_EQ(unrelated->pin_data.at(pin.first).bel_pins, pin.second.bel_pins);
    }
    const auto data = ctx->bel_data(consumer->bel).lab_data;
    const auto &alm = ctx->labs.at(data.lab).alms.at(data.alm);
    EXPECT_TRUE(alm.l6_mode);
    EXPECT_EQ(ctx->getBoundBelCell(alm.lut_bels[1 - data.idx]), nullptr);
    for (IdString pin : pins) EXPECT_FALSE(consumer->pin_data.at(pin).bel_pins.empty());
    EXPECT_EQ(truth(), expected);
    for (const auto &cell : ctx->cells) EXPECT_TRUE(ctx->isBelLocationValid(cell.second->bel));
    ctx->check();
}

TEST_F(ReductionReassociateBackend, PreservesPinInversionsAndCorrelatedConsumerAlias)
{
    consumer->disconnectPort(id_A);
    consumer->connectPort(id_A, inputs[0]);
    producer->pin_data[id_A].state = PIN_INV;
    producer->pin_data[id_D].state = PIN_INV;
    consumer->pin_data[id_A].state = PIN_INV;
    consumer->pin_data[id_E].state = PIN_INV;
    ctx->assignArchInfo();
    const auto expected = truth();
    const auto original_consumer = consumer->bel;
    const auto removed_slot = producer->ports.at(id_A).user_idx;
    const auto reused_slot = consumer->ports.at(id_A).user_idx;
    const auto count = inputs[0]->users.entries();
    ASSERT_TRUE(run()) << reason;
    EXPECT_EQ(consumer->type, id_MISTRAL_ALUT5);
    EXPECT_EQ(consumer->bel, original_consumer);
    EXPECT_EQ(consumer->ports.at(id_A).user_idx, reused_slot);
    EXPECT_FALSE(inputs[0]->users.count(removed_slot));
    EXPECT_EQ(inputs[0]->users.entries(), count - 1);
    EXPECT_EQ(producer->get_pin_state(id_C), PIN_INV);
    EXPECT_EQ(consumer->get_pin_state(id_A), PIN_INV);
    EXPECT_EQ(consumer->get_pin_state(id_E), PIN_INV);
    EXPECT_EQ(truth(), expected);
    ctx->check();
}

TEST_F(ReductionReassociateBackend, CallbackRejectionAndExceptionRestoreExactState)
{
    Snapshot before(ctx.get());
    bool inspected = false;
    EXPECT_FALSE(run([&] { inspected = true; EXPECT_EQ(consumer->type, id_MISTRAL_ALUT6); return false; }));
    EXPECT_TRUE(inspected);
    before.expect_exact(ctx.get());
    EXPECT_THROW(run([&]() -> bool { EXPECT_EQ(consumer->type, id_MISTRAL_ALUT6); throw std::runtime_error("test"); }),
                 std::runtime_error);
    before.expect_exact(ctx.get());
    ctx->check();
}

TEST_F(ReductionReassociateBackend, RejectsProtectedAndExternallyObservedProducer)
{
    auto kept = ctx->id("keep");
    producer->attrs[kept] = 1;
    rejects_without_mutation();
    producer->attrs.erase(kept);
    mate->attrs[kept] = 1;
    rejects_without_mutation();
    mate->attrs.erase(kept);
    producer->getPort(id_A)->attrs[kept] = 1;
    rejects_without_mutation();
    producer->getPort(id_A)->attrs.erase(kept);
    const auto port = ctx->id("observable_producer");
    ctx->ports[port] = PortInfo{port, producer->getPort(id_Q), PORT_OUT, {}};
    rejects_without_mutation();
    ctx->ports.erase(port);
    producer->cluster = producer->name;
    rejects_without_mutation();
    producer->cluster = ClusterId();
    producer->constr_x = 1;
    rejects_without_mutation();
    producer->constr_x = 0;
    producer->constr_abs_z = true;
    rejects_without_mutation();
    producer->constr_abs_z = false;
    ctx->fes_fence_active = true;
    rejects_without_mutation();
}

TEST_F(ReductionReassociateBackend, RejectsWrongCheckpointRoutesAndMalformedInputs)
{
    ctx->attrs[id_step] = std::string("route");
    rejects_without_mutation();
    ctx->attrs[id_step] = std::string("place");
    const auto wire = ctx->getBelPinWire(unrelated->bel, id_COMBOUT);
    ASSERT_NE(wire, WireId());
    ctx->bindWire(wire, unrelated->getPort(id_Q), STRENGTH_WEAK);
    rejects_without_mutation();
    ctx->unbindWire(wire);
    producer->pin_data[id_B].state = PIN_0;
    rejects_without_mutation();
    producer->pin_data[id_B].state = PIN_SIG;
    const auto mask = producer->params.at(id_LUT);
    producer->params[id_LUT] = Property(int64_t(1), 32);
    rejects_without_mutation();
    producer->params[id_LUT] = mask;
    producer->getPort(id_Q)->users.add(PortRef{unrelated, id_A});
    rejects_without_mutation();
}

TEST_F(ReductionReassociateBackend, ExhaustedEmptyAlmSearchRestoresExactState)
{
    const auto home = ctx->getBelLocation(consumer->bel);
    std::vector<BelId> blockers;
    for (auto bel : ctx->getBels()) {
        if (!ctx->isValidBelForCellType(id_MISTRAL_ALUT2, bel)) continue;
        const auto data = ctx->bel_data(bel).lab_data;
        if (data.idx != 0) continue;
        const auto at = ctx->getBelLocation(bel);
        if (std::abs(at.x - home.x) + std::abs(at.y - home.y) > 2) continue;
        const auto &alm = ctx->labs.at(data.lab).alms.at(data.alm);
        bool empty = true;
        for (auto other : {alm.lut_bels[0], alm.lut_bels[1], alm.ff_bels[0], alm.ff_bels[1], alm.ff_bels[2], alm.ff_bels[3]})
            empty &= ctx->checkBelAvail(other);
        if (empty) blockers.push_back(bel);
    }
    for (size_t i = 0; i < blockers.size(); ++i) {
        const auto name = "placement_blocker_" + std::to_string(i);
        lut(name.c_str(), id_MISTRAL_ALUT2, 8, {inputs[0], inputs[1]}, ctx->getBelLocation(blockers[i]));
    }
    rejects_without_mutation();
    EXPECT_EQ(reason, "expanded consumer has no legal nearby placement");
    ctx->check();
}

TEST_F(ReductionReassociateBackend, MissingNameAndInvalidPinDoNotInternOrMutate)
{
    Snapshot before(ctx.get());
    auto first = ctx->id("reassociation_missing_before");
    EXPECT_FALSE(reassociate_producer(ctx.get(), "unknown_reassociation_producer", id_A, &reason));
    auto next = ctx->id("reassociation_missing_after");
    EXPECT_EQ(next.index, first.index + 1);
    EXPECT_FALSE(reassociate_producer(ctx.get(), "producer", id_Q, &reason));
    before.expect_exact(ctx.get());
}
} // namespace
