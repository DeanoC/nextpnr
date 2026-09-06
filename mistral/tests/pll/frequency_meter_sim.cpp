#include "Vpll_meter.h"
#include "verilated.h"
#include <cassert>
#include <cstdint>
#include <iostream>

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    // Half periods in 0.5 ns ticks: reference50 MHz, outputs20/40/100 MHz.
    for (unsigned mhz : {20U, 40U, 100U}) {
        Vpll_meter dut;
        uint64_t tick = 0;
        bool stopped = false;
        auto step = [&]() {
            dut.refclk = (tick / 20) & 1;
            dut.testclk = stopped ? 0 : (tick / (1000 / mhz)) & 1;
            dut.eval();
            ++tick;
        };
        dut.request = 0;
        dut.locked = 1;
        for (int i = 0; i < 100000; ++i) step();
        auto measure = [&](unsigned rate, bool lost) {
            dut.request = !dut.request;
            bool busy = false, complete = false;
            for (unsigned i = 0; i < 200000; ++i) {
                step();
                busy |= dut.busy;
                if (busy && !dut.busy && dut.done == dut.request) {
                    // WINDOW_BITS=12: expected count = rate *4096 /(50*256).
                    unsigned low = rate * 4096 / (50 * 256);
                    unsigned high = (rate * 4096 + 50 * 256 - 1) / (50 * 256);
                    assert(dut.result >= (low ? low - 1 : 0) && dut.result <= high + 1);
                    if (rate == 0) assert(dut.result == 0);
                    assert(bool(dut.lost_lock) == lost);
                    unsigned snapshot = dut.result;
                    for (int j = 0; j < 1000; ++j) step();
                    assert(dut.result == snapshot);
                    complete = true;
                    break;
                }
            }
            assert(complete);
        };
        for (int trial = 0; trial < 3; ++trial) {
            measure(mhz, false);
            stopped = true; dut.locked = 0;
            for (int i = 0; i < 10000; ++i) step();
            measure(0, true);
            stopped = false; dut.locked = 1;
            for (int i = 0; i < 100000; ++i) step();
        }
        std::cout << "PASS: " << mhz << " MHz meter, stopped clock, repeated restart\n";
    }
}
