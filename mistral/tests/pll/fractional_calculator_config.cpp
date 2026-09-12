#include "pll.h"

#include <cassert>
#include <cmath>

int main()
{
    using namespace mistral_pll;

    // A rate-specific case must not be needed for a new 50 MHz-reference rate.
    auto video = select_fractional(27000000, 50);
    assert(video);
    assert(video->fractional);
    assert(video->m == 8 && video->n == 1 && video->c == 15);
    assert(video->fraction == 0x1999999a);
    assert(std::abs(achieved_hz(*video, 50) - 27000000.00031044) < 0.000001);

    // The calculator must move M when the first legal C value crosses an
    // integer feedback multiplier.
    auto high = select_fractional(99000000, 50);
    assert(high);
    assert(high->m == 9 && high->n == 1 && high->c == 5);
    assert(high->fraction == 0xe6666666);
    assert(std::abs(achieved_hz(*high, 50) - 98999999.99906868) < 0.000001);

    // The legal output range includes both counter-window endpoints.
    auto low = select_fractional(1000000, 50);
    assert(low && low->m == 8 && low->c == 400 && low->fraction == 0);
    auto top = select_fractional(100000000, 50);
    assert(top && top->m == 8 && top->c == 4 && top->fraction == 0);

    // Two outputs may use one calculated VCO when their exact ratio has a
    // common counter pair in the bounded VCO window.
    auto dual = select_fractional_dual(27000000, 13500000, 50);
    assert(dual);
    assert(dual->feedback.m == 8 && dual->feedback.n == 1);
    assert(dual->feedback.c == 15 && dual->c1 == 30);
    assert(dual->feedback.fraction == 0x1999999a);

    assert(!select_fractional(27000000, 25));
    assert(!select_fractional(100000001, 50));
    assert(!select_fractional_dual(27000000, 13000000, 50));
}
