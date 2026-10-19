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


def extract_function_body(source: str, marker: str) -> str:
    start = source.find(marker)
    if start < 0:
        raise AssertionError(marker + " not found")
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
    raise AssertionError("unbalanced function body: " + marker)


def assert_gui_copies_settings():
    window = (ROOT / "gui/basewindow.cc").read_text()
    start = window.find("void BaseMainWindow::open_json()")
    end = window.find("void BaseMainWindow::save_json()")
    if start < 0 or end < start:
        raise AssertionError("open_json not found")
    body = window[start:end]
    replace = body.find("new Context(chipArgs)")
    copy = body.find("copyDedicatedCommandSettings")
    load = body.find("handler->load_json")
    if replace < 0 or copy < 0 or load < 0 or not (replace < copy < load):
        raise AssertionError(
            "open_json must copy dedicated command settings onto the replacement context before load_json")


LOAD_JSON_HARNESS = r'''
#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
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
    // Conflicting architecture options, plus a key this invocation did not request.
    ctx->settings["disable_iobs"] = Property(std::string("0"));
    ctx->settings["compress_rbf"] = Property(std::string("0"));
    ctx->settings["gpurouter/histCongWeight"] = Property(std::string("9"));
    return true;
}

// Same key rule as CommandHandler::dedicated_command_setting_keys. The harness
// only models the options these cases pass.
std::set<std::string> dedicated_command_setting_keys(const variables_map &vm)
{
    std::set<std::string> keys;
    if (vm.count("router2-profile"))
        keys.insert("router2/profilePath");
    if (vm.count("gpu-telemetry"))
        keys.insert("gpurouter/telemetryPath");
    if (vm.count("gpu-telemetry") && vm.count("seed"))
        keys.insert("gpurouter/telemetrySeed");
    return keys;
}

void log_error(const char *, ...) { throw std::runtime_error("log_error"); }

class CommandHandler
{
  public:
    variables_map vm;
    std::string telemetry_seed;
    void setupArchContext(Context *) {}
    std::map<std::string, Property> dedicated_setting_snapshot;
    void setupContext(Context *ctx)
    {
        if (vm.count("router2-profile"))
            ctx->settings[ctx->id("router2/profilePath")] = vm["router2-profile"].as<std::string>();
        if (vm.count("gpu-telemetry"))
            ctx->settings[ctx->id("gpurouter/telemetryPath")] = vm["gpu-telemetry"].as<std::string>();
        // Architecture settings such as disable_iobs are written by createContext,
        // not here. A replacement context does not get them unless they are copied.
    }
    // Same architecture keys as GenericCommandHandler / MistralCommandHandler.
    std::set<std::string> getArchSettingKeys() const
    {
        std::set<std::string> keys;
        if (vm.count("no-iobs"))
            keys.insert("disable_iobs");
        if (vm.count("compress-rbf"))
            keys.insert("compress_rbf");
        return keys;
    }
    void snapshotDedicatedCommandSettings(Context *ctx)
    {
        dedicated_setting_snapshot.clear();
        auto keys = dedicated_command_setting_keys(vm);
        const auto arch_keys = getArchSettingKeys();
        keys.insert(arch_keys.begin(), arch_keys.end());
        for (const auto &key : keys) {
            auto found = ctx->settings.find(ctx->id(key));
            if (found != ctx->settings.end())
                dedicated_setting_snapshot[found->first] = found->second;
        }
    }
    void copyDedicatedCommandSettings(const Context *from, Context *to) const;
    void applyEarlySettingOverrides(Context *) {}
    void applySettingOverrides(Context *ctx)
    {
        for (const auto &saved : dedicated_setting_snapshot)
            ctx->settings[saved.first] = saved.second;
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

void CommandHandler::copyDedicatedCommandSettings(const Context *from, Context *to) const
{
COPY_BODY
}

void CommandHandler::load_json(Context *ctx, std::string filename)
{
LOAD_BODY
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
    // No architecture option on this case: the checkpoint's values remain,
    // and a key with no command-line option is left untouched.
    assert(get(&ctx, "disable_iobs") == "0");
    assert(get(&ctx, "compress_rbf") == "0");
    assert(get(&ctx, "gpurouter/histCongWeight") == "9");
    std::cout << "load_json " << name << " ok" << std::endl;
    return 0;
}

int run_arch_case(const char *name)
{
    CommandHandler handler;
    handler.vm.data["no-iobs"].value = "1";
    Context ctx;
    // Same context createContext already wrote. load_json does not rebuild it.
    ctx.settings["disable_iobs"] = Property(std::string("1"));
    handler.load_json(&ctx, "imported.json");
    assert(get(&ctx, "disable_iobs") == "1");
    assert(get(&ctx, "compress_rbf") == "0");
    assert(get(&ctx, "gpurouter/histCongWeight") == "9");
    assert(!has(&ctx, "router2/profilePath"));
    assert(!has(&ctx, "gpurouter/telemetryPath"));
    assert(!has(&ctx, "gpurouter/telemetrySeed"));
    std::cout << "load_json " << name << " ok" << std::endl;
    return 0;
}

// GUI open_json builds a blank context, copies explicit settings onto it, then
// load_json. createContext is not run again, and setupContext does not rewrite
// architecture options.
int run_gui_replacement_case(const char *name, bool requested)
{
    CommandHandler handler;
    if (requested) {
        handler.vm.data["no-iobs"].value = "1";
        handler.vm.data["compress-rbf"].value = "1";
    }
    Context original;
    original.settings["disable_iobs"] = Property(std::string("1"));
    original.settings["compress_rbf"] = Property(std::string("1"));
    original.settings["custom/leftover"] = Property(std::string("not-explicit"));
    Context replacement;
    handler.copyDedicatedCommandSettings(&original, &replacement);
    handler.load_json(&replacement, "imported.json");
    if (requested) {
        assert(get(&replacement, "disable_iobs") == "1");
        assert(get(&replacement, "compress_rbf") == "1");
    } else {
        assert(get(&replacement, "disable_iobs") == "0");
        assert(get(&replacement, "compress_rbf") == "0");
    }
    assert(!has(&replacement, "custom/leftover"));
    assert(get(&replacement, "gpurouter/histCongWeight") == "9");
    std::cout << "load_json " << name << " ok" << std::endl;
    return 0;
}

} // namespace nextpnr

int main()
{
    nextpnr::run_case("cli-profile", true, false);
    nextpnr::run_case("no-option", false, false);
    nextpnr::run_case("cli-profile-gpu", true, true);
    nextpnr::run_arch_case("arch-no-iobs");
    nextpnr::run_gui_replacement_case("gui-replace-arch", true);
    nextpnr::run_gui_replacement_case("gui-replace-unrequested", false);
    return 0;
}
'''


def write_load_json_harness(path: Path, load_body: str, copy_body: str):
    text = LOAD_JSON_HARNESS.replace("LOAD_BODY", load_body).replace("COPY_BODY", copy_body)
    if "LOAD_BODY" in text or "COPY_BODY" in text:
        raise AssertionError("harness placeholders were not replaced")
    path.write_text(text)


def snapshot_sources():
    return [TESTS / "router2_profile_snapshot_repair.cpp", ROOT / "3rdparty/json11/json11.cpp"]


def run_snapshot(binary: Path, tmp: Path, win32: bool):
    tmp.mkdir(parents=True, exist_ok=True)
    dest = tmp / ("win.json" if win32 else "posix.json")
    run([str(binary), str(dest), "replace"])
    dest_hb = tmp / ("win-hb.json" if win32 else "posix-hb.json")
    run([str(binary), str(dest_hb), "replace", "heartbeat"])
    dest_utf8 = tmp / ("caf\u00e9-" + ("win" if win32 else "posix") + ".json")
    run([str(binary), str(dest_utf8), "utf8"])
    dest_n = tmp / ("win-n.json" if win32 else "posix-n.json")
    run([str(binary), str(dest_n), "neighbor"])
    dest_s = tmp / ("win-s.json" if win32 else "posix-s.json")
    victim = tmp / ("win-victim" if win32 else "posix-victim")
    run([str(binary), str(dest_s), "symlink", str(victim)])
    dest_a = tmp / ("win-a.json" if win32 else "posix-a.json")
    dest_b = tmp / ("win-b.json" if win32 else "posix-b.json")
    run([str(binary), str(dest_a), "concurrent", str(dest_b)])


def main():
    assert_gui_copies_settings()
    command = (ROOT / "common/kernel/command.cc").read_text()
    load_body = extract_function_body(command, "void CommandHandler::load_json(Context *ctx, std::string filename)")
    copy_body = extract_function_body(
        command, "void CommandHandler::copyDedicatedCommandSettings(const Context *from, Context *to) const")
    with tempfile.TemporaryDirectory(prefix="router2-profile-repair-") as raw:
        tmp = Path(raw)
        posix = tmp / "snapshot-posix"
        win32 = tmp / "snapshot-win32"
        load_src = tmp / "load_json_extracted.cc"
        load_bin = tmp / "load_json_extracted"
        write_load_json_harness(load_src, load_body, copy_body)
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
