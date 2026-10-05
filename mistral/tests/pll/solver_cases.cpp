// Check the Cyclone V PLL solver against every compiled Quartus 17.0.2 oracle
// PLL in fixtures/solver/cases.txt.  Usage: solver_cases <cases.txt>
// Build: c++ -std=c++17 -I mistral mistral/tests/pll/solver_cases.cpp -o solver_cases
#include "pll_solver.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace mistral_pll_solver;

static std::string vco_string(const Rat &hz)
{
    // Quartus reports the VCO in MHz truncated to six decimals, trailing zeros removed.
    i128 micro = rfloor(hz / Rat(1));
    int64_t whole = int64_t(micro / 1000000), frac = int64_t(micro % 1000000);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%lld.%06lld", (long long)whole, (long long)frac);
    std::string s = buf;
    while (s.back() == '0')
        s.pop_back();
    if (s.back() == '.')
        s += '0';
    return s + "MHz";
}

int main(int argc, char **argv)
{
    std::ifstream in(argc > 1 ? argv[1] : "cases.txt");
    std::string line;
    int checked = 0, failures = 0, garbage = 0, closed = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream all(line);
        std::string run, pll, bar;
        int64_t ref;
        int frac;
        all >> run >> pll >> ref >> frac >> bar;
        std::vector<OutputRequest> outs;
        std::string tok;
        while (all >> tok && tok != "|") {
            OutputRequest o;
            o.hz = std::stoll(tok);
            all >> o.phase_ps >> o.duty;
            outs.push_back(o);
        }
        std::string rest;
        std::getline(all, rest);
        Result r = solve(ref, frac != 0, outs);
        if (rest.find("garbage") != std::string::npos) {
            ++garbage;
            if (r.solution) {
                std::cerr << "FAIL " << run << " " << pll << ": accepted a configuration Quartus mis-implements\n";
                ++failures;
            }
            continue;
        }
        if (!r.solution) {
            ++closed;
            std::cout << "fail-closed " << run << " " << pll << ": " << r.error << "\n";
            continue;
        }
        const Solution &s = *r.solution;
        std::ostringstream got;
        got << " " << vco_string(s.fractional ? Rat(i128(rfloor(s.vco_hz * Rat(1))), 1) : s.vco_hz) << " " << s.m
            << " " << s.n << " " << s.k << " " << s.bwctrl << " " << s.cp_current << " " << s.m_low_preset << " "
            << s.m_phase_preset << " " << s.vco_div_setting << " " << int(s.m_odd) << " " << int(s.n_bypass) << " |";
        for (const auto &c : s.outputs)
            got << " C" << c.c << ":" << (c.high ? c.high : 256) << "/" << (c.low ? c.low : 256)
                << (c.odd ? "o" : "") << (c.bypass ? "b" : "") << "@" << c.preset << "." << c.phase_mux;
        std::string expect = rest;
        std::string g = got.str();
        // Bypassed counters report hi/lo 0 in the bitstream decode; normalise.
        for (std::string *str : {&expect, &g}) {
            size_t p;
            while ((p = str->find("0/0b")) != std::string::npos)
                str->replace(p, 4, "256/256b");
        }
        if (g != expect) {
            ++failures;
            std::cerr << "FAIL " << run << " " << pll << "\n  quartus:" << expect << "\n  solver: " << g << "\n";
        }
        ++checked;
    }
    std::cout << "checked " << checked << " Quartus PLLs, " << garbage << " Quartus mis-implementations rejected, "
              << closed << " fail-closed, " << failures << " failures\n";
    return failures ? 1 : 0;
}
