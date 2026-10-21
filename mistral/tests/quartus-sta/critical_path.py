#!/usr/bin/env python3
"""Decompose nextpnr's worst path and re-cost it with Quartus arcs (#174).

critical_path.py <timing.json> <routes.tsv> <clock> [--old]

routes.tsv is route_delay's output for the RBF nextpnr wrote with that
timing report. Each routing segment is matched to the decoded route that
ends in the sink's LAB with the same analogue delay, which checks that the
route tool reproduces the timer. Each LUT arc's physical pin follows from
the decoded destination port. The report's cell charge may also include
the terminal input mux; its total delay no longer identifies the LUT pin.
--old retains the reported cell charges for a routing-only re-cost.

The Quartus re-cost replaces every LUT arc with the Quartus 17.0.2 cell arc
of its physical pin (mistral/tests/lut6-select, worst transition) and adds
the block-input stage (GOUT -> ALM pin) that nextpnr's route delay stops
before. Everything else (clock-to-Q, setup, skew) stays nextpnr's.
"""
import csv
import json
import re
import sys

# Physical pin -> (nextpnr delay.cc max, Quartus slow 100 C, Quartus slow -40 C) in ps.
# Quartus: worst rr/rf/fr/ff over the two retained lut6-select ALMs.
PINS = {
    "F": (97, 90, 103),
    "E": (335, 335, 353),
    "C/D": (512, 504, 599),
    "B": (583, 579, 679),
    "A": (605, 598, 701),
}
PINS_BY_PORT = {"F0": "F", "F1": "F", "E0": "E", "E1": "E", "C": "C/D", "D": "C/D", "B": "B", "A": "A"}


def main():
    timing, routes_tsv, clock = sys.argv[1:4]
    old = "--old" in sys.argv
    d = json.load(open(timing))
    paths = [p for p in d["critical_paths"] if p["to"] == f"posedge {clock}" and clock in p["from"]]
    path = paths[0]["path"]
    routes = list(csv.DictReader(open(routes_tsv), delimiter="\t"))
    by_lab = {}
    for r in routes:
        m = re.fullmatch(r"\w+\.(\d+)\.(\d+)[.:].*", r["dst"])
        if m and float(r["rise100"]) >= 0:
            by_lab.setdefault((int(m[1]), int(m[2])), []).append(r)
    total = sum(s["delay"] for s in path) * 1000
    rows, unmatched = [], 0
    sums = {"np": 0.0, "q100": 0.0, "qn40": 0.0}
    input_pin = None
    for s in path:
        ps = s["delay"] * 1000
        kind, q100, qn40, note = s["type"], ps, ps, ""
        if s["type"] == "routing":
            input_pin = None
            x, y = s["to"]["loc"]
            lab = by_lab.get((x, y), [])
            # A LUT pin wire is the GOUT rnode itself, so nextpnr's arc stops
            # before the block-input stage. FF data/control wires are
            # nextpnr-created and their arc already includes it.
            cands = [(r, False) for r in lab if abs(max(float(r["rise100"]), float(r["fall100"])) - ps) < 1.5]
            cands += [(r, True) for r in lab if abs(max(float(r["port_rise100"]), float(r["port_fall100"])) - ps) < 1.5]
            if cands:
                r, included = cands[0]
                input_pin = PINS_BY_PORT.get(r["dst"].rsplit(":", 1)[-1])
                q100 = max(float(r["port_rise100"]), float(r["port_fall100"]))
                qn40 = max(float(r["port_risen40"]), float(r["port_falln40"]))
                note = r["chain"].replace("GIN.", "").split(",")
                note = "+".join(n.split(".")[0] for n in note[1:-1]) or "direct"
                note += " (stage incl.)" if included else ""
            else:
                unmatched += 1
                note = "unmatched"
        elif s["type"] == "logic" and s["to"]["port"] == "Q" and not old:
            pin = input_pin
            if pin:
                q100, qn40 = PINS[pin][1], PINS[pin][2]
                kind, note = f"lut:{pin}", s["from"]["port"]
        sums["np"] += ps
        sums["q100"] += q100
        sums["qn40"] += qn40
        rows.append((kind, ps, q100, qn40, note))
    for kind, ps, q100, qn40, note in rows:
        print(f"{kind:<10}{ps:>8.0f}{q100:>8.0f}{qn40:>8.0f}  {note}")
    half = paths[0]["from"].startswith("negedge") != paths[0]["to"].startswith("negedge")
    scale = 2 if half else 1
    print(f"total ps: nextpnr {sums['np']:.0f} (report {total:.0f}); Quartus-arc re-cost 100C {sums['q100']:.0f}, "
          f"-40C {sums['qn40']:.0f}; unmatched routing segments {unmatched}")
    print(f"MHz{' (half-cycle path)' if half else ''}: nextpnr {1e6 / (scale * sums['np']):.2f}, "
          f"re-cost 100C {1e6 / (scale * sums['q100']):.2f}, -40C {1e6 / (scale * sums['qn40']):.2f}")


if __name__ == "__main__":
    main()
