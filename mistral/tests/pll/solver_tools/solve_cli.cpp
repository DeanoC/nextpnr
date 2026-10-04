#include "pll_solver.h"
#include <iostream>
#include <sstream>
using namespace mistral_pll_solver;
// stdin lines: ref_hz frac f0 ph0 d0 [f1 ph1 d1 ...]
int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        int64_t ref; int frac; ss >> ref >> frac;
        std::vector<OutputRequest> outs; OutputRequest o;
        while (ss >> o.hz >> o.phase_ps >> o.duty) outs.push_back(o);
        Result r = solve(ref, frac, outs);
        if (!r.solution) { std::cout << "ERR " << r.error << "\n"; continue; }
        auto &s = *r.solution;
        std::cout << "OK vco=" << (long long)(s.vco_hz.n / s.vco_hz.d) << "+" << (long long)(s.vco_hz.n % s.vco_hz.d) << "/" << (long long)s.vco_hz.d
                  << " M=" << s.m << " N=" << s.n << " K=" << s.k << " BW=" << s.bwctrl << " CP=" << s.cp_current
                  << " pre=" << s.m_low_preset << "/" << s.m_phase_preset << " vdiv=" << s.vco_div_setting;
        for (auto &c : s.outputs) std::cout << " C" << c.c << ":" << c.high << "/" << c.low << (c.odd ? "o" : "") << (c.bypass ? "b" : "") << "@" << c.preset << "." << c.phase_mux;
        std::cout << "\n";
    }
}
