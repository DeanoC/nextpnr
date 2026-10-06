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
    ctx->settings[ctx->id("gpurouter/analogueRounds")] = std::string("0");
    EXPECT_TRUE(ctx->analogue_repair());
}
