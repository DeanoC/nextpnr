#!/usr/bin/env python3
"""Check two independent PLLs sharing the V11 reference; host-only, never programs hardware."""
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

    def synth(source, target):
        run([str(args.yosys.resolve()), "-p",
             f'read_verilog "{source}"; synth_intel_alm -nobram -nolutram -nodsp -top top; '
             f'write_json "{target}"'], target.with_suffix(".log"))
        return json.loads(target.read_text())

    design = synth(fixture / "two_pll.v", out / "synth.json")
    assert sum(c["type"] == "altera_pll" for c in design["modules"]["top"]["cells"].values()) == 2
    sdc = out / "clocks.sdc"
    sdc.write_text("create_clock -name FPGA_CLK1_50 -period 20 [get_ports {FPGA_CLK1_50}]\n")
    command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
               "--qsf", str(fixture / "diagnostic.qsf"), "--sdc", str(sdc),
               "--freq", "50", "--compress-rbf"]

    def route(case, netlist, outputs=2, exact_sites=False):
        case.mkdir(parents=True, exist_ok=True)
        path = case / "synth.json"
        path.write_text(json.dumps(netlist))
        log = run(command + ["--json", str(path), "--rbf", str(case / "top.rbf"),
                             "--report", str(case / "timing.json"), "--write", str(case / "routed.json")],
                  case / "route.log")
        report = json.loads((case / "timing.json").read_text())
        util = report["utilization"]
        assert util["altera_pll"] == {"used": 2, "available": 6}
        assert util["MISTRAL_CLKENA"]["used"] == outputs
        assert util["cyclonev_hps_interface_mpu_general_purpose"]["used"] == 1
        for kind in ("MISTRAL_MUL9X9", "MISTRAL_M10K", "MISTRAL_MLAB"):
            assert util.get(kind, {"used": 0})["used"] == 0
        for name in ("video_clock", "audio_clock"):
            clock = report["fmax"][name]
            expected = float(netlist["modules"]["top"]["cells"][name.replace("clock", "pll")]
                             ["parameters"]["output_clock_frequency0"].split()[0])
            assert abs(clock["constraint"] - expected) <= expected * 0.00005, clock
            assert clock["achieved"] >= clock["constraint"], clock
        routed = json.loads((case / "routed.json").read_text())["modules"]["top"]["cells"]
        sites = {routed[name]["attributes"]["NEXTPNR_BEL"] for name in ("video_pll", "audio_pll")}
        assert sites == {"altera_pll.0.14.0", "altera_pll.0.31.0"}, sites
        if exact_sites:
            assert routed["video_pll"]["attributes"]["NEXTPNR_BEL"] == "altera_pll.0.14.0"
            assert routed["audio_pll"]["attributes"]["NEXTPNR_BEL"] == "altera_pll.0.31.0"
        buffers = [cell["attributes"]["NEXTPNR_BEL"] for cell in routed.values()
                   if cell["type"] == "MISTRAL_CLKBUF"]
        assert len(buffers) == outputs and set(buffers) == {f"MISTRAL_CLKENA.0.35.{i}" for i in range(outputs)}, buffers
        return report, log

    def compare_oracle(case, reference):
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
             str(case / "top.rbf"), str(case / "top.bt")], case / "decomp.log")
        bt = (case / "top.bt").read_text()
        assert len(re.findall(r"^s FPLL.*:FPLL_ENABLE 1$", bt, re.M)) == 2
        for line in ("s FPLL.000.014:CLKIN_0_SRC 4", "s FPLL.000.031:CLKIN_0_SRC 6",
                     "s CMUXHG.000.035:INPUT_SEL.2 16", "s CMUXHG.000.035:INPUT_SEL.3 0e"):
            assert line in bt.splitlines(), line
        hashes = json.loads((reference / "sha256.json").read_text())
        compressed = (reference / "top.rbf.gz").read_bytes()
        assert hashlib.sha256(compressed).hexdigest() == hashes["rbf.gz"]
        raw = gzip.decompress(compressed)
        assert hashlib.sha256(raw).hexdigest() == hashes["rbf"]
        (case / "quartus.rbf").write_bytes(raw)
        run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
             str(case / "quartus.rbf"), str(case / "quartus.bt")], case / "quartus-decomp.log")
        oracle, settings = fpll_settings((case / "quartus.bt").read_text()), fpll_settings(bt)
        assert oracle and settings
        differences = {key: (oracle.get(key), settings.get(key)) for key in oracle.keys() | settings.keys()
                       if oracle.get(key) != settings.get(key)}
        assert not differences, f"FPLL oracle mismatch (oracle, actual): {differences}"


    report, _ = route(out, design, exact_sites=True)
    compare_oracle(out, fixture / "fixtures" / "two-pll")
    swapped = copy.deepcopy(design)
    cells = swapped["modules"]["top"]["cells"]
    cells["video_pll"]["parameters"], cells["audio_pll"]["parameters"] = (
        cells["audio_pll"]["parameters"], cells["video_pll"]["parameters"])
    route(out / "swapped", swapped, exact_sites=True)
    compare_oracle(out / "swapped", fixture / "fixtures" / "two-pll" / "swapped")

    # Two outputs per PLL occupy every dedicated global lane shared by both sites.
    source = (fixture / "two_pll.v").read_text()
    source = source.replace('wire video_clock, audio_clock;',
                            'wire video_clock, audio_clock, video_second, audio_second;')
    source = source.replace('.number_of_clocks(1)', '.number_of_clocks(2)')
    source = source.replace('.output_clock_frequency0("12.288 MHz")', '.output_clock_frequency0("40.0 MHz")')
    source = source.replace('.fractional_vco_multiplier("true")', '.fractional_vco_multiplier("false")')
    source = source.replace('.phase_shift0("0 ps"),',
                            '.output_clock_frequency1("50.0 MHz"), .phase_shift1("0 ps"), '
                            '.duty_cycle1(50), .phase_shift0("0 ps"),')
    source = source.replace('.outclk(video_clock)', '.outclk({video_second, video_clock})')
    source = source.replace('.outclk(audio_clock)', '.outclk({audio_second, audio_clock})')
    source = source.replace('    wire [1:0] locked;', '    wire [1:0] locked;\n'
                            '    reg video_extra = 0, audio_extra = 0;\n'
                            '    always @(posedge video_second) video_extra <= !video_extra;\n'
                            '    always @(posedge audio_second) audio_extra <= !audio_extra;')
    source = source.replace("14'b0, locked", "12'b0, video_extra, audio_extra, locked")
    (out / "four-lanes.v").write_text(source)
    four = synth(out / "four-lanes.v", out / "four-lanes.json")
    four_report, _ = route(out / "four-lanes", four, outputs=4)
    for name in ("video_second", "audio_second"):
        assert four_report["fmax"][name]["constraint"] == 50
        assert four_report["fmax"][name]["achieved"] >= 50

    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
         str(out / "four-lanes" / "top.rbf"), str(out / "four-lanes" / "top.bt")],
        out / "four-lanes" / "decomp.log")
    four_bt = (out / "four-lanes" / "top.bt").read_text().splitlines()
    routed = json.loads((out / "four-lanes" / "routed.json").read_text())["modules"]["top"]["cells"]
    # Mistral p2p: site14 C6/C7 -> PLLIN14/13; site31 C6/C7 -> PLLIN6/5.
    # INPUT_SEL encodes PLLIN index + 8. BEL indices preserve lane order 2,3,1,0.
    selectors = {"altera_pll.0.14.0": ("16", "15"), "altera_pll.0.31.0": ("0e", "0d")}
    for pll in ("video_pll", "audio_pll"):
        site = routed[pll]["attributes"]["NEXTPNR_BEL"]
        for index, bit in enumerate(four["modules"]["top"]["cells"][pll]["connections"]["outclk"]):
            buffer_name = next(name for name, cell in four["modules"]["top"]["cells"].items()
                               if cell["type"] == "MISTRAL_CLKBUF" and cell["connections"]["A"] == [bit])
            bel = routed[buffer_name]["attributes"]["NEXTPNR_BEL"]
            lane = (2, 3, 1, 0)[int(bel.rsplit(".", 1)[1])]
            assert f"s CMUXHG.000.035:INPUT_SEL.{lane} {selectors[site][index]}" in four_bt
            assert f"s CMUXHG.000.035:TESTSYN_ENOUT_SELECT.{lane} PRE_SYNENB" in four_bt

    # Three video outputs take three of the four shared CMUXHG lanes at
    # (0,14). Audio's two outputs then use FPLL (0,0) and its vertical
    # CMUXVG lanes. One PLL still cannot take five outputs: every
    # V11-reachable site has only counters C5..C8.
    source = source.replace('wire video_clock, audio_clock, video_second, audio_second;',
                            'wire video_clock, audio_clock, video_second, audio_second, video_third;')
    source = source.replace('.number_of_clocks(2)', '.number_of_clocks(3)', 1)
    source = source.replace('.output_clock_frequency1("50.0 MHz"),',
                            '.output_clock_frequency2("100.0 MHz"), .phase_shift2("0 ps"), '
                            '.duty_cycle2(50), .output_clock_frequency1("50.0 MHz"),', 1)
    source = source.replace('.outclk({video_second, video_clock})',
                            '.outclk({video_third, video_second, video_clock})')
    source = source.replace('    wire [1:0] locked;', '    wire [1:0] locked;\n'
                            '    reg third_extra = 0;\n'
                            '    always @(posedge video_third) third_extra <= !third_extra;')
    source = source.replace("12'b0, video_extra", "11'b0, third_extra, video_extra")
    (out / "five-lanes.v").write_text(source)
    five = synth(out / "five-lanes.v", out / "five-lanes.json")
    assert sum(c["type"] == "altera_pll" for c in five["modules"]["top"]["cells"].values()) == 2
    five_case = out / "five-lanes"
    five_case.mkdir(parents=True, exist_ok=True)
    five_path = five_case / "synth.json"
    five_path.write_text(json.dumps(five))
    log = run(command + ["--json", str(five_path), "--rbf", str(five_case / "top.rbf"),
                         "--report", str(five_case / "timing.json"),
                         "--write", str(five_case / "routed.json")],
              five_case / "route.log")
    assert re.search(r"PLL 'video_pll': .* counters C6,7,5, bel altera_pll\.0\.14\.0", log), log
    assert re.search(r"PLL 'audio_pll': .* counters C6,7, bel altera_pll\.0\.0\.0", log), log
    five_report = json.loads((five_case / "timing.json").read_text())
    assert five_report["utilization"]["altera_pll"] == {"used": 2, "available": 6}
    # CLKBUF cells stay in the MISTRAL_CLKENA bucket when bound to a vertical bel.
    assert five_report["utilization"]["MISTRAL_CLKENA"]["used"] == 5
    for name, mhz in (("video_clock", 25), ("video_second", 50), ("video_third", 100),
                      ("audio_clock", 40), ("audio_second", 50)):
        clock = five_report["fmax"][name]
        assert clock["constraint"] == mhz, clock
        assert clock["achieved"] >= clock["constraint"], clock
    routed = json.loads((five_case / "routed.json").read_text())["modules"]["top"]["cells"]
    assert routed["video_pll"]["attributes"]["NEXTPNR_BEL"] == "altera_pll.0.14.0"
    assert routed["audio_pll"]["attributes"]["NEXTPNR_BEL"] == "altera_pll.0.0.0"
    assert routed["video_pll"]["attributes"]["MISTRAL_PLL_COUNTERS"] == "6,7,5"
    assert routed["audio_pll"]["attributes"]["MISTRAL_PLL_COUNTERS"] == "6,7"
    run([str(args.mistral_cv.resolve()), "decomp", "5CSEBA6U23I7",
         str(five_case / "top.rbf"), str(five_case / "top.bt")], five_case / "decomp.log")
    five_bt = (five_case / "top.bt").read_text().splitlines()
    # (0,14) C6/C7/C5 -> CMUXHG PLLIN14/13/15, lanes 2/3/1.
    # (0,0) C6/C7 -> CMUXVG (42,0) PLLIN1/2, vertical lanes 0/1.
    expected = {
        "video_pll": (
            ("MISTRAL_CLKENA.0.35.0", "s CMUXHG.000.035:INPUT_SEL.2 16"),
            ("MISTRAL_CLKENA.0.35.1", "s CMUXHG.000.035:INPUT_SEL.3 15"),
            ("MISTRAL_CLKENA.0.35.2", "s CMUXHG.000.035:INPUT_SEL.1 17"),
        ),
        "audio_pll": (
            ("MISTRAL_CLKENA_PLL.42.0.0", "s CMUXVG.042.000:INPUT_SEL.0 09"),
            ("MISTRAL_CLKENA_PLL.42.0.1", "s CMUXVG.042.000:INPUT_SEL.1 0a"),
        ),
    }
    for pll, rows in expected.items():
        for index, bit in enumerate(five["modules"]["top"]["cells"][pll]["connections"]["outclk"]):
            buffer_name = next(name for name, cell in five["modules"]["top"]["cells"].items()
                               if cell["type"] == "MISTRAL_CLKBUF" and cell["connections"]["A"] == [bit])
            bel = routed[buffer_name]["attributes"]["NEXTPNR_BEL"]
            assert bel == rows[index][0], (pll, index, bel)
            assert rows[index][1] in five_bt, rows[index][1]
    for lane in (1, 2, 3):
        assert f"s CMUXHG.000.035:TESTSYN_ENOUT_SELECT.{lane} PRE_SYNENB" in five_bt
    for line in ("s FPLL.000.014:CLKIN_0_SRC 4", "s FPLL.000.000:CLKIN_0_SRC 4",
                 "s FPLL.000.014:FPLL_ENABLE 1", "s FPLL.000.000:FPLL_ENABLE 1"):
        assert line in five_bt, line

    # Five outputs of one PLL: the solver accepts these dividers of a 600 MHz
    # VCO, and packing still rejects them because V11 reaches no site with
    # five counters. (89,0) has eight, and V11 does not reach it.
    overflow = """module top(input wire FPGA_CLK1_50);
    wire [4:0] clk;
    wire locked;
    reg [4:0] tog = 0;
    always @(posedge clk[0]) tog[0] <= !tog[0];
    always @(posedge clk[1]) tog[1] <= !tog[1];
    always @(posedge clk[2]) tog[2] <= !tog[2];
    always @(posedge clk[3]) tog[3] <= !tog[3];
    always @(posedge clk[4]) tog[4] <= !tog[4];
    altera_pll #(.reference_clock_frequency("50.0 MHz"), .number_of_clocks(5),
        .output_clock_frequency0("150.0 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50),
        .output_clock_frequency1("120.0 MHz"), .phase_shift1("0 ps"), .duty_cycle1(50),
        .output_clock_frequency2("100.0 MHz"), .phase_shift2("0 ps"), .duty_cycle2(50),
        .output_clock_frequency3("75.0 MHz"), .phase_shift3("0 ps"), .duty_cycle3(50),
        .output_clock_frequency4("60.0 MHz"), .phase_shift4("0 ps"), .duty_cycle4(50),
        .operation_mode("direct"), .fractional_vco_multiplier("false"))
        pll (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(clk), .locked(locked));
    cyclonev_hps_interface_mpu_general_purpose hps_gp (
        .gp_in({26'b0, tog, locked}), .gp_out());
endmodule
"""
    (out / "five-outputs.v").write_text(overflow)
    overflow_design = synth(out / "five-outputs.v", out / "five-outputs.json")
    assert sum(c["type"] == "altera_pll" for c in overflow_design["modules"]["top"]["cells"].values()) == 1
    log = run(command + ["--json", str(out / "five-outputs.json")],
              out / "invalid-five-outputs.log", success=False)
    assert "no available dedicated PLL/clock-buffer pair" in log, log

    # Other references at the second site are covered by the general solver
    # (fixtures/general/refs places a 27 MHz reference at FPLL (0,31)).

    # A real observed crossing must remain cross-domain, even with equal output periods.
    source = (fixture / "two_pll.v").read_text().replace(
        "    wire [1:0] locked;", "    wire [1:0] locked;\n    reg crossing = 0;\n"
        "    always @(posedge audio_clock) crossing <= video_count[0];")
    source = source.replace("14'b0, locked", "13'b0, crossing, locked")
    (out / "crossing.v").write_text(source)
    crossing = synth(out / "crossing.v", out / "crossing.json")
    for name, equal in (("crossing-unrelated", False), ("crossing-equal-period", True)):
        variant = copy.deepcopy(crossing)
        if equal:
            params = variant["modules"]["top"]["cells"]["audio_pll"]["parameters"]
            params["fractional_vco_multiplier"] = "false"
            params["output_clock_frequency0"] = "25.0 MHz"
        cross_report, log = route(out / name, variant)
        assert "cross-domain path 'posedge video_clock' -> 'posedge audio_clock'" in log, log
        paths = [p for p in cross_report["critical_paths"]
                 if p["from"] == "posedge video_clock" and p["to"] == "posedge audio_clock"]
        assert paths, cross_report

    # V11 reaches three FPLL sites: a third PLL uses FPLL (0,0) and its
    # vertical clock lanes; a fourth exceeds the dedicated reference sites.
    source = (fixture / "two_pll.v").read_text()

    def extra_pll(name, mhz):
        return f'''
    wire {name}_clock;
    reg {name}_count = 0;
    always @(posedge {name}_clock) {name}_count <= !{name}_count;
    altera_pll #(.reference_clock_frequency("50.0 MHz"), .number_of_clocks(1),
        .output_clock_frequency0("{mhz} MHz"), .phase_shift0("0 ps"), .duty_cycle0(50),
        .operation_mode("direct"), .fractional_vco_multiplier("false"))
        {name}_pll (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk({name}_clock), .locked());
'''
    third_source = source.replace("    cyclonev_hps_interface", extra_pll("third", "40.0") + "    cyclonev_hps_interface")
    third_source = third_source.replace("14'b0, locked", "13'b0, third_count, locked")
    (out / "third.v").write_text(third_source)
    third_design = synth(out / "third.v", out / "third.json")
    assert sum(c["type"] == "altera_pll" for c in third_design["modules"]["top"]["cells"].values()) == 3
    log = run(command + ["--json", str(out / "third.json")], out / "third.log")
    assert re.search(r"PLL 'third_pll': .* bel altera_pll\.0\.0\.0", log), log
    fourth_source = source.replace("    cyclonev_hps_interface", extra_pll("third", "40.0") + extra_pll("fourth", "20.0") +
                                   "    cyclonev_hps_interface")
    fourth_source = fourth_source.replace("14'b0, locked", "12'b0, fourth_count, third_count, locked")
    (out / "fourth.v").write_text(fourth_source)
    fourth_design = synth(out / "fourth.v", out / "fourth.json")
    assert sum(c["type"] == "altera_pll" for c in fourth_design["modules"]["top"]["cells"].values()) == 4
    log = run(command + ["--json", str(out / "fourth.json")], out / "invalid-fourth.log", success=False)
    assert "no available dedicated PLL/clock-buffer pair" in log, log
    conflicting = out / "conflicting.sdc"
    conflicting.write_text("create_clock -name FPGA_CLK1_50 -period 40 [get_ports {FPGA_CLK1_50}]\n")
    invalid_command = command.copy()
    invalid_command[invalid_command.index("--sdc") + 1] = str(conflicting)
    log = run(invalid_command + ["--json", str(out / "synth.json")],
              out / "invalid-reference.log", success=False)
    assert "conflicting clock constraint" in log, log
    print("PASS: two independent PLLs, full FPLL oracle, cross-domain timing and rejection checks", report["fmax"])
    print("RBF sha256", hashlib.sha256((out / "top.rbf").read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
