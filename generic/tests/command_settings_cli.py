#!/usr/bin/env python3
import subprocess
import sys


result = subprocess.run(
    [sys.argv[1], "--no-iobs", "--set", "disable_iobs=0"], capture_output=True, text=True
)
output = result.stdout + result.stderr
if result.returncode == 0 or "disable_iobs" not in output or "dedicated command-line option" not in output:
    raise SystemExit(f"generic dedicated-option conflict was not clearly rejected:\n{output}")
