#include <memory>
#include <sstream>
#include "gtest/gtest.h"
#include "io_delay.h"
#include "jsonwrite.h"
#include "json_frontend.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE

// Synthetic architecture models exercise the common timing graph without
// inventing production Cyclone V pad characterization.
class IoBoundaryTestContext : public Context
{
  public:
    using Context::Context;
    dict<std::pair<IdString, bool>, std::vector<RegisteredIoTiming>> boundary_models;
    dict<IdString, std::vector<PrimitiveClockRequirement>> clock_models;
    bool native_models = false;
    std::vector<PrimitiveClockRequirement> getPrimitiveClockRequirements(const CellInfo *cell) const override
    {
        auto found = clock_models.find(cell->name);
        return found == clock_models.end() ? (native_models ? Arch::getPrimitiveClockRequirements(cell)
                                                            : std::vector<PrimitiveClockRequirement>{}) : found->second;
    }
    std::vector<RegisteredIoTiming> getRegisteredIoTiming(const CellInfo *cell, IdString pad, bool input) const override
    {
        auto found = boundary_models.find(std::make_pair(cell->name, input));
        if (native_models) return Arch::getRegisteredIoTiming(cell, pad, input);
        return pad == id_PAD && found != boundary_models.end() ? found->second : std::vector<RegisteredIoTiming>{};
    }
};

class IoDelayTest : public ::testing::Test
{
  protected:
    std::unique_ptr<Context> ctx;
    NetInfo *clock;
    CellInfo *input, *output, *capture, *launch, *bidir;

    NetInfo *net(const std::string &name) { return ctx->createNet(ctx->id(name)); }

    void port(const std::string &name, PortType type, NetInfo *net)
    {
        PortInfo info;
        info.name = ctx->id(name); info.type = type; info.net = net;
        ctx->ports[info.name] = info;
    }

    CellInfo *ff(const std::string &name)
    {
        auto *cell = ctx->createCell(ctx->id(name), id_MISTRAL_FF);
        for (auto pin : {id_CLK, id_ENA, id_ACLR, id_SCLR, id_SLOAD, id_SDATA, id_DATAIN}) cell->addInput(pin);
        cell->addOutput(id_Q);
        cell->connectPort(id_CLK, clock);
        cell->pin_data[id_ENA].state = PIN_1;
        cell->pin_data[id_ACLR].state = PIN_1;
        cell->pin_data[id_SCLR].state = PIN_0;
        cell->pin_data[id_SLOAD].state = PIN_0;
        cell->connectPort(id_Q, net(name + "$q"));
        return cell;
    }

    void sdc(const std::string &text)
    {
        std::istringstream stream(text);
        ctx->read_sdc(stream);
    }

    void SetUp() override
    {
        ArchArgs args; args.device = "5CSEBA6U23I7";
        ctx = std::make_unique<IoBoundaryTestContext>(args);
        ctx->settings[ctx->id("target_freq")] = 1e8;
        net("$PACKER_GND_NET"); net("$PACKER_VCC_NET");
        clock = net("clock");
        port("clk", PORT_IN, clock);
        input = ctx->createCell(ctx->id("input_pad"), id_MISTRAL_IB);
        input->addInput(id_PAD); input->addOutput(id_O);
        input->connectPort(id_PAD, net("din$pad")); input->connectPort(id_O, net("din$data"));
        port("din", PORT_IN, input->getPort(id_PAD));
        output = ctx->createCell(ctx->id("output_pad"), id_MISTRAL_OB);
        output->addInput(id_I); output->addOutput(id_PAD);
        output->connectPort(id_PAD, net("dout$pad"));
        port("dout", PORT_OUT, output->getPort(id_PAD));
        capture = ff("capture"); capture->connectPort(id_DATAIN, input->getPort(id_O));
        launch = ff("launch"); output->connectPort(id_I, launch->getPort(id_Q));
        bidir = ctx->createCell(ctx->id("dq_pad"), id_MISTRAL_IO);
        bidir->addInout(id_PAD); bidir->addInput(id_I); bidir->addInput(id_OE); bidir->addOutput(id_O);
        bidir->connectPort(id_PAD, net("dq$pad")); bidir->connectPort(id_O, net("dq$data"));
        bidir->connectPort(id_I, launch->getPort(id_Q)); bidir->connectPort(id_OE, launch->getPort(id_Q));
        port("dq[0]", PORT_INOUT, bidir->getPort(id_PAD));
        ctx->assignArchInfo();
        sdc("create_clock -period 10 -name memory [get_ports clk]\n");
    }

    CriticalPath find_path(const TimingResult &result, CellInfo *sink)
    {
        for (const auto &entry : result.clock_paths)
            if (entry.second.segments.back().to.first == sink->name) return entry.second;
        for (const auto &entry : result.xclock_paths)
            if (entry.segments.back().to.first == sink->name) return entry;
        ADD_FAILURE() << "Missing path to " << sink->name.str(ctx.get());
        return {};
    }

    void registered_boundaries()
    {
        bidir->type = id_MISTRAL_SDRIO;
        for (auto param : {"IOREG_IN", "IOREG_OUT", "IOREG_OE"}) bidir->params[ctx->id(param)] = 1;
        for (auto pin : {id_CLK, id_CLKIN}) {
            bidir->addInput(pin);
            bidir->connectPort(pin, clock);
        }
        auto &models = static_cast<IoBoundaryTestContext *>(ctx.get())->boundary_models;
        TimingClockingInfo read{}; read.clock_port = id_CLKIN; read.edge = RISING_EDGE;
        read.setup = DelayPair(200); read.hold = DelayPair(100);
        models[{bidir->name, true}].push_back({ctx->id("rise"), read});
        read.edge = FALLING_EDGE; read.setup = DelayPair(300); read.hold = DelayPair(150);
        models[{bidir->name, true}].push_back({ctx->id("fall"), read});
        TimingClockingInfo write{}; write.clock_port = id_CLK; write.edge = RISING_EDGE;
        write.clockToQ = DelayQuad(100, 500);
        models[{bidir->name, false}].push_back({ctx->id("data"), write});
        write.clockToQ = DelayQuad(300, 900);
        models[{bidir->name, false}].push_back({ctx->id("oe"), write});
        clock->clkconstr->high = DelayPair(4000); clock->clkconstr->low = DelayPair(6000);
        sdc("set_input_delay -clock memory -min 1 [get_ports {dq[*]}]\n"
            "set_input_delay -clock memory -max 3 [get_ports {dq[*]}]\n"
            "set_output_delay -clock memory -min -1 [get_ports {dq[*]}]\n"
            "set_output_delay -clock memory -max 4 [get_ports {dq[*]}]\n");
        ctx->assignArchInfo();
    }

    CellPortKey boundary_key(const char *direction, const char *channel, bool local)
    {
        return CellPortKey(bidir->name, ctx->id(std::string("PAD$timing$") + direction + "$" + channel +
                                               (local ? "$register" : "$external")));
    }
};

TEST_F(IoDelayTest, InputBudgetAndExternalDelayReport)
{
    sdc("set_input_delay -clock [get_clocks memory] -min 1 [get_ports din]\n"
        "set_input_delay -clock memory -max 3 [get_ports din]\n");
    TimingAnalyser timing(ctx.get()); timing.setup(false, false, true);
    auto info = ctx->getPortClockingInfo(capture, id_DATAIN, 0);
    EXPECT_EQ(timing.get_setup_slack(CellPortKey(capture->name, id_DATAIN)), 10000 - 3000 - info.setup.maxDelay());
    auto path = find_path(timing.get_timing_result(), capture);
    ASSERT_FALSE(path.segments.empty());
    EXPECT_EQ(path.segments.front().type, CriticalPath::Segment::Type::SOURCE);
    EXPECT_EQ(path.segments.front().delay, 3000);
    EXPECT_EQ(path.max_delay, 10000);
    timing.set_route_delay(CellPortKey(capture->name, id_DATAIN), DelayPair(200, 900));
    timing.run(false);
    EXPECT_EQ(timing.get_setup_slack(CellPortKey(capture->name, id_DATAIN)), 10000 - 3000 - 900 - info.setup.maxDelay());
    EXPECT_GT(timing.get_criticality(CellPortKey(capture->name, id_DATAIN)), 0);
}

TEST_F(IoDelayTest, OutputSetupAndNegativeMinimumHold)
{
    sdc("set_output_delay -clock memory -min -2 [get_ports dout]\n"
        "set_output_delay -clock memory -max 4 [get_ports dout]\n");
    TimingAnalyser timing(ctx.get()); timing.setup(false, false, true);
    auto info = ctx->getPortClockingInfo(launch, id_Q, 0);
    EXPECT_EQ(timing.get_setup_slack(CellPortKey(output->name, id_I)), 10000 - info.clockToQ.maxDelay() - 4000);
    auto path = find_path(timing.get_timing_result(), output);
    ASSERT_FALSE(path.segments.empty());
    EXPECT_EQ(path.segments.back().delay, 4000);
    EXPECT_EQ(path.segments.back().type, CriticalPath::Segment::Type::SETUP);
    const auto &holds = timing.get_timing_result().min_delay_violations;
    ASSERT_EQ(holds.size(), 1);
    EXPECT_EQ(holds[0].segments.back().delay, -2000);
    EXPECT_EQ(holds[0].segments.back().type, CriticalPath::Segment::Type::HOLD);
}

TEST_F(IoDelayTest, FallingReferenceUsesHalfCycleAndMatchesHoldSlack)
{
    sdc("set_input_delay -clock memory -clock_fall -min -6 [get_ports din]\n"
        "set_input_delay -clock memory -clock_fall -max 1 [get_ports din]\n");
    TimingAnalyser timing(ctx.get()); timing.setup(false, false, true);
    auto info = ctx->getPortClockingInfo(capture, id_DATAIN, 0);
    EXPECT_EQ(timing.get_setup_slack(CellPortKey(capture->name, id_DATAIN)), 5000 - 1000 - info.setup.maxDelay());
    auto path = find_path(timing.get_timing_result(), capture);
    EXPECT_EQ(path.clock_pair.start.edge, FALLING_EDGE);
    EXPECT_EQ(path.max_delay, 5000);
    ASSERT_EQ(timing.get_timing_result().min_delay_violations.size(), 1);
}

TEST_F(IoDelayTest, BidirectionalDataAndOeReceiveOutputBudget)
{
    sdc("set_input_delay -clock memory 1 [get_ports {dq[*]}]\n"
        "set_output_delay -clock memory -min -1 [get_ports {dq[*]}]\n"
        "set_output_delay -clock memory -max 2 [get_ports {dq[*]}]\n");
    TimingAnalyser timing(ctx.get()); timing.setup(false, false, true);
    auto info = ctx->getPortClockingInfo(launch, id_Q, 0);
    for (auto pin : {id_I, id_OE})
        EXPECT_EQ(timing.get_setup_slack(CellPortKey(bidir->name, pin)), 10000 - info.clockToQ.maxDelay() - 2000);
}

TEST_F(IoDelayTest, RejectsMalformedAndUnsupportedConstraints)
{
    for (const auto &command : {
             "set_input_delay -clock missing 1 [get_ports din]\n",
             "set_input_delay -clock memory 1 [get_ports missing]\n",
             "set_input_delay -clock memory 1 [get_ports {}]\n",
             "set_input_delay -clock memory 1 [get_ports dout]\n",
             "set_output_delay -clock memory 1 [get_ports din]\n",
             "set_input_delay -clock memory nan [get_ports din]\n",
             "set_input_delay -clock memory -rise 1 [get_ports din]\n",
             "set_input_delay -clock memory -add_delay 1 [get_ports din]\n",
             "set_input_delay -clock\n"}) {
        EXPECT_THROW(sdc(command), log_execution_error_exception) << command;
    }
}

TEST_F(IoDelayTest, RejectsMissingMinimumReversedBoundsAndUnrelatedClock)
{
    sdc("set_input_delay -clock memory -max 1 [get_ports din]\n");
    EXPECT_THROW(TimingAnalyser(ctx.get()).setup(), log_execution_error_exception);
    sdc("set_input_delay -clock memory -min 2 [get_ports din]\n");
    EXPECT_THROW(TimingAnalyser(ctx.get()).setup(), log_execution_error_exception);
    sdc("set_input_delay -clock memory -min 0 [get_ports din]\n");
    auto *other = net("unrelated"); ctx->addClock(other->name, 100);
    capture->disconnectPort(id_CLK); capture->connectPort(id_CLK, other); ctx->assignArchInfo();
    EXPECT_THROW(TimingAnalyser(ctx.get()).setup(), log_execution_error_exception);
}

TEST_F(IoDelayTest, ConstraintsSurviveJsonCheckpoint)
{
    struct Diagnostics {
        std::ostringstream stream;
        Diagnostics() { log_streams.emplace_back(&stream, LogLevel::INFO_MSG); }
        ~Diagnostics() { log_streams.pop_back(); }
    } diagnostics;
    sdc("set_input_delay -clock memory -min 1 [get_ports din]\n"
        "set_input_delay -clock memory -max 3 [get_ports din]\n");
    ctx->settings[ctx->id("synth")] = 1;
    std::ostringstream written; std::string filename = "io-delay.json";
    ASSERT_TRUE(write_json_file(written, filename, ctx.get()));
    std::string error;
    auto json = json11::Json::parse(written.str(), error);
    ASSERT_TRUE(error.empty()) << error;
    const auto &settings = json["modules"]["top"]["settings"];
    auto saved = read_io_delays(ctx.get());
    ASSERT_TRUE(settings["timing/io_delays"].is_string());
    // Reload the graph and settings into a fresh context. No cell, net, or
    // clock pointers from the first analysis may be retained.
    ctx.reset();
    ArchArgs args; args.device = "5CSEBA6U23I7";
    ctx = std::make_unique<Context>(args);
    // Unused fixture nets are not created by JSON import, but native FF
    // annotation requires them during import.
    net("$PACKER_GND_NET"); net("$PACKER_VCC_NET");
    std::istringstream incoming(written.str());
    bool loaded = false;
    ASSERT_NO_THROW(loaded = parse_json(incoming, filename, ctx.get()));
    ASSERT_TRUE(loaded);
    ASSERT_NO_THROW(ctx->assignArchInfo());
    ASSERT_TRUE(ctx->cells.count(ctx->id("capture")));
    capture = ctx->cells.at(ctx->id("capture")).get();
    EXPECT_EQ(json11::Json(read_io_delays(ctx.get())).dump(), json11::Json(saved).dump());
    auto *restored_input = ctx->cells.at(ctx->id("input_pad")).get();
    ASSERT_EQ(restored_input->getPort(id_PAD), ctx->ports.at(ctx->id("din")).net);
    ASSERT_NE(restored_input->getPort(id_O), nullptr);
    ASSERT_GT(ctx->ports.at(ctx->id("din")).net->users.entries(), 0);
    TimingAnalyser timing(ctx.get()); ASSERT_NO_THROW(timing.setup()) << diagnostics.stream.str();
    auto info = ctx->getPortClockingInfo(capture, id_DATAIN, 0);
    EXPECT_EQ(timing.get_setup_slack(CellPortKey(capture->name, id_DATAIN)), 7000 - info.setup.maxDelay());
}

TEST_F(IoDelayTest, PhaseRelatedOutputAndClockSkewUseRealWindows)
{
    auto *shifted = net("shifted");
    shifted->clkconstr = std::make_unique<ClockConstraint>(*clock->clkconstr);
    clock->clkconstr->phase_group = shifted->clkconstr->phase_group = ctx->id("pll");
    shifted->clkconstr->phase_shift = 5000;
    sdc("set_output_delay -clock shifted -min -6 [get_ports dout]\n"
        "set_output_delay -clock shifted -max 2 [get_ports dout]\n");
    TimingAnalyser timing(ctx.get()); timing.with_clock_skew = true; timing.setup(false, false, true);
    auto info = ctx->getPortClockingInfo(launch, id_Q, 0);
    EXPECT_EQ(timing.get_setup_slack(CellPortKey(output->name, id_I)), 5000 - info.clockToQ.maxDelay() - 2000);
    auto path = find_path(timing.get_timing_result(), output);
    EXPECT_EQ(path.max_delay, 5000);
    const auto &holds = timing.get_timing_result().min_delay_violations;
    ASSERT_EQ(holds.size(), 1);
    EXPECT_EQ(holds[0].segments.front().type, CriticalPath::Segment::Type::CLK_TO_CLK);
    EXPECT_EQ(holds[0].segments.front().delay, 5000);
    timing.set_route_delay(CellPortKey(launch->name, id_CLK), DelayPair(500, 700));
    timing.run(false);
    EXPECT_EQ(timing.get_setup_slack(CellPortKey(output->name, id_I)),
              5000 - info.clockToQ.maxDelay() - 2000 - 700);
}

TEST_F(IoDelayTest, RegisteredIoAndVirtualClocksFailExplicitly)
{
    EXPECT_THROW(sdc("create_clock -period 10 -name virtual\n"), log_execution_error_exception);
    sdc("set_output_delay -clock memory 2 [get_ports dout]\n");
    output->type = id_MISTRAL_SDROUT;
    output->addInput(id_CLK); output->connectPort(id_CLK, clock);
    EXPECT_THROW(TimingAnalyser(ctx.get()).setup(), log_execution_error_exception);
}

TEST_F(IoDelayTest, ClockFalsePathCutsExternalSetupAndHold)
{
    sdc("set_output_delay -clock [get_clocks memory] -min -2 [get_ports dout]\n"
        "set_output_delay -clock memory -max 40 [get_ports dout]\n"
        "set_false_path -from [get_clocks memory] -to [get_clocks memory]\n");
    TimingAnalyser timing(ctx.get()); timing.setup(false, false, true);
    const auto &result = timing.get_timing_result();
    EXPECT_TRUE(result.clock_paths.empty());
    EXPECT_TRUE(result.min_delay_violations.empty());
    EXPECT_TRUE(result.clock_setup_slack.empty());
}

TEST_F(IoDelayTest, SetupMulticycleRelaxesExternalSetupWithoutRelaxingHold)
{
    sdc("set_output_delay -clock memory -min -2 [get_ports dout]\n"
        "set_output_delay -clock memory -max 4 [get_ports dout]\n"
        "set_multicycle_path -setup 2 -from [get_clocks memory] -to [get_clocks memory]\n");
    TimingAnalyser timing(ctx.get()); timing.setup(false, false, true);
    auto info = ctx->getPortClockingInfo(launch, id_Q, 0);
    EXPECT_EQ(timing.get_setup_slack(CellPortKey(output->name, id_I)), 20000 - info.clockToQ.maxDelay() - 4000);
    const auto &holds = timing.get_timing_result().min_delay_violations;
    ASSERT_EQ(holds.size(), 1);
    EXPECT_EQ(holds[0].segments.back().delay, -2000);
}

TEST_F(IoDelayTest, RegisteredDdrFabricHandoffUsesRisingEdgeForBothWords)
{
    bidir->type = id_MISTRAL_SDRIO;
    bidir->params[ctx->id("IOREG_IN")] = 1;
    bidir->disconnectPort(id_O);
    bidir->ports.erase(id_O);
    bidir->addInput(id_CLKIN);
    bidir->connectPort(id_CLKIN, clock);
    bidir->addOutput(id_Q_H);
    bidir->addOutput(id_Q_L);
    auto *high_data = input->getPort(id_O);
    input->disconnectPort(id_O);
    bidir->connectPort(id_Q_H, high_data);
    auto *low_capture = ff("low_capture");
    bidir->connectPort(id_Q_L, net("low_data"));
    low_capture->connectPort(id_DATAIN, bidir->getPort(id_Q_L));
    ctx->assignArchInfo();
    TimingAnalyser timing(ctx.get()); timing.setup(false, false, true);
    for (auto *sink : {capture, low_capture}) {
        auto reports = timing.get_report_setup_paths(16);
        for (const auto &entry : timing.get_timing_result().clock_paths) reports.push_back(entry.second);
        auto found = std::find_if(reports.begin(), reports.end(), [&](const CriticalPath &path) {
            return !path.segments.empty() && path.segments.back().to.first == sink->name;
        });
        ASSERT_NE(found, reports.end());
        const auto &path = *found;
        EXPECT_EQ(path.clock_pair.start.clock, clock->name);
        EXPECT_EQ(path.clock_pair.start.edge, RISING_EDGE);
        EXPECT_EQ(path.max_delay, 10000);
        EXPECT_GT(timing.get_criticality(CellPortKey(sink->name, id_DATAIN)), 0);
    }
}

TEST_F(IoDelayTest, RegisteredFabricTimingDoesNotEnableExternalPadTiming)
{
    bidir->type = id_MISTRAL_SDRIO;
    bidir->params[ctx->id("IOREG_OUT")] = 1;
    bidir->params[ctx->id("IOREG_OE")] = 1;
    bidir->addInput(id_CLK);
    bidir->connectPort(id_CLK, clock);
    int count = 0;
    EXPECT_EQ(ctx->getPortTimingClass(bidir, id_I, count), TMG_REGISTER_INPUT);
    EXPECT_EQ(count, 1);
    sdc("set_output_delay -clock memory 1 [get_ports {dq[*]}]\n");
    ctx->assignArchInfo();
    TimingAnalyser timing(ctx.get());
    EXPECT_THROW(timing.setup(false, false, true), log_execution_error_exception);
}

TEST_F(IoDelayTest, UncharacterizedElectricalProfilesKeepRegisterTimingUnsupported)
{
    bidir->type = id_MISTRAL_SDRIO;
    bidir->params[ctx->id("IOREG_IN")] = 1;
    bidir->addOutput(id_Q_H);
    int count = 0;
    EXPECT_EQ(ctx->getPortTimingClass(bidir, id_Q_H, count), TMG_REGISTER_OUTPUT);
    for (const auto &setting : {std::make_pair("D3_DELAY", "1"),
                                std::make_pair("ENABLE_BUS_HOLD_CIRCUITRY", "ON"),
                                std::make_pair("IO_STANDARD", "3.3-V LVCMOS")}) {
        bidir->attrs[ctx->id(setting.first)] = std::string(setting.second);
        EXPECT_EQ(ctx->getPortTimingClass(bidir, id_Q_H, count), TMG_IGNORE);
        EXPECT_EQ(count, 0);
        bidir->attrs.erase(ctx->id(setting.first));
    }
    output->type = id_MISTRAL_SDROUT;
    output->attrs[ctx->id("D5_DELAY")] = std::string("31");
    EXPECT_EQ(ctx->getPortTimingClass(output, id_I, count), TMG_ENDPOINT);
    EXPECT_EQ(count, 0);
}

TEST_F(IoDelayTest, RegisteredOutputFabricPathIsTimedButExternalPadRemainsUnsupported)
{
    output->type = id_MISTRAL_SDROUT;
    output->addInput(id_CLK);
    output->addInput(id_CEOUT);
    output->connectPort(id_CLK, clock);
    output->connectPort(id_CEOUT, launch->getPort(id_Q));
    ctx->assignArchInfo();
    TimingAnalyser timing(ctx.get()); timing.setup(false, false, true);
    auto source = ctx->getPortClockingInfo(launch, id_Q, 0);
    for (auto pin : {id_I, id_CEOUT}) {
        int count = 0;
        EXPECT_EQ(ctx->getPortTimingClass(output, pin, count), TMG_REGISTER_INPUT);
        EXPECT_EQ(count, 1);
        EXPECT_EQ(timing.get_setup_slack(CellPortKey(output->name, pin)),
                  10000 - source.clockToQ.maxDelay() - 120);
        EXPECT_GT(timing.get_criticality(CellPortKey(output->name, pin)), 0);
    }
    auto path = find_path(timing.get_timing_result(), output);
    ASSERT_FALSE(path.segments.empty());
    EXPECT_EQ(path.clock_pair.end.clock, clock->name);
    EXPECT_EQ(path.segments.back().type, CriticalPath::Segment::Type::SETUP);
    EXPECT_EQ(path.segments.back().delay, 120);
    sdc("set_output_delay -clock memory 1 [get_ports dout]\n");
    TimingAnalyser external(ctx.get());
    EXPECT_THROW(external.setup(false, false, true), log_execution_error_exception);
}

TEST_F(IoDelayTest, RegisteredPadReadEdgesAndWriteDataOeAreIndependent)
{
    registered_boundaries();
    TimingAnalyser timing(ctx.get()); timing.setup(true, true, true);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("read", "rise", true)), 6800);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("read", "fall", true)), 700);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("write", "data", false)), 5500);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("write", "oe", false)), 5100);
    auto paths = timing.get_report_setup_paths(16);
    for (const auto &entry : timing.get_timing_result().clock_paths) paths.push_back(entry.second);
    bool rising = false, falling = false;
    int write_setups = 0;
    for (const auto &path : paths) {
        auto end = path.segments.back().to.second;
        if (end == boundary_key("write", "data", false).port || end == boundary_key("write", "oe", false).port) {
            ++write_setups;
            EXPECT_EQ(path.segments.front().type, CriticalPath::Segment::Type::CLK_TO_Q);
            EXPECT_EQ(path.segments.back().delay, 4000);
            EXPECT_EQ(path.segments.size(), 2);
        }
        if (path.segments.back().to != std::make_pair(bidir->name, boundary_key("read", "rise", true).port) &&
            path.segments.back().to != std::make_pair(bidir->name, boundary_key("read", "fall", true).port)) continue;
        EXPECT_EQ(path.segments.front().type, CriticalPath::Segment::Type::SOURCE);
        EXPECT_EQ(path.segments.front().delay, 3000);
        EXPECT_EQ(path.segments.back().type, CriticalPath::Segment::Type::SETUP);
        EXPECT_EQ(path.segments.size(), 2);
        if (path.clock_pair.end.edge == RISING_EDGE) { rising = true; EXPECT_EQ(path.max_delay, 10000); }
        else { falling = true; EXPECT_EQ(path.max_delay, 4000); }
    }
    EXPECT_TRUE(rising); EXPECT_TRUE(falling);
    EXPECT_EQ(write_setups, 2);
    int writes = 0;
    for (const auto &path : timing.get_timing_result().min_delay_violations) {
        auto end = path.segments.back().to.second;
        if (end != boundary_key("write", "data", false).port && end != boundary_key("write", "oe", false).port) continue;
        ++writes;
        auto launch = std::find_if(path.segments.begin(), path.segments.end(), [](const CriticalPath::Segment &seg) {
            return seg.type == CriticalPath::Segment::Type::CLK_TO_Q;
        });
        ASSERT_NE(launch, path.segments.end());
        EXPECT_EQ(launch->delay, end == boundary_key("write", "data", false).port ? 100 : 300);
        EXPECT_EQ(path.segments.back().type, CriticalPath::Segment::Type::HOLD);
        delay_t total = 0; for (const auto &seg : path.segments) total += seg.delay;
        EXPECT_EQ(total, end == boundary_key("write", "data", false).port ? -900 : -700);
    }
    EXPECT_EQ(writes, 2);
}

TEST_F(IoDelayTest, RegisteredPadClockSkewAndMulticycleUseTheSameReportFrame)
{
    registered_boundaries();
    TimingAnalyser timing(ctx.get()); timing.with_clock_skew = true; timing.setup(false, false, true);
    timing.set_route_delay(CellPortKey(bidir->name, id_CLKIN), DelayPair(400, 700));
    timing.set_route_delay(CellPortKey(bidir->name, id_CLK), DelayPair(200, 500));
    timing.run(false, false, false, true);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("read", "rise", true)), 7200);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("read", "fall", true)), 1100);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("write", "data", false)), 5000);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("write", "oe", false)), 4600);
    auto paths = timing.get_report_setup_paths(16);
    for (const auto &entry : timing.get_timing_result().clock_paths) paths.push_back(entry.second);
    for (const auto &path : paths) {
        auto port = path.segments.back().to.second;
        if (path.segments.back().to.first != bidir->name || port.str(ctx.get()).find("$timing$") == std::string::npos) continue;
        delay_t total = 0; for (const auto &seg : path.segments) total += seg.delay;
        EXPECT_EQ(path.max_delay - total, timing.get_setup_slack(CellPortKey(bidir->name, port)));
    }
    auto hold = timing.get_timing_result().clock_hold_slack.at(clock->name);
    sdc("set_multicycle_path -setup 2 -from [get_clocks memory] -to [get_clocks memory]\n");
    timing.setup(false, false, true);
    timing.set_route_delay(CellPortKey(bidir->name, id_CLKIN), DelayPair(400, 700));
    timing.set_route_delay(CellPortKey(bidir->name, id_CLK), DelayPair(200, 500));
    timing.run(false, false, false, true);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("read", "fall", true)), 11100);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("write", "oe", false)), 14600);
    EXPECT_EQ(timing.get_timing_result().clock_hold_slack.at(clock->name), hold);
}

TEST_F(IoDelayTest, RegisteredPadGraphDoesNotChangeCheckpointOrLeakAcrossSetup)
{
    struct Diagnostics {
        std::ostringstream stream;
        Diagnostics() { log_streams.emplace_back(&stream, LogLevel::INFO_MSG); }
        ~Diagnostics() { log_streams.pop_back(); }
    } diagnostics;
    registered_boundaries();
    ctx->settings[ctx->id("synth")] = 1;
    std::ostringstream before, after; std::string filename = "boundary.json";
    ASSERT_TRUE(write_json_file(before, filename, ctx.get()));
    auto users = bidir->getPort(id_PAD)->users.entries();
    auto nets = ctx->nets.size(), cells = ctx->cells.size(), pins = bidir->ports.size();
    TimingAnalyser timing(ctx.get()); timing.setup(true, true, true);
    timing.setup(true, true, true);
    ASSERT_TRUE(write_json_file(after, filename, ctx.get()));
    EXPECT_EQ(before.str(), after.str());
    EXPECT_EQ(ctx->nets.size(), nets); EXPECT_EQ(ctx->cells.size(), cells); EXPECT_EQ(bidir->ports.size(), pins);
    EXPECT_EQ(bidir->getPort(id_PAD)->users.entries(), users);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("read", "fall", true)), 700);
    std::vector<EndpointClockPairTiming> rows;
    EXPECT_FALSE(timing.get_endpoint_clock_pair_timings(boundary_key("read", "fall", true), rows));
    ArchArgs args; args.device = "5CSEBA6U23I7";
    auto fresh = std::make_unique<IoBoundaryTestContext>(args);
    fresh->createNet(fresh->id("$PACKER_GND_NET")); fresh->createNet(fresh->id("$PACKER_VCC_NET"));
    std::istringstream incoming(before.str());
    ASSERT_TRUE(parse_json(incoming, filename, fresh.get()));
    const auto &models = static_cast<IoBoundaryTestContext *>(ctx.get())->boundary_models;
    for (const auto &entry : models) {
        auto key = std::make_pair(fresh->id(entry.first.first.str(ctx.get())), entry.first.second);
        for (auto model : entry.second) {
            model.name = fresh->id(model.name.str(ctx.get()));
            model.clocking.clock_port = fresh->id(model.clocking.clock_port.str(ctx.get()));
            fresh->boundary_models[key].push_back(model);
        }
    }
    fresh->assignArchInfo();
    TimingAnalyser restored(fresh.get()); ASSERT_NO_THROW(restored.setup(true, true, true)) << diagnostics.stream.str();
    EXPECT_EQ(restored.get_setup_slack(CellPortKey(fresh->id(bidir->name.str(ctx.get())),
                  fresh->id("PAD$timing$read$fall$register"))), 700);
    // Reports may name aliases, but must not look them up as routed pins or
    // assume an external input pad net has a physical design driver.
    for (const auto &entry : ctx->cells) {
        auto *cell = entry.second.get();
        for (auto bel : ctx->getBels()) {
            if (ctx->getBoundBelCell(bel) || !ctx->isValidBelForCellType(cell->type, bel)) continue;
            ctx->bindBel(bel, cell, STRENGTH_USER);
            break;
        }
        ASSERT_NE(cell->bel, BelId());
    }
    ctx->timing_result = timing.get_timing_result();
    ctx->timing_result.report_setup_paths = timing.get_report_setup_paths(16);
    ctx->detailed_timing_report = true;
    std::ostringstream report; ASSERT_NO_THROW(ctx->writeJsonReport(report));
    std::string error;
    auto json = json11::Json::parse(report.str(), error);
    ASSERT_TRUE(error.empty()) << error;
    bool found_pad = false;
    for (const auto &row : json["detailed_net_timings"].array_items()) {
        if (row["net"].string_value() != bidir->getPort(id_PAD)->name.str(ctx.get())) continue;
        found_pad = true;
        EXPECT_TRUE(row["driver"].is_null());
        for (const auto &sink : row["endpoints"].array_items()) {
            EXPECT_EQ(sink["source"]["cell"].string_value(), bidir->name.str(ctx.get()));
            EXPECT_NE(sink["source"]["port"].string_value().find("$timing$"), std::string::npos);
        }
    }
    EXPECT_TRUE(found_pad);
}

TEST_F(IoDelayTest, RegisteredPadCutsDoNotBypassModelAndClockValidation)
{
    registered_boundaries();
    sdc("set_false_path -from [get_clocks memory] -to [get_clocks memory]\n");
    TimingAnalyser cut(ctx.get()); cut.setup(false, false, true);
    EXPECT_EQ(cut.get_criticality(boundary_key("read", "fall", true)), 0);
    EXPECT_EQ(cut.get_criticality(boundary_key("write", "oe", false)), 0);
    EXPECT_TRUE(cut.get_timing_result().clock_setup_slack.empty());
    EXPECT_TRUE(cut.get_timing_result().min_delay_violations.empty());
    auto &models = static_cast<IoBoundaryTestContext *>(ctx.get())->boundary_models;
    auto saved = models[{bidir->name, true}];
    models[{bidir->name, true}].front().clocking.clock_port = id_ACLR;
    TimingAnalyser invalid(ctx.get());
    EXPECT_THROW(invalid.setup(), log_execution_error_exception);
    models[{bidir->name, true}] = saved;
    models[{bidir->name, true}].front().clocking.clock_port = id_I;
    TimingAnalyser not_clock(ctx.get());
    EXPECT_THROW(not_clock.setup(), log_execution_error_exception);
    models[{bidir->name, true}] = saved;
    models[{bidir->name, true}].front().clocking.setup = DelayPair(300, 200);
    TimingAnalyser reversed(ctx.get());
    EXPECT_THROW(reversed.setup(), log_execution_error_exception);
    models[{bidir->name, true}] = saved;
    models[{bidir->name, true}].push_back(saved.front());
    TimingAnalyser overlap(ctx.get());
    EXPECT_THROW(overlap.setup(), log_execution_error_exception);
    models[{bidir->name, true}] = saved;
    auto *unrelated = net("unrelated"); ctx->addClock(unrelated->name, 125);
    auto write_saved = models[{bidir->name, false}];
    models[{bidir->name, false}].front().clocking.clockToQ = DelayQuad(100, 500, 300, 200);
    TimingAnalyser invalid_output(ctx.get());
    EXPECT_THROW(invalid_output.setup(), log_execution_error_exception);
    models[{bidir->name, false}] = write_saved;
    bidir->disconnectPort(id_CLKIN); bidir->connectPort(id_CLKIN, unrelated);
    TimingAnalyser mismatched(ctx.get());
    EXPECT_THROW(mismatched.setup(), log_execution_error_exception);
}

TEST_F(IoDelayTest, RegisteredPadUsesSeparatePhaseRelatedCaptureClock)
{
    registered_boundaries();
    auto *shifted = net("shifted_capture");
    shifted->clkconstr = std::make_unique<ClockConstraint>(*clock->clkconstr);
    clock->clkconstr->phase_group = shifted->clkconstr->phase_group = ctx->id("memory_pll");
    shifted->clkconstr->phase_shift = 3000;
    bidir->disconnectPort(id_CLKIN); bidir->connectPort(id_CLKIN, shifted);
    TimingAnalyser timing(ctx.get()); timing.with_clock_skew = true; timing.setup(false, false, true);
    timing.set_route_delay(CellPortKey(bidir->name, id_CLKIN), DelayPair(400, 700));
    timing.run(false, false, false, true);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("read", "rise", true)), 200);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("read", "fall", true)), 4100);
    auto paths = timing.get_report_setup_paths(16);
    for (const auto &entry : timing.get_timing_result().clock_paths) paths.push_back(entry.second);
    for (const auto &path : timing.get_timing_result().xclock_paths) paths.push_back(path);
    bool found = false;
    for (const auto &path : paths) {
        if (path.segments.back().to.second != boundary_key("read", "rise", true).port) continue;
        found = true;
        EXPECT_EQ(path.clock_pair.end.clock, shifted->name);
        EXPECT_EQ(path.max_delay, 3000);
        delay_t total = 0; for (const auto &segment : path.segments) total += segment.delay;
        EXPECT_EQ(path.max_delay - total, 200);
    }
    EXPECT_TRUE(found);
}

TEST_F(IoDelayTest, PrimitiveClockLimitsFollowRoutedWaveformAndReentry)
{
    registered_boundaries();
    auto &models = static_cast<IoBoundaryTestContext *>(ctx.get())->clock_models;
    models[bidir->name] = {{id_CLKIN, 1538, 170, 190}, {id_CLK, 1538, 810, 780}};
    TimingAnalyser timing(ctx.get());
    EXPECT_NO_THROW(timing.setup());
    // A large constant delay moves both edges without narrowing either pulse.
    timing.set_route_delay(CellPortKey(bidir->name, id_CLK), DelayPair(5000));
    EXPECT_NO_THROW(timing.run(false));
    timing.set_route_delay(CellPortKey(bidir->name, id_CLK), DelayPair(0, 3190));
    EXPECT_NO_THROW(timing.run(false)); // exactly the 810 ps high requirement
    timing.set_route_delay(CellPortKey(bidir->name, id_CLK), DelayPair(0, 3191));
    EXPECT_THROW(timing.run(false), log_execution_error_exception);
    timing.set_route_delay(CellPortKey(bidir->name, id_CLK), DelayPair(0));
    EXPECT_NO_THROW(timing.run(false));
    clock->clkconstr->high = DelayPair(8000);
    clock->clkconstr->low = DelayPair(2000);
    timing.set_route_delay(CellPortKey(bidir->name, id_CLKIN), DelayPair(0, 1811));
    EXPECT_THROW(timing.run(false), log_execution_error_exception); // low capture pulse
    EXPECT_NO_THROW(timing.setup()); // no stale route bounds after setup
    models.clear();
    EXPECT_NO_THROW(timing.setup()); // no cached primitive checks either
}

TEST_F(IoDelayTest, PrimitiveClockLimitsCannotBeCutOrRelaxed)
{
    registered_boundaries();
    static_cast<IoBoundaryTestContext *>(ctx.get())->clock_models[bidir->name] = {{id_CLKIN, 1538, 170, 190}};
    sdc("set_false_path -from [get_clocks memory] -to [get_clocks memory]\n");
    TimingAnalyser timing(ctx.get());
    timing.with_clock_skew = false;
    timing.setup_only = true;
    clock->clkconstr->period = DelayPair(1537);
    clock->clkconstr->high = DelayPair(770);
    clock->clkconstr->low = DelayPair(767);
    EXPECT_THROW(timing.setup(), log_execution_error_exception);
    clock->clkconstr->period = DelayPair(1538);
    clock->clkconstr->low = DelayPair(768);
    EXPECT_NO_THROW(timing.setup());
    // setup restores persisted explicit clocks; use a genuinely unconstrained net.
    bidir->disconnectPort(id_CLKIN);
    bidir->connectPort(id_CLKIN, net("unconstrained-clock"));
    EXPECT_THROW(timing.setup(), log_execution_error_exception);
}

TEST_F(IoDelayTest, PrimitiveClockModelsRejectMalformedDeclarations)
{
    registered_boundaries();
    auto &models = static_cast<IoBoundaryTestContext *>(ctx.get())->clock_models;
    models[bidir->name] = {{id_CLKIN, -1, 170, 190}};
    EXPECT_THROW(TimingAnalyser(ctx.get()).setup(), log_execution_error_exception);
    models[bidir->name] = {{id_I, 1538, 170, 190}};
    EXPECT_THROW(TimingAnalyser(ctx.get()).setup(), log_execution_error_exception);
    models[bidir->name] = {{id_CLKIN, 1538, 170, 190}, {id_CLKIN, 1538, 170, 190}};
    EXPECT_THROW(TimingAnalyser(ctx.get()).setup(), log_execution_error_exception);
    models[bidir->name] = {{id_CLKIN, 1538, 170, 190}};
    clock->clkconstr->high = DelayPair(5000, 4000);
    EXPECT_THROW(TimingAnalyser(ctx.get()).setup(), log_execution_error_exception);
}

TEST_F(IoDelayTest, NativeReferencePadModelsTimeCompleteDqAndGuardClock)
{
    registered_boundaries();
    auto *native = static_cast<IoBoundaryTestContext *>(ctx.get());
    native->native_models = true;
    bidir->params[ctx->id("IOREG_IN_DDR")] = 1;
    bidir->attrs[id_LOC] = std::string("PIN_V12");
    bidir->attrs[ctx->id("NEXTPNR_GPIO_TIMING_PROFILE")] = std::string("QUARTUS_17_0_2_RAMTEST");
    bidir->attrs[ctx->id("BOARD_MODEL_FAR_C")] = std::string("30P");
    ctx->bindBel(ctx->get_io_pin_bel(ctx->cyclonev->pin_find_name("V12")), bidir, STRENGTH_LOCKED);
    ASSERT_EQ(ctx->getRegisteredIoTiming(bidir, id_PAD, true).size(), 2u);
    ASSERT_EQ(ctx->getRegisteredIoTiming(bidir, id_PAD, false).size(), 2u);
    TimingAnalyser timing(ctx.get());
    EXPECT_NO_THROW(timing.setup(true, false, true));
    EXPECT_EQ(timing.get_setup_slack(boundary_key("read", "high", true)), 5090);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("read", "low", true)), -900);
    const auto read = ctx->getRegisteredIoTiming(bidir, id_PAD, true);
    EXPECT_EQ(read[0].clocking.hold.maxDelay(), -310);
    EXPECT_EQ(read[1].clocking.hold.maxDelay(), -300);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("write", "data", false)), 720);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("write", "oe", false)), 580);
    EXPECT_EQ(ctx->getPrimitiveClockRequirements(bidir).size(), 2u);
    clock->clkconstr->high = DelayPair(800);
    clock->clkconstr->low = DelayPair(9200);
    sdc("set_false_path -from [get_clocks memory] -to [get_clocks memory]\n");
    EXPECT_THROW(timing.setup(), log_execution_error_exception);
}

TEST_F(IoDelayTest, NativeReferenceProfileRejectsUnqualifiedPinsLoadsAndModes)
{
    registered_boundaries();
    static_cast<IoBoundaryTestContext *>(ctx.get())->native_models = true;
    bidir->params[ctx->id("IOREG_IN_DDR")] = 1;
    bidir->attrs[id_LOC] = std::string("PIN_V12");
    bidir->attrs[ctx->id("NEXTPNR_GPIO_TIMING_PROFILE")] = std::string("QUARTUS_17_0_2_RAMTEST");
    bidir->attrs[ctx->id("BOARD_MODEL_FAR_C")] = std::string("3e-11");
    ctx->bindBel(ctx->get_io_pin_bel(ctx->cyclonev->pin_find_name("V12")), bidir, STRENGTH_LOCKED);
    ASSERT_EQ(ctx->getRegisteredIoTiming(bidir, id_PAD, false).size(), 2u);
    for (const char *value : {"31P", "-1P", "nan", "inf", "30Pjunk", "", "30", "1e300"}) {
        bidir->attrs[ctx->id("BOARD_MODEL_FAR_C")] = std::string(value);
        EXPECT_TRUE(ctx->getRegisteredIoTiming(bidir, id_PAD, false).empty()) << value;
        // External capacitance does not alter the input-capture envelope.
        EXPECT_EQ(ctx->getRegisteredIoTiming(bidir, id_PAD, true).size(), 2u);
    }
    bidir->attrs.erase(ctx->id("BOARD_MODEL_FAR_C"));
    EXPECT_THROW(TimingAnalyser(ctx.get()).setup(), log_execution_error_exception);
    bidir->attrs[ctx->id("BOARD_MODEL_FAR_C")] = std::string("30P");
    bidir->params[ctx->id("IOREG_OE")] = 0;
    EXPECT_THROW(TimingAnalyser(ctx.get()).setup(), log_execution_error_exception);
    bidir->params[ctx->id("IOREG_OE")] = 1;
    bidir->params[ctx->id("IOREG_OUT")] = 0;
    EXPECT_TRUE(ctx->getRegisteredIoTiming(bidir, id_PAD, false).empty());
    bidir->params[ctx->id("IOREG_OUT")] = 1;
    bidir->params[ctx->id("IOREG_IN_DDR")] = 0;
    EXPECT_EQ(ctx->getRegisteredIoTiming(bidir, id_PAD, true).size(), 1u);
    bidir->params[ctx->id("IOREG_IN")] = 0;
    EXPECT_TRUE(ctx->getRegisteredIoTiming(bidir, id_PAD, true).empty());
    bidir->params[ctx->id("IOREG_IN")] = 1;
    bidir->params[ctx->id("IOREG_IN_DDR")] = 1;
    for (const auto &option : std::vector<std::pair<const char *, const char *>>{
            {"CURRENT_STRENGTH_NEW", "8MA"}, {"SLEW_RATE", "0"}, {"WEAK_PULL_UP_RESISTOR", "ON"},
            {"D5_DELAY", "1"}, {"ENABLE_BUS_HOLD_CIRCUITRY", "ON"}, {"IO_STANDARD", "3.3-V LVCMOS"}}) {
        bidir->attrs[ctx->id(option.first)] = std::string(option.second);
        EXPECT_TRUE(ctx->getRegisteredIoTiming(bidir, id_PAD, true).empty()) << option.first;
        EXPECT_TRUE(ctx->getRegisteredIoTiming(bidir, id_PAD, false).empty()) << option.first;
        bidir->attrs.erase(ctx->id(option.first));
    }
    bidir->attrs[id_LOC] = std::string("PIN_E8"); // another tested pin, but not this physical BEL
    EXPECT_TRUE(ctx->getRegisteredIoTiming(bidir, id_PAD, true).empty());
    bidir->attrs[id_LOC] = std::string("PIN_W15"); // not among the qualified DQ pins
    EXPECT_TRUE(ctx->getRegisteredIoTiming(bidir, id_PAD, false).empty());
    bidir->attrs[id_LOC] = std::string("PIN_V12");
    bidir->attrs.erase(ctx->id("NEXTPNR_GPIO_TIMING_PROFILE"));
    EXPECT_THROW(TimingAnalyser(ctx.get()).setup(), log_execution_error_exception);
}

TEST_F(IoDelayTest, NativeReferenceProfileSupportsStandaloneSdrOutputAndDdrInput)
{
    static_cast<IoBoundaryTestContext *>(ctx.get())->native_models = true;
    output->type = id_MISTRAL_SDROUT;
    output->addInput(id_CLK); output->connectPort(id_CLK, clock);
    output->attrs[id_LOC] = std::string("PIN_Y11");
    output->attrs[ctx->id("NEXTPNR_GPIO_TIMING_PROFILE")] = std::string("QUARTUS_17_0_2_RAMTEST");
    output->attrs[ctx->id("BOARD_MODEL_FAR_C")] = std::string("0");
    ctx->bindBel(ctx->get_io_pin_bel(ctx->cyclonev->pin_find_name("Y11")), output, STRENGTH_LOCKED);
    auto write = ctx->getRegisteredIoTiming(output, id_PAD, false);
    ASSERT_EQ(write.size(), 1u);
    EXPECT_EQ(write.front().name, ctx->id("data"));
    EXPECT_EQ(write.front().clocking.clockToQ.maxDelay(), 5280);
    EXPECT_EQ(ctx->getPrimitiveClockRequirements(output).size(), 1u);
    input->type = id_MISTRAL_DDRIN;
    input->addInput(id_CLK); input->connectPort(id_CLK, clock);
    input->attrs[id_LOC] = std::string("PIN_V12");
    input->attrs[ctx->id("NEXTPNR_GPIO_TIMING_PROFILE")] = std::string("QUARTUS_17_0_2_RAMTEST");
    ctx->bindBel(ctx->get_io_pin_bel(ctx->cyclonev->pin_find_name("V12")), input, STRENGTH_LOCKED);
    auto read = ctx->getRegisteredIoTiming(input, id_PAD, true);
    ASSERT_EQ(read.size(), 2u);
    EXPECT_EQ(read.front().clocking.clock_port, id_CLK);
    EXPECT_EQ(ctx->getPrimitiveClockRequirements(input).size(), 1u);
    input->type = id_MISTRAL_SDRIN;
    EXPECT_EQ(ctx->getRegisteredIoTiming(input, id_PAD, true).size(), 1u);
    output->type = id_MISTRAL_DDROUT;
    EXPECT_TRUE(ctx->getRegisteredIoTiming(output, id_PAD, false).empty());
}

TEST_F(IoDelayTest, NativeClockForwardingPreservesPadPolarityAndBothLaunchPhases)
{
    static_cast<IoBoundaryTestContext *>(ctx.get())->native_models = true;
    output->disconnectPort(id_I); output->ports.erase(id_I);
    output->type = id_MISTRAL_DDROUT;
    output->addInput(id_CLK); output->connectPort(id_CLK, clock);
    output->attrs[id_LOC] = std::string("PIN_AD20");
    output->attrs[ctx->id("NEXTPNR_GPIO_TIMING_PROFILE")] = std::string("QUARTUS_17_0_2_RAMTEST");
    output->attrs[ctx->id("BOARD_MODEL_FAR_C")] = std::string("30P");
    ctx->bindBel(ctx->get_io_pin_bel(ctx->cyclonev->pin_find_name("AD20")), output, STRENGTH_LOCKED);
    for (bool normal : {true, false}) {
        output->params[id_DDR_HIGH] = int(normal);
        auto models = ctx->getRegisteredIoTiming(output, id_PAD, false);
        ASSERT_EQ(models.size(), 2u);
        EXPECT_EQ(models[0].name, ctx->id(normal ? "rise" : "fall"));
        EXPECT_EQ(models[0].clocking.edge, RISING_EDGE);
        EXPECT_EQ(models[1].name, ctx->id(normal ? "fall" : "rise"));
        EXPECT_EQ(models[1].clocking.edge, FALLING_EDGE);
        sdc("set_output_delay -clock memory -min 0 [get_ports dout]\n"
            "set_output_delay -clock memory -max 1 [get_ports dout]\n");
        TimingAnalyser timing(ctx.get()); timing.setup(true, false, true);
        for (const auto &model : models) {
            const auto key = CellPortKey(output->name, ctx->id(std::string("PAD$timing$write$") +
                                                             model.name.str(ctx.get()) + "$external"));
            const int window = model.clocking.edge == RISING_EDGE ? 10000 : 5000;
            EXPECT_EQ(timing.get_setup_slack(key), window - model.clocking.clockToQ.maxDelay() - 1000);
        }
        EXPECT_EQ(ctx->getPrimitiveClockRequirements(output).size(), 1u);
    }
    output->addInput(id_D_H); output->connectPort(id_D_H, launch->getPort(id_Q));
    EXPECT_TRUE(ctx->getRegisteredIoTiming(output, id_PAD, false).empty());
    EXPECT_THROW(TimingAnalyser(ctx.get()).setup(), log_execution_error_exception);
    output->disconnectPort(id_D_H); output->ports.erase(id_D_H);
    output->params.erase(id_DDR_HIGH);
    EXPECT_TRUE(ctx->getRegisteredIoTiming(output, id_PAD, false).empty());
}

TEST_F(IoDelayTest, NativeSdrCaptureHasOnePadEdgeAndCompleteDataOeTiming)
{
    registered_boundaries();
    static_cast<IoBoundaryTestContext *>(ctx.get())->native_models = true;
    bidir->attrs[id_LOC] = std::string("PIN_V12");
    bidir->attrs[ctx->id("NEXTPNR_GPIO_TIMING_PROFILE")] = std::string("QUARTUS_17_0_2_RAMTEST");
    bidir->attrs[ctx->id("BOARD_MODEL_FAR_C")] = std::string("30P");
    ctx->bindBel(ctx->get_io_pin_bel(ctx->cyclonev->pin_find_name("V12")), bidir, STRENGTH_LOCKED);
    const auto read = ctx->getRegisteredIoTiming(bidir, id_PAD, true);
    ASSERT_EQ(read.size(), 1u);
    EXPECT_EQ(read.front().clocking.edge, RISING_EDGE);
    EXPECT_EQ(read.front().clocking.clock_port, id_CLKIN);
    EXPECT_EQ(read.front().clocking.hold.maxDelay(), -310);
    EXPECT_EQ(ctx->getRegisteredIoTiming(bidir, id_PAD, false).size(), 2u);
    TimingAnalyser timing(ctx.get()); timing.setup(true, false, true);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("read", "rise", true)), 5090);
    EXPECT_EQ(timing.get_setup_slack(boundary_key("write", "oe", false)), 580);
    auto private_ports = ctx->cells.at(bidir->name)->ports;
    EXPECT_FALSE(private_ports.count(ctx->id("PAD$timing$read$rise$register")));
    // An early external arrival used to pass with the mismatched -2180 ps
    // hold model. The native zero-delay input must flag this violation.
    sdc("set_input_delay -clock memory -min -2 [get_ports {dq[*]}]\n");
    timing.setup(false, false, true);
    EXPECT_EQ(timing.get_timing_result().clock_hold_slack.at(clock->name), -1690);
}
