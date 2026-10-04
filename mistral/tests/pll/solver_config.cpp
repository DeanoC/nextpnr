// Standalone checks of the Quartus-derived PLL solver (no device database).
// Every expectation below is a Quartus 17.0.2 result: the earlier checked
// profiles (now produced by the general rules), the fractional words Quartus
// emits, and fail-closed cases.
// Build: c++ -std=c++17 -I mistral mistral/tests/pll/solver_config.cpp -o solver_config
#include "pll_solver.h"

#include <cassert>
#include <iostream>

using namespace mistral_pll_solver;

static Solution solve_ok(int64_t ref, bool frac, std::vector<OutputRequest> outs)
{
    Result r = solve(ref, frac, outs);
    if (!r.solution) {
        std::cerr << "unexpected rejection: " << r.error << "\n";
        assert(false);
    }
    return *r.solution;
}

static OutputRequest out(int64_t hz, int64_t phase = 0, int duty = 50) { return OutputRequest{hz, phase, duty}; }

struct Expect
{
    int m, n, bw, cp, lo, ph;
};

static void check_feedback(const Solution &s, const Expect &e)
{
    assert(s.m == e.m && s.n == e.n && s.bwctrl == e.bw && s.cp_current == e.cp && s.m_low_preset == e.lo &&
           s.m_phase_preset == e.ph);
}

int main()
{
    const int64_t MHz = 1000000;
    // The checked reference table (README "Checked reference frequencies"):
    // reported VCO 300/320/400/520 MHz at 25/50/100 MHz references.
    struct Row
    {
        int64_t ref;
        std::vector<OutputRequest> outs;
        int vco;
        Expect e;
    };
    const std::vector<Row> rows = {
            {25, {out(25 * MHz)}, 300, {24, 2, 6, 1, 1, 0}},
            {25, {out(40 * MHz)}, 320, {64, 5, 3, 2, 7, 3}},
            {25, {out(25 * MHz), out(40 * MHz)}, 400, {32, 2, 6, 1, 1, 0}},
            {25, {out(52 * MHz)}, 520, {104, 5, 2, 2, 11, 3}},
            {50, {out(25 * MHz)}, 300, {12, 2, 7, 1, 1, 0}},
            {50, {out(40 * MHz)}, 320, {32, 5, 6, 2, 4, 2}},
            {50, {out(25 * MHz), out(40 * MHz)}, 400, {16, 2, 7, 1, 1, 0}},
            {50, {out(52 * MHz)}, 520, {52, 5, 4, 2, 6, 2}},
            {100, {out(25 * MHz)}, 300, {6, 2, 8, 1, 1, 0}},
            {100, {out(40 * MHz)}, 320, {32, 10, 6, 1, 1, 0}},
            {100, {out(25 * MHz), out(40 * MHz)}, 400, {8, 2, 7, 1, 1, 0}},
            {100, {out(52 * MHz)}, 520, {52, 10, 4, 1, 1, 0}},
    };
    for (const auto &row : rows) {
        Solution s = solve_ok(row.ref * MHz, false, row.outs);
        assert(s.vco_hz == Rat(int64_t(row.vco) * MHz));
        assert(s.vco_div_setting == 0 && s.k == 1 && !s.fractional);
        check_feedback(s, row.e);
    }
    // Output counters: odd dividers use even-duty correction; duty splits.
    {
        Solution s = solve_ok(50 * MHz, false, {out(25 * MHz), out(50 * MHz), out(100 * MHz), out(75 * MHz)});
        assert(s.outputs[2].c == 3 && s.outputs[2].high == 2 && s.outputs[2].low == 1 && s.outputs[2].odd);
        assert(s.outputs[3].c == 4 && s.outputs[3].high == 2 && s.outputs[3].low == 2 && !s.outputs[3].odd);
        Solution d25 = solve_ok(50 * MHz, false, {out(25 * MHz, 0, 25)});
        assert(d25.outputs[0].high == 3 && d25.outputs[0].low == 9);
        Solution d75 = solve_ok(50 * MHz, false, {out(25 * MHz, 0, 75)});
        assert(d75.outputs[0].high == 9 && d75.outputs[0].low == 3);
    }
    // Static phase presets (VCO/8 steps): 25 MHz quadrature and 50 MHz eighths.
    {
        Solution q = solve_ok(50 * MHz, false,
                              {out(25 * MHz), out(25 * MHz, 10000), out(25 * MHz, 20000), out(25 * MHz, 30000)});
        assert(q.outputs[1].preset == 4 && q.outputs[2].preset == 7 && q.outputs[3].preset == 10);
        const int presets[8][2] = {{1, 0}, {1, 6}, {2, 4}, {3, 2}, {4, 0}, {4, 6}, {5, 4}, {6, 2}};
        for (int i = 0; i < 8; ++i) {
            Solution e = solve_ok(50 * MHz, false, {out(50 * MHz), out(50 * MHz, 2500 * i)});
            assert(e.outputs[1].preset == presets[i][0] && e.outputs[1].phase_mux == presets[i][1]);
            assert(e.outputs[1].phase_ps == 2500 * i);
        }
        // 130 MHz SDRAM pair: 650 MHz VCO uses the /1 post divider (VCO_DIV 1).
        Solution sd = solve_ok(50 * MHz, false, {out(130 * MHz), out(130 * MHz, 6538)});
        check_feedback(sd, {26, 2, 7, 1, 1, 0});
        assert(sd.vco_div_setting == 1 && sd.outputs[1].preset == 5 && sd.outputs[1].phase_mux == 2);
    }
    // Fractional-N words exactly as Quartus emits them (six-decimal VCO).
    {
        Solution a = solve_ok(50 * MHz, true, {out(74250000)});
        assert(a.m == 8 && a.n == 1 && a.n_bypass && a.k == 0xe8f5c239 && a.outputs[0].c == 6);
        Solution b = solve_ok(50 * MHz, true, {out(12288000)});
        assert(b.m == 8 && b.k == 472790000 && b.outputs[0].c == 33 && b.bwctrl == 7 && b.cp_current == 2);
        Solution c = solve_ok(50 * MHz, true, {out(11289600)});
        assert(c.k == 0x20e6293f && c.outputs[0].c == 36);
        Solution d = solve_ok(50 * MHz, true, {out(12288000), out(24576000)});
        assert(d.k == 0x5b18548b && d.outputs[0].c == 34 && d.outputs[1].c == 17);
        Solution e = solve_ok(50 * MHz, true, {out(99 * MHz)});
        assert(e.m == 9 && e.k == 0xe6666611 && e.m_odd);
        Solution f = solve_ok(100 * MHz, true, {out(27 * MHz)});
        assert(f.m == 8 && f.n == 2 && !f.n_bypass && f.k == 429496730 && f.outputs[0].c == 15);
        Solution g = solve_ok(50 * MHz, true, {out(148500000)});
        assert(g.k == 0xe8f5c239 && g.outputs[0].c == 3);
    }
    // General integer rules beyond the old table.
    {
        Solution h = solve_ok(50 * MHz, false, {out(65 * MHz)}); // 325 MHz, odd M
        check_feedback(h, {13, 2, 7, 2, 1, 0});
        assert(h.m_odd);
        Solution i = solve_ok(50 * MHz, false, {out(108 * MHz)}); // odd N preset round(4M/N)
        check_feedback(i, {54, 5, 4, 2, 6, 3});
        Solution j = solve_ok(50 * MHz, false, {out(85909090), out(21477272)}); // NTSC from 945 MHz
        assert(j.vco_hz == Rat(945 * MHz) && j.m == 189 && j.n == 10 && j.bwctrl == 3 && j.vco_div_setting == 1);
        assert(j.outputs[0].c == 11 && j.outputs[1].c == 44);
        Solution k = solve_ok(27 * MHz, false, {out(74250000), out(148500000)});
        // Quartus oracle fixtures/general/refs: 445.5 MHz, M33/N2.
        assert(k.vco_hz == Rat(int64_t(4455) * MHz / 10) && k.m == 33 && k.n == 2 && k.m_odd);
    }
    // Fail closed.
    assert(!solve(50 * MHz, false, {out(148500000)}).solution); // Quartus mis-implements (M=297)
    assert(!solve(50 * MHz, false, {out(600 * MHz)}).solution); // above the -7 global clock limit
    assert(!solve(4 * MHz, false, {out(25 * MHz)}).solution);   // reference below 5 MHz
    assert(!solve(25 * MHz, true, {out(25 * MHz)}).solution);   // fractional reference below 50 MHz
    assert(!solve(50 * MHz, false, {out(25 * MHz, -100)}).solution); // negative phase not modelled
    assert(!solve(50 * MHz, true, {out(85909090), out(21477272), out(42954545)}).solution); // merged VCOs
    std::vector<OutputRequest> ten(10, out(25 * MHz));
    assert(!solve(50 * MHz, false, ten).solution);
    std::cout << "PASS: PLL solver reproduces the checked Quartus profiles and fails closed\n";
    return 0;
}
