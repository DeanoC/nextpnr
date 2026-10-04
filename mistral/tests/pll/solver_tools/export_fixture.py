#!/usr/bin/env python3
"""Export compiled oracle runs into mistral/tests/pll/fixtures/solver (portable evidence)."""
import hashlib, json, re, shutil, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import collect
runs = Path(sys.argv[1]); dst = Path(sys.argv[2])
SITE_COUNTER = {"FPLL.000.000": lambda y: y, "FPLL.000.014": lambda y: y - 14, "FPLL.000.031": lambda y: 39 - y,
                "FPLL.000.055": lambda y: y - 55, "FPLL.000.073": lambda y: 81 - y, "FPLL.089.000": lambda y: y}
def hz(s):
    w, _, f = s.split()[0].partition(".")
    return int(w) * 10**6 + int((f + "000000")[:6])
lines = ["# run pll ref_hz fractional | outputs (hz phase_ps duty)... | quartus: vco M N K BWCTRL CP_CURRENT M_LO_PRESET "
         "M_PH_PRESET VCO_DIV M_ODD N_BYPASS | counters per output C:hi/lo[o][b]@preset.phmux",
         "# 'garbage' marks configurations Quartus implements with an unrelated VCO (the solver must reject them)."]
used = set()
for x in collect.load(runs):
    if not x["spec"] or x["rc"] != 0:
        continue
    used.add(x["run"])
    sp, f = x["spec"], x["full"]
    outs = " ".join("%d %d %d" % (hz(o[0]), int(o[1].split()[0]), o[2]) for o in sp["outputs"])
    head = "%s %s %d %d | %s |" % (x["run"], x["pll"], hz(sp["ref"]), 1 if sp.get("fractional") else 0, outs)
    if x["type"] == "Integer PLL" and float(x["vco"].split()[0]) < 300:
        lines.append(head + " garbage")
        continue
    k = int(x["frac"].split()[0]) if x["type"] == "Fractional PLL" else int(f.get("FRACTIONAL_DIVISION_SETTING", "0"), 16)
    st = x["settings"]
    cnt = []
    for c in sorted(x["counters"], key=lambda c: int(re.search(r"general\[(\d+)\]", c["name"]).group(1))):
        y = int(re.search(r"_Y(\d+)_", c["Output Clock Location"]).group(1))
        kk = SITE_COUNTER[x["site"]](y)
        cnt.append("C%s:%d/%d%s%s@%d.%d" % (c["C Counter"], int(st.get("DPRIO0_CNT_HI_DIV.%d" % kk, "01"), 16),
                   int(st.get("DPRIO0_CNT_LO_DIV.%d" % kk, "01"), 16),
                   "o" if st.get("DPRIO0_CNT_ODD_DIV_EVEN_DUTY_EN.%d" % kk) == "1" else "",
                   "b" if st.get("BYPASS_EN.%d" % kk) == "1" else "",
                   int(st.get("CNT_PRESET.%d" % kk, "01"), 16), int(st.get("CNT_PH_MUX_PRESET.%d" % kk, "0"))))
    lines.append(head + " %s %d %d %d %d %d %d %s %d %d %d | %s" % (
        x["vco"].replace(" ", ""), x["M"], x["N"], k, int(f["BWCTRL"]), int(f["CP_CURRENT"]),
        int(f["M_CNT_LO_PRESET_SETTING"], 16), f["M_CNT_PH_MUX_PRESET_SETTING"], int(f["VCO_DIV"]),
        int(f.get("M_CNT_ODD_DIV_DUTY_EN", "0")), int(f.get("N_CNT_BYPASS_EN", "0")), " ".join(cnt)))
dst.mkdir(parents=True, exist_ok=True)
(dst / "cases.txt").write_text("\n".join(lines) + "\n")
sums = {}
for run in sorted(used):
    src = runs / run; d = dst / "runs" / run
    d.mkdir(parents=True, exist_ok=True)
    for name in ("top.v", "top.qsf", "clocks.sdc"):
        shutil.copy(src / name, d / name)
    rpt = (src / "output_files/top.fit.rpt").read_text(encoding="latin-1")
    a = rpt.find("; PLL Usage Summary"); b = rpt.find("\n\n", a)
    start = rpt.rfind("\n", 0, rpt.rfind("\n", 0, a)) + 1
    (d / "fitter-pll.txt").write_text(rpt[start:b] + "\n")
    bt = (src / "top.bt").read_text()
    keep = [l for l in bt.splitlines() if re.match(r"^[si] (FPLL|CMUX)", l)]
    (d / "pll-settings.txt").write_text("\n".join(keep) + "\n")
    sums[run] = hashlib.sha256((src / "output_files/top.rbf").read_bytes()).hexdigest()
(dst / "rbf-sha256.txt").write_text("".join("%s  runs/%s/output_files/top.rbf\n" % (v, k) for k, v in sums.items()))
print("cases", len(lines) - 2, "runs", len(used))
