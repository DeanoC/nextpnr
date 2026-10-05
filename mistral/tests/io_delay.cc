#include <memory>
#include <sstream>
#include "gtest/gtest.h"
#include "io_delay.h"
#include "jsonwrite.h"
#include "json_frontend.h"
#include "timing.h"

USING_NEXTPNR_NAMESPACE

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
        ctx = std::make_unique<Context>(args);
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
