#ifndef MISTRAL_PLL_H
#define MISTRAL_PLL_H

#include <array>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>

// Checked 25/50/100 MHz references, exact decimal outputs, integer dividers, direct mode. These
// feedback/analog tuples were checked against Quartus 17.0.2. The output
// selector calculates C dividers from the table; it does not derive analog
// settings from the frequency equation alone.
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

struct Profile
{
    Config config;
    int vco_mhz;
    bool single_output;
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

inline Profile make_profile(int vco_mhz, int m, int n, int bandwidth, int charge_pump,
                            int m_low_preset, int m_phase_preset, bool single_output = true)
{
    return Profile{Config{m, n, 0, bandwidth, charge_pump, m_low_preset, m_phase_preset}, vco_mhz,
                   single_output};
}

inline std::array<Profile, 4> checked_profiles(int reference_mhz)
{
    // Order: reported 300, 320, 400 and 520 MHz. Each complete tuple is
    // oracle-checked. The 400 MHz tuple is retained for shared dual/multi
    // output feedback; the other tuples are valid for a single output too.
    if (reference_mhz == 25)
        return {make_profile(300, 24, 2, 6, 1, 1, 0),
                make_profile(320, 64, 5, 3, 2, 7, 3),
                make_profile(400, 32, 2, 6, 1, 1, 0, false),
                make_profile(520, 104, 5, 2, 2, 11, 3)};
    if (reference_mhz == 100)
        return {make_profile(300, 6, 2, 8, 1, 1, 0),
                make_profile(320, 32, 10, 6, 1, 1, 0),
                make_profile(400, 8, 2, 7, 1, 1, 0, false),
                make_profile(520, 52, 10, 4, 1, 1, 0)};
    return {make_profile(300, 12, 2, 7, 1, 1, 0),
            make_profile(320, 32, 5, 6, 2, 4, 2),
            make_profile(400, 16, 2, 7, 1, 1, 0, false),
            make_profile(520, 52, 5, 4, 2, 6, 2)};
}

inline std::array<Config, 4> checked_configs(int reference_mhz)
{
    std::array<Config, 4> result{};
    auto profiles = checked_profiles(reference_mhz);
    for (size_t i = 0; i < profiles.size(); ++i)
        result[i] = profiles[i].config;
    return result;
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

// Solve an output counter for one complete feedback profile. Keeping this
// arithmetic separate from profile selection means new rates only need to be
// exact divisors of a checked VCO; no output-specific case is required.
inline std::optional<int> select_counter(int vco_mhz, int64_t hz, int duty = 50)
{
    if (vco_mhz <= 0 || hz < 1000000 || hz > 100000000)
        return std::nullopt;
    int64_t vco_hz = int64_t(vco_mhz) * 1000000;
    if (vco_hz % hz)
        return std::nullopt;
    int64_t c = vco_hz / hz;
    if (c < 2 || c > 512 || !duty_counts(int(c), duty))
        return std::nullopt;
    return int(c);
}

inline std::optional<Config> select_hz(int64_t hz, int reference_mhz = 50, int duty = 50)
{
    if (!valid_reference(reference_mhz) || hz < 1000000 || hz > 100000000)
        return std::nullopt;
    // Prefer the established 300 MHz tuple, preserving the 25 MHz bitstream.
    for (const auto &profile : checked_profiles(reference_mhz)) {
        if (!profile.single_output)
            continue;
        Config config = profile.config;
        auto counter = select_counter(profile.vco_mhz, hz, duty);
        if (!counter)
            continue;
        config.c = *counter;
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
    for (const auto &profile : checked_profiles(reference_mhz)) {
        Config config = profile.config;
        auto c0 = select_counter(profile.vco_mhz, hz0, duty0);
        auto c1 = select_counter(profile.vco_mhz, hz1, duty1);
        if (c0 && c1) {
            config.c = *c0;
            return DualConfig{config, *c1};
        }
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
    for (const auto &profile : checked_profiles(reference_mhz)) {
        Config config = profile.config;
        std::array<int, 4> counters{};
        bool valid = true;
        for (int i = 0; i < count; ++i) {
            auto counter = select_counter(profile.vco_mhz, hz[i], duties[i]);
            if (!counter) {
                valid = false;
                break;
            }
            counters[i] = *counter;
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
