#!/usr/bin/env python3
"""Design single/dual-output integer PLL experiments that force a chosen VCO.

For each target (ref MHz, VCO MHz) search output sets whose first legal VCO
(Quartus GENERIC_PLL rule) is the target.  Writes a batch spec JSON packing
six PLLs per compile.
"""
import json
import sys
from fractions import Fraction
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rbc  # noqa: E402

PINS = ["V11", "V12", "E11", "D12", "Y13", "Y15"]


def fmt(x):
    return rbc.mhz(float(x))


def candidates(vco):
    """Yield candidate output lists for a VCO (Fraction, MHz)."""
    for c in range(2, 41):
        f = vco / c
        if f > 400:
            continue
        yield [(fmt(f), "0 ps", 50)]
    for a in range(2, 13):
        for b in range(a + 1, 14):
            from math import gcd
            if gcd(a, b) != 1:
                continue
            fa, fb = vco / a, vco / b
            if fa > 400:
                continue
            yield [(fmt(fa), "0 ps", 50), (fmt(fb), "0 ps", 50)]


def first_vco(res):
    lst = rbc.parse_list(res)
    return rbc.parse_mhz(lst[0]) if lst else None


def design(targets, ref_of):
    """targets: list of (ref_mhz Fraction, vco Fraction). Returns {target: outputs}."""
    batch, owners = [], []
    for t in targets:
        for outs in candidates(t[1]):
            batch.append(rbc.vco_list(ref_of(t[0]), False, outs))
            owners.append((t, outs))
    res = rbc.query(batch)
    found = {}
    for (t, outs), r in zip(owners, res):
        if t in found:
            continue
        v = first_vco(r)
        if v is not None and abs(v - float(t[1])) < 1e-4:
            found[t] = outs
    return found


def main():
    out = Path(sys.argv[1])
    targets = []
    for line in Path(sys.argv[2]).read_text().split():
        ref, vco = line.split(",")
        targets.append((Fraction(ref), Fraction(vco)))
    found = design(targets, lambda r: fmt(r))
    missing = [t for t in targets if t not in found]
    print("found", len(found), "missing", len(missing), [(float(a), float(b)) for a, b in missing])
    specs = {}
    items = sorted(found.items())
    prefix = sys.argv[3] if len(sys.argv) > 3 else "mn"
    k = 0
    while items:
        # pack up to six PLLs, one per pin
        group, items = items[:6], items[6:]
        plls = []
        for i, ((ref, vco), outs) in enumerate(group):
            plls.append({"name": "pll%d" % i, "pin": PINS[i], "ref": fmt(ref),
                         "outputs": [list(o) for o in outs],
                         "target_vco": float(vco)})
        specs["%s%03d" % (prefix, k)] = {"plls": plls}
        k += 1
    out.write_text(json.dumps(specs, indent=1))
    print("wrote", len(specs), "specs to", out)


if __name__ == "__main__":
    main()
