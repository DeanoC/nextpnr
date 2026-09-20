#!/usr/bin/env python3
"""Exercise real FES cart merge on a shell with independent clock domains."""

import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    shell = output / "shell.v"
    shell.write_text('''module top(input clk_a, input clk_b, input address,
                                 output qa, output qb, output data);
    (* keep *) wire plug_addr;
    (* keep *) wire plug_rdata_d = 1'b0;
    MISTRAL_FF plug_addr_ff_0 (.CLK(clk_a), .DATAIN(address), .Q(plug_addr),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    MISTRAL_FF second_domain (.CLK(clk_b), .DATAIN(address), .Q(qb),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    MISTRAL_FF plug_rdata_ff_0 (.CLK(clk_a), .DATAIN(plug_rdata_d), .Q(data),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    wire [9:0] shell_data;
    MISTRAL_M10K #(.CFG_ABITS(10), .CFG_DBITS(10), .CFG_ASYNC_READ(1)) shell_memory (
        .CLK1(clk_a), .A1ADDR(10'b0), .A1DATA(10'b0), .A1EN(address),
        .B1ADDR(10'b0), .B1DATA(shell_data), .B1EN(1'b1),
        .ACLR0(1'b0), .ACLR1(1'b0));
    assign qa = plug_addr ^ shell_data[0];
endmodule
''')
    cart = output / "cart.v"
    cart.write_text('''module cart(input FPGA_CLK1_50, input plug_addr,
                                  output plug_rdata);
    wire [9:0] data;
    MISTRAL_M10K #(.CFG_ABITS(10), .CFG_DBITS(10), .CFG_ASYNC_READ(1)) memory (
        .CLK1(FPGA_CLK1_50), .A1ADDR(10'b0), .A1DATA(10'b0), .A1EN(1'b0),
        .B1ADDR({9'b0, plug_addr}), .B1DATA(data), .ACLR0(1'b0), .ACLR1(1'b0));
    wire [9:0] a_data, b_data;
    wire state;
    MISTRAL_FF state_ff (.CLK(FPGA_CLK1_50), .DATAIN(plug_addr), .Q(state),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    MISTRAL_M10K_TDP #(.CFG_ABITS(10), .CFG_DBITS(10), .CFG_ASYNC_READ(1)) writable (
        .CLK1(FPGA_CLK1_50), .CLK2(FPGA_CLK1_50),
        .A1ADDR({9'b0, plug_addr}), .B1ADDR(10'b0),
        .A1DATA({9'b0, state}), .B1DATA(10'b0), .A1Q(a_data), .B1Q(b_data),
        .A1EN(1'b1), .B1EN(1'b1), .A1WE(plug_addr), .B1WE(1'b0),
        .A1BE(2'b11), .B1BE(2'b11), .ACLR0(1'b0), .ACLR1(1'b0));
    assign plug_rdata = data[0] ^ a_data[0] ^ b_data[0] ^ state;
endmodule
''')
    for name, top in (("shell", "top"), ("cart", "cart")):
        command = [str(args.yosys.resolve()), "-p",
                   f"read_verilog {output / (name + '.v')}; "
                   f"synth_intel_alm -nolutram -nodsp -top {top}; "
                   f"write_json {output / (name + '.json')}"]
        with (output / (name + "-synth.log")).open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    # A real scaffold was packed before being frozen; packing also creates
    # constant nets and the physical clock buffers used by cart merge.
    raw = output / "shell-unpacked.json"
    (output / "shell.json").rename(raw)
    qsf = output / "shell.qsf"
    qsf.write_text("\n".join(f"set_location_assignment PIN_{pin} -to {port}"
                             for pin, port in (("V11", "clk_a"), ("U10", "clk_b"),
                                               ("W8", "address"), ("AG5", "qa"),
                                               ("AD19", "qb"), ("AD12", "data"))) + "\n")
    with (output / "shell-pack.log").open("w") as log:
        subprocess.run([str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
                        "--qsf", str(qsf), "--json", str(raw), "--pack-only",
                        "--write", str(output / "shell.json")],
                       stdout=log, stderr=subprocess.STDOUT, check=True)

    def merge(name, clock=None, expected=None, shell_name="shell", cart_name="cart"):
        command = [str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
                   "--json", str(output / (shell_name + ".json")),
                   "--fes-cart", str(output / (cart_name + ".json")),
                   "--no-pack", "--no-place", "--no-route",
                   "--write", str(output / (name + ".json"))]
        if clock is not None:
            command += ["--fes-slot-clock", clock]
        result = subprocess.run(command, capture_output=True, text=True)
        (output / (name + ".log")).write_text(result.stdout + result.stderr)
        if expected:
            assert result.returncode != 0 and expected in result.stdout + result.stderr, result
            return
        assert result.returncode == 0, result.stdout + result.stderr
        return json.loads((output / (name + ".json")).read_text())["modules"]["top"]

    merge("ambiguous", expected="multiple clocks")
    merge("missing", "missing_clock", expected="not a driven shell net")
    # Use the actual FF clock aliases after synthesis (before global promotion).
    source = json.loads((output / "shell.json").read_text())
    design = source["modules"]["top"]
    for clock_cell in ("plug_addr_ff_0", "second_domain"):
        bit = design["cells"][clock_cell]["connections"]["CLK"]
        name = next(name for name, net in design["netnames"].items() if net["bits"] == bit)
        merged = merge("selected-" + clock_cell, name)
        cells = merged["cells"]
        assert cells["fes_cart$memory"]["connections"]["CLK1"] == cells[clock_cell]["connections"]["CLK"]
        for cell, port in (("writable", "CLK1"), ("writable", "CLK2"), ("state_ff", "CLK")):
            assert cells["fes_cart$" + cell]["connections"][port] == cells[clock_cell]["connections"]["CLK"]
        ground = cells["$PACKER_GND_DRV"]["connections"]["Q"]
        supply = cells["$PACKER_VCC_DRV"]["connections"]["Q"]
        assert cells["fes_cart$memory"]["connections"]["A1EN"] == ground
        assert cells["fes_cart$writable"]["connections"]["B1WE"] == ground
        assert cells["fes_cart$writable"]["connections"]["B1EN"] == supply
        assert cells["fes_cart$writable"]["connections"]["A1BE"] == supply * 2
    cart_source = json.loads((output / "cart.json").read_text())
    cart_cells = cart_source["modules"]["cart"]["cells"]
    # An unsupported divided clock must fail, not become a full-rate clock.
    cart_cells["writable"]["connections"]["CLK2"] = cart_cells["state_ff"]["connections"]["Q"]
    (output / "derived-cart.json").write_text(json.dumps(cart_source))
    merge("derived", name, expected="does not use the declared socket clock", cart_name="derived-cart")
    cart_cells["writable"]["connections"]["CLK2"] = ["0"]
    (output / "constant-clock-cart.json").write_text(json.dumps(cart_source))
    merge("constant-clock", name, expected="does not use the declared socket clock", cart_name="constant-clock-cart")
    del cart_cells["writable"]["connections"]["CLK2"]
    (output / "missing-clock-cart.json").write_text(json.dumps(cart_source))
    merge("missing-cart-clock", name, expected="requires one declared socket clock input", cart_name="missing-clock-cart")
    # A routed shell carries packed physical pins, unlike the pack-only
    # fixture above. Re-entering normal RAM setup would reinterpret cleared
    # hard pins, while logical sink traversal used to throw dict::at().
    with (output / "shell-route.log").open("w") as log:
        subprocess.run([str(args.nextpnr.resolve()), "--device", "5CSEBA6U23I7",
                        "--qsf", str(qsf), "--json", str(raw), "--router", "router2",
                        "--write", str(output / "routed-shell.json")],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    routed = json.loads((output / "routed-shell.json").read_text())["modules"]["top"]
    bit = routed["cells"]["plug_addr_ff_0"]["connections"]["CLK"]
    clock = next(key for key, net in routed["netnames"].items() if net["bits"] == bit)
    merged = merge("routed-merge", clock, shell_name="routed-shell")
    def same_connections(cell, ports=None):
        before, after = routed["cells"][cell], merged["cells"][cell]
        for port, bits in before["connections"].items():
            if not bits or (ports is not None and port not in ports):
                continue
            assert len(bits) == len(after["connections"][port]), (cell, port)
            for index, bit in enumerate(bits):
                aliases = [(key, position) for key, net in routed["netnames"].items()
                           for position, value in enumerate(net["bits"]) if value == bit]
                assert any(key in merged["netnames"] and
                           merged["netnames"][key]["bits"][position] == after["connections"][port][index]
                           for key, position in aliases), (cell, port, index)
    for cell in ("shell_memory", "second_domain", "plug_addr_ff_0"):
        assert merged["cells"][cell]["parameters"] == routed["cells"][cell]["parameters"], cell
        assert merged["cells"][cell]["attributes"] == routed["cells"][cell]["attributes"], cell
        same_connections(cell)
    same_connections("plug_rdata_ff_0", {"SCLR", "ACLR", "ENA"})
    for constant in ("$PACKER_GND_NET", "$PACKER_VCC_NET"):
        assert merged["netnames"][constant]["attributes"]["ROUTING"] == routed["netnames"][constant]["attributes"]["ROUTING"]
    # The original one-clock diagnostic retains implicit clock selection.
    design["cells"]["second_domain"]["connections"]["CLK"] = design["cells"]["plug_addr_ff_0"]["connections"]["CLK"]
    (output / "single.json").write_text(json.dumps(source))
    merge("single", shell_name="single")
    print("FES explicit clock, missing clock, ambiguous shell and single-clock fallback: PASS")


if __name__ == "__main__":
    main()
