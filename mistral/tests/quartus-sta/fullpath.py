#!/usr/bin/env python3
"""Full-path Fmax of a nextpnr critical path, with its data part re-timed by Quartus (#174).

fullpath.py <routed.json> <timing.json> <clock> <replica dir> <nextpnr routes.tsv>

The replica (replica.py) covers launch Q -> last LUT output. For each slow
corner this keeps nextpnr's clock skew, clock-to-Q and setup and swaps in:
the replica's TimeQuest data delay; the side branches the replica drops
(Mistral delay of the original route minus the replica's, at that corner);
and the final hop as Mistral times it at that corner, including the sink's
block-input stage (nextpnr's arc already includes it for FF sinks, not for
an M10K port). It also costs the same path's LUT arcs with the pre-#172
logical-position timer.
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


COLUMNS = {"slow_1100mv_100c": ("rise100", "fall100"), "slow_1100mv_-40c": ("risen40", "falln40")}


def delay(row, corner, port=False):
    rise, fall = COLUMNS[corner]
    prefix = "port_" if port else ""
    return max(float(row[prefix + rise]), float(row[prefix + fall]))


def tile(dst):
    m = re.match(r"\w+\.(\d+)\.(\d+)[.:]", dst)
    return (int(m[1]), int(m[2])) if m else None


def main():
    routed, timing, clock, rep_dir, orig_tsv = sys.argv[1:6]
    cells = next(iter(json.load(open(routed))["modules"].values()))["cells"]
    p = next(p for p in json.load(open(timing))["critical_paths"]
             if p["to"] == f"posedge {clock}" and clock in p["from"])
    path = p["path"]
    half = p["from"].startswith("negedge") != p["to"].startswith("negedge")
    rep = json.load(open(os.path.join(rep_dir, "replica.json")))
    replica = {r["dst"]: r for r in csv.DictReader(open(os.path.join(rep_dir, "routes.tsv")), delimiter="\t")}
    orig = [r for r in csv.DictReader(open(orig_tsv), delimiter="\t") if float(r["rise100"]) >= 0]
    by_dst = {r["dst"]: r for r in orig}
    total = sum(s["delay"] for s in path) * 1000
    data_np = sum(l["route_ps"] + l["cell_ps"] for l in rep["luts"])
    dsts = [f"{'MLAB' if l['kind'] == 'MISTRAL_MCOMB' else 'LAB'}.{l['x']:03d}.{l['y']:03d}.{l['alm']}:{l['phys']}"
            for l in rep["luts"]]
    # The final hop's route: in the sink's tile, with the delay nextpnr charged
    # either without or with the block-input stage. It must be unique.
    last = path[-2]
    assert last["type"] == "routing"
    last_ps = last["delay"] * 1000
    finals = [r for r in orig if tile(r["dst"]) == tuple(last["to"]["loc"]) and
              min(abs(delay(r, "slow_1100mv_100c") - last_ps),
                  abs(delay(r, "slow_1100mv_100c", True) - last_ps)) < 1.5]
    if len(finals) != 1:
        sys.exit(f"final hop into {last['to']} matches {len(finals)} routes")
    final = finals[0]
    old = 0
    logic = [s for s in path if s["type"] == "logic" and s["to"]["port"] == "Q"]
    for s in logic:
        w = WIDTH.get(cells[s["from"]["cell"]]["type"], 1)
        old += OLD_FROM_LAST[w - 1 - "ABCDEF".index(s["from"]["port"])]
    new_lut = sum(s["delay"] * 1000 for s in logic)
    scale = 2 if half else 1
    mhz = lambda ps: 1e6 / (scale * ps)
    out = {"report_ps": round(total), "report_mhz": round(mhz(total), 2), "luts": len(logic),
           "old_timer_same_path_mhz": round(mhz(total - new_lut + old), 2), "final_sink": final["dst"]}
    for f in sorted(glob.glob(os.path.join(rep_dir, "paths-7_slow*.tsv"))):
        corner = os.path.basename(f)[8:-4]
        rows = list(csv.DictReader(open(f), delimiter="\t"))
        first = next(r["path"] for r in rows if r["src"].startswith("q0"))
        pts = [r for r in rows if r["path"] == first and r["point"].startswith("arrival")]
        q = sum(float(r["incr"]) * 1000 for r in pts
                if (r["type"] == "ic" and r["dst_type"] == "cyclonev_lcell_comb") or
                (r["type"] == "cell" and r["src_type"] == "cyclonev_lcell_comb"))
        # Dropped branches also change the waveform at the terminal input
        # mux. Include that stage on both sides of the loading correction.
        side = sum(delay(by_dst[d], corner, True) - delay(replica[d], corner, True) for d in dsts)
        final_ps = delay(final, corner, True)
        full = total - data_np - last_ps + q + side + final_ps
        out[corner] = {"data_nextpnr_ps": round(data_np), "data_quartus_ps": round(q), "side_branch_ps": round(side),
                       "final_hop_nextpnr_ps": round(last_ps), "final_hop_ps": round(final_ps),
                       "full_ps": round(full), "mhz": round(mhz(full), 2)}
    print(json.dumps(out))


if __name__ == "__main__":
    main()
