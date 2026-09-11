#include "pll.h"
#include <cassert>

int main()
{
    using namespace mistral_pll;
    const int expected_m[] = {104, 52, 52};
    const int expected_n[] = {5, 5, 10};
    const int expected_bw[] = {2, 4, 4};
    const int expected_cp[] = {2, 2, 1};
    const int expected_low[] = {11, 6, 1};
    const int expected_phase[] = {3, 2, 0};
    const int profile_refs[] = {25, 50, 100};
    for (int i = 0; i < 3; ++i) {
        auto profile = select(52, profile_refs[i]);
        assert(profile && profile->m == expected_m[i] && profile->n == expected_n[i]);
        assert(profile->bandwidth == expected_bw[i] && profile->charge_pump == expected_cp[i]);
        assert(profile->m_low_preset == expected_low[i] && profile->m_phase_preset == expected_phase[i]);
        assert(profile->c == 10);
    }
    for (int ref : {25, 50, 100}) {
        for (int a = 0; a <= 101; ++a) {
            auto single = select(a, ref);
            assert(bool(single) == (a > 0 && a <= 100 && (300 % a == 0 || 320 % a == 0 || 520 % a == 0)));
            if (single) assert(ref * single->m == a * single->n * single->c);
            for (int b = 0; b <= 101; ++b) {
                int vco = 0;
                if (a > 0 && a <= 100 && b > 0 && b <= 100)
                    for (int candidate : {300, 320, 400, 520})
                        if (!vco && candidate % a == 0 && candidate % b == 0 && candidate / a <= 512 && candidate / b <= 512)
                            vco = candidate;
                auto pair = select_dual(a, b, ref);
                assert(bool(pair) == bool(vco));
                if (pair) {
                    assert(ref * pair->feedback.m == a * pair->feedback.n * pair->feedback.c);
                    assert(ref * pair->feedback.m == b * pair->feedback.n * pair->c1);
                    assert(ref * pair->feedback.m == vco * pair->feedback.n);
                }
            }
        }
    }
    const int refs[] = {25, 50, 100};
    const int outputs[] = {50, 64, 40};
    const int m[][3] = {{24, 64, 32}, {12, 32, 16}, {6, 32, 8}};
    const int n[][3] = {{2, 5, 2}, {2, 5, 2}, {2, 10, 2}};
    const int bw[][3] = {{6, 3, 6}, {7, 6, 7}, {8, 6, 7}};
    for (int r = 0; r < 3; ++r)
        for (int v = 0; v < 3; ++v) {
            auto result = select_dual(v == 1 ? 40 : 25, outputs[v], refs[r]);
            const auto &c = result->feedback;
            assert(c.m == m[r][v] && c.n == n[r][v] && c.bandwidth == bw[r][v]);
            assert(c.charge_pump == ((v == 1 && r != 2) ? 2 : 1));
            assert(c.m_low_preset == (v == 1 && r == 0 ? 7 : v == 1 && r == 1 ? 4 : 1));
            assert(c.m_phase_preset == (v == 1 && r == 0 ? 3 : v == 1 && r == 1 ? 2 : 0));
        }
    for (int r = 0; r < 3; ++r) {
        const std::array<int64_t, 4> triple = {25000000, 50000000, 100000000, 0};
        const std::array<int64_t, 4> quad = {40000000, 80000000, 16000000, 20000000};
        for (int v = 0; v < 3; ++v) {
            const auto hz = v == 1 ? quad : triple;
            const std::array<int, 4> duties = v == 0 ? std::array<int, 4>{50, 50, 50, 50}
                : v == 1 ? std::array<int, 4>{25, 75, 25, 75} : std::array<int, 4>{25, 50, 25, 50};
            const int count = v == 1 ? 4 : 3;
            auto result = select_multi_hz(hz, count, refs[r], duties);
            assert(result);
            const auto &c = result->feedback;
            assert(c.m == m[r][v] && c.n == n[r][v] && c.bandwidth == bw[r][v]);
            assert(c.charge_pump == ((v == 1 && r != 2) ? 2 : 1));
            assert(c.m_low_preset == (v == 1 && r == 0 ? 7 : v == 1 && r == 1 ? 4 : 1));
            assert(c.m_phase_preset == (v == 1 && r == 0 ? 3 : v == 1 && r == 1 ? 2 : 0));
            for (int i = 0; i < count; ++i) {
                assert(int64_t(refs[r]) * 1000000 * c.m == hz[i] * c.n * result->counters[i]);
                assert(duty_counts(result->counters[i], duties[i]));
            }
            assert(c.c == result->counters[0]);
        }
    }
    for (int ref : {-1, 0, 24, 26, 49, 51, 99, 101}) {
        assert(!select_multi_hz({25000000, 50000000, 100000000, 0}, 3, ref));
        assert(!select_multi_hz({40000000, 80000000, 16000000, 20000000}, 4, ref));
        assert(!select(25, ref));
        assert(!select_dual(25, 40, ref));
    }
}
