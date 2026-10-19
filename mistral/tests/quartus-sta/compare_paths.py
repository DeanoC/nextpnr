#!/usr/bin/env python3
"""Time Quartus TimeQuest paths with nextpnr's Mistral model on the same wiring (#174).

compare_paths.py <paths-corner.tsv> <routes.tsv> <temp: 100|n40> <out-prefix>

paths-corner.tsv comes from paths.tcl, routes.tsv from route_delay on the
same Quartus RBF. Every IC hop whose source and destination are ALMs is
matched to the decoded route that ends at its destination port; every LUT
cell arc is given the per-pin delay of mistral/delay.cc. Writes
<prefix>-hops.tsv (one row per matched hop) and <prefix>-paths.tsv (LUT-only
paths whose every hop matched).
"""
import csv
import re
import statistics
import sys
from collections import defaultdict

# Arch::get_lut_pin_delay maxima (ps): current main, used since #213.
NEXTPNR_PIN = {"A": 605, "B": 583, "C": 512, "D": 512, "E": 335, "F": 97}
# Pre-#172 logical-position model, keyed by inputs counted from the last.
LOC = re.compile(r"(LABCELL|MLABCELL|FF)_X(\d+)_Y(\d+)_N(\d+)")
FF_REG = {1: "FFT0", 2: "FFT1", 4: "FFB0", 5: "FFB1"}


def alm_of(loc):
    m = LOC.fullmatch(loc or "")
    if not m:
        return None
    kind, x, y, n = m[1], int(m[2]), int(m[3]), int(m[4])
    return ("MLAB" if kind == "MLABCELL" else "LAB"), x, y, n // 6, n % 6, kind


def dst_candidates(loc, port):
    a = alm_of(loc)
    if a is None:
        return []
    bt, x, y, alm, sub, kind = a
    base = f"{bt}.{x:03d}.{y:03d}.{alm}:"
    if kind == "FF":
        if port == "asdata":
            return [base + p for p in ("E0", "E1", "F0", "F1")]
        return []
    half = 0 if sub < 3 else 1
    table = {"dataa": ["A"], "datab": ["B"], "datac": ["C", "D"], "datad": ["D", "C"],
             "datae": [f"E{half}"], "dataf": [f"F{half}"]}
    return [base + p for p in table.get(port, [])]


def src_alm(pnode):
    m = re.fullmatch(r"M?LAB\.(\d+)\.(\d+)\.(\d+):\w+", pnode)
    return (int(m[1]), int(m[2]), int(m[3])) if m else None


def hop_kind(chain):
    types = [n.split(".")[0] for n in chain.split(",")]
    return "+".join(t for t in types if t not in ("GIN", "GOUT"))


def main():
    paths_tsv, routes_tsv, temp, prefix = sys.argv[1:5]
    rcol, fcol = ("rise100", "fall100") if temp == "100" else ("risen40", "falln40")
    prcol, pfcol = "port_" + rcol, "port_" + fcol
    routes = {}
    for r in csv.DictReader(open(routes_tsv), delimiter="\t"):
        routes[r["dst"]] = r
    by_path = defaultdict(list)
    for r in csv.DictReader(open(paths_tsv), delimiter="\t"):
        by_path[r["path"]].append(r)

    hops_out = open(prefix + "-hops.tsv", "w")
    hops_out.write("path\tkind\tsrc_loc\tdst_loc\tdst_port\tpnode\tchain\tquartus_ps\tnextpnr_ps\tport_stage_ps\n")
    paths_out = open(prefix + "-paths.tsv", "w")
    paths_out.write("path\tto_clock\tslack\tluts\tquartus_lut_ps\tnextpnr_lut_ps\tquartus_ic_ps\tnextpnr_ic_ps"
                    "\tport_ic_ps\tquartus_ps\tnextpnr_ps\tport_ps\tpins\n")
    stats = defaultdict(list)
    for pid, points in by_path.items():
        arrival = [p for p in points if p["point"].startswith("arrival")]
        # Data part: after the launch register's output pin.
        start = next((i for i, p in enumerate(arrival)
                      if p["dst_type"] == "cyclonev_ff" and p["dst_port"] == "q"), None)
        if start is None:
            continue
        q_lut = n_lut = q_ic = n_ic = p_ic = 0.0
        luts, pins, clean = 0, [], True
        for p in arrival[start + 1:]:
            incr = float(p["incr"]) * 1000
            if p["type"] == "ic":
                if p["dst_type"] == "cyclonev_ff" and p["dst_port"] == "d":
                    if incr > 1:
                        clean = False
                    continue
                cands = dst_candidates(p["dst_loc"], p["dst_port"])
                sa = alm_of(p["src_loc"])
                match = None
                for c in cands:
                    r = routes.get(c)
                    if r and sa and src_alm(r["src"]) == sa[1:4]:
                        match = r
                        break
                if match is None or float(match[rcol]) < 0:
                    clean = False
                    continue
                n_ps = max(float(match[rcol]), float(match[fcol]))
                port_ps = max(float(match[prcol]), float(match[pfcol]))
                q_ic += incr
                n_ic += n_ps
                p_ic += port_ps
                kind = hop_kind(match["chain"])
                stats["ic:" + kind].append((incr, n_ps, port_ps))
                hops_out.write(f"{pid}\tic:{kind}\t{p['src_loc']}\t{p['dst_loc']}\t{p['dst_port']}\t{match['dst']}"
                               f"\t{match['chain']}\t{incr:.0f}\t{n_ps:.0f}\t{port_ps:.0f}\n")
            elif p["type"] == "cell" and p["src_type"] == "cyclonev_lcell_comb":
                if p["dst_port"] != "combout":
                    clean = False  # carry/arith arc: not in the LUT pin model
                    continue
                pin = p["src_port"][-1].upper()
                n_ps = NEXTPNR_PIN[pin]
                q_lut += incr
                n_lut += n_ps
                luts += 1
                pins.append(pin)
                stats["lut:" + pin].append((incr, n_ps, n_ps))
                hops_out.write(f"{pid}\tlut:{pin}\t{p['src_loc']}\t{p['dst_loc']}\t{p['src_port']}\t\t\t{incr:.0f}"
                               f"\t{n_ps}\t{n_ps}\n")
            elif p["type"] == "cell" and p["src_type"] not in ("cyclonev_ff", ""):
                clean = False
        if clean and luts:
            paths_out.write(f"{pid}\t{points[0]['to_clock']}\t{points[0]['slack']}\t{luts}\t{q_lut:.0f}\t{n_lut:.0f}"
                            f"\t{q_ic:.0f}\t{n_ic:.0f}\t{p_ic:.0f}\t{q_lut + q_ic:.0f}\t{n_lut + n_ic:.0f}"
                            f"\t{n_lut + p_ic:.0f}\t{''.join(pins)}\n")
    hops_out.close()
    paths_out.close()
    print(f"{'arc':<28}{'n':>6}{'quartus':>9}{'nextpnr':>9}{'delta':>8}{'ratio':>7}{'+port':>8}{'delta':>8}")
    for key in sorted(stats):
        q = [a for a, _, _ in stats[key]]
        n = [b for _, b, _ in stats[key]]
        w = [c for _, _, c in stats[key]]
        qm, nm, wm = statistics.mean(q), statistics.mean(n), statistics.mean(w)
        print(f"{key:<28}{len(q):>6}{qm:>9.1f}{nm:>9.1f}{nm - qm:>+8.1f}{nm / qm if qm else 0:>7.3f}{wm:>8.1f}"
              f"{wm - qm:>+8.1f}")


if __name__ == "__main__":
    main()
