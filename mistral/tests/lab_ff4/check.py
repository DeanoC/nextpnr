#!/usr/bin/env python3
"""Secondary ALM registers (--mistral-ff4) and the second LAB clock (--mistral-clkb).

For every case in cases.py, decode the stored Quartus 17.0.2 RBF, derive the
LAB X30 Y20 register facts (control-group selectors, clock pairs, clears,
enables, packed-register bits of the used registers), check them against
oracle/mapping.json, then place the same registers on the same FF sites with
nextpnr and require identical facts from its RBF.  Also checks that the
default flow is unchanged and that the flags are required on reload.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from cases import CASES, site  # noqa: E402

LAB = "LAB.030.020"
PKREG = {0: "TPKREG1", 1: "TPKREG0", 2: "BPKREG1", 3: "BPKREG0"}
CLK_SEL = {0: "TCLK_SEL", 1: "BCLK_SEL"}
CLR_SEL = {0: "TCLR_SEL", 1: "BCLR_SEL"}
GROUP = {0: 0, 3: 0, 1: 1, 2: 1}
DEFAULTS = {"CLK0_SEL": "CLKA", "CLK1_SEL": "CLKA", "CLK2_SEL": "CLKA", "CLKA_SEL": "CIN0", "CLKB_SEL": "CIN1",
            "CLK0_INV": "0", "CLK1_INV": "0", "CLK2_INV": "0", "EN0_EN": "1", "EN1_EN": "1", "EN2_EN": "1",
            "EN0_NINV": "1", "EN1_NINV": "1", "EN2_NINV": "1", "ACLR0_SEL": "DIN3", "ACLR1_SEL": "DIN2",
            "ACLR0_INV": "0", "ACLR1_INV": "0", "TCLK_SEL": "OFF", "BCLK_SEL": "OFF", "TCLR_SEL": "0",
            "BCLR_SEL": "0", "TPKREG0": "0", "TPKREG1": "0", "BPKREG0": "0", "BPKREG1": "0",
            "TSCLR_DIS": "0", "BSCLR_DIS": "0", "SCLR_DIS": "0", "SLOAD_EN": "1"}


def run(cmd, log, expect_fail=None):
    r = subprocess.run([str(c) for c in cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log.write_text(r.stdout)
    if expect_fail is not None:
        assert r.returncode != 0 and expect_fail in r.stdout, (cmd, r.stdout[-2000:])
    else:
        assert r.returncode == 0, (cmd, r.stdout[-3000:])
    return r.stdout


def lab_settings(bt):
    out = {}
    for line in Path(bt).read_text().splitlines():
        m = re.match(r"^s " + LAB + r":(\S+) (\S+)", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def facts(settings, case):
    """Register-relevant LAB settings for the registers the case uses."""
    get = lambda key: settings.get(key, DEFAULTS.get(key.split(".")[0], "0"))
    f = {}
    pairs, slots_used = set(), set()
    for name, n, data, *_ in case["regs"]:
        alm, ff = site(n)
        g = GROUP[ff]
        f[f"{name}:clk_sel"] = get(f"{CLK_SEL[g]}.{alm}")
        f[f"{name}:clr_sel"] = get(f"{CLR_SEL[g]}.{alm}")
        f[f"{name}:sclr_dis"] = get(f"{'TB'[ff // 2]}SCLR_DIS.{alm}")
        pk = get(f"{PKREG[ff]}.{alm}")
        f[f"{name}:pkreg"] = pk
        pairs.add(int(f[f"{name}:clk_sel"][-1]))
        slots_used.add(int(f[f"{name}:clr_sel"]))
    for k in sorted(pairs):
        for key in (f"CLK{k}_SEL", f"CLK{k}_INV", f"EN{k}_EN", f"EN{k}_NINV"):
            f[key] = get(key)
    for key in ("CLKA_SEL", "CLKB_SEL", "ACLR0_SEL", "ACLR1_SEL", "SCLR_DIS", "SLOAD_EN"):
        f[key] = get(key)
    for j in sorted(slots_used):
        f[f"ACLR{j}_INV"] = get(f"ACLR{j}_INV")
    return f


def packed_regs(routed, case):
    """Registers whose routed DATAIN goes through the half's E/F selector (nextpnr wires TEF/BEF)."""
    module = json.loads(Path(routed).read_text())["modules"]["top"]
    routing = {}
    for net in module["netnames"].values():
        for bit in net["bits"]:
            routing.setdefault(bit, net.get("attributes", {}).get("ROUTING", ""))
    packed = set()
    for i, (rname, n, *_) in enumerate(case["regs"]):
        alm, ff = site(n)
        bit = module["cells"][f"ff{i}"]["connections"]["DATAIN"][0]
        ffin = alm * 4 + ff
        if f"EF[{alm}].WIRE.30.20.FFIN[{ffin}]" in routing[bit]:
            packed.add(rname)
    return packed


def netlist(name, case):
    """Structural netlist with every register and LUT locked to the oracle's site."""
    regs, luts = case["regs"], case["luts"]
    ins = sorted({p for p in case["pins"] if not p.startswith("q[")})
    outs = sorted(p for p in case["pins"] if p.startswith("q["))
    v = ["(* blackbox *) module MISTRAL_FF(input DATAIN, CLK, ACLR, ENA, SCLR, SLOAD, SDATA, output Q); endmodule",
         "(* blackbox *) module MISTRAL_ALUT2 #(parameter [3:0] LUT = 0) (input A, B, output Q); endmodule",
         "(* blackbox *) module MISTRAL_NOT(input A, output Q); endmodule",
         "(* blackbox *) module MISTRAL_CLKBUF(input A, output Q); endmodule",
         "(* blackbox *) module MISTRAL_IB(input PAD, output O); endmodule",
         "(* blackbox *) module MISTRAL_OB(input I, output PAD); endmodule"]
    vec_in = sorted({p.split("[")[0] for p in ins})
    width = lambda base, ports: max(int(p.split("[")[1][:-1]) for p in ports if p.startswith(base + "[")) + 1
    decl = []
    for base in vec_in:
        if any(p.startswith(base + "[") for p in ins):
            decl.append(f"input wire [{width(base, ins) - 1}:0] {base}")
        else:
            decl.append(f"input wire {base}")
    decl.append(f"output wire [{width('q', outs) - 1}:0] q")
    v.append(f"module top({', '.join(decl)});")
    clocks = sorted({r[3] for r in regs})
    for c in clocks:
        v.append(f"    wire {c}_g; MISTRAL_CLKBUF {c}_buf (.A({c}), .Q({c}_g));")
        if any(r[3] == c and r[4] == "neg" for r in regs):
            v.append(f"    wire {c}_n; MISTRAL_NOT {c}_inv (.A({c}_g), .Q({c}_n));")
    for rst in sorted({r[5] for r in regs if r[5]}):
        v.append(f"    wire {rst}_n; MISTRAL_NOT {rst}_inv (.A({rst}), .Q({rst}_n));")
    for lname, (half, kind, a, b) in luts.items():
        mask = {"and": "4'b1000", "xor": "4'b0110"}[kind]
        v.append(f'    wire {lname}; (* BEL = "MISTRAL_COMB.30.20.{half}" *) '
                 f'MISTRAL_ALUT2 #(.LUT({mask})) {lname}_lut (.A({a}), .B({b}), .Q({lname}));')
    for i, (rname, n, data, clk, edge, aclr, ena) in enumerate(regs):
        alm, ff = site(n)
        d = data[4:] if data.startswith("lut:") else data
        c = f"{clk}_n" if edge == "neg" else f"{clk}_g"
        v.append(f'    (* BEL = "MISTRAL_FF.30.20.{alm * 6 + 2 + ff}" *) MISTRAL_FF ff{i} (.DATAIN({d}), .CLK({c}), '
                 f".ACLR({aclr + '_n' if aclr else 1}), .ENA({ena or 1}), .SCLR(1'b0), .SLOAD(1'b0), "
                 f".SDATA(1'b0), .Q(q[{i}]));")
    v.append("endmodule")
    return "\n".join(v) + "\n"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ("yosys", "nextpnr", "mistral-cv", "output"):
        p.add_argument("--" + key, required=True, type=Path)
    p.add_argument("--update-mapping", action="store_true", help="rewrite oracle/mapping.json from the RBFs")
    a = p.parse_args()
    here = Path(__file__).resolve().parent
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    mapping_path = here / "oracle" / "mapping.json"
    mapping = json.loads(mapping_path.read_text()) if mapping_path.exists() else {"cases": {}}
    for name, case in CASES.items():
        d = out / name
        d.mkdir(exist_ok=True)
        # Oracle facts, re-derived from the Quartus RBF
        rbf = d / "quartus.rbf"
        rbf.write_bytes(gzip.decompress((here / "oracle" / name / "top.rbf.gz").read_bytes()))
        assert hashlib.sha256(rbf.read_bytes()).hexdigest() == mapping["cases"][name]["rbf_sha256"], name
        run([a.mistral_cv, "decomp", "5CSEBA6U23I7", rbf, d / "quartus.bt"], d / "quartus-decomp.log")
        expected = facts(lab_settings(d / "quartus.bt"), case)
        if a.update_mapping:
            mapping["cases"].setdefault(name, {})["facts"] = expected
        assert mapping["cases"][name]["facts"] == expected, (name, expected)
        # nextpnr on the same sites
        (d / "top.v").write_text(netlist(name, case))
        (d / "pins.qsf").write_text("".join(f"set_location_assignment PIN_{pin} -to {port}\n"
                                            for port, pin in sorted(case["pins"].items())))
        run([a.yosys, "-p", f"read_verilog {d / 'top.v'}; hierarchy -top top; "
             "iopadmap -bits -inpad MISTRAL_IB O:PAD -outpad MISTRAL_OB I:PAD; "
             f"write_json {d / 'input.json'}"], d / "yosys.log")
        run([a.nextpnr, "--device", "5CSEBA6U23I7", "--json", d / "input.json", "--qsf", d / "pins.qsf",
             "--seed", "1", "--write", d / "routed.json", "--rbf", d / "nextpnr.rbf"] + case["flags"],
            d / "nextpnr.log")
        run([a.mistral_cv, "decomp", "5CSEBA6U23I7", d / "nextpnr.rbf", d / "nextpnr.bt"], d / "nextpnr-decomp.log")
        got = facts(lab_settings(d / "nextpnr.bt"), case)
        # Which register of a half gets the LUT route-through and which is packed through E/F is a free choice
        # (Quartus itself picks either); the PKREG bit must instead agree with the routed data path.
        strict = lambda f: {k: v for k, v in f.items() if not k.endswith(":pkreg")}
        diff = {k: (v, got.get(k)) for k, v in strict(expected).items() if got.get(k) != v}
        assert not diff, (name, "quartus vs nextpnr", diff)
        packed = packed_regs(d / "routed.json", case)
        for rname, *_ in case["regs"]:
            assert got[f"{rname}:pkreg"] == ("1" if rname in packed else "0"), (name, rname, packed)
        print(f"PASS {name}: {len(strict(expected))} register facts match Quartus; "
              f"PKREG matches the E/F data path of {sorted(packed)}")
    if a.update_mapping:
        mapping_path.write_text(json.dumps(mapping, indent=2, sort_keys=True) + "\n")

    # A design placed with a LAB model must be reloaded with it
    d = out / "g_abba"
    for flag, rest in (("--mistral-ff4", ["--mistral-clkb"]), ("--mistral-clkb", ["--mistral-ff4"])):
        run([a.nextpnr, "--device", "5CSEBA6U23I7", "--json", d / "routed.json", "--no-pack", "--no-place",
             "--no-route"] + rest, out / f"reload-without{flag}.log", expect_fail=f"pass {flag} again")
    run([a.nextpnr, "--device", "5CSEBA6U23I7", "--json", d / "routed.json", "--no-pack", "--no-place", "--no-route",
         "--mistral-ff4", "--mistral-clkb", "--rbf", out / "reload.rbf"], out / "reload.log")
    assert (out / "reload.rbf").read_bytes() == (d / "nextpnr.rbf").read_bytes(), "reload changed the bitstream"
    print("PASS reload: both flags are required and reproduce the same RBF")


if __name__ == "__main__":
    main()
