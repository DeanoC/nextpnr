/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  Deano Calver
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "command_settings.h"

#include <gtest/gtest.h>

#include <map>

USING_NEXTPNR_NAMESPACE

TEST(CommandSettingOverrides, ParsesKeyValueAndKeepsEqualsInValue)
{
    const auto parsed = parse_command_setting_overrides({"gpurouter/maxBatches=4", "custom/value=a=b"});
    ASSERT_EQ(parsed.size(), 2u);
    EXPECT_EQ(parsed[0].key, "gpurouter/maxBatches");
    EXPECT_EQ(parsed[0].value, "4");
    EXPECT_EQ(parsed[1].key, "custom/value");
    EXPECT_EQ(parsed[1].value, "a=b");
}

TEST(CommandSettingOverrides, RejectsMalformedArguments)
{
    try {
        parse_command_setting_override("missing-equals");
        FAIL() << "expected missing '=' to be rejected";
    } catch (const std::invalid_argument &error) {
        EXPECT_NE(std::string(error.what()).find("--set expects key=value"), std::string::npos);
    }

    try {
        parse_command_setting_override("=empty-key");
        FAIL() << "expected an empty key to be rejected";
    } catch (const std::invalid_argument &error) {
        EXPECT_NE(std::string(error.what()).find("--set key must not be empty"), std::string::npos);
    }
}

TEST(CommandSettingOverrides, ParsesSeedAsUnsignedInteger)
{
    EXPECT_EQ(parse_command_setting_seed("123"), 123u);
    EXPECT_EQ(parse_command_setting_seed("+123"), 123u);
    EXPECT_THROW(parse_command_setting_seed("not-a-number"), std::invalid_argument);
    EXPECT_THROW(parse_command_setting_seed("-1"), std::invalid_argument);
}

TEST(CommandSettingOverrides, AppliesValuesToSettings)
{
    std::map<std::string, std::string> settings;
    const auto overrides = parse_command_setting_overrides({"gpurouter/maxBatches=4", "custom/value=first",
                                                              "custom/value=last"});
    apply_command_setting_overrides(settings, [](const std::string &key) { return key; }, overrides);
    EXPECT_EQ(settings.at("gpurouter/maxBatches"), "4");
    EXPECT_EQ(settings.at("custom/value"), "last");
}

TEST(CommandSettingOverrides, OnlyAppliesFrontendSelectionBeforeImport)
{
    std::map<std::string, std::string> settings;
    const auto overrides = parse_command_setting_overrides(
            {"frontend/top=child", "synth=0", "gpurouter/perfProfile=1"});
    apply_command_setting_frontend_overrides(settings, [](const std::string &key) { return key; }, overrides);
    EXPECT_EQ(settings.at("frontend/top"), "child");
    EXPECT_EQ(settings.count("synth"), 0u);
    EXPECT_EQ(settings.count("gpurouter/perfProfile"), 0u);
}

TEST(CommandSettingOverrides, RejectsDedicatedOptionConflicts)
{
    const auto overrides = parse_command_setting_overrides({"gpurouter/device=0", "custom/value=1"});
    const std::set<std::string> dedicated_keys = {"gpurouter/device"};
    EXPECT_THROW(validate_command_setting_override_conflicts(overrides, dedicated_keys), std::invalid_argument);
    EXPECT_NO_THROW(validate_command_setting_override_conflicts({overrides[1]}, dedicated_keys));
}
