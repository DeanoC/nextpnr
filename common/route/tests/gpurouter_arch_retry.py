#!/usr/bin/env python3
"""Exercise the production architecture-binding retry loop with a rejecting Arch.

No device/database needed: compile the actual loop against a small fake context.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
source = (ROOT / 'common/route/gpurouter.cc').read_text()
start = source.index('        int arch_retries = 0;')
end = source.index('        const auto &st = backend->stats();', start)
loop = source[start:end]
harness = r'''
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>
namespace gpuroute {
struct Telemetry {
    struct Field {
        static Field string(const char *, const char *) { return {}; }
        static Field boolean_value(const char *, bool) { return {}; }
        static Field null_value(const char *, const char *) { return {}; }
    };
    void emit(const char *, const char *, int, std::initializer_list<Field>) {}
};
}
std::string error;
void log_info(const char *, ...) {}
void log_warning(const char *, ...) {}
void log_error(const char *fmt, ...) {
    char buf[4096]; va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    error = buf; throw std::runtime_error(error);
}
struct Context { const char *nameOf(int) { return "rejected_net"; } };
struct Router {
    struct Config {
        int max_iter = 2000, arch_retry_limit = 8, repair_rounds = 1;
        void check_stop() {}
    } cfg;
    Context context, *ctx = &context;
    gpuroute::Telemetry writer, *telemetry = &writer;
    bool timing_driven = true, mistral_analogue_downstream = false;
    int iter = 5, overused_wires = 0, retries = 0, repairs = 0;
    int reject_passes = 10000;
    std::vector<int> failed_nets, route_queue, nets_by_udata{0};
    void bind_and_check_all() {
        failed_nets.clear();
        if (retries < reject_passes) failed_nets.push_back(0);
    }
    bool negotiate() {
        ++retries; ++iter;
        // Reproduce the bug: clean negotiation forgets the Arch rejection.
        failed_nets.clear(); route_queue.clear(); return true;
    }
    void timing_repair() { ++repairs; }
    bool run() {
''' + loop + r'''
        return true;
    }
};
int main() {
    for (int limit : {0, 1, 8}) {
        Router r; r.cfg.arch_retry_limit = limit;
        try { r.run(); assert(false); } catch (const std::runtime_error &) {}
        assert(r.retries == limit);
        assert(error.find("rejected_net") != std::string::npos);
        assert(error.find(std::to_string(limit) + " re-routing attempts") != std::string::npos);
    }
    Router transient; transient.reject_passes = 2;
    assert(transient.run()); assert(transient.retries == 2); assert(transient.repairs == 2);
    Router clean; clean.reject_passes = 0;
    assert(clean.run()); assert(clean.retries == 0);
    Router global; global.cfg.max_iter = 5;
    try { global.run(); assert(false); } catch (const std::runtime_error &) {}
    assert(global.retries == 1);
}
'''
with tempfile.TemporaryDirectory(prefix='gpurouter-arch-retry-') as tmp:
    cpp = Path(tmp) / 'test.cc'
    exe = Path(tmp) / 'test'
    cpp.write_text(harness)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra',
                    '-Werror', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('GPU router architecture-binding retry regression passed')
