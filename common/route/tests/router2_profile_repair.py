#!/usr/bin/env python3
"""Compile and run real router2 snapshot + extracted load_json regressions."""
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TESTS = Path(__file__).resolve().parent
CXX = os.environ.get("CXX", "/usr/bin/g++")


def run(argv, **kwargs):
    print("RUN", argv, flush=True)
    return subprocess.run(argv, check=True, **kwargs)


def compile_cpp(sources, output, extra_args=None):
    argv = [CXX, "-std=c++17", "-pthread", "-O0", "-g",
            "-I" + str(ROOT / "common/route"),
            "-I" + str(ROOT / "common/route/tests"),
            "-I" + str(ROOT / "3rdparty/json11")]
    if extra_args:
        argv.extend(extra_args)
    argv.extend(str(s) for s in sources)
    argv.extend(["-o", str(output)])
    run(argv)


def extract_load_json_body(source: str) -> str:
    marker = "void CommandHandler::load_json(Context *ctx, std::string filename)"
    start = source.find(marker)
    if start < 0:
        raise AssertionError("CommandHandler::load_json not found")
    brace = source.find("{", start)
    depth = 0
    i = brace
    while i < len(source):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:i]
        i += 1
    raise AssertionError("unbalanced load_json body")


LOAD_JSON_HARNESS = r'''
#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace nextpnr {

using IdString = std::string;

struct Property {
    std::string str;
    bool is_string = true;
    Property() = default;
    Property(const std::string &value) : str(value), is_string(true) {}
    const std::string &as_string() const { return str; }
};

struct Context {
    std::map<std::string, Property> settings;
    IdString id(const std::string &s) const { return s; }
    IdString id(const char *s) const { return s; }
};

struct OptionValue {
    std::string value;
    template <typename T> T as() const { return T(value); }
};

struct variables_map {
    std::map<std::string, OptionValue> data;
    size_t count(const char *key) const { return data.count(key); }
    const OptionValue &operator[](const char *key) const { return data.at(key); }
};

std::ifstream open_ifstream_and_log_error(std::string, const char *) { return std::ifstream(); }

bool parse_json(std::istream &, const std::string &filename, Context *ctx)
{
    (void)filename;
    ctx->settings["router2/profilePath"] = Property(std::string("stale-checkpoint.json"));
    ctx->settings["gpurouter/telemetryPath"] = Property(std::string("stale-gpu.json"));
    ctx->settings["gpurouter/telemetrySeed"] = Property(std::string("stale-seed"));
    return true;
}

void log_error(const char *, ...) { throw std::runtime_error("log_error"); }

class CommandHandler
{
  public:
    variables_map vm;
    std::string telemetry_seed;
    void setupArchContext(Context *) {}
    void setupContext(Context *ctx)
    {
        if (vm.count("router2-profile"))
            ctx->settings[ctx->id("router2/profilePath")] = vm["router2-profile"].as<std::string>();
        if (vm.count("gpu-telemetry"))
            ctx->settings[ctx->id("gpurouter/telemetryPath")] = vm["gpu-telemetry"].as<std::string>();
    }
    void restoreTelemetrySettings(Context *ctx)
    {
        auto cpu_path_key = ctx->id("router2/profilePath");
        ctx->settings.erase(cpu_path_key);
        if (vm.count("router2-profile"))
            ctx->settings[cpu_path_key] = vm["router2-profile"].as<std::string>();
        auto path_key = ctx->id("gpurouter/telemetryPath");
        auto seed_key = ctx->id("gpurouter/telemetrySeed");
        if (!vm.count("gpu-telemetry")) {
            ctx->settings.erase(path_key);
            ctx->settings.erase(seed_key);
            return;
        }
        ctx->settings[path_key] = vm["gpu-telemetry"].as<std::string>();
        if (telemetry_seed.empty())
            ctx->settings.erase(seed_key);
        else
            ctx->settings[seed_key] = telemetry_seed;
    }
    void load_json(Context *ctx, std::string filename);
};

void CommandHandler::load_json(Context *ctx, std::string filename)
{
BODY
}

std::string get(Context *ctx, const std::string &key)
{
    auto it = ctx->settings.find(key);
    return it == ctx->settings.end() ? std::string() : it->second.as_string();
}

bool has(Context *ctx, const std::string &key) { return ctx->settings.count(key) != 0; }

int run_case(const char *name, bool cli_profile, bool cli_gpu)
{
    CommandHandler handler;
    if (cli_profile)
        handler.vm.data["router2-profile"].value = "invocation-profile.json";
    if (cli_gpu) {
        handler.vm.data["gpu-telemetry"].value = "invocation-gpu.json";
        handler.telemetry_seed = "seed-7";
    }
    Context ctx;
    handler.load_json(&ctx, "imported.json");
    if (cli_profile) {
        assert(has(&ctx, "router2/profilePath"));
        assert(get(&ctx, "router2/profilePath") == "invocation-profile.json");
    } else {
        assert(!has(&ctx, "router2/profilePath"));
    }
    if (cli_gpu) {
        assert(get(&ctx, "gpurouter/telemetryPath") == "invocation-gpu.json");
        assert(get(&ctx, "gpurouter/telemetrySeed") == "seed-7");
    } else {
        assert(!has(&ctx, "gpurouter/telemetryPath"));
        assert(!has(&ctx, "gpurouter/telemetrySeed"));
    }
    std::cout << "load_json " << name << " ok" << std::endl;
    return 0;
}

} // namespace nextpnr

int main()
{
    nextpnr::run_case("cli-profile", true, false);
    nextpnr::run_case("no-option", false, false);
    nextpnr::run_case("cli-profile-gpu", true, true);
    return 0;
}
'''


def write_load_json_harness(path: Path, body: str):
    path.write_text(LOAD_JSON_HARNESS.replace("BODY", body))


def snapshot_sources():
    return [TESTS / "router2_profile_snapshot_repair.cpp", ROOT / "3rdparty/json11/json11.cpp"]


def run_snapshot(binary: Path, tmp: Path, win32: bool):
    tmp.mkdir(parents=True, exist_ok=True)
    dest = tmp / ("win.json" if win32 else "posix.json")
    run([str(binary), str(dest), "replace"])
    dest_hb = tmp / ("win-hb.json" if win32 else "posix-hb.json")
    run([str(binary), str(dest_hb), "replace", "heartbeat"])
    dest_n = tmp / ("win-n.json" if win32 else "posix-n.json")
    run([str(binary), str(dest_n), "neighbor"])
    dest_s = tmp / ("win-s.json" if win32 else "posix-s.json")
    victim = tmp / ("win-victim" if win32 else "posix-victim")
    run([str(binary), str(dest_s), "symlink", str(victim)])
    dest_a = tmp / ("win-a.json" if win32 else "posix-a.json")
    dest_b = tmp / ("win-b.json" if win32 else "posix-b.json")
    run([str(binary), str(dest_a), "concurrent", str(dest_b)])


def main():
    command = (ROOT / "common/kernel/command.cc").read_text()
    body = extract_load_json_body(command)
    with tempfile.TemporaryDirectory(prefix="router2-profile-repair-") as raw:
        tmp = Path(raw)
        posix = tmp / "snapshot-posix"
        win32 = tmp / "snapshot-win32"
        load_src = tmp / "load_json_extracted.cc"
        load_bin = tmp / "load_json_extracted"
        write_load_json_harness(load_src, body)
        compile_cpp(snapshot_sources(), posix)
        compile_cpp(snapshot_sources(), win32, extra_args=[
            "-D_WIN32", "-DROUTER2_PROFILE_WIN32_SHIM",
            "-include", str(TESTS / "router2_profile_win32_shim.h")])
        compile_cpp([load_src], load_bin)
        run_snapshot(posix, tmp / "posix", win32=False)
        run_snapshot(win32, tmp / "win", win32=True)
        run([str(load_bin)])
    print("Router2 repair regressions passed (Win32 via injected shim, not Windows-native)", flush=True)


if __name__ == "__main__":
    main()
