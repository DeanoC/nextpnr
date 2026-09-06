#include "pll.h"
#include <cassert>
#include <cmath>
int main() {
    using namespace mistral_pll;
    auto c = select_fractional(12288000, 50);
    assert(c && c->m == 8 && c->n == 1 && c->c == 33);
    assert(c->fractional && c->fraction == 472790000);
    assert(std::abs(achieved_hz(*c, 50) - 12288000.000019869) < 0.000001);
    assert(!select_fractional(12288001, 50));
    assert(!select_fractional(12288000, 25));
    assert(!select_hz(12288000, 50));
}
