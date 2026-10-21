#!/usr/bin/env python3
"""Compare a replica's TimeQuest path with nextpnr's report, hop by hop (#174).

replica_compare.py <replica dir>

Reads replica.json (replica.py), the replica's routes.tsv (route_delay on
the replica RBF) and paths-<corner>.tsv (paths.tcl), and prints one row per
routing hop and LUT arc of the q0 -> qc path for every Quartus corner.
The nextpnr cell charge comes from its report, including the terminal
input stage; Quartus charges that stage to interconnect instead.
"""
import csv
import glob
import json
import os
import sys

def main():
    rep_dir = sys.argv[1]
    rep = json.load(open(os.path.join(rep_dir, "replica.json")))
    replica = {r["dst"]: r for r in csv.DictReader(open(os.path.join(rep_dir, "routes.tsv")), delimiter="\t")}
    corners = {}
    for f in sorted(glob.glob(os.path.join(rep_dir, "paths-*.tsv"))):
        rows = [r for r in csv.DictReader(open(f), delimiter="\t")]
        first = next(r["path"] for r in rows if r["src"] == "q0" or r["src"].startswith("q0|"))
        pts = [r for r in rows if r["path"] == first and r["point"].startswith("arrival")]
        ic = [float(p["incr"]) * 1000 for p in pts if p["type"] == "ic" and p["dst_type"] == "cyclonev_lcell_comb"]
        cell = [float(p["incr"]) * 1000 for p in pts if p["type"] == "cell" and p["src_type"] == "cyclonev_lcell_comb"]
        corners[os.path.basename(f)[6:-4]] = (ic, cell)
    names = list(corners)
    print("hop  pin  nextpnr_route  replica_route(+stage)  " + "  ".join(f"Q_ic[{n}]" for n in names)
          + "  nextpnr_lut  " + "  ".join(f"Q_lut[{n}]" for n in names))
    tot = {"np_r": 0, "rep_r": 0, "rep_s": 0, "np_l": 0}
    qtot = {n: [0, 0] for n in names}
    for k, l in enumerate(rep["luts"]):
        dst = f"{'MLAB' if l['kind'] == 'MISTRAL_MCOMB' else 'LAB'}.{l['x']:03d}.{l['y']:03d}.{l['alm']}:{l['phys']}"
        r = replica[dst]
        np_r = l["route_ps"]
        rep_r = max(float(r["rise100"]), float(r["fall100"]))
        rep_s = max(float(r["port_rise100"]), float(r["port_fall100"]))
        np_l = l["cell_ps"]
        tot["np_r"] += np_r
        tot["rep_r"] += rep_r
        tot["rep_s"] += rep_s
        tot["np_l"] += np_l
        for n in names:
            ic, cell = corners[n]
            qtot[n][0] += ic[k]
            qtot[n][1] += cell[k]
        print(f"{l['atom']:<4} {l['phys']:<3} {np_r:>8.0f} {rep_r:>8.0f} ({rep_s:.0f}) "
              + " ".join(f"{corners[n][0][k]:>8.0f}" for n in names) + f" {np_l:>8.0f} "
              + " ".join(f"{corners[n][1][k]:>8.0f}" for n in names))
    print(f"sum routing: nextpnr {tot['np_r']:.0f}, replica mistral {tot['rep_r']:.0f} (+stage {tot['rep_s']:.0f}); "
          + ", ".join(f"Quartus {n} {qtot[n][0]:.0f}" for n in names))
    print(f"sum LUT: nextpnr {tot['np_l']:.0f}; " + ", ".join(f"Quartus {n} {qtot[n][1]:.0f}" for n in names))
    print(f"sum data (Q -> last LUT out): nextpnr {tot['np_r'] + tot['np_l']:.0f}; "
          + ", ".join(f"Quartus {n} {sum(qtot[n]):.0f}" for n in names))


if __name__ == "__main__":
    main()
