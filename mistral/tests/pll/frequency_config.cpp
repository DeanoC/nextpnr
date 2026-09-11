#include "pll.h"
#include <cassert>
#include <string>

int main()
{
    using namespace mistral_pll;
    for (int ref : {25, 50, 100})
        for (const auto &profile : checked_profiles(ref))
            assert(int64_t(ref) * profile.config.m == int64_t(profile.vco_mhz) * profile.config.n);
    assert(select_counter(520, 52000000) == 10);
    assert(!select_counter(520, 51000000));
    assert(!select_counter(520, 1000000)); // C=520 exceeds the hardware counter.
    for (int mhz = 1; mhz <= 100; ++mhz) {
        auto config = select(mhz);
        bool supported = 300 % mhz == 0 || 320 % mhz == 0 || 520 % mhz == 0;
        assert(bool(config) == supported);
        if (config) {
            assert(50 * config->m == mhz * config->n * config->c);
            assert(config->c >= 2 && config->c <= 512);
            assert((config->c + 1) / 2 <= 255);
            assert(50 / config->n >= 5); // PFD minimum
        }
    }
    for (int a = 0; a <= 101; ++a)
        for (int b = 0; b <= 101; ++b) {
            auto dual = select_dual(a, b);
            int expected_vco = 0;
            if (a >= 1 && a <= 100 && b >= 1 && b <= 100)
                for (int vco : {300, 320, 400, 520})
                    if (!expected_vco && vco % a == 0 && vco % b == 0 && vco / a <= 512 && vco / b <= 512)
                        expected_vco = vco;
            assert(bool(dual) == (expected_vco != 0));
            if (dual) {
                assert(50 * dual->feedback.m / dual->feedback.n == expected_vco);
                assert(50 * dual->feedback.m == a * dual->feedback.n * dual->feedback.c);
                assert(50 * dual->feedback.m == b * dual->feedback.n * dual->c1);
            }
        }
    auto old = select(25);
    assert(old->m == 12 && old->n == 2 && old->c == 12);
    auto forty = select(40);
    assert(forty->m == 32 && forty->n == 5 && forty->c == 8);
    assert(forty->bandwidth == 6 && forty->charge_pump == 2);
    assert(forty->m_low_preset == 4 && forty->m_phase_preset == 2);
    auto fifty_two = select(52);
    assert(fifty_two->m == 52 && fifty_two->n == 5 && fifty_two->c == 10);
    assert(fifty_two->bandwidth == 4 && fifty_two->charge_pump == 2);
    assert(fifty_two->m_low_preset == 6 && fifty_two->m_phase_preset == 2);
    assert(select(65)->m == 52 && select(65)->n == 5 && select(65)->c == 8);
    assert(select(20)->c == 15 && select(100)->c == 3);
    for (int invalid : {-1, 0, 7, 99, 101, 300}) assert(!select(invalid));
    for (auto text : {"20 MHz", "20.0 MHz", "20.000 MHz"}) assert(parse_mhz(text) == 20);
    for (auto text : {"", "0 MHz", "101 MHz", "20.1 MHz", "20MHz", "20 MHz junk", "-20 MHz",
                      "nan MHz", "inf MHz", "2e1 MHz", "999999999999999999999 MHz"}) assert(!parse_mhz(text));
}
