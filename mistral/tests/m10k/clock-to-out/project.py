#!/usr/bin/env python3
"""Generate a Quartus 17.0.2 project that times every M10K mode nextpnr packs (#232).

project.py <outdir> [--seed N] [--fanout N]

Each RAM is an altsyncram mapped to one M10K. Every RAM input is driven straight by
its own register, so TimeQuest's write paths isolate the data pin -> internal register
and clock pin -> register arcs. Every RAM output bit drives --fanout dont_touch LUTs,
each captured by its own register, so the read paths isolate clock pin -> internal
register -> dataout. Registers sit on the clock of the port they serve (clka / clkb).
"""
import argparse
import os

FANOUT = 1

# name, operation_mode, wa, wd, ra, rd, out_reg_a, out_reg_b, dual_clock, extra
# For DUAL_PORT: port A writes (wa/wd), port B reads (ra/rd).
# For BIDIR_DUAL_PORT: both ports read and write; A on clka, B on clkb.
CONFIGS = [
    # simple dual-port, one clock, unregistered B (nextpnr's default MISTRAL_M10K)
    ("sdp1", "DUAL_PORT", 13, 1, 13, 1, False, False, False, {}),
    ("sdp2", "DUAL_PORT", 12, 2, 12, 2, False, False, False, {}),
    ("sdp5", "DUAL_PORT", 11, 5, 11, 5, False, False, False, {}),
    ("sdp10", "DUAL_PORT", 10, 10, 10, 10, False, False, False, {}),
    ("sdp10b", "DUAL_PORT", 10, 10, 10, 10, False, False, False, {}),
    ("sdp20", "DUAL_PORT", 9, 20, 9, 20, False, False, False, {}),
    ("sdp40", "DUAL_PORT", 8, 40, 8, 40, False, False, False, {}),
    # simple dual-port, one clock, registered B
    ("sdp10r", "DUAL_PORT", 10, 10, 10, 10, False, True, False, {}),
    ("sdp20r", "DUAL_PORT", 9, 20, 9, 20, False, True, False, {}),
    ("sdp40r", "DUAL_PORT", 8, 40, 8, 40, False, True, False, {}),
    ("sdp2r", "DUAL_PORT", 12, 2, 12, 2, False, True, False, {}),
    # simple dual-port, independent read clock
    ("dc10", "DUAL_PORT", 10, 10, 10, 10, False, False, True, {}),
    ("dc20", "DUAL_PORT", 9, 20, 9, 20, False, False, True, {}),
    ("dc40", "DUAL_PORT", 8, 40, 8, 40, False, False, True, {}),
    ("dc20r", "DUAL_PORT", 9, 20, 9, 20, False, True, True, {}),
    ("dc40r", "DUAL_PORT", 8, 40, 8, 40, False, True, True, {}),
    # byte-enabled simple dual-port
    ("sdp20be", "DUAL_PORT", 9, 20, 9, 20, False, False, False, {"byteena": True}),
    # mixed widths (independent clocks, as nextpnr requires)
    ("mw10r20", "DUAL_PORT", 10, 10, 9, 20, False, False, True, {}),
    ("mw20r10", "DUAL_PORT", 9, 20, 10, 10, False, False, True, {}),
    ("mw10r40", "DUAL_PORT", 10, 10, 8, 40, False, False, True, {}),
    ("mw40r10", "DUAL_PORT", 8, 40, 10, 10, False, False, True, {}),
    ("mw20r40", "DUAL_PORT", 9, 20, 8, 40, False, False, True, {}),
    # true dual-port, A on clka, B on clkb
    ("tdp1", "BIDIR_DUAL_PORT", 13, 1, 13, 1, False, False, True, {}),
    ("tdp2", "BIDIR_DUAL_PORT", 12, 2, 12, 2, False, False, True, {}),
    ("tdp5", "BIDIR_DUAL_PORT", 11, 5, 11, 5, False, False, True, {}),
    ("tdp10", "BIDIR_DUAL_PORT", 10, 10, 10, 10, False, False, True, {}),
    ("tdp20", "BIDIR_DUAL_PORT", 9, 20, 9, 20, False, False, True, {}),
    ("tdp20ra", "BIDIR_DUAL_PORT", 9, 20, 9, 20, True, False, True, {}),
    ("tdp20rb", "BIDIR_DUAL_PORT", 9, 20, 9, 20, False, True, True, {}),
    ("tdp10rab", "BIDIR_DUAL_PORT", 10, 10, 10, 10, True, True, True, {}),
    ("tdpmw10r20", "BIDIR_DUAL_PORT", 10, 10, 9, 20, False, False, True, {}),
    # one clock for both TDP ports
    ("tdp10sc", "BIDIR_DUAL_PORT", 10, 10, 10, 10, False, False, False, {}),
    # output clears (nextpnr selects the output register when ACLR is live)
    ("sdp20clr", "DUAL_PORT", 9, 20, 9, 20, False, True, False, {"aclr_b": True}),
    ("sdp40clr", "DUAL_PORT", 8, 40, 8, 40, False, True, False, {"aclr_b": True}),
    ("dc20clr", "DUAL_PORT", 9, 20, 9, 20, False, True, True, {"aclr_b": True}),
    ("tdp20clr", "BIDIR_DUAL_PORT", 9, 20, 9, 20, True, True, True, {"aclr_a": True, "aclr_b": True}),
    ("tdp10clra", "BIDIR_DUAL_PORT", 10, 10, 10, 10, True, False, True, {"aclr_a": True}),
]


def emit(out, seed):
    lines = []
    ports = ["input clka", "input clkb", "input dina", "input dinb", "output [31:0] qout"]
    lines.append("module top(%s);" % ", ".join(ports))
    chains = {"a": [], "b": []}
    outs = []

    def src(dom, name, width):
        """Registers driving an input bus, appended to the domain's shift chain."""
        regs = []
        for i in range(width):
            r = "si_%s_%d" % (name, i)
            chains[dom].append(r)
            regs.append(r)
        return "{%s}" % ", ".join(reversed(regs)) if width > 1 else regs[0]

    body = []
    for (name, mode, wa, wd, ra, rd, rega, regb, dual, extra) in CONFIGS:
        ca = "clka"
        cb = "clkb" if dual else "clka"
        db = "b" if dual else "a"
        tdp = mode == "BIDIR_DUAL_PORT"
        p = {
            "intended_device_family": '"Cyclone V"', "operation_mode": '"%s"' % mode,
            "ram_block_type": '"M10K"', "width_a": wd, "widthad_a": wa, "numwords_a": 1 << wa,
            "width_b": rd, "widthad_b": ra, "numwords_b": 1 << ra,
            "address_reg_b": '"%s"' % ("CLOCK1" if dual else "CLOCK0"),
            "outdata_reg_a": '"%s"' % ("CLOCK0" if rega else "UNREGISTERED"),
            "outdata_reg_b": '"%s"' % (("CLOCK1" if dual else "CLOCK0") if regb else "UNREGISTERED"),
            "outdata_aclr_a": '"%s"' % ("CLEAR0" if extra.get("aclr_a") else "NONE"),
            "outdata_aclr_b": '"%s"' % ("CLEAR1" if extra.get("aclr_b") else "NONE"),
            "read_during_write_mode_mixed_ports": '"DONT_CARE"', "power_up_uninitialized": '"FALSE"',
        }
        if tdp:
            p["indata_reg_b"] = '"%s"' % ("CLOCK1" if dual else "CLOCK0")
            p["wrcontrol_wraddress_reg_b"] = '"%s"' % ("CLOCK1" if dual else "CLOCK0")
            p["read_during_write_mode_port_a"] = '"NEW_DATA_NO_NBE_READ"'
            p["read_during_write_mode_port_b"] = '"NEW_DATA_NO_NBE_READ"'
        else:
            p["rdcontrol_reg_b"] = '"%s"' % ("CLOCK1" if dual else "CLOCK0")
        be = extra.get("byteena")
        p["width_byteena_a"] = 2 if be else 1
        p["width_byteena_b"] = 1
        if be:
            p["byte_size"] = 10
        conn = {
            "clock0": ca,
            "address_a": src("a", name + "_wa", wa), "data_a": src("a", name + "_wd", wd),
            "wren_a": src("a", name + "_we", 1),
            "address_b": src(db, name + "_ra", ra),
            "byteena_a": src("a", name + "_be", 2) if be else "1'b1",
            "byteena_b": "1'b1",
            "aclr0": src("a", name + "_clra", 1) if extra.get("aclr_a") else "1'b0",
            "aclr1": src(db, name + "_clr", 1) if extra.get("aclr_b") else "1'b0",
            "addressstall_a": "1'b0", "addressstall_b": "1'b0",
            "clocken0": "1'b1", "clocken1": "1'b1", "clocken2": "1'b1", "clocken3": "1'b1",
        }
        if dual:
            conn["clock1"] = cb
        if tdp:
            conn["data_b"] = src(db, name + "_bd", rd)
            conn["wren_b"] = src(db, name + "_bwe", 1)
            conn["rden_a"] = "1'b1"
            conn["rden_b"] = "1'b1"
        else:
            conn["data_b"] = "%d'b0" % rd
            conn["wren_b"] = "1'b0"
            conn["rden_a"] = "1'b1"
            conn["rden_b"] = src(db, name + "_re", 1)
        qa = "q_%s_a" % name
        qb = "q_%s_b" % name
        body.append("wire [%d:0] %s;" % (wd - 1, qa))
        body.append("wire [%d:0] %s;" % (rd - 1, qb))
        conn["q_a"] = qa
        conn["q_b"] = qb
        body.append("altsyncram #(%s) ram_%s (%s);" % (
            ", ".join(".%s(%s)" % kv for kv in p.items()), name,
            ", ".join(".%s(%s)" % kv for kv in conn.items())))
        readers = [("b", qb, rd, cb)]
        if tdp:
            readers.append(("a", qa, wd, ca))
        for (port, q, w, clk) in readers:
            for i in range(w):
                # A dont_touch LUT between the RAM and its capture register stops
                # the fitter from packing the register into the M10K output register.
                # FANOUT > 1 loads each RAM output with that many LUT/register pairs.
                for f in range(FANOUT):
                    sfx = "" if f == 0 else "_f%d" % f
                    r = "so_%s_%s_%d%s" % (name, port, i, sfx)
                    l = "lo_%s_%s_%d%s" % (name, port, i, sfx)
                    body.append("wire %s_n;" % l)
                    body.append('cyclonev_lcell_comb #(.lut_mask(64\'h5555555555555555), .shared_arith("off"), '
                                '.extended_lut("off"), .dont_touch("on")) %s (.dataa(%s[%d]), .combout(%s_n));' % (l, q, i, l))
                    body.append("(* preserve *) reg %s;" % r)
                    body.append("always @(posedge %s) %s <= %s_n;" % (clk, r, l))
                    outs.append((r, clk))
    for dom, clk, din in (("a", "clka", "dina"), ("b", "clkb", "dinb")):
        prev = din
        for r in chains[dom]:
            lines.append("(* preserve *) reg %s;" % r)
            lines.append("always @(posedge %s) %s <= %s;" % (clk, r, prev))
            prev = r
    lines.extend(body)
    # Fold the capture registers into 32 outputs so nothing is trimmed.
    groups = [[] for _ in range(32)]
    for i, (r, clk) in enumerate(outs):
        groups[i % 32].append(r)
    for i, g in enumerate(groups):
        lines.append("assign qout[%d] = %s;" % (i, " ^ ".join(g) if g else "1'b0"))
    lines.append("endmodule")
    open(os.path.join(out, "top.v"), "w").write("\n".join(lines) + "\n")
    open(os.path.join(out, "top.qpf"), "w").write('PROJECT_REVISION = "top"\n')
    open(os.path.join(out, "top.qsf"), "w").write("\n".join([
        'set_global_assignment -name FAMILY "Cyclone V"',
        "set_global_assignment -name DEVICE 5CSEBA6U23I7",
        "set_global_assignment -name TOP_LEVEL_ENTITY top",
        "set_global_assignment -name VERILOG_FILE top.v",
        "set_global_assignment -name SDC_FILE top.sdc",
        'set_global_assignment -name PROJECT_OUTPUT_DIRECTORY "output_files"',
        "set_global_assignment -name NUM_PARALLEL_PROCESSORS 4",
        "set_global_assignment -name AUTO_RAM_RECOGNITION OFF",
        'set_global_assignment -name LAST_QUARTUS_VERSION "17.0.2 Lite Edition"',
        "set_global_assignment -name SEED %d" % seed,
        "",
    ]))
    open(os.path.join(out, "top.sdc"), "w").write(
        "create_clock -name clka -period 10.000 [get_ports clka]\n"
        "create_clock -name clkb -period 10.000 [get_ports clkb]\n"
        "set_clock_groups -asynchronous -group clka -group clkb\n")
    return len(outs), {d: len(c) for d, c in chains.items()}


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--fanout", type=int, default=1)
    args = ap.parse_args()
    FANOUT = args.fanout
    os.makedirs(args.outdir, exist_ok=True)
    print(emit(args.outdir, args.seed))
