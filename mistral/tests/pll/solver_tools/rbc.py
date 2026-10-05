#!/usr/bin/env python3
"""Batch front end for rbc_query.tcl (Quartus legality engine queries)."""
import os
import subprocess
import tempfile
from pathlib import Path

Q = os.path.join(os.environ.get("QUARTUS_BIN", "quartus/bin"), "quartus_sh")
TCL = Path(__file__).resolve().parent / "rbc_query.tcl"


def query(lines):
    with tempfile.TemporaryDirectory() as tmp:
        inp = Path(tmp) / "in.txt"
        out = Path(tmp) / "out.txt"
        inp.write_text("\n".join(lines) + "\n")
        subprocess.run([Q, "-t", str(TCL), str(inp), str(out)], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=True, cwd=tmp)
        res = out.read_text().splitlines()
    assert len(res) == len(lines), (len(res), len(lines))
    return res


def mhz(x):
    """Format a frequency in MHz the way the IP generator does (6 decimals max)."""
    s = ("%.6f" % x).rstrip("0")
    if s.endswith("."):
        s += "0"
    return s + " MHz"


def vco_list(ref, frac, outputs):
    """outputs: [(freq_str, phase_str, duty)] -> request line"""
    return "vco|%s|%s|%s" % (ref, "true" if frac else "false",
                             "|".join("%s;%s;%s" % o for o in outputs))


def parse_list(r):
    r = r.strip()
    if r.startswith("{") and r.endswith("}"):
        r = r[1:-1]
    if not r:
        return []
    return [x.strip() for x in r.split("|")]


def parse_mhz(s):
    s = s.strip()
    if s.endswith(" MHz"):
        return float(s[:-4])
    if s.endswith(" ps"):
        return 1e6 / float(s[:-3])
    raise ValueError(s)
