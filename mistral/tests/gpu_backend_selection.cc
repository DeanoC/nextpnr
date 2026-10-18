#include "gpu/gpuroute_backend.h"

#include <limits>
#include <string>

#include "gtest/gtest.h"

using namespace gpuroute;

namespace {

class InitFailingBackend : public Backend
{
  public:
    bool init(const GraphData &, int, int, int, int, std::string &error) override
    {
        error = "deliberate initialisation failure";
        return false;
    }
    void update_wire_state(size_t, const int32_t *, const int32_t *, const float *, const int32_t *) override {}
    void route(const RouteParams &, const std::vector<TaskDesc> &, const std::vector<ArcDesc> &,
               const std::vector<int32_t> &, const std::vector<float> &, const std::vector<float> &,
               const std::vector<int32_t> &, bool, std::vector<ArcResult> &, std::vector<PathEntry> &) override
    {
    }
    const char *name() const override { return "init-failing-test-backend"; }
    const BackendStats &stats() const override { return stats_; }

  private:
    BackendStats stats_;
};

class InitSucceedingBackend : public InitFailingBackend
{
  public:
    bool init(const GraphData &, int, int, int, int, std::string &error) override
    {
        error.clear();
        return true;
    }
    const char *name() const override { return "cpu-reference-test"; }
};

} // namespace

TEST(GpuBackendSelection, RequiredDeviceFailureDoesNotFallBackToCpu)
{
    std::string error;
    auto selection = select_routing_backend(std::numeric_limits<int>::max(), false, true, error);
    EXPECT_FALSE(selection.backend);
    EXPECT_FALSE(error.empty());
}

TEST(GpuBackendSelection, OptionalDeviceFailureKeepsCpuFallback)
{
    std::string error;
    auto selection = select_routing_backend(std::numeric_limits<int>::max(), false, false, error);
    ASSERT_TRUE(selection.backend);
    EXPECT_TRUE(selection.cpu_fallback);
    EXPECT_STREQ(selection.backend->name(), "cpu-reference");
}

TEST(GpuBackendSelection, RequiredGpuRejectsExplicitCpuBackend)
{
    std::string error;
    auto selection = select_routing_backend(-1, true, true, error);
    EXPECT_FALSE(selection.backend);
    EXPECT_NE(error.find("CPU reference backend"), std::string::npos);
}

TEST(GpuBackendSelection, RequiredGpuDoesNotCreateCpuLane)
{
    EXPECT_FALSE(should_create_cpu_lane(4, true, false));
    EXPECT_FALSE(should_create_cpu_lane(4, true, true));
    EXPECT_TRUE(should_create_cpu_lane(4, false, false));
    EXPECT_FALSE(should_create_cpu_lane(0, false, false));
    EXPECT_FALSE(should_create_cpu_lane(4, false, true));
}

TEST(GpuBackendSelection, RequiredDeviceInitializationFailureIsDistinct)
{
    InitFailingBackend backend;
    std::string error;
    EXPECT_EQ(initialize_backend(backend, GraphData{}, 1, 1, 1, 1, true, error),
              BackendInitStatus::required_failure);
    EXPECT_EQ(error, "deliberate initialisation failure");
}

TEST(GpuBackendSelection, OptionalDeviceInitializationFailureFallsBackToCpu)
{
    std::unique_ptr<Backend> backend = std::make_unique<InitFailingBackend>();
    std::string error;
    bool cpu_factory_called = false;
    auto result = initialize_backend_with_cpu_fallback(
            backend, GraphData{}, 1, 1, 1, 1, false, true,
            [&]() -> std::unique_ptr<Backend> {
                cpu_factory_called = true;
                return std::make_unique<InitSucceedingBackend>();
            },
            error);
    EXPECT_EQ(result.status, BackendInitStatus::ready);
    EXPECT_TRUE(result.cpu_fallback);
    EXPECT_TRUE(cpu_factory_called);
    ASSERT_TRUE(backend);
    EXPECT_STREQ(backend->name(), "cpu-reference-test");
    EXPECT_EQ(error, "deliberate initialisation failure");
}

TEST(GpuBackendSelection, RequiredDeviceInitializationFailureDoesNotFallBackToCpu)
{
    std::unique_ptr<Backend> backend = std::make_unique<InitFailingBackend>();
    std::string error;
    bool cpu_factory_called = false;
    auto result = initialize_backend_with_cpu_fallback(
            backend, GraphData{}, 1, 1, 1, 1, true, true,
            [&]() -> std::unique_ptr<Backend> {
                cpu_factory_called = true;
                return std::make_unique<InitSucceedingBackend>();
            },
            error);
    EXPECT_EQ(result.status, BackendInitStatus::required_failure);
    EXPECT_FALSE(result.cpu_fallback);
    EXPECT_FALSE(cpu_factory_called);
    ASSERT_TRUE(backend);
    EXPECT_STREQ(backend->name(), "init-failing-test-backend");
}
