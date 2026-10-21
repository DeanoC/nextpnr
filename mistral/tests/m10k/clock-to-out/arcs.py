#!/usr/bin/env python3
"""Extract M10K cell arcs from a dump.tcl table (#232).

arcs.py <char-<corner>.tsv> <out.json>

Read paths (RAM -> so_* register): the clock pin -> internal register cell,
utco and the register -> dataout cell. Their sum is the clock-to-output from the
RAM's clock pin. Write paths (si_* register -> RAM): the data pin -> internal
register cell against the clock pin -> register cell of the required side;
setup = pin + utsu - clock, hold = clock + uth - pin. A read path that ends in
the M10K itself means the fitter packed the capture register into the M10K's
output register; it is counted and dropped (project.py's LUTs prevent that).
"""
import collections
import csv
import json
import re
import sys

csv.field_size_limit(1 << 30)


def cfg_of(name):
    m = re.search(r"ram_([A-Za-z0-9]+)\|", name)
    return m[1] if m else None


def ram_clock(points):
    return next((k for k, p in enumerate(points) if p["dst_type"] == "cyclonev_ram_block" and
                 re.fullmatch(r"clk[01]", p["dst_port"])), None)


def main():
    rows = collections.defaultdict(list)
    with open(sys.argv[1]) as f:
        for r in csv.DictReader(f, delimiter="\t"):
            rows[(r["analysis"], r["path"])].append(r)
    reads, writes = [], []
    packed = collections.Counter()
    for (analysis, _), pts in rows.items():
        kind, side = analysis.split("-")
        arr = [p for p in pts if p["point"].startswith("arrival")]
        req = [p for p in pts if p["point"].startswith("required")]
        if side == "read":
            i = ram_clock(arr)
            if i is None:
                continue
            j = next((k for k in range(i + 1, len(arr)) if re.match(r"port[ab]dataout", arr[k]["dst_port"])), None)
            if j is None:
                packed["%s/%s" % (kind, cfg_of(arr[i]["dst"]))] += 1
                continue
            seg = arr[i + 1:j + 1]
            assert [p["type"] for p in seg] == ["cell", "utco", "cell"], [p["type"] for p in seg]
            clk_cell, utco, out_cell = (float(p["incr"]) for p in seg)
            reads.append(dict(kind=kind, cfg=cfg_of(arr[i]["dst"]), clk_pin=arr[i]["dst_port"],
                              out=arr[j]["dst_port"], reg=(seg[0]["dst"] or seg[1]["src"]).split("|")[-1],
                              clk_cell=clk_cell, utco=utco, out_cell=out_cell, rf=seg[-1]["rf"],
                              tco=round(clk_cell + utco + out_cell, 3)))
        else:
            if not arr or arr[-1]["src_type"] != "cyclonev_ram_block":
                continue
            pin_cell = float(arr[-1]["incr"])
            r = ram_clock(req)
            clk_cell = float(req[r + 1]["incr"])
            ut = sum(float(p["incr"]) for p in req if p["type"] in ("utsu", "uth"))
            # The required side lists utsu as a negative increment, uth as a positive one.
            eff = pin_cell - clk_cell - ut if kind == "setup" else clk_cell + ut - pin_cell
            writes.append(dict(kind=kind, cfg=cfg_of(arr[-1]["src"]), pin=re.sub(r"\[\d+\]", "", arr[-1]["src_port"]),
                               bit=arr[-1]["src_port"], clk_pin=req[r]["dst_port"], pin_cell=pin_cell,
                               clk_cell=clk_cell, ut=ut, eff=round(eff, 3)))
    json.dump(dict(reads=reads, writes=writes, packed=dict(packed)), open(sys.argv[2], "w"), indent=0)
    print("%d read arcs, %d write arcs, %d packed captures" % (len(reads), len(writes), sum(packed.values())))


if __name__ == "__main__":
    main()
