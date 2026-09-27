#!/usr/bin/env python3
"""Time the fabric paths into and out of the HPS FPGA-to-SDRAM hard block.

cyclonev_hps_interface_fpga2sdram carries Quartus 17.0.2 TimeQuest arcs
(mistral/delay.cc): setup/hold on its registered inputs and clock-to-output
on its outputs, each relative to the pin's port clock. Before that the
whole cell was ignored, so the registers feeding a command/write port and
capturing read data had no required time at the HPS boundary.

The design uses the FES/MiSTer f2h_sdram2 layout: command port 2 on clk_a is
fed by registers, and read port 3 on clk_b is captured by registers. Every
other port is tied off with a constant clock, as the FES cores do, and
wr_valid_3 is driven by a live register although its port clock is tied
off. Each clock thus has only one kind of internal path:

- the clk_a critical path must end at an fpga2sdram input with the modelled
  setup, and the clk_b one must start at an fpga2sdram output with the
  modelled clock-to-output;
- no constant net may become a clock domain, and the pins of the tied-off
  ports (including the live wr_valid_3) must stay untimed.

The script exits 0 when all checks hold and 1 otherwise.
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys

CELL = "cyclonev_hps_interface_fpga2sdram"

# Modelled arcs of the ports used here (ns), as in mistral/delay.cc.
SETUP = {"cmd_data_2": 1.466, "cmd_valid_2": 1.254}
CLOCK_TO_Q = {"rd_data_3": 1.431, "rd_valid_3": 1.239}

TOP = '''module top(input clk_a, input clk_b, input [2:0] din, output [2:0] dout);
    // Command port 2 (clk_a): registers straight into the hard block.
    reg [2:0] cmd = 3'd0;
    always @(posedge clk_a) cmd <= din;
    // Read port 3 (clk_b): registers straight out of it.
    wire [79:0] rd_data;
    wire rd_valid;
    reg [2:0] rd = 3'd0;
    always @(posedge clk_b) rd <= {rd_valid, rd_data[1:0]};
    assign dout = rd;

    cyclonev_hps_interface_fpga2sdram f2sdram (
        .cfg_axi_mm_select(6'h00), .cfg_cport_rfifo_map(18'h000d0), .cfg_cport_type(12'h03f),
        .cfg_cport_wfifo_map(18'h000d0), .cfg_port_width(12'h016),
        .cfg_rfifo_cport_map(16'h2100), .cfg_wfifo_cport_map(16'h2100),
        .cmd_port_clk_0(1'b0), .cmd_port_clk_1(1'b0), .cmd_port_clk_2(clk_a),
        .cmd_port_clk_3(1'b0), .cmd_port_clk_4(1'b0), .cmd_port_clk_5(1'b0),
        .cmd_valid_0(1'b0), .cmd_valid_1(1'b0), .cmd_valid_2(cmd[2]),
        .cmd_valid_3(1'b0), .cmd_valid_4(1'b0), .cmd_valid_5(1'b0),
        .cmd_data_2({58'd0, cmd[1:0]}),
        .wr_clk_0(1'b0), .wr_clk_1(1'b0), .wr_clk_2(1'b0), .wr_clk_3(1'b0),
        .wr_valid_0(1'b0), .wr_valid_1(1'b0), .wr_valid_2(1'b0), .wr_valid_3(cmd[2]),
        .wr_data_3(90'd0),
        .rd_clk_0(1'b0), .rd_clk_1(1'b0), .rd_clk_2(1'b0), .rd_clk_3(clk_b),
        .rd_ready_0(1'b1), .rd_ready_1(1'b1), .rd_ready_2(1'b1), .rd_ready_3(1'b1),
        .rd_data_3(rd_data), .rd_valid_3(rd_valid),
        .wrack_ready_0(1'b1), .wrack_ready_1(1'b1), .wrack_ready_2(1'b1),
        .wrack_ready_3(1'b1), .wrack_ready_4(1'b1), .wrack_ready_5(1'b1));
endmodule
'''

PINS = (("V11", "clk_a"), ("Y13", "clk_b"), ("Y24", "din[0]"), ("W24", "din[1]"), ("W21", "din[2]"),
        ("W15", "dout[0]"), ("AA24", "dout[1]"), ("V16", "dout[2]"))


def run(command, log):
    with log.open("w") as handle:
        return subprocess.run([str(part) for part in command], stdout=handle, stderr=subprocess.STDOUT).returncode


def group(port):
    return port.split("[", 1)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("yosys", "nextpnr", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    (out / "top.v").write_text(TOP)
    script = f"read_verilog {out / 'top.v'}; synth_intel_alm -nobram -nolutram -nodsp -top top; " \
             f"write_json {out / 'synth.json'}"
    if run([args.yosys.resolve(), "-p", script], out / "synth.log"):
        raise SystemExit(f"yosys failed; see {out / 'synth.log'}")
    cells = json.loads((out / "synth.json").read_text())["modules"]["top"]["cells"]
    atoms = [name for name, cell in cells.items() if cell["type"] == CELL]
    if len(atoms) != 1:
        raise SystemExit(f"expected one {CELL} after synthesis, found {atoms}")
    atom, = atoms
    (out / "pins.qsf").write_text("".join(
        f"set_location_assignment PIN_{pin} -to {port}\n"
        f"set_instance_assignment -name IO_STANDARD \"3.3-V LVTTL\" -to {port}\n" for pin, port in PINS))
    code = run([args.nextpnr.resolve(), "--device", "5CSEBA6U23I7", "--qsf", out / "pins.qsf",
                "--json", out / "synth.json", "--freq", "50", "--seed", "1",
                "--rbf", out / "top.rbf", "--report", out / "report.json", "--detailed-timing-report"],
               out / "route.log")
    log = (out / "route.log").read_text()
    if code != 0 or "Program finished normally." not in log:
        raise SystemExit(f"place/route failed (exit {code}); see {out / 'route.log'}")
    report = json.loads((out / "report.json").read_text())

    failures = []
    clocks = sorted(report["fmax"])
    print(f"clock domains: {clocks}")
    if len(clocks) != 2 or any("$PACKER" in clock for clock in clocks):
        failures.append(f"expected the two port clocks as the only domains, got {clocks}")
    if "$PACKER" in "".join(line for line in log.splitlines() if "has no interior paths" in line):
        failures.append("a constant net is reported as a clock")

    def check(label, kind, cell_end, expected):
        found = []
        for path in report["critical_paths"]:
            if path["from"] != path["to"]:
                continue
            for segment in path["path"]:
                end = segment[cell_end]
                if segment["type"] == kind and end["cell"] == atom:
                    found.append((path["from"], end["port"], segment["delay"]))
        for clock, port, delay in found:
            want = expected.get(group(port))
            ok = want is not None and abs(delay - want) < 5e-4
            print(f"{label}: {clock} critical path {kind} {port} {delay:.3f} ns "
                  f"(modelled {want if want is not None else '-'} ns) {'ok' if ok else 'MISMATCH'}")
            if not ok:
                failures.append(f"{label} {port} {kind} {delay:.3f} ns")
        if not found:
            failures.append(f"no critical path with an fpga2sdram {kind} segment")

    check("into the hard block", "setup", "to", SETUP)
    check("out of the hard block", "clk-to-q", "from", CLOCK_TO_Q)

    timed = sorted({endpoint["port"] for net in report["detailed_net_timings"]
                    for endpoint in net["endpoints"] if endpoint["cell"] == atom})
    print(f"timed fpga2sdram endpoints: {timed}")
    if not timed or any(group(port) not in SETUP for port in timed):
        failures.append(f"timed endpoints should be the live command port 2 pins only, got {timed}")

    if failures:
        print(f"FAIL: {'; '.join(failures)}")
        return 1
    print("PASS: fpga2sdram setup and clock-to-output are timed and tied-off ports stay untimed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
