#!/usr/bin/env python3
"""Compile both DQM control forms on the issue #125 pads; optional Quartus oracle."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def run(command, log):
    with log.open("w") as stream:
        subprocess.run(list(map(str, command)), stdout=stream, stderr=subprocess.STDOUT,
                       check=True, timeout=600)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def settings(path):
    return dict(re.findall(r"^s (\S+) (\S+)(?: ;.*)?$", path.read_text(), re.M))


def dqm_lines(path):
    return [line for line in path.read_text().splitlines()
            if "; AG13" in line or "; AF13" in line]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--quartus-root", type=Path)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    receipt = {"classification": "host-only; no pad waveform or physical byte acceptance",
               "tools": {name: {"path": str(path.resolve()), "sha256": digest(path)}
                         for name, path in (("yosys", args.yosys), ("nextpnr", args.nextpnr),
                                            ("mistral-cv", args.mistral_cv))}, "cases": {}}
    for name in ("sclr", "plain-d", "zero", "one"):
        case = output / name
        case.mkdir(exist_ok=True)
        source = fixture / "top.v"
        defines = "-D MASK_PLAIN_D" if name == "plain-d" else ""
        if name in ("zero", "one"):
            source = case / "top.v"
            source.write_text("(* blackbox *) module MISTRAL_IB (input PAD, output O); endmodule\n"
                              "(* blackbox *) module MISTRAL_OB (input I, output PAD); endmodule\n"
                              "module top(input clk, clear_masks, lower_data, upper_data, "
                              f"output dqml, dqmh); assign dqml = 1'b{int(name == 'one')}; "
                              f"assign dqmh = 1'b{int(name == 'one')}; endmodule\n")
        run([args.yosys, "-p", f'read_verilog {defines} "{source}"; hierarchy -top top; '
             'proc; flatten; opt; iopadmap -bits -inpad MISTRAL_IB O:PAD '
             f'-outpad MISTRAL_OB I:PAD; write_json "{case / "synth.json"}"'], case / "synth.log")
        run([args.nextpnr, "--device", "5CSEBA6U23I7", "--json", case / "synth.json",
             "--qsf", fixture / "pins.qsf", "--sdc", fixture / "clocks.sdc", "--seed", "1",
             "--write", case / "routed.json", "--report", case / "timing.json",
             "--rbf", case / "core.rbf", "--compress-rbf"], case / "route.log")
        run([args.mistral_cv, "decomp", "5CSEBA6U23I7", case / "core.rbf", case / "core.bt"],
            case / "decode.log")
        cells = json.loads((case / "routed.json").read_text())["modules"]["top"]["cells"]
        for port, bel in (("dqml", "MISTRAL_IO.50.0.0"), ("dqmh", "MISTRAL_IO.50.0.1")):
            pad = cells[f"$iopadmap$top.{port}"]
            assert pad["type"] == "MISTRAL_OB" and pad["attributes"]["NEXTPNR_BEL"] == bel, pad
            assert pad["connections"]["I"], pad
        decoded = settings(case / "core.bt")
        lines = dqm_lines(case / "core.bt")
        for pin, lane, oe in (("AG13", 4, 3), ("AF13", 0, 1)):
            assert any(line.startswith("r ") and f"IOINTDQDOUT.{lane} ; {pin}" in line for line in lines), lines
            assert any(line.startswith(f"i HMC.025.000:IOINTDQOE.{oe} 0") for line in lines), lines
            assert not any(line.startswith(f"i HMC.025.000:IOINTDQDOUT.{lane} ") for line in lines), lines
        lab = "LAB.042.003:"
        if name in ("sclr", "plain-d"):
            assert sum(c["type"] == "MISTRAL_FF" for c in cells.values()) == 2
            for ff in ("lower_ff", "upper_ff"):
                assert bool(cells[ff]["connections"].get("SCLR")) == (name == "sclr"), cells[ff]
            if name == "sclr":
                assert decoded.get(lab + "TSLOAD_EN.9") == "1", decoded
                assert decoded.get(lab + "BSLOAD_EN.2") == "1", decoded
                assert decoded.get(lab + "SLOAD_EN") == "0", decoded
            else:
                assert decoded.get(lab + "TSCLR_DIS.9") == "1", decoded
                assert decoded.get(lab + "BSCLR_DIS.2") == "1", decoded
        else:
            for port in ("dqml", "dqmh"):
                bit = cells[f"$iopadmap$top.{port}"]["connections"]["I"]
                driver = next(c for c in cells.values()
                              if c["type"] == "MISTRAL_CONST" and c["connections"]["Q"] == bit)
                assert int(driver["parameters"]["LUT"], 2) == int(name == "one"), driver
        receipt["cases"][name] = {
            "rbf_sha256": digest(case / "core.rbf"),
            "routed_sha256": digest(case / "routed.json"),
            "lab_controls": {k: v for k, v in decoded.items() if k.startswith(lab)},
            "pad_settings": {k: v for k, v in decoded.items()
                             if k.startswith(("GPIO.050.000:", "HMC.025.000:"))},
            "dqm_lines": lines,
        }
        print("PASS:", name, "uses AG13/AF13 with the intended fabric controls")

    if args.quartus_root:
        for name in ("sclr", "plain-d"):
            case = output / ("quartus-" + name)
            case.mkdir(exist_ok=True)
            qsf = (fixture / "pins.qsf").read_text() + (
                'set_global_assignment -name FAMILY "Cyclone V"\n'
                'set_global_assignment -name DEVICE 5CSEBA6U23I7\n'
                'set_global_assignment -name TOP_LEVEL_ENTITY top\n'
                'set_global_assignment -name NUM_PARALLEL_PROCESSORS 2\n'
                'set_global_assignment -name GENERATE_RBF_FILE ON\n'
                'set_global_assignment -name PROJECT_OUTPUT_DIRECTORY output_files\n'
                f'set_global_assignment -name VERILOG_FILE "{fixture / "oracle.v"}"\n'
                f'set_global_assignment -name SDC_FILE "{fixture / "clocks.sdc"}"\n')
            if name == "plain-d":
                qsf += 'set_global_assignment -name VERILOG_MACRO "MASK_PLAIN_D"\n'
            (case / "top.qsf").write_text(qsf)
            (case / "top.qpf").write_text('PROJECT_REVISION = "top"\n')
            run([args.quartus_root / "bin/quartus_sh", "--flow", "compile", case / "top"],
                case / "compile.log")
            rbf = case / "output_files/top.rbf"
            run([args.mistral_cv, "decomp", "5CSEBA6U23I7", rbf, case / "core.bt"], case / "decode.log")
            receipt["cases"]["quartus-" + name] = {"rbf_sha256": digest(rbf),
                "settings": settings(case / "core.bt"), "dqm_lines": dqm_lines(case / "core.bt")}
            fit = (case / "output_files/top.fit.rpt").read_text(encoding="latin-1")
            assert re.search(r"; Total registers\s*; 2\s*;", fit), fit
            assert re.search(r"; I/O registers\s*; 0\s*;", fit), fit
            decoded = settings(case / "core.bt")
            if name == "sclr":
                assert any(":TSLOAD_EN." in k or ":BSLOAD_EN." in k for k in decoded), decoded
                assert any(k.endswith(":SLOAD_EN") and v == "0" for k, v in decoded.items()), decoded
            else:
                assert not any(":TSLOAD_EN." in k or ":BSLOAD_EN." in k for k in decoded), decoded
            for key, value in receipt["cases"][name]["pad_settings"].items():
                assert decoded.get(key) == value, (key, value, decoded.get(key))
            print("PASS: Quartus", name, "compiled on the same pads with fabric output registers")
    (output / "results.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
