#ifndef MISTRAL_PLL_H
#define MISTRAL_PLL_H

#include <array>
#include <optional>
#include <regex>
#include <string>

// Checked 25/50/100 MHz references, integer outputs, direct mode. These
// feedback/analog tuples were checked against Quartus 17.0.2; do not derive
// additional analog settings from the frequency equation alone.
namespace mistral_pll {
struct Config
{
    int m, n, c;
    int bandwidth, charge_pump, m_low_preset, m_phase_preset;
};

inline int parse_mhz(const std::string &text)
{
    static const std::regex pattern("^([0-9]{1,3})(\\.0+)? MHz$");
    std::smatch match;
    if (text.size() > 32 || !std::regex_match(text, match, pattern))
        return 0;
    int mhz = std::stoi(match[1].str());
    return mhz >= 1 && mhz <= 100 ? mhz : 0;
}

inline std::array<Config, 3> checked_configs(int reference_mhz)
{
    // Order: reported 300, 320, 400 MHz. Each complete tuple is oracle-checked.
    if (reference_mhz == 25)
        return {{{24, 2, 0, 6, 1, 1, 0}, {64, 5, 0, 3, 2, 7, 3}, {32, 2, 0, 6, 1, 1, 0}}};
    if (reference_mhz == 100)
        return {{{6, 2, 0, 8, 1, 1, 0}, {32, 10, 0, 6, 1, 1, 0}, {8, 2, 0, 7, 1, 1, 0}}};
    return {{{12, 2, 0, 7, 1, 1, 0}, {32, 5, 0, 6, 2, 4, 2}, {16, 2, 0, 7, 1, 1, 0}}};
}

inline bool valid_reference(int mhz) { return mhz == 25 || mhz == 50 || mhz == 100; }

inline std::optional<Config> select(int mhz, int reference_mhz = 50)
{
    if (!valid_reference(reference_mhz) || mhz < 1 || mhz > 100)
        return std::nullopt;
    // Prefer the established 300 MHz tuple, preserving the 25 MHz bitstream.
    auto configs = checked_configs(reference_mhz);
    for (int i = 0; i < 2; ++i) {
        Config config = configs[i];
        int numerator = reference_mhz * config.m;
        int denominator = config.n * mhz;
        if (numerator % denominator != 0)
            continue;
        config.c = numerator / denominator;
        if (config.c >= 2 && config.c <= 512)
            return config;
    }
    return std::nullopt;
}
struct DualConfig
{
    Config feedback;
    int c1;
};

inline std::optional<DualConfig> select_dual(int mhz0, int mhz1, int reference_mhz = 50)
{
    if (!valid_reference(reference_mhz) || mhz0 < 1 || mhz0 > 100 || mhz1 < 1 || mhz1 > 100)
        return std::nullopt;
    // Both counters must share one checked feedback/analog configuration.
    for (Config config : checked_configs(reference_mhz)) {
        int numerator = reference_mhz * config.m;
        int denominator0 = config.n * mhz0, denominator1 = config.n * mhz1;
        if (numerator % denominator0 || numerator % denominator1)
            continue;
        config.c = numerator / denominator0;
        int c1 = numerator / denominator1;
        if (config.c >= 2 && config.c <= 512 && c1 >= 2 && c1 <= 512)
            return DualConfig{config, c1};
    }
    return std::nullopt;
}
} // namespace mistral_pll
#endif
