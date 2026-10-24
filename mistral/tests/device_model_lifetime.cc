// A Context owns the CycloneV from CycloneV::get_model. On 5CSEBA6 that model
// holds the decompressed device database, about 320 MiB. Dropping the pointer
// made nextpnr-mistral-test retain one copy per test (#288).

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

#include "gtest/gtest.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

namespace {

long resident_kb()
{
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        long value = 0;
        if (std::sscanf(line.c_str(), "VmRSS: %ld", &value) == 1)
            return value;
    }
    return -1;
}

} // namespace

TEST(DeviceModelLifetime, RepeatedConstructionReleasesTheModel)
{
    if (resident_kb() < 0)
        GTEST_SKIP() << "VmRSS is not available";

    auto make = []() {
        ArchArgs args;
        args.device = "5CSEBA6U23I7";
        return std::make_unique<Context>(args);
    };

    make().reset();
    const long baseline = resident_kb();
    make().reset();
    make().reset();
    const long after = resident_kb();
    std::printf("device model lifetime: baseline %ld kB, after two more constructions %ld kB, growth %ld kB\n",
                baseline, after, after - baseline);
    // Two further constructions. One retained 5CSEBA6 model is about 320 MiB,
    // so growth below half of that is allocator noise, not another model.
    EXPECT_LT(after - baseline, 160L * 1024) << "baseline " << baseline << " kB, after " << after << " kB";
}
