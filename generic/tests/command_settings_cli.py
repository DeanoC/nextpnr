#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
from pathlib import Path


nextpnr = sys.argv[1]


def run(*args):
    return subprocess.run([nextpnr, *args], capture_output=True, text=True)


result = run("--no-iobs", "--set", "disable_iobs=0")
output = result.stdout + result.stderr
if result.returncode == 0 or "disable_iobs" not in output or "dedicated command-line option" not in output:
    raise SystemExit(f"generic dedicated-option conflict was not clearly rejected:\n{output}")


def as_int(value):
    # Numeric settings are written MSB-first. A trailing space marks a string.
    if isinstance(value, str) and value and set(value) <= {"0", "1"}:
        return int(value, 2)
    return value


def require(condition, message, settings):
    if not condition:
        raise SystemExit(message + "\nsettings: " + json.dumps(settings, sort_keys=True))


with tempfile.TemporaryDirectory(prefix="nextpnr-json-cli-settings-") as temporary:
    temporary = Path(temporary)
    checkpoint = temporary / "checkpoint.json"
    written = temporary / "written.json"
    checkpoint.write_text(json.dumps({
        "modules": {
            "top": {
                "attributes": {"top": "1"},
                "ports": {},
                "cells": {},
                "netnames": {},
                "settings": {
                    "gpurouter/cpu": "0 ",
                    "gpurouter/maxIter": "2000",
                    "gpurouter/perfProfile": "0",
                    "gpurouter/device": "0",
                    "gpurouter/maxBatches": "1",
                    "gpurouter/histCongWeight": "9",
                    "disable_iobs": "0",
                    "custom/note": "from-json",
                },
            }
        }
    }), encoding="utf-8")
    imported = run("--json", str(checkpoint), "--no-pack", "--no-place", "--no-route",
                   "--no-iobs", "--gpu-cpu", "--gpu-opt", "maxIter=600", "--gpu-perf",
                   "--gpu-device", "4", "--gpu-batches", "3", "--set", "custom/note=from-cli",
                   "--write", str(written))
    output = imported.stdout + imported.stderr
    if imported.returncode != 0:
        raise SystemExit(f"checkpoint import with explicit options failed:\n{output}")
    settings = json.loads(written.read_text(encoding="utf-8"))["modules"]["top"]["settings"]
    cpu = settings.get("gpurouter/cpu")
    perf = settings.get("gpurouter/perfProfile")
    require(as_int(cpu) not in (0, "0", "0 "), f"scaffold settings overrode --gpu-cpu: cpu={cpu!r}", settings)
    require(as_int(perf) not in (0, "0", "0 "), f"scaffold settings overrode --gpu-perf: perf={perf!r}", settings)
    require(settings.get("gpurouter/maxIter") == "600",
            f"scaffold settings overrode --gpu-opt maxIter: {settings.get('gpurouter/maxIter')!r}", settings)
    require(as_int(settings.get("gpurouter/device")) == 4,
            f"scaffold settings overrode --gpu-device: {settings.get('gpurouter/device')!r}", settings)
    require(as_int(settings.get("gpurouter/maxBatches")) == 3,
            f"scaffold settings overrode --gpu-batches: {settings.get('gpurouter/maxBatches')!r}", settings)
    require(as_int(settings.get("disable_iobs")) == 1,
            f"scaffold settings overrode --no-iobs: disable_iobs={settings.get('disable_iobs')!r}", settings)
    require(settings.get("gpurouter/histCongWeight") == "9",
            f"unspecified scaffold setting was dropped: {settings.get('gpurouter/histCongWeight')!r}", settings)
    require(settings.get("custom/note") == "from-cli",
            f"--set lost to the scaffold or the replay: {settings.get('custom/note')!r}", settings)

    # Options that were not passed leave the checkpoint value in place.
    kept = temporary / "kept.json"
    untouched = run("--json", str(checkpoint), "--no-pack", "--no-place", "--no-route", "--write", str(kept))
    output = untouched.stdout + untouched.stderr
    if untouched.returncode != 0:
        raise SystemExit(f"checkpoint import without overriding options failed:\n{output}")
    kept_settings = json.loads(kept.read_text(encoding="utf-8"))["modules"]["top"]["settings"]
    require(kept_settings.get("gpurouter/cpu") == "0 ",
            f"saved gpurouter/cpu changed without --gpu-cpu: {kept_settings.get('gpurouter/cpu')!r}", kept_settings)
    require(kept_settings.get("gpurouter/maxIter") == "2000",
            f"saved maxIter changed without --gpu-opt: {kept_settings.get('gpurouter/maxIter')!r}", kept_settings)
    require(as_int(kept_settings.get("disable_iobs")) == 0,
            f"saved disable_iobs changed without --no-iobs: {kept_settings.get('disable_iobs')!r}", kept_settings)
    require(kept_settings.get("gpurouter/histCongWeight") == "9",
            f"unspecified scaffold setting was dropped: {kept_settings.get('gpurouter/histCongWeight')!r}",
            kept_settings)
    # The GPU device belongs to the invocation, not to the checkpoint.
    require("gpurouter/device" not in kept_settings,
            f"checkpoint reload retained stale gpurouter/device: {kept_settings.get('gpurouter/device')!r}",
            kept_settings)

    # That clear runs before packing and placement, where a new IdString can
    # change placement, so it must not intern gpurouter/device in a run that
    # never names it. jsonwrite numbers disconnected cell bits after the last
    # interned IdString, so naming the key only in the saved settings of an
    # otherwise identical design must add exactly one.
    def disconnected_bit(name, settings):
        design = temporary / f"{name}.json"
        rewritten = temporary / f"{name}-written.json"
        design.write_text(json.dumps({
            "modules": {
                "top": {
                    "attributes": {"top": "1"},
                    "ports": {},
                    "cells": {"probe": {"type": "PROBE", "port_directions": {"A": "input"},
                                        "connections": {"A": ["x", "x"]}}},
                    "netnames": {},
                    "settings": settings,
                }
            }
        }), encoding="utf-8")
        result = run("--json", str(design), "--no-pack", "--no-place", "--no-route", "--write", str(rewritten))
        if result.returncode != 0:
            raise SystemExit(f"{name} design import failed:\n{result.stdout}{result.stderr}")
        return json.loads(rewritten.read_text(encoding="utf-8"))["modules"]["top"]["cells"]["probe"][
            "connections"]["A"][0]

    fresh = disconnected_bit("fresh", {})
    saved = disconnected_bit("saved-device", {"gpurouter/device": "1"})
    if saved != fresh + 1:
        raise SystemExit("a run that never names gpurouter/device interned it: first disconnected bit "
                         f"{fresh} without the key, {saved} with it only in the saved settings")
