#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
from pathlib import Path


nextpnr = sys.argv[1]


def run(*args):
    return subprocess.run([nextpnr, *args], capture_output=True, text=True)


valid = run("--set", "custom/first=1", "--set", "custom/second=a=b", "--set", "seed=+1", "--help")
if valid.returncode != 0:
    raise SystemExit(f"repeatable --set was rejected:\n{valid.stdout}{valid.stderr}")

for argument, expected in (("missing-equals", "--set expects key=value"), ("=empty-key", "--set key must not be empty")):
    malformed = run("--set", argument)
    output = malformed.stdout + malformed.stderr
    if malformed.returncode == 0 or expected not in output:
        raise SystemExit(f"malformed --set {argument!r} was not clearly rejected:\n{output}")

for value in ("not-a-number", "-1"):
    bad_seed = run("--set", f"seed={value}")
    output = bad_seed.stdout + bad_seed.stderr
    if bad_seed.returncode == 0 or "--set seed expects an unsigned integer" not in output:
        raise SystemExit(f"malformed --set seed={value!r} was not clearly rejected:\n{output}")

conflict = run("--gpu-device", "0", "--set", "gpurouter/device=1")
output = conflict.stdout + conflict.stderr
if conflict.returncode == 0 or "gpurouter/device" not in output or "dedicated command-line option" not in output:
    raise SystemExit(f"dedicated-option conflict was not clearly rejected:\n{output}")

required_conflict = run("--gpu-required", "--set", "gpurouter/required=false")
output = required_conflict.stdout + required_conflict.stderr
if required_conflict.returncode == 0 or "gpurouter/required" not in output or "dedicated command-line option" not in output:
    raise SystemExit(f"--gpu-required setting conflict was not clearly rejected:\n{output}")

for value in ("false", "0"):
    gpu_opt_conflict = run("--gpu-required", "--gpu-opt", f"required={value}", "--device", "5CSEBA6U23I7", "--test")
    output = gpu_opt_conflict.stdout + gpu_opt_conflict.stderr
    if gpu_opt_conflict.returncode == 0 or "--gpu-required conflicts with --gpu-opt required=false" not in output:
        raise SystemExit(f"contradictory --gpu-opt required={value} was not clearly rejected:\n{output}")

with tempfile.TemporaryDirectory(prefix="nextpnr-gpu-required-") as temporary:
    checkpoint = Path(temporary) / "checkpoint.json"
    written = Path(temporary) / "written.json"
    checkpoint.write_text(json.dumps({
        "modules": {
            "top": {
                "attributes": {"top": "1"},
                "ports": {},
                "cells": {},
                "netnames": {},
                "settings": {"gpurouter/required": "0"},
            }
        }
    }), encoding="utf-8")
    imported = run("--device", "5CSEBA6U23I7", "--json", str(checkpoint), "--gpu-required", "--no-pack",
                   "--no-place", "--no-route", "--write", str(written))
    output = imported.stdout + imported.stderr
    if imported.returncode != 0:
        raise SystemExit(f"checkpoint import with --gpu-required failed:\n{output}")
    settings = json.loads(written.read_text(encoding="utf-8"))["modules"]["top"]["settings"]
    required = settings.get("gpurouter/required")
    if not isinstance(required, str) or set(required) - {"0", "1"} or "1" not in required:
        raise SystemExit(f"checkpoint settings overrode --gpu-required: {required!r}")
