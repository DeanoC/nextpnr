#!/usr/bin/env python3
"""Cross-check the C++ solver's VCO choice against the Quartus legality engine."""
import random, subprocess, sys
from fractions import Fraction as F
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import rbc
CLI = sys.argv[1]
seed, count, frac, nout_max = int(sys.argv[2]), int(sys.argv[3]), sys.argv[4] == "1", int(sys.argv[5])
pph = float(sys.argv[6]) if len(sys.argv) > 6 else 0.0
pduty = float(sys.argv[7]) if len(sys.argv) > 7 else 0.0
rng = random.Random(seed)
refs = (["125", "200", "300", "150", "700", "250"] if len(sys.argv) > 8 else ["50", "50", "50", "50", "60", "74.25", "100", "99.999", "54", "81"]) if frac else \
       ["50", "50", "50", "25", "100", "27", "12", "33.333333", "74.25", "125", "10", "300", "20", "48", "5", "320"]
cases = []
for _ in range(count):
    ref = F(rng.choice(refs))
    while True:
        n = rng.randint(1, 10); m = rng.randint(1, 400)
        v = ref * m / n
        if 300 <= v <= 1600: break
    if frac:
        v = v * (1 + F(rng.randint(-50000, 50000), 1000000))
    outs = []
    nout = rng.randint(1, nout_max)
    for i in range(nout):
        c = rng.randint(1, 40)
        f = F(round(v / c * 10**6), 10**6)
        if rng.random() < 0.3:
            f += F(rng.randint(-700, 700), 10**6)
        if f <= 0: f = F(1)
        ph, d = 0, 50
        if rng.random() < pph:
            step = F(10**6, 8) / v
            ph = int(round(rng.randint(0, 8 * c - 1) * step)) + rng.choice([0, 0, 1, -1, 3, 6])
            ph = max(ph, 0)
        if rng.random() < pduty:
            d = rng.choice([10, 20, 25, 30, 33, 40, 45, 60, 66, 75, 80, 90])
        outs.append((f, ph, d))
    cases.append((ref, outs))
inp = "\n".join("%d %d %s" % (int(ref * 10**6), 1 if frac else 0, " ".join("%d %d %d" % (int(f * 10**6), ph, d) for f, ph, d in outs)) for ref, outs in cases)
sol = subprocess.run([CLI], input=inp + "\n", capture_output=True, text=True).stdout.splitlines()
lines = [rbc.vco_list("%.6f MHz" % float(ref), frac, [("%.6f MHz" % float(f), "%d ps" % ph, d) for f, ph, d in outs]) for ref, outs in cases]
res = rbc.query(lines)
ok = bad = err = 0
for (ref, outs), s, r in zip(cases, sol, res):
    q = [F(x.split()[0]) for x in rbc.parse_list(r)]
    if s.startswith("ERR"):
        err += 1
        continue
    vco = s.split()[1][4:]
    whole, rest = vco.split("+")
    num, den = rest.split("/")
    v = (F(int(whole)) + F(int(num), int(den))) / 10**6
    if q and abs(q[0] - v) <= F(3, 10**6) and len(q) >= 1:
        ok += 1
    else:
        bad += 1
        print("BAD", float(ref), [(str(f), ph, d) for f, ph, d in outs], "solver", float(v), "quartus", [float(x) for x in q[:3]])
print("ok", ok, "bad", bad, "fail-closed", err)
