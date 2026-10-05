/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Cyclone V FPLL parameter solver reproducing Quartus Prime 17.0.2's choices
 *  for a generic altera_pll (pll_type "General", direct compensation) on
 *  5CSEBA6U23I7.  Header-only so the standalone selector tests can include it.
 *
 *  Every rule below was derived from the Quartus legality engine
 *  (::quartus::advanced_pll_legality, GENERIC_PLL / CYCLONEV_PLL_CONFIG) and
 *  from decoded Quartus bitstreams; see mistral/tests/pll/README.md
 *  ("General solver").  Where the measured behaviour was not fully
 *  characterised the solver reports Status::Unknown and the caller fails
 *  closed instead of guessing.
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
 *
 */

#ifndef MISTRAL_PLL_SOLVER_H
#define MISTRAL_PLL_SOLVER_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace mistral_pll_solver {

__extension__ typedef __int128 i128;

inline i128 gcd128(i128 a, i128 b)
{
    if (a < 0)
        a = -a;
    if (b < 0)
        b = -b;
    while (b) {
        i128 t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// Exact non-negative rational number (frequencies in Hz).
struct Rat
{
    i128 n = 0, d = 1;
    Rat() = default;
    Rat(i128 num, i128 den = 1) : n(num), d(den)
    {
        if (d < 0) {
            n = -n;
            d = -d;
        }
        i128 g = gcd128(n, d);
        if (g > 1) {
            n /= g;
            d /= g;
        }
    }
    double to_double() const { return double(n) / double(d); }
};
inline Rat operator+(const Rat &a, const Rat &b) { return Rat(a.n * b.d + b.n * a.d, a.d * b.d); }
inline Rat operator-(const Rat &a, const Rat &b) { return Rat(a.n * b.d - b.n * a.d, a.d * b.d); }
inline Rat operator*(const Rat &a, const Rat &b) { return Rat(a.n * b.n, a.d * b.d); }
inline Rat operator/(const Rat &a, const Rat &b) { return Rat(a.n * b.d, a.d * b.n); }
inline bool operator<(const Rat &a, const Rat &b) { return a.n * b.d < b.n * a.d; }
inline bool operator>(const Rat &a, const Rat &b) { return b < a; }
inline bool operator<=(const Rat &a, const Rat &b) { return !(b < a); }
inline bool operator>=(const Rat &a, const Rat &b) { return !(a < b); }
inline bool operator==(const Rat &a, const Rat &b) { return a.n == b.n && a.d == b.d; }
inline bool operator!=(const Rat &a, const Rat &b) { return !(a == b); }
inline Rat rabs(const Rat &a) { return a.n < 0 ? Rat(-a.n, a.d) : a; }
inline i128 rfloor(const Rat &a) { return a.n >= 0 ? a.n / a.d : -((-a.n + a.d - 1) / a.d); }
// Round half away from zero; callers only use it where ties cannot occur or are flagged.
inline i128 rround(const Rat &a) { return rfloor(a + Rat(1, 2)); }

enum class Status
{
    Accept,
    Unknown,
    Reject
};

inline Status worst(Status a, Status b) { return a > b ? a : b; }

struct OutputRequest
{
    int64_t hz = 0;       // exact request, 1 Hz resolution
    int64_t phase_ps = 0; // static phase shift in picoseconds
    int duty = 50;        // integer duty percent
};

struct CounterSetting
{
    int c = 0;              // division ratio (1 = bypass)
    int high = 0, low = 0;  // high/low counts (256 encodes as 0)
    bool odd = false;       // odd-divider even-duty correction
    bool bypass = false;
    int preset = 1;         // CNT_PRESET
    int phase_mux = 0;      // CNT_PH_MUX_PRESET
    Rat achieved_hz;        // nominal output frequency
    int64_t phase_ps = 0;   // realised shift, rounded to integer ps
};

struct Solution
{
    bool fractional = false;
    Rat vco_hz;  // selected (Quartus-reported) VCO frequency
    int m = 0, n = 0;
    bool n_bypass = false;
    uint32_t k = 1;  // FRACTIONAL_DIVISION_SETTING (Quartus default 1 in integer mode)
    int vco_div_setting = 0; // Mistral VCO_DIV value: 0 = post divider /2, 1 = /1
    int bwctrl = 0, cp_current = 0;
    int m_low_preset = 1, m_phase_preset = 0;
    bool m_odd = false;
    std::vector<CounterSetting> outputs;
};

struct Result
{
    std::optional<Solution> solution;
    std::string error;
};

// Requests are parsed as Quartus does: "<decimal> MHz", at most six decimal
// places (1 Hz).  Returns 0 for anything else.
inline int64_t parse_hz(const std::string &text)
{
    static const std::regex pattern("^([0-9]{1,4})(\\.([0-9]+))? ?MHz$");
    std::smatch match;
    if (text.size() > 40 || !std::regex_match(text, match, pattern))
        return 0;
    std::string fraction = match[3].str();
    while (!fraction.empty() && fraction.back() == '0')
        fraction.pop_back();
    if (fraction.size() > 6)
        return 0;
    while (fraction.size() < 6)
        fraction += '0';
    return int64_t(std::stoi(match[1].str())) * 1000000 + std::stoi(fraction);
}

inline std::optional<int64_t> parse_ps(const std::string &text)
{
    static const std::regex pattern("^(-?[0-9]{1,9}) ?ps$");
    std::smatch match;
    if (text.size() > 20 || !std::regex_match(text, match, pattern))
        return std::nullopt;
    return std::stoll(match[1].str());
}

// --- Measured device/engine constants -------------------------------------
static const Rat VCO_MIN(300000000), VCO_MAX(1600000000);
static const Rat VCO_DIV_BOUNDARY(600000000);
static const int64_t MAX_OUTPUT_HZ = 550000000;
// Engine boundary guard: Quartus evaluates these comparisons in floating
// point; anything this close to a boundary is treated as Unknown.
static const Rat EPS(1, 1000); // 1 mHz

inline Rat output_tolerance(int64_t hz)
{
    // Measured: 500 Hz absolute, or 100 ppm for outputs below 5 MHz.
    Rat ppm = Rat(hz, 10000);
    return ppm < Rat(500) ? ppm : Rat(500);
}

// Integer-mode realizable VCO (Quartus GENERIC_PLL legality).
inline Status integer_vco_legal(const Rat &ref, const Rat &v)
{
    Rat r = v / ref;
    i128 m0 = r.n, n0 = r.d;
    if (v >= VCO_DIV_BOUNDARY) {
        if (ref / Rat(n0) < Rat(5000000))
            return Status::Reject;
        if (m0 % 2 == 0 && m0 > 255)
            return Status::Reject;
        if (m0 > 255)
            return Status::Unknown;
        return Status::Accept;
    }
    if (m0 % 2 == 0)
        return ref / Rat(n0) >= Rat(5000000) ? Status::Accept : Status::Reject;
    return ref / Rat(2 * n0) > Rat(5000000) ? Status::Accept : Status::Reject;
}

// Duty-cycle counter split: h = high time in half VCO periods.
inline Status duty_split(int c, int duty, CounterSetting &cs)
{
    if (duty == 50) {
        if (c == 1) {
            cs.bypass = true;
            cs.high = cs.low = 0;
            cs.odd = false;
        } else {
            cs.high = (c + 1) / 2;
            cs.low = c / 2;
            cs.odd = (c & 1) != 0;
        }
        return Status::Accept;
    }
    if (c < 2 || duty < 1 || duty > 99)
        return Status::Reject;
    bool near = false;
    for (int h = 1; h <= 2 * c - 2; ++h) {
        int high = (h + 1) / 2, low = c - high;
        if (high < 1 || low < 1 || high > 256 || low > 256)
            continue;
        Rat pct(100 * h, 2 * c);
        Rat diff = rabs(pct - Rat(duty));
        if (diff == Rat(0)) {
            cs.high = high;
            cs.low = low;
            cs.odd = (h & 1) != 0;
            return Status::Accept;
        }
        if (diff <= Rat(1, 2))
            near = true;
    }
    // Quartus rounds inexact duty requests; that rounding is not modelled.
    return near ? Status::Unknown : Status::Reject;
}

inline Status check_output(const Rat &v, int c, const OutputRequest &o, CounterSetting &cs)
{
    if (c < 1 || c > 512)
        return Status::Reject;
    Status st = Status::Accept;
    // Requests and the truncated actual frequency are whole hertz, so these
    // comparisons have no floating-point ambiguity except exact equality
    // with the tolerance, which Quartus resolves inconsistently.
    Rat req = Rat(o.hz) * Rat(c);
    if (req < VCO_MIN || req > VCO_MAX)
        return Status::Reject;
    Rat act(rfloor(v / Rat(c))); // Quartus truncates the actual output to 1 Hz
    Rat err = rabs(act - Rat(o.hz));
    Rat tol = output_tolerance(o.hz);
    if (err > tol)
        return Status::Reject;
    if (err == tol)
        st = worst(st, Status::Unknown);
    cs = CounterSetting();
    cs.c = c;
    st = worst(st, duty_split(c, o.duty, cs));
    if (st == Status::Reject)
        return st;
    cs.achieved_hz = v / Rat(c);
    if (o.phase_ps < 0)
        return worst(st, Status::Unknown); // negative phases are not modelled
    if (o.phase_ps) {
        // Phase step is one eighth of the VCO period.
        Rat step = Rat(1000000000000LL) / (v * Rat(8));
        Rat e = Rat(o.phase_ps) / step;
        i128 k = rround(e);
        Rat dist = rabs(e - Rat(k)) * step;
        if (dist > Rat(5) + EPS)
            return Status::Reject;
        if (dist > Rat(5) - EPS)
            st = worst(st, Status::Unknown);
        if (k > 2047)
            return Status::Reject;
        if (k / 8 + 1 > 255)
            st = worst(st, Status::Unknown);
        cs.preset = int(k / 8) + 1;
        cs.phase_mux = int(k % 8);
        cs.phase_ps = int64_t(rround(Rat(k) * step));
    }
    return st;
}

inline Status check_all_outputs(const Rat &v, const std::vector<OutputRequest> &outs,
                                std::vector<CounterSetting> &settings)
{
    Status total = Status::Accept;
    settings.assign(outs.size(), CounterSetting());
    for (size_t i = 0; i < outs.size(); ++i) {
        i128 c0 = rround(v / Rat(outs[i].hz));
        Status best = Status::Reject;
        for (i128 c = c0 - 1; c <= c0 + 1; ++c) {
            CounterSetting cs;
            Status s = check_output(v, int(std::max<i128>(0, std::min<i128>(c, 100000))), outs[i], cs);
            if (s < best) {
                best = s;
                settings[i] = cs;
            }
            if (s == Status::Accept)
                break;
        }
        total = worst(total, best);
        if (total == Status::Reject)
            break;
    }
    return total;
}

// Bandwidth/charge-pump tables indexed by M x post-divider (Quartus
// "Medium"/Auto for ordinary configurations, "Low" after M/N doubling).
// Values: resistor code (Mistral BWCTRL) and charge pump code (CP_CURRENT).
struct BwEntry
{
    int limit, bwctrl, cp; // cp < 0: not observed, fail closed
};
static const BwEntry TABLE_MEDIUM[] = {{2, 9, 2}, {3, 9, 3},   {7, 8, 2},   {13, 8, 3}, {32, 7, 2},
                                       {70, 6, 2}, {124, 4, 2}, {192, 3, 2}, {256, 2, 2}};
static const BwEntry TABLE_LOW[] = {{2, 8, -1}, {3, 9, -1},  {7, 8, 1},   {13, 8, 1}, {32, 7, 1},
                                    {70, 6, 1}, {124, 4, 1}, {192, 3, 1}, {256, 2, 1}};

inline const BwEntry *bw_lookup(bool low, int m_eff)
{
    const BwEntry *table = low ? TABLE_LOW : TABLE_MEDIUM;
    for (int i = 0; i < 9; ++i)
        if (m_eff <= table[i].limit)
            return &table[i];
    return nullptr;
}

// Integer feedback (M, N) and analogue settings for a chosen VCO.
inline Status integer_feedback(const Rat &ref, const Rat &v, Solution &s, std::string &why)
{
    Rat r = v / ref;
    i128 m0 = r.n, n0 = r.d;
    bool doubled = (n0 % 2 == 1) && ref / Rat(2 * n0) > Rat(5000000) && 2 * m0 <= 256;
    i128 m = doubled ? 2 * m0 : m0, n = doubled ? 2 * n0 : n0;
    if (m > 256 || (!doubled && m > 255) || n > 512) {
        why = "feedback counter out of range";
        return Status::Reject;
    }
    int vdiv = v < VCO_DIV_BOUNDARY ? 2 : 1;
    const BwEntry *e = bw_lookup(doubled, int(m) * vdiv);
    if (!e || e->cp < 0) {
        why = "loop-filter setting not characterised";
        return Status::Unknown;
    }
    s.m = int(m);
    s.n = int(n);
    s.n_bypass = (n == 1);
    s.vco_div_setting = vdiv == 2 ? 0 : 1;
    s.bwctrl = e->bwctrl;
    s.cp_current = e->cp;
    s.m_odd = (m & 1) != 0;
    if (n % 2 == 1 && n > 1) {
        // Odd N: Quartus shifts the M counter by half a reference period.
        i128 q = rround(Rat(4 * m, n));
        s.m_low_preset = int(1 + q / 8);
        s.m_phase_preset = int(q % 8);
        if (s.m_low_preset > 255) {
            why = "M preset out of range";
            return Status::Unknown;
        }
    }
    return Status::Accept;
}

// Quartus 17 computes the fractional word in double precision, prints the
// resulting VCO truncated to six decimal places in MHz and derives the final
// word from that printed value.  Reproduce that sequence exactly.
inline double truncate_6dp(double value)
{
    int exponent = 0;
    double mantissa = std::frexp(value, &exponent);
    int64_t imant = int64_t(std::ldexp(mantissa, 53));
    int shift = exponent - 53;
    i128 scaled = i128(imant) * 1000000;
    i128 whole;
    if (shift >= 0)
        whole = scaled << shift;
    else
        whole = scaled >> (-shift);
    char buf[64];
    int64_t ip = int64_t(whole / 1000000), fp = int64_t(whole % 1000000);
    std::snprintf(buf, sizeof buf, "%lld.%06lld", (long long)ip, (long long)fp);
    return std::strtod(buf, nullptr);
}

inline void fractional_word(const Rat &ref, int n, const Rat &v, int &m, uint32_t &k)
{
    double ref_mhz = double(ref.n) / double(ref.d * 1000000);
    double v_mhz = double(v.n) / double(v.d * 1000000);
    double r = ref_mhz / n;
    double x = v_mhz / r;
    double mf = std::floor(x);
    double k1 = std::nearbyint((x - mf) * 4294967296.0);
    double v1 = r * (mf + k1 / 4294967296.0);
    double vs = truncate_6dp(v1);
    double k2 = std::nearbyint((vs / r - mf) * 4294967296.0);
    m = int(mf);
    k = uint32_t(int64_t(k2) & 0xffffffffLL);
}

inline Result solve_integer(int64_t ref_hz, const std::vector<OutputRequest> &outs)
{
    Result res;
    Rat ref(ref_hz);
    if (ref_hz < 5000000 || ref_hz > 320000000) {
        res.error = "integer mode supports reference clocks from 5 to 320 MHz";
        return res;
    }
    // Candidate VCOs: realizable ref*M/N values within tolerance of f0*C.
    std::vector<Rat> cands;
    const OutputRequest &o0 = outs.front();
    Rat tol0 = output_tolerance(o0.hz);
    int nmax = int(ref_hz / 5000000) + 1;
    for (int c = 1; c <= 512; ++c) {
        Rat req = Rat(o0.hz) * Rat(c);
        if (req < VCO_MIN - EPS || req > VCO_MAX + EPS)
            continue;
        Rat lo = (Rat(o0.hz) - tol0 - EPS) * Rat(c), hi = (Rat(o0.hz) + tol0 + EPS) * Rat(c);
        for (int n = 1; n <= nmax; ++n) {
            Rat a = lo * Rat(n) / ref, b = hi * Rat(n) / ref;
            i128 mlo = rfloor(a), mhi = rfloor(b);
            for (i128 m = std::max<i128>(1, mlo); m <= mhi; ++m) {
                Rat v = ref * Rat(m) / Rat(n);
                if (v >= lo && v <= hi)
                    cands.push_back(v);
            }
        }
    }
    std::sort(cands.begin(), cands.end());
    cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
    for (const Rat &v : cands) {
        Status st = integer_vco_legal(ref, v);
        if (st == Status::Reject)
            continue;
        std::vector<CounterSetting> settings;
        st = worst(st, check_all_outputs(v, outs, settings));
        if (st == Status::Reject)
            continue;
        if (st == Status::Unknown) {
            res.error = "Quartus's choice is not determined for this request (first candidate VCO " +
                        std::to_string(v.to_double() / 1e6) + " MHz is outside the characterised rules)";
            return res;
        }
        Solution s;
        s.vco_hz = v;
        s.outputs = settings;
        std::string why;
        Status fb = integer_feedback(ref, v, s, why);
        if (fb != Status::Accept) {
            res.error = "VCO " + std::to_string(v.to_double() / 1e6) + " MHz: " + why;
            return res;
        }
        res.solution = s;
        return res;
    }
    res.error = "no legal integer-mode VCO reproduces every requested output within Quartus's tolerance";
    return res;
}

inline Result solve_fractional(int64_t ref_hz, const std::vector<OutputRequest> &outs)
{
    Result res;
    Rat ref(ref_hz);
    if (ref_hz < 50000000 || ref_hz > 100000000) {
        // Above 100 MHz Quartus rejects some VCOs this model accepts.
        res.error = "fractional-N mode supports reference clocks from 50 to 100 MHz";
        return res;
    }
    int nmax = int(ref_hz / 50000000); // PFD must stay at or above 50 MHz
    struct Cand
    {
        Rat v;
        Status st;
        bool integer;
        int n;
    };
    std::vector<Cand> cands;
    static const Rat FLO(5, 100), FHI(95, 100), FEPS(1, 10000000);
    // Fractional legality for one N: integer part >= 8 and the fraction inside
    // the measured [0.05, 0.95] window (exact integers are handled separately).
    auto frac_status = [&](const Rat &v, int n) {
        Rat x = v * Rat(n) / ref;
        i128 m = rfloor(x);
        Rat fr = x - Rat(m);
        if (m < 8 || fr == Rat(0))
            return Status::Reject;
        if (fr > FLO + FEPS && fr < FHI - FEPS)
            return Status::Accept;
        if (rabs(fr - FLO) <= FEPS || rabs(fr - FHI) <= FEPS)
            return Status::Unknown;
        return Status::Reject;
    };
    for (const auto &o : outs) {
        for (int c = 1; c <= 512; ++c) {
            Rat v = Rat(o.hz) * Rat(c);
            if (v < Rat(399000000) || v > VCO_MAX)
                continue;
            // Quartus may realise a VCO with any N <= nmax; when more than
            // one N is legal its choice is not characterised.
            int legal_n = 0, legal_count = 0;
            Status st = Status::Reject;
            for (int n = 1; n <= nmax; ++n) {
                Status sn = frac_status(v, n);
                if (sn != Status::Reject) {
                    ++legal_count;
                    legal_n = n;
                    st = worst(st == Status::Reject ? Status::Accept : st, sn);
                }
            }
            if (legal_count > 1)
                st = Status::Unknown;
            if (legal_count > 0)
                cands.push_back({v, st, false, legal_n});
            // An integer multiple of the PFD within tolerance is also legal.
            for (int n = 1; n <= nmax; ++n) {
                Rat pfd = ref / Rat(n);
                i128 mi = rround(v / pfd);
                Rat vi = pfd * Rat(mi);
                if (mi >= 8 && vi >= VCO_MIN && vi <= VCO_MAX)
                    cands.push_back({vi, nmax > 1 ? Status::Unknown : Status::Accept, true, n});
            }
        }
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.v < b.v; });
    for (size_t i = 0; i < cands.size(); ++i) {
        if (i > 0 && cands[i].v == cands[i - 1].v)
            continue; // the first-inserted entry for a value is authoritative
        Cand cand = cands[i];
        Status st = cand.st;
        if (outs.size() > 1) {
            // Quartus merges nearby per-output candidates; only an exact
            // common VCO for all outputs is modelled.
            for (const auto &o : outs)
                if ((cand.v / Rat(o.hz)).d != 1)
                    st = worst(st, Status::Unknown);
        }
        std::vector<CounterSetting> settings;
        st = worst(st, check_all_outputs(cand.v, outs, settings));
        if (st == Status::Reject)
            continue;
        if (st == Status::Unknown) {
            res.error = "Quartus's fractional-N choice is not determined for this request (first candidate VCO " +
                        std::to_string(cand.v.to_double() / 1e6) + " MHz is outside the characterised rules)";
            return res;
        }
        Solution s;
        s.fractional = true;
        int n = cand.n;
        Rat pfd = ref / Rat(n);
        s.n = n;
        s.n_bypass = (n == 1);
        int m;
        uint32_t k;
        fractional_word(ref, n, cand.v, m, k);
        if (cand.integer || Rat(m) == cand.v / pfd) {
            m = int(rround(cand.v / pfd));
            k = 0;
        }
        s.m = m;
        s.k = k;
        s.m_odd = (m & 1) != 0;
        int vdiv = cand.v < VCO_DIV_BOUNDARY ? 2 : 1;
        s.vco_div_setting = vdiv == 2 ? 0 : 1;
        const BwEntry *e = bw_lookup(false, m * vdiv);
        if (!e || e->cp < 0) {
            res.error = "fractional-N loop-filter setting not characterised";
            return res;
        }
        s.bwctrl = e->bwctrl;
        s.cp_current = e->cp;
        // The realised VCO follows the programmed word.
        Rat real = pfd * (Rat(m) + Rat(i128(k), i128(1) << 32));
        s.vco_hz = real;
        for (auto &cs : settings)
            cs.achieved_hz = real / Rat(cs.c);
        s.outputs = settings;
        res.solution = s;
        return res;
    }
    res.error = "no legal fractional-N VCO reproduces every requested output within Quartus's tolerance";
    return res;
}

inline Result solve(int64_t ref_hz, bool fractional, const std::vector<OutputRequest> &outs)
{
    Result res;
    if (outs.empty() || outs.size() > 9) {
        res.error = "between 1 and 9 outputs are supported";
        return res;
    }
    for (const auto &o : outs) {
        if (o.hz <= 0) {
            res.error = "output frequencies must be positive";
            return res;
        }
        if (o.duty < 1 || o.duty > 99) {
            res.error = "duty cycles must be integer percentages from 1 to 99";
            return res;
        }
        // Cyclone V -7 datasheet fOUT limit for global/regional clocks.  Quartus
        // itself accepts counter outputs up to 1340 MHz (checked at 550 MHz).
        if (o.hz > MAX_OUTPUT_HZ) {
            res.error = "output frequencies above 550 MHz exceed the -7 global clock limit";
            return res;
        }
    }
    return fractional ? solve_fractional(ref_hz, outs) : solve_integer(ref_hz, outs);
}

} // namespace mistral_pll_solver

#endif
