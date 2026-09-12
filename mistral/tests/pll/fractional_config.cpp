#include "pll.h"
#include <cassert>
#include <cmath>
int main() {
    using namespace mistral_pll;
    auto video = select_fractional(74250000, 50);
    assert(video && video->m == 8 && video->n == 1 && video->c == 6);
    assert(video->fractional && video->fraction == 0xe8f5c239);
    assert(std::abs(achieved_hz(*video, 50) - 74249999.83243954) < 0.000001);
    assert(!select_hz(74250000, 50));
    assert(!select_fractional(74250000, 25));
    assert(!select_fractional(74250000, 100));
    auto video_near = select_fractional(74250001, 50);
    assert(video_near && video_near->c == 6 && video_near->m == 8);
    assert(std::abs(achieved_hz(*video_near, 50) - 74250001.0) < 0.001);
    auto equal = select_fractional_dual(74250000, 74250000, 50);
    assert(equal && equal->feedback.c == equal->c1);
    auto dual = select_fractional_dual(12288000, 24576000, 50);
    assert(dual && dual->feedback.c == 34 && dual->c1 == 17);
    assert(dual->feedback.fraction == 0x5b18548b);
    auto swapped = select_fractional_dual(24576000, 12288000, 50);
    assert(swapped && swapped->feedback.c == 17 && swapped->c1 == 34);
    assert(swapped->feedback.fraction == 0x5b18548b);
    assert(!select_fractional_dual(12288000, 24576000, 25));
    assert(!select_fractional_dual(12288000, 24576001, 50));
    auto audio441 = select_fractional(11289600, 50);
    assert(audio441 && audio441->m == 8 && audio441->n == 1 && audio441->c == 36);
    assert(audio441->fraction == 0x20e6293f && audio441->fractional);
    assert(std::abs(achieved_hz(*audio441, 50) - 11289599.972143251) < 0.000001);
    assert(!select_fractional(11289600, 25));
    assert(!select_hz(11289600, 50));
    auto audio441_near = select_fractional(11289601, 50);
    assert(audio441_near && audio441_near->c == 36);
    auto c = select_fractional(12288000, 50);
    assert(c && c->m == 8 && c->n == 1 && c->c == 33);
    assert(c->fractional && c->fraction == 472790000);
    assert(std::abs(achieved_hz(*c, 50) - 12288000.000019869) < 0.000001);
    auto audio48_near = select_fractional(12288001, 50);
    assert(audio48_near && audio48_near->c == 33);
    assert(!select_fractional(12288000, 25));
    assert(!select_hz(12288000, 50));
}
