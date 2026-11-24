#include <memory>
#include <sstream>
#include "gtest/gtest.h"
#include "analogue_repair.h"
#include "gpurouter.h"
#include "json11.hpp"
#include "log.h"

USING_NEXTPNR_NAMESPACE

TEST(GpuRepairCancellation, RouterReturnsFailureAndReleasesContext)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    auto ctx = std::make_unique<Context>(args);
    ctx->settings[ctx->id("target_freq")] = std::string("100000000");
    ctx->settings[ctx->id("timing_driven")] = Property(0);
    GpuRouterCfg cfg(ctx.get());
    cfg.cpu_backend = true;
    // Request cancellation only after routing has acquired the Context lock;
    // this does not depend on the number of setup/batch checkpoints.
#ifndef NPNR_DISABLE_THREADS
    cfg.stop_requested = [&] { return ctx->mutex_owner == boost::this_thread::get_id(); };
#else
    cfg.stop_requested = [] { return true; };
#endif
    EXPECT_FALSE(gpurouter(ctx.get(), cfg));
    // Cancellation must unwind the router without holding the Context lock.
    ctx->lock();
    ctx->unlock();
    cfg.stop_requested = [] { return true; };
    EXPECT_THROW(GpuCandidateRouter(ctx.get(), cfg), GpuRouterCancelled);
}

TEST(AnalogueRepairBudget, RejectsInvalidBudgetEvenWithoutRouting)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    auto ctx = std::make_unique<Context>(args);
    for (const char *value : {"-1", "nan", "inf"}) {
        ctx->settings[ctx->id("gpurouter/analogueTimeBudget")] = std::string(value);
        EXPECT_THROW(ctx->analogue_repair(), log_execution_error_exception);
    }
    ctx->settings[ctx->id("gpurouter/analogueTimeBudget")] = std::string("0");
    ctx->settings[ctx->id("gpurouter/analogueMaxRounds")] = std::string("-1");
    {
        // analogueMaxRounds caps repair actions (candidate passes, re-routes
        // and improvement passes); 0 disables that cap rather than selecting
        // the internal round bound.
        struct Errors
        {
            std::ostringstream stream;
            Errors() { log_streams.emplace_back(&stream, LogLevel::ERROR_MSG); }
            ~Errors() { log_streams.pop_back(); }
        } errors;
        EXPECT_THROW(ctx->analogue_repair(), log_execution_error_exception);
        const std::string message = errors.stream.str();
        EXPECT_NE(message.find("analogueMaxRounds must be nonnegative"), std::string::npos) << message;
        EXPECT_NE(message.find("candidate passes, re-routes and improvement passes"), std::string::npos) << message;
        EXPECT_NE(message.find("0 disables the cap"), std::string::npos) << message;
    }
    ctx->settings[ctx->id("gpurouter/analogueMaxRounds")] = std::string("4");
    ctx->settings[ctx->id("gpurouter/analogueRounds")] = std::string("0");
    EXPECT_TRUE(ctx->analogue_repair());
    EXPECT_EQ(ctx->analogue_repair_stop_reason, "disabled");
    EXPECT_FALSE(ctx->analogue_repair_time_budget_truncated);

    std::ostringstream report;
    ctx->writeJsonReport(report);
    std::string error;
    auto parsed = json11::Json::parse(report.str(), error);
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(parsed["analogue_repair"]["stop_reason"].string_value(), "disabled");
    EXPECT_FALSE(parsed["analogue_repair"]["time_budget_truncated"].bool_value());
}

TEST(AnalogueRepair, RequiredGpuFailureEscapesRecoverableRouteHandling)
{
    EXPECT_TRUE(try_analogue_repair_reroute(1, [] { return true; }));
    EXPECT_FALSE(try_analogue_repair_reroute(1, []() -> bool { throw log_execution_error_exception{}; }));
    EXPECT_THROW(try_analogue_repair_reroute(1, []() -> bool { throw GpuRequiredFailure{}; }), GpuRequiredFailure);
}

TEST(AnalogueRepairBudget, RejectsOutOfRangePolishBeforeRouting)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    auto ctx = std::make_unique<Context>(args);
    ctx->settings[ctx->id("gpurouter/analogueRounds")] = std::string("0");
    for (const char *value : {"-1", "33"}) {
        ctx->settings[ctx->id("gpurouter/analoguePolishCandidates")] = std::string(value);
        EXPECT_THROW(ctx->analogue_repair(), log_execution_error_exception);
    }
    ctx->settings[ctx->id("gpurouter/analoguePolishCandidates")] = std::string("0");
    EXPECT_TRUE(ctx->analogue_repair());
}

TEST(AnalogueRepairBudget, RejectsOutOfRangeLutInputTrialsBeforeRouting)
{
    ArchArgs args;
    args.device = "5CSEBA6U23I7";
    auto ctx = std::make_unique<Context>(args);
    ctx->settings[ctx->id("gpurouter/analogueRounds")] = std::string("0");
    for (const char *value : {"-1", "17"}) {
        ctx->settings[ctx->id("gpurouter/analogueLutPinTrials")] = std::string(value);
        EXPECT_THROW(ctx->analogue_repair(), log_execution_error_exception);
    }
    ctx->settings[ctx->id("gpurouter/analogueLutPinTrials")] = std::string("0");
    EXPECT_TRUE(ctx->analogue_repair());
}
