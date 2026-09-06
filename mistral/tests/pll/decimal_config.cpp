#include "pll.h"
#include <cassert>
#include <cstdint>
int main()
{
    using namespace mistral_pll;
    assert(parse_output_hz("12.5 MHz") == 12500000);
    assert(parse_output_hz("12.500000000 MHz") == 12500000);
    assert(parse_output_hz("1.000001 MHz") == 1000001);
    for (auto s : {"0.5 MHz", "100.000001 MHz", "12.5000001 MHz", "12.5MHz", "1e1 MHz", "nan MHz", "-12.5 MHz"})
        assert(!parse_output_hz(s));
    assert(!parse_output_hz("12. MHz"));
    assert(!parse_output_hz("12.5 MHz junk"));
    assert(!parse_output_hz("999999999999999999999999999999999 MHz"));
    for (int ref : {25, 50, 100}) {
        for (int c = 3; c <= 400; ++c)
            for (int vco : {300, 320, 400}) {
                if (int64_t(vco) * 1000000 % c) continue;
                int64_t hz = int64_t(vco) * 1000000 / c;
                if (hz < 1000000 || hz > 100000000) continue;
                auto pair = select_dual_hz(hz, hz, ref);
                assert(pair);
                assert(int64_t(ref) * 1000000 * pair->feedback.m == hz * pair->feedback.n * pair->feedback.c);
                assert(pair->feedback.c == pair->c1);
            }
        assert(select_hz(12500000, ref)->c == 24);
        assert(select_hz(6400000, ref)->c == 50);
        assert(!select_hz(12300000, ref));
        auto pair = select_dual_hz(12500000, 40000000, ref);
        assert(pair && pair->feedback.c == 32 && pair->c1 == 10);
        assert(!select_dual_hz(12500000, 6400000, ref));
        for (int mhz = 1; mhz <= 100; ++mhz) {
            auto old = select(mhz, ref), decimal = select_hz(int64_t(mhz) * 1000000, ref);
            assert(bool(old) == bool(decimal));
            if (old) assert(old->m == decimal->m && old->n == decimal->n && old->c == decimal->c);
        }
    }
}
