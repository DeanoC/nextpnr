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


with tempfile.TemporaryDirectory(prefix="nextpnr-json-cli-settings-") as temporary:
    checkpoint = Path(temporary) / "checkpoint.json"
    written = Path(temporary) / "written.json"
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
                    "gpurouter/histCongWeight": "9",
                },
            }
        }
    }), encoding="utf-8")
    imported = run("--no-iobs", "--json", str(checkpoint), "--no-pack", "--no-place", "--no-route",
                   "--gpu-cpu", "--gpu-opt", "maxIter=600", "--gpu-perf", "--gpu-device", "4",
                   "--write", str(written))
    output = imported.stdout + imported.stderr
    if imported.returncode != 0:
        raise SystemExit(f"checkpoint import with explicit GPU options failed:\n{output}")
    settings = json.loads(written.read_text(encoding="utf-8"))["modules"]["top"]["settings"]
    cpu = settings.get("gpurouter/cpu")
    perf = settings.get("gpurouter/perfProfile")
    if as_int(cpu) in (0, "0", "0 ") or as_int(perf) in (0, "0", "0 "):
        raise SystemExit(f"scaffold settings overrode --gpu-cpu/--gpu-perf: cpu={cpu!r} perf={perf!r}")
    if settings.get("gpurouter/maxIter") != "600":
        raise SystemExit(f"scaffold settings overrode --gpu-opt maxIter: {settings.get('gpurouter/maxIter')!r}")
    if as_int(settings.get("gpurouter/device")) != 4:
        raise SystemExit(f"scaffold settings overrode --gpu-device: {settings.get('gpurouter/device')!r}")
    if settings.get("gpurouter/histCongWeight") != "9":
        raise SystemExit(f"unspecified scaffold setting was dropped: {settings.get('gpurouter/histCongWeight')!r}")
