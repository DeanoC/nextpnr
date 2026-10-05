#!/usr/bin/env python3
"""Compile many oracle specs with at most three concurrent Quartus runs.

Usage: batch.py OUTROOT specs.json   (specs.json = {"case": spec, ...})
Cases whose result.json already exists are skipped.
"""
import json
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pllgen  # noqa: E402


def one(root, name, spec):
    out = root / name
    if (out / "result.json").exists():
        return name, "cached"
    pllgen.generate(spec, out)
    res = pllgen.compile_project(out)
    return name, "rc=%s plls=%d" % (res["quartus_rc"], len(res.get("pll_report", [])))


def main():
    root = Path(sys.argv[1]).resolve()
    specs = json.loads(Path(sys.argv[2]).read_text())
    jobs = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    root.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=min(jobs, 3)) as pool:
        futures = [pool.submit(one, root, name, spec) for name, spec in specs.items()]
        for f in futures:
            name, status = f.result()
            print(name, status, flush=True)


if __name__ == "__main__":
    main()
