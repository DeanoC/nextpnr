#!/usr/bin/env python3
import subprocess
import sys


nextpnr = sys.argv[1]


def run(*args):
    return subprocess.run([nextpnr, *args], capture_output=True, text=True)


valid = run("--set", "custom/first=1", "--set", "custom/second=a=b", "--help")
if valid.returncode != 0:
    raise SystemExit(f"repeatable --set was rejected:\n{valid.stdout}{valid.stderr}")

for argument, expected in (("missing-equals", "--set expects key=value"), ("=empty-key", "--set key must not be empty")):
    malformed = run("--set", argument)
    output = malformed.stdout + malformed.stderr
    if malformed.returncode == 0 or expected not in output:
        raise SystemExit(f"malformed --set {argument!r} was not clearly rejected:\n{output}")

conflict = run("--gpu-device", "0", "--set", "gpurouter/device=1")
output = conflict.stdout + conflict.stderr
if conflict.returncode == 0 or "gpurouter/device" not in output or "dedicated command-line option" not in output:
    raise SystemExit(f"dedicated-option conflict was not clearly rejected:\n{output}")
