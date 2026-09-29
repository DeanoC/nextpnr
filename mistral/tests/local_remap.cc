#include "gtest/gtest.h"
#include "local_remap_policy.h"
#include "reduction_balance_policy.h"
#include "nextpnr.h"
#include <map>
#include <memory>

USING_NEXTPNR_NAMESPACE

NEXTPNR_NAMESPACE_BEGIN
int capture_locality(Context *, const std::string &, int, int);
NEXTPNR_NAMESPACE_END

TEST(CaptureLocality, RenamedPipelineMovesWithoutRewiringAndHonorsProtection)
{
    ArchArgs args; args.device = "5CSEBA6U23I7";
    auto ctx = std::make_unique<Context>(args);
    ctx->settings[ctx->id("target_freq")] = 130e6;
    auto *clock = ctx->createNet(ctx->id("arbitrary_clock"));
    clock->is_global = true;
    clock->clkconstr = std::make_unique<ClockConstraint>();
    clock->clkconstr->period = DelayPair(7692);
    clock->clkconstr->high = clock->clkconstr->low = DelayPair(3846);
    auto *clock_driver = ctx->createCell(ctx->id("arbitrary_clock_driver"), id_MISTRAL_CLKBUF);
    clock_driver->addOutput(id_Q); clock_driver->connectPort(id_Q,clock);
    ctx->createNet(ctx->id("$PACKER_GND_NET"));
    ctx->createNet(ctx->id("$PACKER_VCC_NET"));
    auto *hard = ctx->createCell(ctx->id("renamed_fixed_source"), id_cyclonev_hps_interface_fpga2sdram);
    auto clock_port = ctx->id("rd_clk_0"), data_port = ctx->id("rd_data_0[0]");
    hard->addInput(clock_port); hard->connectPort(clock_port,clock);
    hard->addOutput(data_port);
    auto *input = ctx->createNet(ctx->id("renamed_data")); hard->connectPort(data_port,input);
    auto ff = [&](const char *name, NetInfo *data) {
        auto *cell = ctx->createCell(ctx->id(name),id_MISTRAL_FF);
        for (IdString pin : {id_CLK,id_ENA,id_ACLR,id_SCLR,id_SLOAD,id_SDATA,id_DATAIN}) cell->addInput(pin);
        cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        cell->connectPort(id_CLK,clock); cell->connectPort(id_DATAIN,data);
        cell->addOutput(id_Q); cell->connectPort(id_Q,ctx->createNet(ctx->idf("%s$out",name)));
        return cell;
    };
    auto *capture = ff("renamed_capture",input), *downstream = ff("renamed_consumer",capture->getPort(id_Q));
    auto other_clock_port = ctx->id("rd_clk_1"), other_data_port = ctx->id("rd_data_1[0]");
    hard->addInput(other_clock_port); hard->connectPort(other_clock_port,clock);
    hard->addOutput(other_data_port);
    auto *other_input = ctx->createNet(ctx->id("unselected_channel_data")); hard->connectPort(other_data_port,other_input);
    auto *other_capture = ff("unselected_channel_capture",other_input);
    auto *other_downstream = ff("unselected_channel_consumer",other_capture->getPort(id_Q));
    ctx->assignArchInfo();
    bool bound = false;
    for (auto bel : ctx->getBels()) if (ctx->getBelType(bel) == hard->type) { ctx->bindBel(bel,hard,STRENGTH_WEAK); bound = true; break; }
    ASSERT_TRUE(bound);
    ctx->bindBel(ctx->getBelByLocation(Loc(25,23,2)),capture,STRENGTH_WEAK);
    ctx->bindBel(ctx->getBelByLocation(Loc(25,17,2)),downstream,STRENGTH_WEAK);
    ctx->bindBel(ctx->getBelByLocation(Loc(27,23,2)),other_capture,STRENGTH_WEAK);
    ctx->bindBel(ctx->getBelByLocation(Loc(27,17,2)),other_downstream,STRENGTH_WEAK);
    auto other_old = other_capture->bel;
    ASSERT_TRUE(ctx->isBelLocationValid(capture->bel));
    auto old = capture->bel;
    auto input_slot = capture->ports.at(id_DATAIN).user_idx, output_slot = downstream->ports.at(id_DATAIN).user_idx;
    std::string report = "{\"critical_paths\":[{\"max_delay\":7.692,\"path\":["
        "{\"type\":\"routing\",\"delay\":10.0,\"net\":\"renamed_data\",\"from\":{\"cell\":\"renamed_fixed_source\",\"port\":\"rd_data_0[0]\"}},"
        "{\"type\":\"setup\",\"delay\":-0.196,\"to\":{\"cell\":\"renamed_capture\",\"port\":\"DATAIN\",\"loc\":[25,23]}}]}]}";
    capture->attrs[ctx->id("keep")] = 1;
    EXPECT_EQ(capture_locality(ctx.get(),report,1,24),0);
    EXPECT_EQ(capture->bel,old);
    capture->attrs.erase(ctx->id("keep"));
    auto *observer = ctx->createCell(ctx->id("implicit_clock_observer"),id_MISTRAL_FF);
    observer->addInput(id_CLK); observer->connectPort(id_CLK,capture->getPort(id_Q));
    EXPECT_EQ(capture_locality(ctx.get(),report,64,24),0);
    observer->disconnectPort(id_CLK); ctx->cells.erase(observer->name);
    EXPECT_EQ(capture_locality(ctx.get(),report,64,24),1);
    EXPECT_EQ(other_capture->bel,other_old);
    EXPECT_NE(capture->bel,old);
    EXPECT_EQ(capture->getPort(id_DATAIN),input);
    EXPECT_EQ(capture->ports.at(id_DATAIN).user_idx,input_slot);
    EXPECT_EQ(downstream->ports.at(id_DATAIN).user_idx,output_slot);
    EXPECT_EQ(downstream->getPort(id_DATAIN),capture->getPort(id_Q));
    EXPECT_TRUE(ctx->isBelLocationValid(capture->bel));
    EXPECT_EQ(ctx->cells.size(),6u);
    ctx->check();
}

using namespace local_remap_policy;

TEST(LocalRemapComposition, ExhaustiveTwoInputTablesAndInvertedIntermediate)
{
    for (unsigned inner = 0; inner < 16; ++inner)
        for (unsigned outer = 0; outer < 16; ++outer)
            for (bool inverted : {false, true}) {
                auto c = compose(inner, {{0, false}, {1, false}}, outer,
                                 {{INTERMEDIATE, inverted}, {2, false}});
                ASSERT_TRUE(c.valid);
                EXPECT_EQ(c.signals, (std::vector<int>{0, 1, 2}));
                for (unsigned row = 0; row < 8; ++row) {
                    unsigned mid = ((inner >> (row & 3)) & 1) ^ inverted;
                    bool expected = (outer >> (mid | ((row >> 2) << 1))) & 1;
                    EXPECT_EQ((c.mask >> row) & 1, expected);
                }
            }
}

TEST(LocalRemapComposition, ConstantsAliasesAndInputInversion)
{
    // inner = !a XOR 1 = a; outer = inner AND !a = 0.
    auto c = compose(0x6, {{7, true}, {ONE, false}}, 0x8,
                     {{INTERMEDIATE, false}, {7, true}});
    ASSERT_TRUE(c.valid);
    EXPECT_EQ(c.signals, (std::vector<int>{7}));
    EXPECT_EQ(c.mask, 0u);
    // Both outer inputs alias the intermediate, with opposite polarities.
    auto duplicate = compose(0x8, {{0, false}, {ZERO, true}}, 0xe,
                            {{INTERMEDIATE, false}, {INTERMEDIATE, true}});
    ASSERT_TRUE(duplicate.valid);
    EXPECT_EQ(duplicate.mask, 3u);
}

TEST(LocalRemapComposition, SixInputBit63AndBounds)
{
    auto c = compose(0x8000000000000000ULL,
                     {{0,false},{1,false},{2,false},{3,false},{4,false},{5,false}},
                     0x8, {{INTERMEDIATE,false},{ONE,false}});
    ASSERT_TRUE(c.valid);
    EXPECT_EQ(c.mask, 0x8000000000000000ULL);
    EXPECT_FALSE(compose(0x8, {{0,false},{1,false}}, 0,
                         {{INTERMEDIATE,false},{2,false},{3,false},{4,false},{5,false},{6,false}}).valid);
    EXPECT_FALSE(compose(0, {{INTERMEDIATE,false},{0,false}}, 0,
                         {{INTERMEDIATE,false},{1,false}}).valid);
    EXPECT_FALSE(compose(0, {{0,false},{1,false}}, 0, {{0,false},{1,false}}).valid);
}

TEST(ReductionBalance, RecognizesSixteenBitZeroConeAndRejectsNonCube)
{
    using reduction_balance_policy::Node;
    std::vector<Node> network = {
        {16, 0x0001, {{12,false},{13,false},{14,false},{15,false}}},
        {17, 0x0001, {{4,false},{5,false},{6,false},{7,false}}},
        {18, 1ULL << 48, {{8,false},{9,false},{10,false},{11,false},{16,false},{17,false}}},
        {19, 1ULL << 16, {{0,false},{1,false},{2,false},{3,false},{18,false}}},
    };
    auto result = reduction_balance_policy::recognize(network, 19);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.outputs, (std::vector<int>{16,17,18,19}));
    ASSERT_EQ(result.literals.size(), 16u);
    for (int bit=0;bit<16;++bit) {
        EXPECT_EQ(result.literals[bit].signal, bit);
        EXPECT_FALSE(result.literals[bit].required);
    }
    network.back().mask |= 1ULL << 17;
    EXPECT_FALSE(reduction_balance_policy::recognize(network,19).valid);
    network.back().mask = 1ULL << 16;
    network.front().inputs.front().inverted = true;
    auto inverted = reduction_balance_policy::recognize(network,19);
    ASSERT_TRUE(inverted.valid);
    EXPECT_TRUE(inverted.literals[12].required);
    network.front().inputs[1].signal = 12;
    EXPECT_FALSE(reduction_balance_policy::recognize(network,19).valid);
}

TEST(ReductionBalance, PreservesConsumerSlotsAndUnrelatedFanout)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    auto ctx = std::make_unique<Context>(args);
    const IdString pins[] = {id_A, id_B, id_C, id_D, id_E, id_F};
    std::vector<NetInfo *> inputs;
    std::map<NetInfo *, store_index<PortRef>> observer_slots, cone_slots;
    for (int i = 0; i < 16; ++i) {
        auto net = ctx->createNet(ctx->idf("errors[%d]", i));
        inputs.push_back(net);
        auto observer = ctx->createCell(ctx->idf("observer_%d", i), id_MISTRAL_ALUT2);
        observer->addInput(id_A);
        observer->connectPort(id_A, net);
        observer_slots.emplace(net, observer->ports.at(id_A).user_idx);
    }
    auto lut = [&](const char *name, IdString type, uint64_t mask, std::vector<NetInfo *> ins) {
        auto cell = ctx->createCell(ctx->id(name), type);
        cell->params[id_LUT] = Property(int64_t(mask), 1 << ins.size());
        for (size_t i = 0; i < ins.size(); ++i) {
            cell->addInput(pins[i]);
            cell->connectPort(pins[i], ins[i]);
            cell->pin_data[pins[i]].state = PIN_SIG;
            ASSERT_TRUE(cone_slots.emplace(ins[i], cell->ports.at(pins[i]).user_idx).second);
        }
        cell->addOutput(id_Q);
        cell->connectPort(id_Q, ctx->createNet(ctx->idf("%s$q", name)));
    };
    lut("leaf_a", id_MISTRAL_ALUT4, 1, {inputs[8], inputs[9], inputs[10], inputs[11]});
    lut("leaf_b", id_MISTRAL_ALUT4, 1, {inputs[12], inputs[13], inputs[14], inputs[15]});
    auto leaf_a = ctx->cells.at(ctx->id("leaf_a")).get();
    auto leaf_b = ctx->cells.at(ctx->id("leaf_b")).get();
    lut("inner", id_MISTRAL_ALUT6, 1ULL << 48,
        {inputs[4], inputs[5], inputs[6], inputs[7], leaf_a->getPort(id_Q), leaf_b->getPort(id_Q)});
    auto inner = ctx->cells.at(ctx->id("inner")).get();
    lut("root", id_MISTRAL_ALUT5, 1ULL << 16, {inputs[0], inputs[1], inputs[2], inputs[3], inner->getPort(id_Q)});
    auto root = ctx->cells.at(ctx->id("root")).get();
    auto output = root->getPort(id_Q);
    std::map<NetInfo *, PortRef> observers;
    for (const auto &entry : observer_slots) observers.emplace(entry.first, entry.first->users.at(entry.second));
    ASSERT_TRUE(ctx->balance_reduction("root"));
    EXPECT_EQ(root->getPort(id_Q), output);
    for (const auto &entry : cone_slots) {
        auto net = entry.first;
        int found = 0;
        for (auto user : net->users.enumerate()) {
            if (user.value.cell->name.str(ctx.get()).find("observer_") == 0) continue;
            ++found;
            EXPECT_EQ(user.index, entry.second) << net->name.str(ctx.get());
            EXPECT_EQ(user.value.cell->getPort(user.value.port), net);
            EXPECT_EQ(user.value.cell->ports.at(user.value.port).user_idx, user.index);
        }
        EXPECT_EQ(found, 1);
        EXPECT_EQ(net->users.entries(), observer_slots.count(net) ? 2 : 1);
    }
    for (const auto &entry : observer_slots) {
        auto user = entry.first->users.at(entry.second);
        EXPECT_EQ(user.cell, observers.at(entry.first).cell);
        EXPECT_EQ(user.port, observers.at(entry.first).port);
    }
    std::map<NetInfo *, int> input_bits;
    for (int i = 0; i < 16; ++i) input_bits.emplace(inputs[i], i);
    auto evaluate = [&](auto &&self, NetInfo *net, unsigned row) -> bool {
        auto bit = input_bits.find(net);
        if (bit != input_bits.end()) return (row >> bit->second) & 1;
        auto cell = net->driver.cell;
        unsigned lut_row = 0;
        for (size_t i = 0; i < cell->ports.size() - 1; ++i)
            lut_row |= unsigned(self(self, cell->getPort(pins[i]), row)) << i;
        return (uint64_t(cell->params.at(id_LUT).as_int64()) >> lut_row) & 1;
    };
    for (unsigned row = 0; row < 65536; ++row) EXPECT_EQ(evaluate(evaluate, output, row), row == 0);
    // Repeat the real rewrite with one inverted leaf: now exactly bit 8 must be
    // high. This exercises pin polarity in the transformed truth tables.
    auto changed = inputs[8]->users.at(cone_slots.at(inputs[8]));
    changed.cell->pin_data[changed.port].state = PIN_INV;
    ASSERT_TRUE(ctx->balance_reduction("root"));
    for (unsigned row = 0; row < 65536; ++row)
        EXPECT_EQ(evaluate(evaluate, output, row), row == (1u << 8));
}

TEST(ReductionBalance, RecognizesElevenLiteralConeWithMixedPolarities)
{
    using reduction_balance_policy::Node;
    // Both intermediate outputs are active low. The three inverted external
    // pins make the conjunction require those raw signals to be low.
    std::vector<Node> network = {
        {11, 0x7fff, {{0,false},{1,true},{2,false},{3,false}}},
        {12, 0xffff7fffULL, {{4,false},{5,false},{6,true},{7,false},{11,false}}},
        {13, 0x80, {{8,false},{9,true},{10,false},{12,false}}},
    };
    auto result = reduction_balance_policy::recognize(network, 13);
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.outputs, (std::vector<int>{11,12,13}));
    ASSERT_EQ(result.literals.size(), 11u);
    for (int bit = 0; bit < 11; ++bit) {
        EXPECT_EQ(result.literals[bit].signal, bit);
        EXPECT_EQ(result.literals[bit].required, bit != 1 && bit != 6 && bit != 9);
    }
    network.back().mask |= 1;
    EXPECT_FALSE(reduction_balance_policy::recognize(network, 13).valid);
    network.back().mask = 0x80;
    network[1].inputs[1].signal = 0;
    EXPECT_FALSE(reduction_balance_policy::recognize(network, 13).valid);
}

namespace {
const IdString reduction_pins[] = {id_A, id_B, id_C, id_D, id_E, id_F};

struct ElevenLiteralCone {
    std::unique_ptr<Context> ctx;
    std::vector<NetInfo *> inputs;
    CellInfo *leaf, *middle, *root;
    NetInfo *output;

    ElevenLiteralCone()
    {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<Context>(args);
        for (int bit = 0; bit < 11; ++bit) {
            inputs.push_back(ctx->createNet(ctx->idf("status_bits[%d]", bit)));
            observer(ctx->idf("early_observer_%d", bit), inputs.back());
        }
        auto lut = [&](const char *name, IdString type, uint64_t mask, const std::vector<NetInfo *> &ins) {
            auto *cell = ctx->createCell(ctx->id(name), type);
            cell->params[id_LUT] = Property(int64_t(mask), 1 << ins.size());
            for (size_t pin = 0; pin < ins.size(); ++pin) {
                cell->addInput(reduction_pins[pin]);
                cell->connectPort(reduction_pins[pin], ins[pin]);
                cell->pin_data[reduction_pins[pin]].state = PIN_SIG;
            }
            cell->addOutput(id_Q);
            cell->connectPort(id_Q, ctx->createNet(ctx->idf("%s$q", name)));
            return cell;
        };
        leaf = lut("first_phase", id_MISTRAL_ALUT4, 0x7fff,
                   {inputs[0], inputs[1], inputs[2], inputs[3]});
        leaf->pin_data[id_B].state = PIN_INV;
        middle = lut("second_phase", id_MISTRAL_ALUT5, 0xffff7fffULL,
                     {inputs[4], inputs[5], inputs[6], inputs[7], leaf->getPort(id_Q)});
        middle->pin_data[id_C].state = PIN_INV;
        root = lut("conjunction", id_MISTRAL_ALUT4, 0x80,
                   {inputs[8], inputs[9], inputs[10], middle->getPort(id_Q)});
        root->pin_data[id_B].state = PIN_INV;
        output = root->getPort(id_Q);
        // Unrelated users on both sides of the cone user exercise actual live
        // indexed_store slots, rather than only counting remaining consumers.
        for (int bit = 0; bit < 11; ++bit)
            observer(ctx->idf("late_observer_%d", bit), inputs[bit]);
        observer(ctx->id("output_observer"), output);
    }

    CellInfo *observer(IdString name, NetInfo *net)
    {
        auto *cell = ctx->createCell(name, id_MISTRAL_ALUT2);
        cell->addInput(id_A);
        cell->connectPort(id_A, net);
        return cell;
    }

    bool evaluate(NetInfo *net, unsigned row) const
    {
        for (size_t bit = 0; bit < inputs.size(); ++bit)
            if (net == inputs[bit]) return (row >> bit) & 1;
        auto *cell = net->driver.cell;
        unsigned lut_row = 0;
        for (size_t pin = 0; pin < cell->ports.size() - 1; ++pin) {
            bool value = evaluate(cell->getPort(reduction_pins[pin]), row);
            if (cell->get_pin_state(reduction_pins[pin]) == PIN_INV) value = !value;
            lut_row |= unsigned(value) << pin;
        }
        return (uint64_t(cell->params.at(id_LUT).as_int64()) >> lut_row) & 1;
    }
};
}

TEST(ReductionBalance, ExhaustiveElevenLiteralRewritePreservesIdentitiesAndEveryConsumerSlot)
{
    ElevenLiteralCone fixture;
    auto &ctx = fixture.ctx;
    struct SavedUser {
        NetInfo *net;
        store_index<PortRef> slot;
        PortRef port;
        bool inside;
    };
    std::vector<SavedUser> users;
    std::map<IdString, CellInfo *> cells;
    std::map<IdString, NetInfo *> nets;
    for (const auto &entry : ctx->cells) cells.emplace(entry.first, entry.second.get());
    for (const auto &entry : ctx->nets) {
        nets.emplace(entry.first, entry.second.get());
        for (auto user : entry.second->users.enumerate()) {
            bool inside = user.value.cell == fixture.leaf || user.value.cell == fixture.middle ||
                          user.value.cell == fixture.root;
            users.push_back({entry.second.get(), user.index, user.value, inside});
        }
    }
    std::vector<bool> before;
    const unsigned required = ((1u << 11) - 1) ^ (1u << 1) ^ (1u << 6) ^ (1u << 9);
    for (unsigned row = 0; row < (1u << 11); ++row) {
        before.push_back(fixture.evaluate(fixture.output, row));
        EXPECT_EQ(before.back(), row == required);
    }

    ASSERT_TRUE(ctx->balance_reduction("conjunction"));
    EXPECT_EQ(fixture.root->type, id_MISTRAL_ALUT2);
    EXPECT_EQ(fixture.leaf->type, id_MISTRAL_ALUT6);
    EXPECT_EQ(fixture.middle->type, id_MISTRAL_ALUT5);
    EXPECT_EQ(fixture.root->getPort(id_Q), fixture.output);
    ASSERT_EQ(ctx->cells.size(), cells.size());
    ASSERT_EQ(ctx->nets.size(), nets.size());
    for (const auto &entry : cells) EXPECT_EQ(ctx->cells.at(entry.first).get(), entry.second);
    for (const auto &entry : nets) EXPECT_EQ(ctx->nets.at(entry.first).get(), entry.second);
    size_t live_users = 0;
    for (const auto &entry : ctx->nets) live_users += entry.second->users.entries();
    ASSERT_EQ(live_users, users.size());
    for (const auto &user : users) {
        ASSERT_TRUE(user.net->users.count(user.slot));
        const auto &current = user.net->users.at(user.slot);
        EXPECT_EQ(current.cell->getPort(current.port), user.net);
        EXPECT_EQ(current.cell->ports.at(current.port).user_idx, user.slot);
        if (!user.inside) {
            EXPECT_EQ(current.cell, user.port.cell);
            EXPECT_EQ(current.port, user.port.port);
        }
    }
    for (unsigned row = 0; row < (1u << 11); ++row)
        EXPECT_EQ(fixture.evaluate(fixture.output, row), before[row]) << "assignment " << row;
    ctx->check();
}

TEST(ReductionBalance, ElevenLiteralRewriteRejectsProtectedSharedNonCubeAndClockConsumers)
{
    ElevenLiteralCone fixture;
    auto &ctx = fixture.ctx;
    fixture.leaf->attrs[ctx->id("keep")] = 1;
    EXPECT_FALSE(ctx->balance_reduction("conjunction"));
    fixture.leaf->attrs.erase(ctx->id("keep"));
    fixture.middle->attrs[ctx->id("dont_touch")] = 1;
    EXPECT_FALSE(ctx->balance_reduction("conjunction"));
    fixture.middle->attrs.erase(ctx->id("dont_touch"));
    fixture.inputs[3]->attrs[ctx->id("keep")] = 1;
    EXPECT_FALSE(ctx->balance_reduction("conjunction"));
    fixture.inputs[3]->attrs.erase(ctx->id("keep"));
    fixture.inputs[3]->is_global = true;
    EXPECT_FALSE(ctx->balance_reduction("conjunction"));
    fixture.inputs[3]->is_global = false;

    auto *shared = fixture.observer(ctx->id("shared_intermediate"), fixture.leaf->getPort(id_Q));
    EXPECT_FALSE(ctx->balance_reduction("conjunction"));
    shared->disconnectPort(id_A);
    ctx->cells.erase(shared->name);
    fixture.root->params[id_LUT] = Property(0x81, 16);
    EXPECT_FALSE(ctx->balance_reduction("conjunction"));
    fixture.root->params[id_LUT] = Property(0x80, 16);

    // No SDC clock constraint is needed to recognize an implicit clock sink.
    auto *clock_sink = ctx->createCell(ctx->id("unconstrained_clock_sink"), id_MISTRAL_FF);
    clock_sink->addInput(id_CLK);
    clock_sink->connectPort(id_CLK, fixture.output);
    EXPECT_FALSE(ctx->balance_reduction("conjunction"));
    clock_sink->disconnectPort(id_CLK);
    clock_sink->connectPort(id_CLK, fixture.inputs[7]);
    EXPECT_FALSE(ctx->balance_reduction("conjunction"));
    clock_sink->disconnectPort(id_CLK);
    ctx->cells.erase(clock_sink->name);

    EXPECT_EQ(fixture.leaf->type, id_MISTRAL_ALUT4);
    EXPECT_EQ(fixture.middle->type, id_MISTRAL_ALUT5);
    EXPECT_EQ(fixture.root->type, id_MISTRAL_ALUT4);
    EXPECT_EQ(fixture.root->getPort(id_Q), fixture.output);
    const unsigned required = ((1u << 11) - 1) ^ (1u << 1) ^ (1u << 6) ^ (1u << 9);
    for (unsigned row = 0; row < (1u << 11); ++row)
        EXPECT_EQ(fixture.evaluate(fixture.output, row), row == required);
    ctx->check();
}
