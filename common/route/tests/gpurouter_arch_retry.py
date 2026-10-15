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
        enum Kind { STRING, BOOLEAN, NULL_VALUE };
        std::string name, text;
        bool boolean = false;
        Kind kind = STRING;
        static Field string(const char *name, const char *value)
        {
            Field field;
            field.name = name;
            field.text = value;
            field.kind = STRING;
            return field;
        }
        static Field boolean_value(const char *name, bool value)
        {
            Field field;
            field.name = name;
            field.boolean = value;
            field.kind = BOOLEAN;
            return field;
        }
        static Field null_value(const char *name, const char *reason)
        {
            Field field;
            field.name = name;
            field.text = reason;
            field.kind = NULL_VALUE;
            return field;
        }
    };
    struct Event {
        std::string event, phase;
        int attempt = 0;
        std::vector<Field> fields;
    };
    std::vector<Event> events;
    void emit(const char *event, const char *phase, int attempt, std::initializer_list<Field> fields)
    {
        events.push_back(Event{event, phase, attempt, std::vector<Field>(fields)});
    }
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
void expect_arch_binding_run_end(const gpuroute::Telemetry &telemetry, const char *analogue_reason)
{
    assert(telemetry.events.size() == 1);
    const auto &event = telemetry.events[0];
    assert(event.event == "run_end");
    assert(event.phase.empty());
    assert(event.attempt == -1);
    const gpuroute::Telemetry::Field *status = nullptr;
    const gpuroute::Telemetry::Field *legal = nullptr;
    const gpuroute::Telemetry::Field *analogue = nullptr;
    for (const auto &field : event.fields) {
        if (field.name == "status")
            status = &field;
        else if (field.name == "routing_legal")
            legal = &field;
        else if (field.name == "analogue_timing_pass")
            analogue = &field;
    }
    assert(status && status->kind == gpuroute::Telemetry::Field::STRING &&
           status->text == "architecture_binding_failure");
    assert(legal && legal->kind == gpuroute::Telemetry::Field::BOOLEAN && !legal->boolean);
    assert(analogue && analogue->kind == gpuroute::Telemetry::Field::NULL_VALUE &&
           analogue->text == analogue_reason);
}
int main() {
    for (int limit : {0, 1, 8}) {
        Router r; r.cfg.arch_retry_limit = limit;
        try { r.run(); assert(false); } catch (const std::runtime_error &) {}
        assert(r.retries == limit);
        assert(error.find("rejected_net") != std::string::npos);
        assert(error.find(std::to_string(limit) + " re-routing attempts") != std::string::npos);
        expect_arch_binding_run_end(*r.telemetry, "not_available_for_architecture");
    }
    Router transient; transient.reject_passes = 2;
    assert(transient.run()); assert(transient.retries == 2); assert(transient.repairs == 2);
    assert(transient.telemetry->events.empty());
    Router clean; clean.reject_passes = 0;
    assert(clean.run()); assert(clean.retries == 0);
    assert(clean.telemetry->events.empty());
    Router global; global.cfg.max_iter = 5;
    try { global.run(); assert(false); } catch (const std::runtime_error &) {}
    assert(global.retries == 1);
    expect_arch_binding_run_end(*global.telemetry, "not_available_for_architecture");
    Router downstream; downstream.cfg.arch_retry_limit = 0;
    downstream.mistral_analogue_downstream = true;
    try { downstream.run(); assert(false); } catch (const std::runtime_error &) {}
    expect_arch_binding_run_end(*downstream.telemetry, "downstream_phase");
    Router silent; silent.telemetry = nullptr; silent.cfg.arch_retry_limit = 0;
    try { silent.run(); assert(false); } catch (const std::runtime_error &) {}
    assert(silent.retries == 0);
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
