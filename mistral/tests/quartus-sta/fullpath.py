#!/usr/bin/env python3
"""Full-path Fmax of a nextpnr critical path, with its data part re-timed by Quartus (#174).

fullpath.py <routed.json> <timing.json> <clock> <replica dir> <nextpnr routes.tsv>

The replica (replica.py) covers launch Q -> last LUT output. This keeps
nextpnr's clock skew, clock-to-Q, final hop and setup, swaps the data part
for the replica's TimeQuest total at each slow corner, adds back the side
branches the replica drops (Mistral delay of the original route minus the
replica route) and, when the final hop ends on a GOUT block port (M10K),
the block-input stage nextpnr's arc omits. It also costs the same path's
LUT arcs with the pre-#172 logical-position timer.
"""
import csv
import glob
import json
import os
import re
import sys

# Pre-#172 (d869795b^) logical-position delays, max of each quad, by inputs from the last.
OLD_FROM_LAST = [97, 400, 512, 510, 583, 605]
WIDTH = {"MISTRAL_ALUT6": 6, "MISTRAL_ALUT5": 5, "MISTRAL_ALUT4": 4, "MISTRAL_ALUT3": 3, "MISTRAL_ALUT2": 2}


def main():
    routed, timing, clock, rep_dir, orig_tsv = sys.argv[1:6]
    cells = next(iter(json.load(open(routed))["modules"].values()))["cells"]
    p = next(p for p in json.load(open(timing))["critical_paths"]
             if p["to"] == f"posedge {clock}" and clock in p["from"])
    path = p["path"]
    half = p["from"].startswith("negedge") != p["to"].startswith("negedge")
    rep = json.load(open(os.path.join(rep_dir, "replica.json")))
    replica = {r["dst"]: r for r in csv.DictReader(open(os.path.join(rep_dir, "routes.tsv")), delimiter="\t")}
    orig = list(csv.DictReader(open(orig_tsv), delimiter="\t"))
    by_dst = {r["dst"]: r for r in orig}
    total = sum(s["delay"] for s in path) * 1000
    data_np = sum(l["route_ps"] + l["cell_ps"] for l in rep["luts"])
    side = 0.0
    for l in rep["luts"]:
        dst = f"{'MLAB' if l['kind'] == 'MISTRAL_MCOMB' else 'LAB'}.{l['x']:03d}.{l['y']:03d}.{l['alm']}:{l['phys']}"
        r = replica[dst]
        side += l["route_ps"] - max(float(r["rise100"]), float(r["fall100"]))
    last = path[-2]
    assert last["type"] == "routing"
    x, y = last["to"]["loc"]
    stage = 0.0
    for r in orig:
        m = re.match(r"\w+\.(\d+)\.(\d+)[.:]", r["dst"])
        if m and (int(m[1]), int(m[2])) == (x, y) and \
                abs(max(float(r["rise100"]), float(r["fall100"])) - last["delay"] * 1000) < 1.5 and \
                not r["dst"].startswith(("LAB", "MLAB")):
            stage = max(float(r["port_rise100"]), float(r["port_fall100"])) - last["delay"] * 1000
            break
    old = 0
    logic = [s for s in path if s["type"] == "logic" and s["to"]["port"] == "Q"]
    for s in logic:
        w = WIDTH.get(cells[s["from"]["cell"]]["type"], 1)
        old += OLD_FROM_LAST[w - 1 - "ABCDEF".index(s["from"]["port"])]
    new_lut = sum(s["delay"] * 1000 for s in logic)
    scale = 2 if half else 1
    mhz = lambda ps: 1e6 / (scale * ps)
    out = {"report_ps": round(total), "report_mhz": round(mhz(total), 2), "luts": len(logic),
           "old_timer_same_path_mhz": round(mhz(total - new_lut + old), 2), "side_branch_ps": round(side),
           "final_stage_ps": round(stage)}
    for f in sorted(glob.glob(os.path.join(rep_dir, "paths-7_slow*.tsv"))):
        rows = list(csv.DictReader(open(f), delimiter="\t"))
        first = next(r["path"] for r in rows if r["src"].startswith("q0"))
        pts = [r for r in rows if r["path"] == first and r["point"].startswith("arrival")]
        q = sum(float(r["incr"]) * 1000 for r in pts
                if (r["type"] == "ic" and r["dst_type"] == "cyclonev_lcell_comb") or
                (r["type"] == "cell" and r["src_type"] == "cyclonev_lcell_comb"))
        full = total - data_np + q + side + stage
        out[os.path.basename(f)[8:-4]] = {"data_nextpnr_ps": round(data_np), "data_quartus_ps": round(q),
                                          "full_ps": round(full), "mhz": round(mhz(full), 2)}
    print(json.dumps(out))


if __name__ == "__main__":
    main()
