#!/usr/bin/env python3
"""Envelope each M10K read class over several fits (#232).

classes.py <fit dir>... > classes.json

Each fit directory holds arcs-<corner>.json from arcs.py for the four corners.
A class is the read port mode (SDP, SDP with either port 40 bits wide, TDP port
A or B) and whether the read goes through the output register (CFG_OUT_REG_A/B
or an output clear). For every corner it records the setup (max) and hold (min)
clock-to-output range over every width, bit, edge and fit.
"""
import collections
import json
import sys

CORNERS = ["7_slow_1100mv_100c", "7_slow_1100mv_-40c", "MIN_fast_1100mv_100c", "MIN_fast_1100mv_-40c"]


def read_class(r):
    tdp = r["cfg"].startswith("tdp")
    # project.py names its 40-bit configurations sdp40*, dc40*, and mw*40*.
    wide = not tdp and ("40" in r["cfg"])
    mode = "TDP " + r["out"][4].upper() if tdp else ("SDP 40-bit" if wide else "SDP")
    # An unregistered read starts at the port's read register: PORT_B_WRITE_ENABLE_REG
    # (SDP) or port[ab]_we_reg (TDP). A registered read starts at the output register q_[ab].
    registered = not (r["reg"].endswith("_REG") or r["reg"].endswith("we_reg"))
    return mode, "registered" if registered else "unregistered"


def main():
    env = collections.defaultdict(list)
    cfgs = collections.defaultdict(set)
    for fit in sys.argv[1:]:
        for corner in CORNERS:
            arcs = json.load(open("%s/arcs-%s.json" % (fit, corner)))
            assert not arcs["packed"], (fit, corner, arcs["packed"])
            for r in arcs["reads"]:
                env[(read_class(r), r["kind"], corner)].append(r["tco"])
                cfgs[read_class(r)].add(r["cfg"])
    out = []
    for cls in sorted(cfgs):
        row = {"class": " ".join(cls), "configs": sorted(cfgs[cls])}
        for corner in CORNERS:
            setup, hold = env[(cls, "setup", corner)], env[(cls, "hold", corner)]
            row[corner] = {"setup_min": min(setup), "setup_max": max(setup), "hold_min": min(hold),
                           "hold_max": max(hold), "arcs": len(setup)}
        out.append(row)
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
