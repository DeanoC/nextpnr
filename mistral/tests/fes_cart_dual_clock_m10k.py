#!/usr/bin/env python3
"""FES cart merge must drive every cart input: the socket clock and undriven nets.

Part 1: both clocks of a dual-clock MISTRAL_M10K must be on the socket clock.

Yosys maps an ordinary inferred RAM (one clock, one write port, a registered
read) to MISTRAL_M10K with CFG_DUAL_CLOCK=1 and CLK1 = CLK2 = the buffered
cart clock. merge_fes_cart drops that clock buffer and reconnects CLK1, but
its reconnect list for MISTRAL_M10K omits CLK2 (MISTRAL_M10K_TDP gets both),
so the read clock is left on an undriven fes_cart$bitN net. Placement,
routing, timing and the CRAM fence all pass; on hardware the read register
never loads and every read returns zero (FES Apple II probe card, 2026-09-27).

Part 2: a cart input that is not a socket port (here an extra top-level
input) would reach its logic through a net nothing drives; the merge must
reject it instead of placing and routing a floating input.

The script exits 0 when both checks hold and 1 otherwise. --merge-only skips
the scaffold place/route phase.
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys

SHELL = '''module top(input clk, input address, output data, output q);
    (* keep *) wire plug_addr;
    (* keep *) wire plug_rdata_d = 1'b0;
    (* BEL = "MISTRAL_FF.24.1.2" *) MISTRAL_FF plug_addr_ff_0 (.CLK(clk), .DATAIN(address), .Q(plug_addr),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    (* BEL = "MISTRAL_FF.28.1.2" *) MISTRAL_FF plug_rdata_ff_0 (.CLK(clk), .DATAIN(plug_rdata_d), .Q(data),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    reg [7:0] count = 8'd0;
    always @(posedge clk) count <= count + 8'd1;
    assign q = plug_addr ^ count[7];
endmodule
'''

# The Apple II probe card's $C800 RAM, reduced to one socket input bit.
CART = '''module cart(input FPGA_CLK1_50, input plug_addr, output plug_rdata);
    reg [9:0] address = 10'd0;
    reg [7:0] memory [0:1023];
    reg [7:0] q = 8'd0;
    always @(posedge FPGA_CLK1_50) begin
        address <= address + 10'd1;
        if (plug_addr)
            memory[address] <= address[7:0];
        q <= memory[address];
    end
    assign plug_rdata = ^q;
endmodule
'''

# A cart input that no socket port provides.
UNMAPPED_CART = '''module cart(input FPGA_CLK1_50, input plug_addr, input stray, output plug_rdata);
    reg q = 1'b0;
    always @(posedge FPGA_CLK1_50) q <= plug_addr ^ stray;
    assign plug_rdata = q;
endmodule
'''


def run(command, log):
    with log.open("w") as handle:
        return subprocess.run([str(part) for part in command], stdout=handle, stderr=subprocess.STDOUT).returncode


def top_module(path):
    return json.loads(path.read_text())["modules"]["top"]


def net_of(module, bits):
    return next((name for name, net in module["netnames"].items() if net["bits"] == bits), str(bits))


def drivers(module, bits):
    return [(name, port) for name, cell in module["cells"].items()
            for port, value in cell["connections"].items()
            if cell.get("port_directions", {}).get(port) == "output" and set(value) & set(bits)]


def check_clocks(label, module, clock_bits):
    rams = {name: cell for name, cell in module["cells"].items()
            if name.startswith("fes_cart$") and cell["type"] == "MISTRAL_M10K"}
    if len(rams) != 1:
        raise SystemExit(f"{label}: expected one merged cart MISTRAL_M10K, found {sorted(rams)}")
    (name, ram), = rams.items()
    failures = []
    for port in ("CLK1", "CLK2"):
        bits = ram["connections"].get(port, [])
        state = "socket clock" if bits == clock_bits else \
            f"{net_of(module, bits)} (drivers: {drivers(module, bits) or 'none'})"
        print(f"{label}: {name}.{port} -> {state}")
        if bits != clock_bits:
            failures.append(port)
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("yosys", "nextpnr", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--merge-only", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    nextpnr = [args.nextpnr.resolve(), "--device", "5CSEBA6U23I7"]

    for name, source, top in (("shell", SHELL, "top"), ("cart", CART, "cart"), ("unmapped", UNMAPPED_CART, "cart")):
        (out / (name + ".v")).write_text(source)
        script = f"read_verilog {out / (name + '.v')}; synth_intel_alm -nolutram -nodsp -top {top}; " \
                 f"write_json {out / (name + '-synth.json')}"
        if run([args.yosys.resolve(), "-p", script], out / (name + "-synth.log")):
            raise SystemExit(f"yosys failed for {name}; see {out / (name + '-synth.log')}")

    # Precondition: synthesis chose a dual-clock M10K whose two clocks share the cart clock.
    cart = json.loads((out / "cart-synth.json").read_text())["modules"]["cart"]["cells"]
    ram = next(cell for cell in cart.values() if cell["type"] == "MISTRAL_M10K")
    assert int(ram["parameters"].get("CFG_DUAL_CLOCK", "0"), 2) == 1, ram["parameters"]
    assert ram["connections"]["CLK1"] == ram["connections"]["CLK2"], ram["connections"]
    print("precondition: cart RAM is MISTRAL_M10K CFG_DUAL_CLOCK=1 with CLK1 == CLK2")

    qsf = out / "shell.qsf"
    qsf.write_text("".join(f"set_location_assignment PIN_{pin} -to {port}\n" for pin, port in
                           (("V11", "clk"), ("W8", "address"), ("AD12", "data"), ("AG5", "q"))))
    if run(nextpnr + ["--qsf", qsf, "--json", out / "shell-synth.json", "--pack-only",
                      "--write", out / "shell.json"], out / "shell-pack.log"):
        raise SystemExit("shell pack failed")
    shell = top_module(out / "shell.json")
    clock = net_of(shell, shell["cells"]["plug_addr_ff_0"]["connections"]["CLK"])

    if run(nextpnr + ["--json", out / "shell.json", "--fes-cart", out / "cart-synth.json",
                      "--fes-slot-clock", clock, "--no-pack", "--no-place", "--no-route",
                      "--write", out / "merged.json"], out / "merge.log"):
        raise SystemExit(f"cart merge failed; see {out / 'merge.log'}")
    # Bit numbers are per JSON file: compare within the merged design.
    merged = top_module(out / "merged.json")
    failures = check_clocks("merge", merged, merged["cells"]["plug_addr_ff_0"]["connections"]["CLK"])

    if not args.merge_only:
        if run(nextpnr + ["--qsf", qsf, "--json", out / "shell-synth.json", "--router", "router2",
                          "--write", out / "routed-shell.json"], out / "shell-route.log"):
            raise SystemExit("shell route failed")
        routed_shell = top_module(out / "routed-shell.json")
        routed_clock_bits = routed_shell["cells"]["plug_addr_ff_0"]["connections"]["CLK"]
        routed_clock = net_of(routed_shell, routed_clock_bits)
        code = run(nextpnr + ["--json", out / "routed-shell.json", "--fes-cart", out / "cart-synth.json",
                              "--fes-slot-clock", routed_clock, "--fes-scaffold", "--no-pack",
                              "--router", "router2", "--seed", "2", "--fes-cram-region", "0,0,7605,7024",
                              "--write", out / "scaffold-routed.json", "--rbf", out / "scaffold.rbf",
                              "--compress-rbf"], out / "scaffold-route.log")
        log = (out / "scaffold-route.log").read_text()
        print(f"scaffold place/route exit {code}; "
              f"{'finished normally' if 'Program finished normally.' in log else 'did not finish'}")
        routed_path = out / "scaffold-routed.json"
        if code != 0 or "Program finished normally." not in log or not routed_path.is_file():
            failures.append(f"scaffold place/route failed (exit {code}); see {out / 'scaffold-route.log'}")
        else:
            routed = top_module(routed_path)
            failures += [f"routed {port}" for port in
                         check_clocks("routed", routed, routed["cells"]["plug_addr_ff_0"]["connections"]["CLK"])]

    code = run(nextpnr + ["--json", out / "shell.json", "--fes-cart", out / "unmapped-synth.json",
                          "--fes-slot-clock", clock, "--no-pack", "--no-place", "--no-route",
                          "--write", out / "unmapped-merged.json"], out / "unmapped-merge.log")
    text = (out / "unmapped-merge.log").read_text()
    if code != 0 and "which nothing drives" in text:
        print("unmapped cart input: merge rejected it")
    else:
        print(f"unmapped cart input: merge exit {code} without the undriven-input error")
        failures.append("unmapped cart input accepted")

    if failures:
        print(f"FAIL: {', '.join(failures)}")
        return 1
    print("PASS: the dual-clock cart M10K is on the socket clock and an unmapped cart input is rejected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
