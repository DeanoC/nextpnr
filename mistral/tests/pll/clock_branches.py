#!/usr/bin/env python3
"""Host-only independent gated branches sharing one physical PLL counter."""
import argparse
import copy
import gzip
import hashlib
import json
from pathlib import Path
import re

from check import run
from triple import fpll_settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "mistral-cv", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    fixture = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = (fixture / "clock_branches.v").read_text()

    def synth(text, case):
        case.mkdir(parents=True, exist_ok=True)
        (case / "top.v").write_text(text)
        run([str(args.yosys.resolve()), "-p", f'read_verilog "{case / "top.v"}"; '
             f'synth_intel_alm -nobram -nolutram -nodsp -top top; write_json "{case / "synth.json"}"'], case / "synth.log")
        return json.loads((case / "synth.json").read_text())

    sdc = out / "clocks.sdc"
    sdc.write_text("create_clock -name FPGA_CLK1_50 -period 20 [get_ports {FPGA_CLK1_50}]\n")
    command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7", "--qsf", str(fixture / "diagnostic.qsf"),
               "--sdc", str(sdc), "--freq", "50", "--compress-rbf"]

    def route(case, design, branches=2, two_counters=False):
        case.mkdir(parents=True, exist_ok=True)
        (case / "synth.json").write_text(json.dumps(design))
        log = run(command + ["--json", str(case / "synth.json"), "--rbf", str(case / "top.rbf"),
                             "--report", str(case / "timing.json"), "--write", str(case / "routed.json")], case / "route.log")
        report = json.loads((case / "timing.json").read_text())
        util = report["utilization"]
        assert util["altera_pll"]["used"] == util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
        assert util["MISTRAL_CLKENA"]["used"] == branches
        for kind in ("MISTRAL_MUL9X9", "MISTRAL_M10K", "MISTRAL_MLAB"):
            assert util.get(kind, {"used": 0})["used"] == 0
        for name in ("pll_clock", "gated_clock"):
            assert report["fmax"][name]["constraint"] == 25
            assert report["fmax"][name]["achieved"] >= 25
        cells = json.loads((case / "routed.json").read_text())["modules"]["top"]["cells"]
        buffers = {name: cell for name, cell in cells.items()
                   if cell["type"] in ("MISTRAL_CLKBUF", "MISTRAL_CLKENA")}
        assert len(buffers) == branches
        assert len({tuple(c["connections"]["Q"]) for c in buffers.values()}) == branches
        assert len({tuple(c["connections"]["A"]) for c in buffers.values()}) == (2 if two_counters else 1)
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7", str(case / "top.rbf"), str(case / "top.bt")], case / "decomp.log")
        bt = (case / "top.bt").read_text()
        settings = dict(re.findall(r"^s CMUXHG\.000\.035:(\S+) (\S+)$", bt, re.M))
        for name, cell in buffers.items():
            index = int(cell["attributes"]["NEXTPNR_BEL"].rsplit(".", 1)[1])
            lane = (2, 3, 1, 0)[index]
            assert settings[f"INPUT_SEL.{lane}"] == ("15" if two_counters and lane == 3 else "16")
            assert settings[f"TESTSYN_ENOUT_SELECT.{lane}"] == "PRE_SYNENB"
            if cell["type"] == "MISTRAL_CLKENA":
                assert settings[f"ENABLE_REGISTER_MODE.{lane}"] == "REG1_ENOUT"
                expected = design["modules"]["top"]["cells"][name]["parameters"].get("ena_register_power_up", "high")
                assert settings.get(f"ENABLE_REGISTER_POWER_UP.{lane}", "1") == ("0" if expected == "low" else "1")
                assert re.search(rf"^r \S+ CMUXHG\.000\.035\.{lane}:ENABLE$", bt, re.M)
            else:
                assert lane in ((2, 3) if two_counters else (2,))
                assert settings.get(f"ENABLE_REGISTER_MODE.{lane}") != "REG1_ENOUT"
        return report, bt, log

    design = synth(source, out)
    report, bt, _ = route(out, design)
    reference = fixture / "fixtures" / "clock-branches"
    hashes = json.loads((reference / "sha256.json").read_text())
    compressed = (reference / "top.rbf.gz").read_bytes()
    assert hashlib.sha256(compressed).hexdigest() == hashes["rbf.gz"]
    raw = gzip.decompress(compressed)
    assert hashlib.sha256(raw).hexdigest() == hashes["rbf"]
    (out / "quartus.rbf").write_bytes(raw)
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7", str(out / "quartus.rbf"), str(out / "quartus.bt")], out / "oracle.log")
    oracle = (out / "quartus.bt").read_text()
    assert fpll_settings(bt) == fpll_settings(oracle)
    def hg(text):
        return dict(re.findall(r"^s CMUXHG\.000\.035:(\S+) (\S+)$", text, re.M))
    assert hg(bt) and hg(bt) == hg(oracle)

    raw_design = copy.deepcopy(design)
    top = raw_design["modules"]["top"]
    gate = top["cells"]["gate"]
    old_bit = gate["connections"]["outclk"][0]
    name, buffer = next((n, c) for n, c in top["cells"].items()
                        if c["type"] == "MISTRAL_CLKBUF" and c["connections"]["A"] == [old_bit])
    gate["type"] = "MISTRAL_CLKENA"
    gate["parameters"] = {"ena_register_power_up": "low"}
    gate["connections"] = {"A": gate["connections"]["inclk"], "ENA": gate["connections"]["ena"],
                           "Q": buffer["connections"]["Q"]}
    gate["port_directions"] = {"A": "input", "ENA": "input", "Q": "output"}
    del top["cells"][name]
    for name in list(top["netnames"]):
        if top["netnames"][name]["bits"] == [old_bit]:
            del top["netnames"][name]
    route(out / "raw", raw_design)

    # Isolate a real data crossing so the report retains its destination.
    crossing = source.replace("count <= count + 1'b1", "count <= count + running").replace(
        "running <= running + 1'b1", "running <= ~running")
    cross_report, _, log = route(out / "crossing", synth(crossing, out / "crossing"))
    paths = [p for p in cross_report["critical_paths"] if p["from"] == "posedge pll_clock" and p["to"] == "posedge gated_clock"]
    assert paths, cross_report
    for path in paths:
        assert abs(path["max_delay"] - 40) < 0.002, path
    assert "cross-domain path 'posedge pll_clock' -> 'posedge gated_clock'" not in log

    two_source = source.replace("wire pll_clock, gated_clock, locked;", "wire pll_clock, gated_clock, locked, second_clock;")
    two_source = two_source.replace(".number_of_clocks(1)", ".number_of_clocks(2)").replace(
        '.output_clock_frequency0("25.0 MHz"),', '.output_clock_frequency0("25.0 MHz"), '
        '.output_clock_frequency1("25.0 MHz"), .phase_shift1("0 ps"), .duty_cycle1(50),')
    two_source = two_source.replace(".outclk(pll_clock)", ".outclk({second_clock, pll_clock})")
    two_source = two_source.replace("    reg [7:0] count", "    reg [7:0] second_count = 0;\n"
                                   "    always @(posedge second_clock) second_count <= second_count + running;\n    reg [7:0] count")
    two_source = two_source.replace("15'b0, locked", "7'b0, second_count, locked").replace(
        "running <= running + 1'b1", "running <= ~running")
    two_report, _, two_log = route(out / "two-counters", synth(two_source, out / "two-counters"), 3, True)
    assert two_report["fmax"]["second_clock"]["constraint"] == 25
    assert two_report["fmax"]["second_clock"]["achieved"] >= 25
    assert "cross-domain path 'posedge pll_clock' -> 'posedge second_clock'" in two_log, two_log
    assert any(p["from"] == "posedge pll_clock" and p["to"] == "posedge second_clock"
               for p in two_report["critical_paths"]), two_report

    shifted_source = two_source.replace('.phase_shift1("0 ps")', '.phase_shift1("10000 ps")')
    shifted_report, _, shifted_log = route(out / "shifted-counters",
                                          synth(shifted_source, out / "shifted-counters"), 3, True)
    shifted_paths = [p for p in shifted_report["critical_paths"]
                     if p["from"] == "posedge pll_clock" and p["to"] == "posedge second_clock"]
    assert shifted_paths, shifted_report
    assert all(abs(path["max_delay"] - 10) < 0.002 for path in shifted_paths), shifted_paths
    assert "cross-domain path 'posedge pll_clock' -> 'posedge second_clock'" not in shifted_log

    for clock in ("pll_clock", "gated_clock"):
        bad_sdc = out / f"invalid-{clock}.sdc"
        bad_sdc.write_text(sdc.read_text() + f"create_clock -name {clock} -period 20 [get_nets {{{clock}}}]\n")
        cmd = command.copy()
        cmd[cmd.index("--sdc") + 1] = str(bad_sdc)
        log = run(cmd + ["--json", str(out / "synth.json")], out / f"invalid-{clock}.log", success=False)
        assert "conflicting clock constraint" in log

    def extra_branches(number):
        text = source
        declarations = []
        observed = []
        for index in range(1, number):
            power = "high" if index % 2 else "low"
            declarations.append(f'''    wire gated{index};
    reg extra{index} = 0;
    always @(posedge gated{index}) extra{index} <= !extra{index};
    cyclonev_clkena #(.clock_type("Global Clock"), .ena_register_mode("falling edge"), .ena_register_power_up("{power}"))
        gate{index} (.inclk(pll_clock), .ena(gp_out[{index}]), .outclk(gated{index}), .enaout());
''')
            observed.append(f"extra{index}")
        text = text.replace("    cyclonev_hps_interface", "".join(declarations) + "    cyclonev_hps_interface")
        return text.replace("15'b0, locked", f"{16 - number}'b0, " + ", ".join(observed) + ", locked")

    four = synth(extra_branches(3), out / "four")
    four_report, _, _ = route(out / "four", four, 4)
    for name in ("gated1", "gated2"):
        assert four_report["fmax"][name]["constraint"] == 25 and four_report["fmax"][name]["achieved"] >= 25
    synth(extra_branches(4), out / "five")
    log = run(command + ["--json", str(out / "five" / "synth.json")], out / "invalid-five.log", success=False)
    assert "no available dedicated PLL/clock-buffer pair" in log, log
    print("PASS: independent clock branches, shared phase, Quartus oracle and resource limits", report["fmax"])
    print("RBF sha256", hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
