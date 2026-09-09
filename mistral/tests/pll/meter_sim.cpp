#include "Vpll_meter.h"
#include "verilated.h"
#include <cassert>
#include <iostream>

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    Vpll_meter dut;
    unsigned tick = 0;
    int output_half_period = 2; // ticks of 10 ns: 25 MHz
    auto step = [&]() {
        dut.refclk = tick & 1;
        dut.testclk = output_half_period ? (tick / output_half_period) & 1 : 0;
        dut.eval();
        ++tick;
    };
    dut.request = 0;
    dut.locked = 1;
    for (int i = 0; i < 1024; ++i)
        step();
    auto measure = [&](unsigned expected, bool lose_lock = false) {
        dut.request = !dut.request;
        bool saw_busy = false;
        for (int i = 0; i < 20000; ++i) {
            if (lose_lock && i == 2000)
                dut.locked = 0;
            step();
            saw_busy |= dut.busy;
            if (saw_busy && !dut.busy && dut.done == dut.request) {
                assert(dut.result >= (expected ? expected - 1 : 0) && dut.result <= expected + 1);
                assert(bool(dut.lost_lock) == lose_lock);
                unsigned held = dut.result;
                for (int j = 0; j < 1024; ++j)
                    step();
                assert(dut.result == held); // byte reads cannot race a new measurement
                return;
            }
        }
        assert(false && "measurement timeout");
    };
    measure(8); // 4096 reference cycles / (2 * 256)
    measure(8);
    output_half_period = 0;
    for (int i = 0; i < 1024; ++i)
        step();
    measure(0);
    output_half_period = 1; // wrong PLL frequency: 50 MHz
    measure(16);
    output_half_period = 2;
    measure(8, true);
    std::cout << "PASS: 25 MHz, repeated requests, stopped clock, wrong frequency, lock loss, stable snapshot\n";
}
