#ifndef MISTRAL_PLL_H
#define MISTRAL_PLL_H

#include <array>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>

// Checked 25/50/100 MHz references, exact decimal outputs, integer dividers, direct mode. These
// feedback/analog tuples were checked against Quartus 17.0.2; do not derive
// additional analog settings from the frequency equation alone.
namespace mistral_pll {
struct Config
{
    int m, n, c;
    int bandwidth, charge_pump, m_low_preset, m_phase_preset;
    bool fractional = false;
    uint32_t fraction = 1;
};

struct PhaseConfig
{
    int shift_ps, c_preset, c_phase_preset;
};

// Quartus-checked static phase presets at the checked 300 MHz configuration.
// 25/100 MHz outputs use quarter cycles; 50 MHz also supports eighth cycles.
// Keep shifts in exact picoseconds.
// Zero phase leaves defaults untouched for all other frequency profiles.
inline std::optional<PhaseConfig> select_phase(const std::string &text, int64_t output_hz)
{
    if (text == "0 ps")
        return PhaseConfig{0, 1, 0};
    if (output_hz == 25000000) {
        if (text == "10000 ps") return PhaseConfig{10000, 4, 0};
        if (text == "20000 ps") return PhaseConfig{20000, 7, 0};
        if (text == "30000 ps") return PhaseConfig{30000, 10, 0};
    } else if (output_hz == 50000000) {
        if (text == "2500 ps") return PhaseConfig{2500, 1, 6};
        if (text == "5000 ps") return PhaseConfig{5000, 2, 4};
        if (text == "7500 ps") return PhaseConfig{7500, 3, 2};
        if (text == "10000 ps") return PhaseConfig{10000, 4, 0};
        if (text == "12500 ps") return PhaseConfig{12500, 4, 6};
        if (text == "15000 ps") return PhaseConfig{15000, 5, 4};
        if (text == "17500 ps") return PhaseConfig{17500, 6, 2};
    } else if (output_hz == 100000000) {
        if (text == "2500 ps") return PhaseConfig{2500, 1, 6};
        if (text == "5000 ps") return PhaseConfig{5000, 2, 4};
        if (text == "7500 ps") return PhaseConfig{7500, 3, 2};
    }
    return std::nullopt;
}

inline int parse_mhz(const std::string &text)
{
    static const std::regex pattern("^([0-9]{1,3})(\\.0+)? MHz$");
    std::smatch match;
    if (text.size() > 32 || !std::regex_match(text, match, pattern))
        return 0;
    int mhz = std::stoi(match[1].str());
    return mhz >= 1 && mhz <= 100 ? mhz : 0;
}

// Decimal MHz is exact to one Hz. Trailing zeroes do not add precision.
inline int64_t parse_output_hz(const std::string &text)
{
    static const std::regex pattern("^([0-9]{1,3})(\\.([0-9]+))? MHz$");
    std::smatch match;
    if (text.size() > 32 || !std::regex_match(text, match, pattern))
        return 0;
    std::string fraction = match[3].str();
    while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
    if (fraction.size() > 6) return 0;
    while (fraction.size() < 6) fraction += '0';
    int64_t hz = int64_t(std::stoi(match[1].str())) * 1000000 + std::stoi(fraction);
    return hz >= 1000000 && hz <= 100000000 ? hz : 0;
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

struct DutyCounts { int high, low; bool odd; };
inline std::optional<DutyCounts> duty_counts(int c, int duty)
{
    if (c < 2 || c > 512 || duty <= 0 || duty >= 100) return std::nullopt;
    int high = (c + 1) / 2, low = c / 2;
    bool odd = (c & 1) != 0;
    if (duty != 50) {
        if ((c * duty) % 100) return std::nullopt;
        high = c * duty / 100;
        low = c - high;
        odd = false;
    }
    if (high < 1 || low < 1 || high > 255 || low > 255) return std::nullopt;
    return DutyCounts{high, low, odd};
}

inline std::optional<Config> select_hz(int64_t hz, int reference_mhz = 50, int duty = 50)
{
    if (!valid_reference(reference_mhz) || hz < 1000000 || hz > 100000000)
        return std::nullopt;
    // Prefer the established 300 MHz tuple, preserving the 25 MHz bitstream.
    auto configs = checked_configs(reference_mhz);
    for (int i = 0; i < 2; ++i) {
        Config config = configs[i];
        int64_t numerator = int64_t(reference_mhz) * 1000000 * config.m;
        int64_t denominator = config.n * hz;
        if (numerator % denominator != 0)
            continue;
        config.c = numerator / denominator;
        if (duty_counts(config.c, duty))
            return config;
    }
    return std::nullopt;
}
struct DualConfig
{
    Config feedback;
    int c1;
};

inline std::optional<DualConfig> select_dual_hz(int64_t hz0, int64_t hz1, int reference_mhz = 50, int duty0 = 50, int duty1 = 50)
{
    if (!valid_reference(reference_mhz) || hz0 < 1000000 || hz0 > 100000000 || hz1 < 1000000 || hz1 > 100000000)
        return std::nullopt;
    // Both counters must share one checked feedback/analog configuration.
    for (Config config : checked_configs(reference_mhz)) {
        int64_t numerator = int64_t(reference_mhz) * 1000000 * config.m;
        int64_t denominator0 = config.n * hz0, denominator1 = config.n * hz1;
        if (numerator % denominator0 || numerator % denominator1)
            continue;
        config.c = numerator / denominator0;
        int c1 = numerator / denominator1;
        if (duty_counts(config.c, duty0) && duty_counts(c1, duty1))
            return DualConfig{config, c1};
    }
    return std::nullopt;
}
struct MultiConfig
{
    Config feedback;
    std::array<int, 4> counters;
};

inline std::optional<MultiConfig> select_multi_hz(const std::array<int64_t, 4> &hz, int count, int reference_mhz,
                                                   const std::array<int, 4> &duties = {50, 50, 50, 50})
{
    // Every frequency and duty must fit one checked reference-specific configuration.
    if (!valid_reference(reference_mhz) || count < 3 || count > 4)
        return std::nullopt;
    for (int i = 0; i < count; ++i)
        if (hz[i] < 1000000 || hz[i] > 100000000)
            return std::nullopt;
    for (Config config : checked_configs(reference_mhz)) {
        std::array<int, 4> counters{};
        int64_t numerator = int64_t(reference_mhz) * 1000000 * config.m;
        bool valid = true;
        for (int i = 0; i < count; ++i) {
            int64_t denominator = config.n * hz[i];
            if (numerator % denominator) {
                valid = false;
                break;
            }
            counters[i] = numerator / denominator;
            if (!duty_counts(counters[i], duties[i])) {
                valid = false;
                break;
            }
        }
        if (valid) {
            config.c = counters[0];
            return MultiConfig{config, counters};
        }
    }
    return std::nullopt;
}

// Keep whole-MHz callers on the same exact selector.
inline std::optional<Config> select(int mhz, int reference_mhz = 50)
{
    return select_hz(int64_t(mhz) * 1000000, reference_mhz);
}
inline std::optional<DualConfig> select_dual(int mhz0, int mhz1, int reference_mhz = 50)
{
    return select_dual_hz(int64_t(mhz0) * 1000000, int64_t(mhz1) * 1000000, reference_mhz);
}
inline std::optional<Config> select_fractional(int64_t hz, int reference_mhz)
{
    if (reference_mhz != 50) return std::nullopt;
    if (hz == 74250000) return Config{8, 1, 6, 7, 2, 1, 0, true, 0xe8f5c239};
    if (hz == 12288000) return Config{8, 1, 33, 7, 2, 1, 0, true, 472790000};
    if (hz == 11289600) return Config{8, 1, 36, 7, 2, 1, 0, true, 0x20e6293f};
    return std::nullopt;
}
inline std::optional<DualConfig> select_fractional_dual(int64_t hz0, int64_t hz1, int reference_mhz)
{
    if (reference_mhz != 50 || hz0 != 12288000 || hz1 != 24576000) return std::nullopt;
    return DualConfig{Config{8, 1, 34, 7, 2, 1, 0, true, 0x5b18548b}, 17};
}
inline double achieved_hz(const Config &config, int reference_mhz)
{
    double multiplier = config.m + (config.fractional ? config.fraction / 4294967296.0 : 0.0);
    return reference_mhz * 1.0e6 * multiplier / (config.n * config.c);
}
} // namespace mistral_pll
#endif
