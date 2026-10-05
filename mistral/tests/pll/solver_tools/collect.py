#!/usr/bin/env python3
"""Collect per-PLL oracle observations from runs/*/result.json into one table."""
import json
import re
import sys
from pathlib import Path

DEFAULTS = {"BWCTRL": "4", "CP_CURRENT": "2", "VCO_DIV": "1", "M_CNT_LO_PRESET_SETTING": "01",
            "M_CNT_PH_MUX_PRESET_SETTING": "0", "N_CNT_BYPASS_EN": "0", "M_CNT_ODD_DIV_DUTY_EN": "0",
            "N_CNT_ODD_DIV_DUTY_EN": "0", "RIPPLECAP_CTRL": "0", "DSM_OUT_SEL": "0"}


def site_of(loc):
    m = re.match(r"FRACTIONALPLL_X(\d+)_Y(\d+)_N0", loc)
    return "FPLL.%03d.%03d" % (int(m.group(1)), int(m.group(2)) - 1)


def load(root):
    rows = []
    for res in sorted(Path(root).glob("*/result.json")):
        r = json.loads(res.read_text())
        spec = json.loads((res.parent / "spec.json").read_text())
        byname = {p["name"]: p for p in spec["plls"]}
        for p in r.get("pll_report", []):
            name = p["name"].split("|")[0].split(":")[-1]
            sp = byname.get(name)
            site = site_of(p["PLL Location"])
            st = r.get("settings", {}).get(site, {})
            full = dict(DEFAULTS)
            full.update(st)
            m_hi = int(st.get("M_CNT_HI_DIV_SETTING", "01"), 16)
            rows.append({
                "run": res.parent.name, "pll": name, "spec": sp, "site": site,
                "ref": p["Reference Clock Frequency"], "vco": p["PLL VCO Frequency"],
                "M": int(p["M Counter"]), "N": int(p["N Counter"]),
                "frac": p["PLL Fractional Division"], "bwrange": p.get("PLL Bandwidth Range"),
                "type": p["PLL Type"], "counters": p["counters"], "settings": st, "full": full,
                "rc": r["quartus_rc"],
            })
    return rows


if __name__ == "__main__":
    rows = load(sys.argv[1])
    for x in rows:
        f = x["full"]
        print(x["run"], x["pll"], x["ref"], x["vco"], "M", x["M"], "N", x["N"], "BW", f["BWCTRL"], "CP", f["CP_CURRENT"],
              "pre", int(f["M_CNT_LO_PRESET_SETTING"], 16), f["M_CNT_PH_MUX_PRESET_SETTING"], "vdiv", f["VCO_DIV"],
              "rng", x["bwrange"], "tgt", x["spec"].get("target_vco") if x["spec"] else None)
