#!/usr/bin/env python3
"""Per-pin LUT delay table: nextpnr's model against Quartus 17.0.2 cell arcs (#174).

per_pin.py > per-pin.tsv

Quartus values are the worst rr/rf/fr/ff transition of each physical input
in the two ALMs retained by mistral/tests/lut6-select (decoded to physical
pins by its check.py), for every operating corner of 5CSEBA6U23I7.
"""
import csv
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "lut6-select"
# Arch::get_lut_pin_delay maxima (main since #213), and #172 as merged (d869795b).
MAIN = {"A": 605, "B": 583, "C": 512, "D": 512, "E": 335, "F": 97}
D869795B = {"A": "605 (L6) / 583 (L5)", "B": "583 (L6) / 510 (L5)", "C": "512", "D": "512",
            "E": "97 (L6) / 400 (L5)", "F": "400 (L6) / 97 (L5)"}
CORNERS = ["arcs-7_slow_1100mv_100c", "arcs-7_slow_1100mv_-40c", "arcs-MIN_fast_1100mv_100c",
           "arcs-MIN_fast_1100mv_-40c"]


def main():
    per = {}
    for half in ("asymmetric", "asymmetric-bottom"):
        out = subprocess.run([sys.executable, "-I", str(ROOT / "check.py"), str(ROOT / half)], capture_output=True,
                             text=True, check=True).stdout
        for corner, pins in json.loads(out)["corner_max_delays_ns"].items():
            for pin, ns in pins.items():
                per.setdefault((corner, pin.rstrip("01")), []).append(round(ns * 1000))
    rows = []
    for p in "ABCDEF":
        s100, sn40 = per[(CORNERS[0], p)], per[(CORNERS[1], p)]
        rows.append({"pin": p, "nextpnr_main_ps": MAIN[p], "nextpnr_d869795b_ps": D869795B[p],
                     "quartus_slow100_top_ps": s100[0], "quartus_slow100_bottom_ps": s100[1],
                     "delta_vs_slow100_ps": MAIN[p] - max(s100),
                     "delta_vs_slow100_pct": round(100 * (MAIN[p] - max(s100)) / max(s100), 1),
                     "quartus_slown40_top_ps": sn40[0], "quartus_slown40_bottom_ps": sn40[1],
                     "delta_vs_slown40_ps": MAIN[p] - max(sn40),
                     "delta_vs_slown40_pct": round(100 * (MAIN[p] - max(sn40)) / max(sn40), 1),
                     "quartus_fast100_ps": max(per[(CORNERS[2], p)]), "quartus_fastn40_ps": max(per[(CORNERS[3], p)])})
    w = csv.DictWriter(sys.stdout, fieldnames=list(rows[0]), delimiter="\t", lineterminator="\n")
    w.writeheader()
    w.writerows(rows)


if __name__ == "__main__":
    main()
