/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  altera_pll cell parameter parsing shared by packing and bit generation.
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

#ifndef MISTRAL_PLL_CELL_H
#define MISTRAL_PLL_CELL_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>

#include "nextpnr.h"
#include "pll_solver.h"

NEXTPNR_NAMESPACE_BEGIN

struct MistralPllRequest
{
    int64_t ref_hz = 0;
    bool fractional = false;
    int clocks = 1;
    std::vector<mistral_pll_solver::OutputRequest> outputs;
};

// Logical output port names: Yosys names a single-bit bus "outclk".
inline IdString mistral_pll_output_port(const Context *ctx, const CellInfo *ci, int index)
{
    if (index == 0 && ci->ports.count(id_outclk))
        return id_outclk;
    return ctx->idf("outclk[%d]", index);
}

inline bool mistral_pll_is_output_port(const Context *ctx, IdString port)
{
    if (port == id_outclk)
        return true;
    for (int i = 0; i < 9; ++i)
        if (port == ctx->idf("outclk[%d]", i))
            return true;
    return false;
}

// Parse and validate generic altera_pll parameters.  Accepts the parameter
// set emitted by Quartus 17's IP generator for a "General" PLL, including the
// zero/default entries for unused outputs.  Returns an error message on any
// unsupported parameter (fail closed).
inline std::string mistral_pll_parse(const Context *ctx, const CellInfo *ci, MistralPllRequest &req)
{
    using namespace mistral_pll_solver;
    auto str = [&](const char *name, const char *fallback, bool &present) -> std::string {
        auto it = ci->params.find(ctx->id(name));
        present = it != ci->params.end();
        if (!present)
            return fallback;
        if (!it->second.is_string)
            return std::string("\x01");
        return it->second.as_string();
    };
    auto integer = [&](IdString name, int fallback, bool &ok) -> int {
        auto it = ci->params.find(name);
        ok = true;
        if (it == ci->params.end())
            return fallback;
        if (it->second.is_string) {
            ok = false;
            return 0;
        }
        // as_int64() keeps only the low 64 bits, and a cast to int keeps 32.
        // 2^32+1 and 2^64+1 both become 1; 2^64+50 becomes 50. Reject a wider
        // bit vector before that truncation, then any value that does not fit
        // in int unchanged.
        if (it->second.size() > 64) {
            ok = false;
            return 0;
        }
        int64_t value = it->second.as_int64();
        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
            ok = false;
            return 0;
        }
        return int(value);
    };
    bool present = false, ok = true;
    req.clocks = integer(ctx->id("number_of_clocks"), 1, ok);
    if (!ok || req.clocks < 1 || req.clocks > 9)
        return "number_of_clocks must be an integer from 1 to 9";
    std::string ref = str("reference_clock_frequency", "", present);
    if (!present)
        return "explicit parameter 'reference_clock_frequency' is required";
    req.ref_hz = parse_hz(ref);
    if (req.ref_hz <= 0)
        return "reference_clock_frequency must be decimal MHz with at most six decimal places";
    std::string mode = str("operation_mode", "", present);
    if (!present)
        return "explicit parameter 'operation_mode' is required";
    if (mode != "direct")
        return "unsupported parameter 'operation_mode'; only direct compensation is supported";
    std::string frac = str("fractional_vco_multiplier", "false", present);
    if (frac != "true" && frac != "false")
        return "fractional_vco_multiplier must be \"true\" or \"false\"";
    req.fractional = frac == "true";
    for (const char *name : {"pll_type", "pll_subtype"}) {
        std::string value = str(name, "General", present);
        if (value != "General")
            return std::string("unsupported parameter '") + name + "'; only the generic \"General\" PLL is supported";
    }
    req.outputs.clear();
    for (int i = 0; i < 18; ++i) {
        IdString fname = ctx->idf("output_clock_frequency%d", i), pname = ctx->idf("phase_shift%d", i),
                 dname = ctx->idf("duty_cycle%d", i);
        auto fit = ci->params.find(fname), pit = ci->params.find(pname);
        bool dok = true;
        int duty = integer(dname, 50, dok);
        if (!dok)
            return "duty_cycle" + std::to_string(i) + " must be an integer percentage";
        std::string phase = pit == ci->params.end() ? "0 ps" : (pit->second.is_string ? pit->second.as_string() : "\x01");
        if (i >= req.clocks) {
            // Unused outputs: only the IP generator's defaults are accepted.
            if (fit != ci->params.end() &&
                (!fit->second.is_string || (fit->second.as_string() != "0 MHz" && fit->second.as_string() != "0 ps")))
                return "unsupported parameter '" + fname.str(ctx) + "' for an unused output";
            if (phase != "0 ps")
                return "unsupported parameter '" + pname.str(ctx) + "' for an unused output";
            if (duty != 50)
                return "unsupported parameter '" + dname.str(ctx) + "' for an unused output";
            continue;
        }
        if (fit == ci->params.end())
            return "explicit parameter '" + fname.str(ctx) + "' is required";
        OutputRequest o;
        o.hz = fit->second.is_string ? parse_hz(fit->second.as_string()) : 0;
        if (o.hz <= 0)
            return fname.str(ctx) + " must be decimal MHz with at most six decimal places";
        auto ps = parse_ps(phase);
        if (!ps)
            return pname.str(ctx) + " must be an integer number of picoseconds (\"<n> ps\")";
        o.phase_ps = *ps;
        if (duty < 1 || duty > 99)
            return "duty cycle must be an integer percent from 1 to 99";
        o.duty = duty;
        req.outputs.push_back(o);
    }
    static const std::vector<std::string> known = {"reference_clock_frequency", "operation_mode",
                                                   "fractional_vco_multiplier", "number_of_clocks", "pll_type",
                                                   "pll_subtype"};
    for (auto &param : ci->params) {
        std::string name = param.first.str(ctx);
        if (std::find(known.begin(), known.end(), name) != known.end())
            continue;
        bool indexed = false;
        for (const char *prefix : {"output_clock_frequency", "phase_shift", "duty_cycle"}) {
            size_t len = std::strlen(prefix);
            if (name.compare(0, len, prefix) == 0 && name.size() > len && name.size() <= len + 2 &&
                std::all_of(name.begin() + len, name.end(), ::isdigit) && std::stoi(name.substr(len)) < 18)
                indexed = true;
        }
        if (!indexed)
            return "unsupported parameter '" + name + "'";
    }
    return "";
}

NEXTPNR_NAMESPACE_END

#endif
