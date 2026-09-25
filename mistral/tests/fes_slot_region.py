#!/usr/bin/env python3
"""Place carts of many control sets inside a small FES_RESERVED_RECT.

The reserved rectangle keeps two frozen shell FFs (X24 Y1 and X28 Y1), so
their LABs are unusable. A cart whose control sets fit the remaining LABs must
place with both placers in bounded time; carts that provably cannot fit must
stop before placement with the failing capacity figure.
"""

import argparse
import json
from pathlib import Path
import subprocess

RECT = "24 1 28 3"
# Columns 24, 25, 27 and 28 hold LABs; 26 is the M10K column. Rows 1-3 give
# twelve LABs; the frozen FFs remove X24 Y1 and X28 Y1.
USABLE_LABS = 10


def cart_source(groups, adder):
    return f'''module cart(input FPGA_CLK1_50, input plug_addr, output plug_rdata);
    reg [7:0] shift = 8'd0;
    always @(posedge FPGA_CLK1_50) shift <= {{shift[6:0], plug_addr}};
    reg [{adder - 1}:0] counter = {adder}'d0;
    always @(posedge FPGA_CLK1_50) counter <= counter + {{{{{adder - 1}{{1'b0}}}}, shift[0]}};
    wire [{groups - 1}:0] taps;
    genvar i;
    generate for (i = 0; i < {groups}; i = i + 1) begin : g
        // Distinct synchronous clear and enable per group: one SCLR per LAB.
        wire clear = shift[i % 8] & shift[(i + 3) % 8] & counter[i % {adder}];
        wire write = shift[(i + 1) % 8] ^ counter[(i + 5) % {adder}];
        reg [7:0] value = 8'd0;
        always @(posedge FPGA_CLK1_50)
            if (clear) value <= 8'd0;
            else if (write) value <= shift ^ counter[7:0] ^ i[7:0];
        assign taps[i] = ^value;
    end endgenerate
    assign plug_rdata = (^taps) ^ counter[{adder - 1}];
endmodule
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("yosys", "nextpnr", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    nextpnr = str(args.nextpnr.resolve())
    shell = output / "shell.v"
    shell.write_text('''module top(input clk_a, input address, output qa, output data);
    (* keep *) wire plug_addr;
    (* keep *) wire plug_rdata_d = 1'b0;
    (* BEL = "MISTRAL_FF.24.1.2" *) MISTRAL_FF plug_addr_ff_0 (.CLK(clk_a), .DATAIN(address), .Q(plug_addr),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    (* BEL = "MISTRAL_FF.28.1.2" *) MISTRAL_FF plug_rdata_ff_0 (.CLK(clk_a), .DATAIN(plug_rdata_d), .Q(data),
        .ACLR(1'b1), .ENA(1'b1), .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0));
    reg [7:0] arithmetic = 8'd0;
    always @(posedge clk_a) arithmetic <= arithmetic + 8'd1;
    assign qa = plug_addr ^ arithmetic[7];
endmodule
''')

    def synth(name, top, source):
        command = [str(args.yosys.resolve()), "-p",
                   f"read_verilog {source}; synth_intel_alm -nolutram -nodsp -top {top}; "
                   f"write_json {output / (name + '.json')}"]
        with (output / (name + "-synth.log")).open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)

    synth("shell-unpacked", "top", shell)
    qsf = output / "shell.qsf"
    qsf.write_text("\n".join(f"set_location_assignment PIN_{pin} -to {port}"
                             for pin, port in (("V11", "clk_a"), ("W8", "address"),
                                               ("AG5", "qa"), ("AD12", "data"))) + "\n")
    with (output / "shell-route.log").open("w") as log:
        subprocess.run([nextpnr, "--device", "5CSEBA6U23I7", "--qsf", str(qsf),
                        "--json", str(output / "shell-unpacked.json"), "--router", "router2",
                        "--write", str(output / "routed-shell.json")],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    routed = json.loads((output / "routed-shell.json").read_text())["modules"]["top"]
    assert routed["cells"]["plug_addr_ff_0"]["attributes"]["NEXTPNR_BEL"] == "MISTRAL_FF.24.1.2"
    assert routed["cells"]["plug_rdata_ff_0"]["attributes"]["NEXTPNR_BEL"] == "MISTRAL_FF.28.1.2"
    bit = routed["cells"]["plug_addr_ff_0"]["connections"]["CLK"]
    clock = next(key for key, net in routed["netnames"].items() if net["bits"] == bit)
    reserved_qsf = output / "cart.qsf"
    reserved_qsf.write_text(qsf.read_text() + f'set_global_assignment -name FES_RESERVED_RECT "{RECT}"\n')
    # The routed shell carries its placer setting; a CLI --placer is replaced
    # when the JSON loads, so select SA through a copy of the scaffold.
    sa_shell = json.loads((output / "routed-shell.json").read_text())
    sa_shell["modules"]["top"]["settings"]["placer"] = "sa"
    (output / "routed-shell-sa.json").write_text(json.dumps(sa_shell))

    # A legacy FES_RESERVED_BEL declared before the default (unnamed, region
    # "cart") FES_RESERVED_RECT it falls inside must not fail as a "region
    # already declared" duplicate: both target the same region and are meant
    # to compose, in either declaration order.
    bel_then_rect_qsf = output / "bel-then-rect.qsf"
    bel_then_rect_qsf.write_text(
        qsf.read_text() +
        'set_global_assignment -name FES_RESERVED_BEL "MISTRAL_FF.24.1.2"\n'
        f'set_global_assignment -name FES_RESERVED_RECT "{RECT}"\n')
    result = subprocess.run([nextpnr, "--device", "5CSEBA6U23I7", "--json", str(output / "routed-shell.json"),
                              "--qsf", str(bel_then_rect_qsf), "--fes-scaffold", "--no-pack", "--no-place",
                              "--no-route"],
                             capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "already declared" not in result.stdout + result.stderr, result.stdout + result.stderr

    def place(name, cart, shell_json="routed-shell.json", timeout=600):
        command = [nextpnr, "--device", "5CSEBA6U23I7", "--json", str(output / shell_json),
                   "--qsf", str(reserved_qsf), "--fes-cart", str(output / (cart + ".json")),
                   "--fes-slot-clock", clock, "--fes-scaffold", "--no-pack", "--no-route",
                   "--seed", "3", "--write", str(output / (name + ".json"))]
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        text = result.stdout + result.stderr
        (output / (name + ".log")).write_text(text)
        return result.returncode, text

    def assert_inside(name):
        cells = json.loads((output / (name + ".json")).read_text())["modules"]["top"]["cells"]
        x0, y0, x1, y1 = (int(v) for v in RECT.split())
        placed = 0
        for cell_name, cell in cells.items():
            if not cell_name.startswith("fes_cart$"):
                continue
            bel = cell.get("attributes", {}).get("NEXTPNR_BEL", "")
            if not bel:
                continue
            _, x, y, _ = bel.split(".")
            assert x0 <= int(x) <= x1 and y0 <= int(y) <= y1, (cell_name, bel)
            assert not (int(x) == 24 and int(y) == 1) and not (int(x) == 28 and int(y) == 1), (cell_name, bel)
            placed += 1
        assert placed > 50, placed
        for frozen in ("plug_addr_ff_0", "plug_rdata_ff_0"):
            assert cells[frozen]["attributes"]["NEXTPNR_BEL"] == routed["cells"][frozen]["attributes"]["NEXTPNR_BEL"]

    # Six SCLR groups fit ten LABs; HeAP must finish inside the bound.
    (output / "cart-fit.v").write_text(cart_source(groups=6, adder=16))
    synth("cart-fit", "cart", output / "cart-fit.v")
    code, text = place("fit-heap", "cart-fit")
    assert code == 0, text
    assert "FES slot region 'cart' constrains" in text, text
    assert "FES paired" in text, text
    assert f"FES slot capacity (region 'cart'): {USABLE_LABS} usable LABs (2 frozen)" in text, text
    assert "carry chains 1 (longest 1 LAB rows)" in text, text
    assert_inside("fit-heap")
    # SA cannot keep cart clusters legal; it must refuse rather than emit an
    # invalid placement.
    code, text = place("fit-sa", "cart-fit", "routed-shell-sa.json", timeout=300)
    assert code != 0, text
    assert "use --placer heap for cart placement" in text, text

    # Fourteen distinct SCLR nets need fourteen LABs: refuse before placing.
    (output / "cart-ctrl.v").write_text(cart_source(groups=14, adder=16))
    synth("cart-ctrl", "cart", output / "cart-ctrl.v")
    code, text = place("ctrl", "cart-ctrl", timeout=300)
    assert code != 0, text
    assert "FF control sets need at least 14 LABs but 10 are usable" in text, text
    assert "FES cart does not fit region 'cart'" in text, text
    assert "Creating initial" not in text, text

    # A 64-cell carry chain needs four vertically adjacent LABs; three exist.
    (output / "cart-chain.v").write_text(cart_source(groups=2, adder=64))
    synth("cart-chain", "cart", output / "cart-chain.v")
    code, text = place("chain", "cart-chain", timeout=300)
    assert code != 0, text
    assert "a carry chain spans 4 LAB rows but the longest usable column run is 3" in text, text
    print("fes_slot_region: ok")

    # Two disjoint named regions on one shell (for example a cartridge slot
    # and an expansion slot present on the same core) must legalise
    # independently, and a cart may never land on another region's BELs.
    dual_qsf = output / "dual.qsf"
    dual_qsf.write_text(qsf.read_text() +
                         'set_global_assignment -name FES_RESERVED_RECT "porta 24 1 25 3"\n'
                         'set_global_assignment -name FES_RESERVED_RECT "portb 27 1 28 3"\n')
    (output / "cart-dual.v").write_text(cart_source(groups=2, adder=16))
    synth("cart-dual", "cart", output / "cart-dual.v")

    def place_region(name, region):
        command = [nextpnr, "--device", "5CSEBA6U23I7", "--json", str(output / "routed-shell.json"),
                   "--qsf", str(dual_qsf), "--fes-cart", str(output / "cart-dual.json"),
                   "--fes-cart-region", region, "--fes-slot-clock", clock, "--fes-scaffold",
                   "--no-pack", "--no-route", "--seed", "3", "--write", str(output / (name + ".json"))]
        result = subprocess.run(command, capture_output=True, text=True, timeout=300)
        text = result.stdout + result.stderr
        (output / (name + ".log")).write_text(text)
        return result.returncode, text

    def assert_inside_rect(name, rect):
        cells = json.loads((output / (name + ".json")).read_text())["modules"]["top"]["cells"]
        x0, y0, x1, y1 = rect
        placed = 0
        for cell_name, cell in cells.items():
            if not cell_name.startswith("fes_cart$"):
                continue
            bel = cell.get("attributes", {}).get("NEXTPNR_BEL", "")
            if not bel:
                continue
            _, x, y, _ = bel.split(".")
            assert x0 <= int(x) <= x1 and y0 <= int(y) <= y1, (name, cell_name, bel)
            placed += 1
        assert placed > 0, (name, placed)

    code, text = place_region("dual-porta", "porta")
    assert code == 0, text
    assert "FES slot region 'porta' constrains" in text, text
    assert_inside_rect("dual-porta", (24, 1, 25, 3))

    code, text = place_region("dual-portb", "portb")
    assert code == 0, text
    assert "FES slot region 'portb' constrains" in text, text
    assert_inside_rect("dual-portb", (27, 1, 28, 3))

    # An unknown region name fails closed instead of silently landing
    # anywhere on the device.
    code, text = place_region("dual-unknown", "portc")
    assert code != 0, text
    assert "has no matching FES_RESERVED_RECT" in text, text
    print("fes_slot_region: dual-region ok")

    # A big card can claim several small declared regions at once via
    # FES_RESERVED_RECT_GROUP: columns 33-36 are four one-column LAB/MLAB
    # slots (no M10K, no frozen shell FFs there), grouped into "big".
    group_qsf = output / "group.qsf"
    group_qsf.write_text(
        qsf.read_text() +
        ''.join(f'set_global_assignment -name FES_RESERVED_RECT "s{i} {33 + i - 1} 1 {33 + i - 1} 3"\n'
                for i in range(1, 5)) +
        'set_global_assignment -name FES_RESERVED_RECT_GROUP "big s1 s2 s3 s4"\n')

    def place_named(name, region, cart_json, qsf_path, timeout=300):
        command = [nextpnr, "--device", "5CSEBA6U23I7", "--json", str(output / "routed-shell.json"),
                   "--qsf", str(qsf_path), "--fes-cart", str(output / cart_json),
                   "--fes-cart-region", region, "--fes-slot-clock", clock, "--fes-scaffold",
                   "--no-pack", "--no-route", "--seed", "3", "--write", str(output / (name + ".json"))]
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        text = result.stdout + result.stderr
        (output / (name + ".log")).write_text(text)
        return result.returncode, text

    # The six-SCLR-group cart needed 10 usable LABs against the original
    # 24-28 socket; the four grouped columns give 12 usable LABs (no frozen
    # occupant in this range), so it must fit as one region.
    code, text = place_named("group-big", "big", "cart-fit.json", group_qsf)
    assert code == 0, text
    # "bels" here is every raw BEL (COMB+FF+...) in the four columns, not LAB
    # tile count: 4 columns x 3 rows x 60 BELs/tile.
    assert "FES reserved rect group 'big' absorbs 4 regions (720 bels)" in text, text
    assert "FES slot region 'big' constrains" in text, text
    assert_inside_rect("group-big", (33, 1, 36, 3))

    # An absorbed sub-region is blocked from independent use, exactly like a
    # big card physically covering its smaller neighbours' backplane slots.
    code, text = place_named("group-absorbed", "s2", "cart-dual.json", group_qsf)
    assert code != 0, text
    assert "was absorbed into group 'big'" in text, text

    def qsf_only(name, qsf_path, timeout=60):
        command = [nextpnr, "--device", "5CSEBA6U23I7", "--json", str(output / "routed-shell.json"),
                   "--qsf", str(qsf_path), "--fes-scaffold", "--no-pack", "--no-place", "--no-route"]
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        return result.returncode, result.stdout + result.stderr

    # A loose FES_RESERVED_BEL under the default "cart" name must be folded
    # into a same-named group instead of the name looking "already declared"
    # (columns 30-31 are LAB, unused by any other case in this file).
    cart_group_qsf = output / "cart-group.qsf"
    cart_group_qsf.write_text(
        qsf.read_text() +
        'set_global_assignment -name FES_RESERVED_BEL "MISTRAL_FF.24.1.2"\n'
        'set_global_assignment -name FES_RESERVED_RECT "s5 30 1 30 3"\n'
        'set_global_assignment -name FES_RESERVED_RECT "s6 31 1 31 3"\n'
        'set_global_assignment -name FES_RESERVED_RECT_GROUP "cart s5 s6"\n')
    code, text = qsf_only("cart-group", cart_group_qsf)
    assert code == 0, text
    assert "already declared" not in text, text
    assert "FES reserved rect group 'cart' absorbs 2 regions" in text, text

    # Re-declaring an absorbed sub-region as an ordinary rect must name the
    # group that consumed it, not just say "already declared".
    redeclare_qsf = output / "redeclare.qsf"
    redeclare_qsf.write_text(group_qsf.read_text() + 'set_global_assignment -name FES_RESERVED_RECT "s2 34 1 34 3"\n')
    code, text = qsf_only("redeclare", redeclare_qsf)
    assert code != 0, text
    assert "was absorbed into group 'big'" in text, text

    # A group member must be an actually-declared region, not just a name
    # that happens to have loose FES_RESERVED_BEL content (column 37 is LAB,
    # unused elsewhere in this file).
    loose_member_qsf = output / "loose-member.qsf"
    loose_member_qsf.write_text(
        qsf.read_text() +
        'set_global_assignment -name FES_RESERVED_BEL "MISTRAL_FF.28.1.2"\n'
        'set_global_assignment -name FES_RESERVED_RECT "s7 37 1 37 3"\n'
        'set_global_assignment -name FES_RESERVED_RECT_GROUP "big2 cart s7"\n')
    code, text = qsf_only("loose-member", loose_member_qsf)
    assert code != 0, text
    assert "region 'cart' was never declared with FES_RESERVED_RECT" in text, text

    # Nested groups retarget absorbed descendants to the outermost group:
    # "inner" absorbs s8+s9, then "outer" absorbs inner+s10, so s8 must
    # report "outer", not the no-longer-usable "inner" (columns 22, 23, 29
    # are LAB, unused elsewhere in this file).
    nested_qsf = output / "nested.qsf"
    nested_qsf.write_text(
        qsf.read_text() +
        'set_global_assignment -name FES_RESERVED_RECT "s8 29 1 29 3"\n'
        'set_global_assignment -name FES_RESERVED_RECT "s9 22 1 22 3"\n'
        'set_global_assignment -name FES_RESERVED_RECT_GROUP "inner s8 s9"\n'
        'set_global_assignment -name FES_RESERVED_RECT "s10 23 1 23 3"\n'
        'set_global_assignment -name FES_RESERVED_RECT_GROUP "outer inner s10"\n')
    code, text = qsf_only("nested", nested_qsf)
    assert code == 0, text
    redeclare_nested_qsf = output / "redeclare-nested.qsf"
    redeclare_nested_qsf.write_text(nested_qsf.read_text() +
                                     'set_global_assignment -name FES_RESERVED_RECT "s8 29 1 29 3"\n')
    code, text = qsf_only("redeclare-nested", redeclare_nested_qsf)
    assert code != 0, text
    assert "was absorbed into group 'outer'" in text, text
    print("fes_slot_region: group ok")


if __name__ == "__main__":
    main()
