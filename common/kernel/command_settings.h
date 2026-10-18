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

#ifndef COMMAND_SETTINGS_H
#define COMMAND_SETTINGS_H

#include <boost/lexical_cast.hpp>

#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "nextpnr_namespaces.h"

NEXTPNR_NAMESPACE_BEGIN

struct CommandSettingOverride
{
    std::string key;
    std::string value;
};

inline CommandSettingOverride parse_command_setting_override(const std::string &argument)
{
    const auto equals = argument.find('=');
    if (equals == std::string::npos)
        throw std::invalid_argument("--set expects key=value, got '" + argument + "'");
    if (equals == 0)
        throw std::invalid_argument("--set key must not be empty, got '" + argument + "'");
    return {argument.substr(0, equals), argument.substr(equals + 1)};
}

inline std::vector<CommandSettingOverride> parse_command_setting_overrides(const std::vector<std::string> &arguments)
{
    std::vector<CommandSettingOverride> overrides;
    overrides.reserve(arguments.size());
    for (const auto &argument : arguments)
        overrides.push_back(parse_command_setting_override(argument));
    return overrides;
}

inline uint64_t parse_command_setting_seed(const std::string &value)
{
    if (value.empty())
        throw std::invalid_argument("--set seed expects an unsigned integer, got '" + value + "'");
    const size_t first_digit = value[0] == '+' ? 1 : 0;
    if (first_digit == value.size())
        throw std::invalid_argument("--set seed expects an unsigned integer, got '" + value + "'");
    for (size_t i = first_digit; i < value.size(); i++) {
        char digit = value[i];
        if (digit < '0' || digit > '9')
            throw std::invalid_argument("--set seed expects an unsigned integer, got '" + value + "'");
    }
    try {
        return boost::lexical_cast<uint64_t>(value.substr(first_digit));
    } catch (const boost::bad_lexical_cast &) {
        throw std::invalid_argument("--set seed expects an unsigned integer, got '" + value + "'");
    }
}

inline void validate_command_setting_override_conflicts(const std::vector<CommandSettingOverride> &overrides,
                                                        const std::set<std::string> &dedicated_setting_keys)
{
    for (const auto &override : overrides) {
        if (dedicated_setting_keys.count(override.key))
            throw std::invalid_argument("setting '" + override.key +
                                        "' is set by both --set and a dedicated command-line option");
    }
}

template <typename Settings, typename IdFunction>
inline void apply_command_setting_overrides(Settings &settings, IdFunction id,
                                            const std::vector<CommandSettingOverride> &overrides)
{
    for (const auto &override : overrides)
        settings[id(override.key)] = override.value;
}

template <typename Settings, typename IdFunction>
inline void apply_command_setting_frontend_overrides(Settings &settings, IdFunction id,
                                                     const std::vector<CommandSettingOverride> &overrides)
{
    for (const auto &override : overrides) {
        if (override.key == "frontend/top")
            settings[id(override.key)] = override.value;
    }
}

NEXTPNR_NAMESPACE_END

#endif // COMMAND_SETTINGS_H
