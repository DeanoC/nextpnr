#include "gpu/gpuroute_backend.h"

#include <limits>
#include <string>

#include "gtest/gtest.h"

using namespace gpuroute;

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
