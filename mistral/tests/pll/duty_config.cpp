#include "pll.h"
#include <cassert>

int main()
{
    using namespace mistral_pll;
    auto quarter = duty_counts(12, 25);
    assert(quarter && quarter->high == 3 && quarter->low == 9 && !quarter->odd);
    auto three_quarters = duty_counts(12, 75);
    assert(three_quarters && three_quarters->high == 9 && three_quarters->low == 3 && !three_quarters->odd);
    auto odd = duty_counts(3, 50);
    assert(odd && odd->high == 2 && odd->low == 1 && odd->odd);
    assert(!duty_counts(3, 25));
    assert(!duty_counts(12, 0));
    assert(!duty_counts(12, 100));
    assert(!duty_counts(512, 50));
    assert(!duty_counts(400, 75));
    auto single = select_hz(25000000, 50, 25);
    assert(single && single->c == 12);
    assert(!select_hz(100000000, 50, 25));
    auto pair = select_dual_hz(50000000, 25000000, 50, 25, 25);
    assert(pair && pair->feedback.m == 16 && pair->feedback.c == 8 && pair->c1 == 16);
}
