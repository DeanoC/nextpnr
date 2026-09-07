#include "pll.h"
#include <cassert>
#include <string>

int main()
{
    using namespace mistral_pll;
    // Values independently checked in Quartus-generated FPLL settings.
    const int presets25[] = {1, 4, 7, 10};
    const int presets50[] = {1, 2, 4, 5};
    const int mux50[] = {0, 4, 0, 4};
    for (int quarter = 0; quarter < 4; ++quarter) {
        auto slow = select_phase(std::to_string(quarter * 10000) + " ps", 25000000);
        assert(slow && slow->shift_ns == quarter * 10);
        assert(slow->c_preset == presets25[quarter] && slow->c_phase_preset == 0);
        auto fast = select_phase(std::to_string(quarter * 5000) + " ps", 50000000);
        assert(fast && fast->shift_ns == quarter * 5);
        assert(fast->c_preset == presets50[quarter] && fast->c_phase_preset == mux50[quarter]);
    }
    for (int64_t hz : {25000000, 50000000}) {
        for (const char *invalid : {"-5000 ps", "100 ps", "40000 ps", "5 ns", "junk"})
            assert(!select_phase(invalid, hz));
    }
    assert(!select_phase("20000 ps", 50000000));
    assert(!select_phase("5000 ps", 25000000));
    for (int64_t hz : {16000000, 40000000, 100000000}) {
        auto aligned = select_phase("0 ps", hz);
        assert(aligned && aligned->shift_ns == 0 && aligned->c_preset == 1 && aligned->c_phase_preset == 0);
        assert(!select_phase("10000 ps", hz));
    }
}
