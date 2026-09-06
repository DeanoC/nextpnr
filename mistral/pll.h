#ifndef MISTRAL_PLL_H
#define MISTRAL_PLL_H

#include <optional>
#include <regex>
#include <string>

// Fixed 50 MHz reference, single integer output, direct mode. These two
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

inline std::optional<Config> select(int mhz)
{
    if (mhz < 1 || mhz > 100)
        return std::nullopt;
    // Prefer the established 300 MHz tuple, preserving the 25 MHz bitstream.
    for (Config config : {Config{12, 2, 0, 7, 1, 1, 0}, Config{32, 5, 0, 6, 2, 4, 2}}) {
        int numerator = 50 * config.m;
        int denominator = config.n * mhz;
        if (numerator % denominator != 0)
            continue;
        config.c = numerator / denominator;
        if (config.c >= 2 && config.c <= 512)
            return config;
    }
    return std::nullopt;
}
} // namespace mistral_pll
#endif
