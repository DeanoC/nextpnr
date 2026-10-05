#!/usr/bin/env python3
"""Generate, compile and decode Quartus 17.0.2 altera_pll oracle projects.

A spec is a JSON object:
  {"plls": [{"name": "p0", "pin": "V11", "ref": "50.0 MHz",
             "fractional": false, "site": "FRACTIONALPLL_X0_Y15_N0" (opt),
             "counters": ["PLLOUTPUTCOUNTER_X0_Y20_N1", ...] (opt),
             "outputs": [["25.0 MHz", "0 ps", 50], ...],
             "extra": {"param": "value"} (opt)}], ...}
Each PLL gets its own reset pin so Quartus does not merge instances.
"""
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

Q = Path(os.environ.get("QUARTUS_BIN", "quartus/bin"))
HERE = Path(__file__).resolve().parent
CV = Path(os.environ.get("MISTRAL_CV", "mistral-cv"))

# Ordinary 3.3 V GPIO pins on the DE10-Nano headers used for resets/outputs.
RESET_PINS = ["AF7", "AH14", "AF4", "AH3", "AD5", "AG14", "AE23", "AE6", "AD23", "AE24"]
OUT_PIN = "W15"


def verilog_param(value):
    if isinstance(value, bool):
        return '"true"' if value else '"false"'
    if isinstance(value, int):
        return str(value)
    return '"%s"' % value


def generate(spec, out):
    out.mkdir(parents=True, exist_ok=True)
    plls = spec["plls"]
    pins = []
    for p in plls:
        if p["pin"] not in pins:
            pins.append(p["pin"])
    lines = ["module top(input wire [%d:0] refs, input wire [%d:0] rsts, output wire out);" % (len(pins) - 1, len(plls) - 1)]
    terms = []
    for i, p in enumerate(plls):
        n = len(p["outputs"])
        name = p["name"]
        params = [("reference_clock_frequency", p["ref"]), ("number_of_clocks", n),
                  ("operation_mode", p.get("mode", "direct")),
                  ("fractional_vco_multiplier", "true" if p.get("fractional") else "false")]
        for k, (f, ph, d) in enumerate(p["outputs"]):
            params += [("output_clock_frequency%d" % k, f), ("phase_shift%d" % k, ph), ("duty_cycle%d" % k, d)]
        for k, v in p.get("extra", {}).items():
            params.append((k, v))
        lines.append("    wire [%d:0] %s_clk;" % (n - 1, name))
        lines.append("    wire %s_locked;" % name)
        lines.append("    altera_pll #(")
        lines.append(",\n".join("        .%s(%s)" % (k, verilog_param(v)) for k, v in params))
        rst = "rsts[%d]" % i if p.get("rst", "pin") == "pin" else "1'b0"
        lines.append("    ) %s (.refclk(refs[%d]), .rst(%s), .outclk(%s_clk), .locked(%s_locked));" %
                     (name, pins.index(p["pin"]), rst, name, name))
        for k in range(n):
            lines.append("    reg [3:0] %s_c%d = 0;" % (name, k))
            lines.append("    always @(posedge %s_clk[%d]) %s_c%d <= %s_c%d + 1'b1;" % (name, k, name, k, name, k))
            terms.append("%s_c%d[3]" % (name, k))
        terms.append("%s_locked" % name)
    lines.append("    assign out = %s;" % " ^ ".join(terms))
    lines.append("endmodule")
    (out / "top.v").write_text("\n".join(lines) + "\n")

    sdc = []
    for k, pin in enumerate(pins):
        ref = next(p["ref"] for p in plls if p["pin"] == pin)
        mhz = float(ref.split()[0])
        sdc.append("create_clock -name ref%d -period %.6f [get_ports {refs[%d]}]" % (k, 1000.0 / mhz, k))
    sdc += ["derive_pll_clocks", "derive_clock_uncertainty"]
    (out / "clocks.sdc").write_text("\n".join(sdc) + "\n")

    qsf = [
        'set_global_assignment -name FAMILY "Cyclone V"',
        "set_global_assignment -name DEVICE 5CSEBA6U23I7",
        "set_global_assignment -name TOP_LEVEL_ENTITY top",
        "set_global_assignment -name VERILOG_FILE top.v",
        "set_global_assignment -name SDC_FILE clocks.sdc",
        "set_global_assignment -name PROJECT_OUTPUT_DIRECTORY output_files",
        "set_global_assignment -name GENERATE_RBF_FILE ON",
        "set_global_assignment -name ON_CHIP_BITSTREAM_DECOMPRESSION OFF",
        'set_global_assignment -name STRATIXV_CONFIGURATION_SCHEME "PASSIVE SERIAL"',
        "set_global_assignment -name ENABLE_CONFIGURATION_PINS OFF",
        "set_global_assignment -name NUM_PARALLEL_PROCESSORS 2",
    ]
    for k, pin in enumerate(pins):
        qsf.append("set_location_assignment PIN_%s -to refs[%d]" % (pin, k))
        qsf.append('set_instance_assignment -name IO_STANDARD "3.3-V LVTTL" -to refs[%d]' % k)
    for i in range(len(plls)):
        qsf.append("set_location_assignment PIN_%s -to rsts[%d]" % (RESET_PINS[i], i))
        qsf.append('set_instance_assignment -name IO_STANDARD "3.3-V LVTTL" -to rsts[%d]' % i)
    qsf.append("set_location_assignment PIN_%s -to out" % OUT_PIN)
    qsf.append('set_instance_assignment -name IO_STANDARD "3.3-V LVTTL" -to out')
    for p in plls:
        if p.get("mode", "direct") == "direct":
            qsf.append('set_instance_assignment -name PLL_COMPENSATION_MODE DIRECT -to "%s|*"' % p["name"])
        if "site" in p:
            qsf.append('set_location_assignment %s -to "%s|general[0].gpll~FRACTIONAL_PLL"' % (p["site"], p["name"]))
        for k, loc in enumerate(p.get("counters", [])):
            if loc:
                qsf.append('set_location_assignment %s -to "%s|general[%d].gpll~PLL_OUTPUT_COUNTER"' % (loc, p["name"], k))
    qsf += spec.get("qsf_extra", [])
    (out / "top.qsf").write_text("\n".join(qsf) + "\n")
    (out / "top.qpf").write_text('PROJECT_REVISION = "top"\n')
    (out / "spec.json").write_text(json.dumps(spec, indent=1))


def parse_pll_report(text):
    """Return a list of PLL dicts from the fitter's PLL Usage Summary."""
    start = text.find("; PLL Usage Summary")
    if start < 0:
        return []
    end = text.find("\n\n", start)
    block = text[start:end]
    plls = []
    cur = None
    counter = None
    for line in block.splitlines():
        m = re.match(r"^; (\S.*?~FRACTIONAL_PLL)\s*;", line)
        if m:
            cur = {"name": m.group(1), "counters": []}
            plls.append(cur)
            counter = None
            continue
        m = re.match(r"^;\s+-- (\S.*?~PLL_OUTPUT_COUNTER)\s*;", line)
        if m and cur is not None:
            counter = {"name": m.group(1)}
            cur["counters"].append(counter)
            continue
        m = re.match(r"^;\s+-- (.*?)\s*;\s*(.*?)\s*;$", line)
        if m and cur is not None:
            key, value = m.group(1), m.group(2)
            if counter is not None:
                counter[key] = value
            else:
                cur[key] = value
    return plls


def compile_project(out, timeout=1800):
    for d in ("db", "incremental_db", "output_files"):
        shutil.rmtree(out / d, ignore_errors=True)
    with (out / "quartus.log").open("w") as log:
        try:
            rc = subprocess.run([str(Q / "quartus_sh"), "--flow", "compile", "top"], cwd=out,
                                stdout=log, stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            rc = -1
    result = {"quartus_rc": rc}
    rpt = out / "output_files/top.fit.rpt"
    if rpt.exists():
        text = rpt.read_text(encoding="latin-1")
        result["pll_report"] = parse_pll_report(text)
    errors = [l for l in (out / "quartus.log").read_text(encoding="latin-1").splitlines()
              if l.startswith("Error") or l.startswith("Critical Warning")]
    result["errors"] = errors[:40]
    rbf = out / "output_files/top.rbf"
    if rc == 0 and rbf.exists():
        subprocess.run([str(CV), "decomp", "5CSEBA6U23I7", str(rbf), str(out / "top.bt")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, check=True)
        bt = (out / "top.bt").read_text()
        settings = {}
        for line in bt.splitlines():
            m = re.match(r"^([si]) ((FPLL|CMUX\w+)\.\d+\.\d+):(\S+) (\S+)$", line)
            if m:
                settings.setdefault(m.group(2), {})[("i:" if m.group(1) == "i" else "") + m.group(4)] = m.group(5)
        result["settings"] = settings
        subprocess.run(["gzip", "-n", "-9", "-k", "-f", str(rbf)], check=True)
    (out / "result.json").write_text(json.dumps(result, indent=1, sort_keys=True))
    return result


def main():
    spec_path = Path(sys.argv[1])
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else spec_path.parent
    spec = json.loads(spec_path.read_text())
    generate(spec, out)
    res = compile_project(out)
    print(out, "rc", res["quartus_rc"], "plls", len(res.get("pll_report", [])))


if __name__ == "__main__":
    main()
