#include <memory>
#include "gtest/gtest.h"
#include "gpurouter.h"
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
    int checkpoints = 0;
    // Setup completes before cancellation inside the locked negotiation loop.
    cfg.stop_requested = [&] { return ++checkpoints >= 6; };
    EXPECT_FALSE(gpurouter(ctx.get(), cfg));
    EXPECT_GE(checkpoints, 6);
    // Cancellation must unwind the router without holding the Context lock.
    ctx->lock();
    ctx->unlock();
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
    ctx->settings[ctx->id("gpurouter/analogueRounds")] = std::string("0");
    EXPECT_TRUE(ctx->analogue_repair());
}
