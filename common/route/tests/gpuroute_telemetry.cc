#include "gtest/gtest.h"
#include "json11.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "gpuroute_telemetry.h"

namespace {

std::filesystem::path temporary_path()
{
    return std::filesystem::temp_directory_path() /
           (gpuroute::Telemetry::make_run_id(17) + "-nextpnr-telemetry.jsonl");
}

TEST(GpuRouteTelemetry, EmitsOrderedFiniteJsonLines)
{
    auto path = temporary_path();
    {
        gpuroute::Telemetry telemetry(path.string(), "run-\"escaped\n");
        telemetry.emit("run_start", "", -1, {gpuroute::Telemetry::Field::string("seed", "17")});
        telemetry.emit("iteration", "negotiation", 2,
                       {gpuroute::Telemetry::Field::integer_value("iteration", 3),
                        gpuroute::Telemetry::Field::number_value("table_wns_ns", -0.25),
                        gpuroute::Telemetry::Field::number_value("missing", NAN, "not_computed")});
        EXPECT_EQ(telemetry.sequence(), 2u);
    }
    std::ifstream input(path);
    std::string first, second, extra;
    ASSERT_TRUE(std::getline(input, first));
    ASSERT_TRUE(std::getline(input, second));
    EXPECT_FALSE(std::getline(input, extra));
    EXPECT_NE(first.find("\"schema_version\":1"), std::string::npos);
    EXPECT_NE(first.find("\"sequence\":0"), std::string::npos);
    EXPECT_NE(first.find("run-\\\"escaped\\n"), std::string::npos);
    EXPECT_NE(second.find("\"sequence\":1"), std::string::npos);
    EXPECT_NE(second.find("\"attempt\":2"), std::string::npos);
    EXPECT_NE(second.find("\"missing\":null"), std::string::npos);
    EXPECT_NE(second.find("\"missing_unavailable_reason\":\"not_computed\""), std::string::npos);
    EXPECT_EQ(second.find("nan"), std::string::npos);
    std::string error;
    EXPECT_TRUE(json11::Json::parse(first, error).is_object()) << error;
    EXPECT_TRUE(json11::Json::parse(second, error).is_object()) << error;
    std::filesystem::remove(path);
}

TEST(GpuRouteTelemetry, RefusesToOverwrite)
{
    auto path = temporary_path();
    {
        std::ofstream existing(path);
        existing << "input";
    }
    EXPECT_THROW(gpuroute::Telemetry(path.string(), "run"), std::runtime_error);
    std::ifstream input(path);
    std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    EXPECT_EQ(contents, "input");
    std::filesystem::remove(path);
}

} // namespace
