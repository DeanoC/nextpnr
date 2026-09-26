#!/usr/bin/env python3
"""Route a large M10K fixture and audit clock and selector consistency.

The fixture is supplied by the caller.  This keeps a generated application
netlist out of nextpnr while making it possible to run the same audit against
Coleco, ZX81, or a future design with a large collection of M10Ks.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess


DEVICE = "5CSEBA6U23I7"
M10K = "MISTRAL_M10K"


def run(command, log, timeout):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                       check=True, timeout=timeout)


def parameter(cell, name, default=0):
    value = cell.get("parameters", {}).get(name, default)
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        text = value.strip()
        if not text:
            return default
        try:
            return int(text, 2) if set(text) <= {"0", "1"} else int(text, 0)
        except ValueError as error:
            raise AssertionError(f"invalid {name} value {value!r}") from error
    raise AssertionError(f"invalid {name} value {value!r}")


def site_name(cell):
    bel = cell.get("attributes", {}).get("NEXTPNR_BEL", "")
    match = re.fullmatch(r"MISTRAL_M10K\.(\d+)\.(\d+)\.\d+", bel)
    if not match:
        raise AssertionError(f"M10K has no canonical BEL: {bel!r}")
    return f"M10K.{int(match.group(1)):03d}.{int(match.group(2)):03d}"


def read_bitstream(path):
    settings = {}
    route_pins = {}
    for line in path.read_text().splitlines():
        setting = re.match(r"s (M10K\.\d+\.\d+):(\S+) (\S+)$", line)
        if setting:
            settings.setdefault(setting.group(1), {})[setting.group(2)] = setting.group(3)
            continue
        route = line.split()
        if len(route) != 3 or route[0] != "r":
            continue
        for token in route[1:]:
            endpoint = re.fullmatch(r"(M10K\.\d+\.\d+):(.+)", token)
            if endpoint:
                route_pins.setdefault(endpoint.group(1), set()).add(endpoint.group(2))
    return settings, route_pins


def check_cell(name, cell, settings, route_pins, gnd_bits, vcc_bits):
    site = site_name(cell)
    if site not in settings:
        raise AssertionError(f"{name}: no decoded settings for {site}")
    fields = settings[site]
    pins = route_pins.get(site, set())

    def field(key, default="0"):
        return fields.get(key, default)

    def require_field(key, value):
        actual = field(key)
        if actual != value:
            raise AssertionError(f"{name} at {site}: {key}={actual!r}, expected {value!r}")

    def require_route(pin, expected=True):
        actual = pin in pins
        if actual != expected:
            state = "present" if actual else "absent"
            wanted = "present" if expected else "absent"
            raise AssertionError(f"{name} at {site}: {pin} route {state}, expected {wanted}")

    connections = cell.get("connections", {})
    clk1 = list(connections.get("CLK1") or [])
    clk2 = list(connections.get("CLK2") or [])
    for clock_name, bits in (("CLK1", clk1), ("CLK2", clk2)):
        if set(bits) & (gnd_bits | vcc_bits):
            raise AssertionError(f"{name} at {site}: {clock_name} still uses a packed constant net")

    tdp = bool(parameter(cell, "CFG_TDP"))
    mixed = bool(parameter(cell, "CFG_MIXED_WIDTH"))
    async_read = bool(parameter(cell, "CFG_ASYNC_READ"))
    dbits = parameter(cell, "CFG_DBITS", 10)
    rdbits = parameter(cell, "CFG_RD_DBITS", dbits) if mixed else dbits
    wide = dbits == 40 or (mixed and rdbits == 40)

    # Async flow-through reads deliberately fan the live write clock to both
    # physical clock sinks.  A live CLK2 has the same two-sink requirement.
    dual_clock = bool(clk2)
    needs_second_sink = dual_clock or async_read
    require_route("CLKIN.0", bool(clk1))
    require_route("CLKIN.1", needs_second_sink)

    independent_selectors = (
        "BOT_CLK_SEL", "BOT_1_CORECLK_SEL", "BOT_1_INCLK_SEL", "BOT_1_OUTCLK_SEL"
    )
    if needs_second_sink:
        for key in independent_selectors:
            require_field(key, "1")
        require_field("BOT_1_INCLK_SEL", "0" if wide else "1")
        require_field("BOT_CLK_INV", "0")
    else:
        for key in independent_selectors:
            require_field(key, "0")

    if tdp:
        require_field("TRUE_DUAL_PORT", "1")
        require_field("TOP_INCLK_SEL", "1")
    elif field("TRUE_DUAL_PORT") == "1":
        raise AssertionError(f"{name} at {site}: unexpected TRUE_DUAL_PORT selector")

    # A registered logical output must select the matching physical register.
    # A 40-bit SDP result occupies both output halves and therefore registers
    # both halves when either request is present.
    reg_a = bool(parameter(cell, "CFG_OUT_REG_A"))
    reg_b = bool(parameter(cell, "CFG_OUT_REG_B"))
    if not tdp and rdbits == 40 and (reg_a or reg_b):
        reg_a = reg_b = True
    if "CFG_OUT_REG_A" in cell.get("parameters", {}) or reg_a:
        actual = field("A_OUTPUT_SEL").upper()
        expected = "REG" if reg_a else "ASYNC"
        if actual != expected:
            raise AssertionError(f"{name} at {site}: A_OUTPUT_SEL={actual!r}, expected {expected!r}")
    if "CFG_OUT_REG_B" in cell.get("parameters", {}) or reg_b:
        actual = field("B_OUTPUT_SEL").upper()
        expected = "REG" if reg_b else "ASYNC"
        if actual != expected:
            raise AssertionError(f"{name} at {site}: B_OUTPUT_SEL={actual!r}, expected {expected!r}")


def check_route(args, seed, output):
    output.mkdir(parents=True, exist_ok=True)
    routed = output / "routed.json"
    report = output / "timing.json"
    rbf = output / "top.rbf"
    command = [
        str(args.nextpnr.resolve()), "--json", str(args.fixture.resolve()),
        "--device", args.device, "--qsf", str(args.qsf.resolve()),
        "--sdc", str(args.sdc.resolve()), "--freq", str(args.freq),
        "--seed", str(seed), "--router", args.router,
        "--compress-rbf", "--rbf", str(rbf), "--write", str(routed),
        "--report", str(report),
    ]
    if args.tmg_ripup:
        command.append("--tmg-ripup")
    run(command, output / "route.log", args.timeout)
    if not rbf.is_file() or rbf.stat().st_size == 0:
        raise AssertionError(f"seed {seed}: no compressed RBF")

    timing = json.loads(report.read_text())
    fmax = timing.get("fmax", {})
    if not fmax:
        raise AssertionError(f"seed {seed}: timing report has no constrained clocks")
    failures = [
        f"{clock}: {row.get('achieved')} < {row.get('constraint')}"
        for clock, row in fmax.items()
        if row.get("achieved") is None or row.get("constraint") is None
        or row["achieved"] < row["constraint"]
    ]
    if failures:
        raise AssertionError(f"seed {seed}: timing failed: {', '.join(failures)}")

    design = json.loads(routed.read_text())
    try:
        top = design["modules"][args.top]
    except KeyError as error:
        raise AssertionError(f"routed design has no top module {args.top!r}") from error
    cells = {name: cell for name, cell in top.get("cells", {}).items()
             if cell.get("type") == M10K}
    if len(cells) < args.min_m10k:
        raise AssertionError(f"seed {seed}: only {len(cells)} M10Ks, expected at least {args.min_m10k}")
    dual_count = sum(bool(cell.get("connections", {}).get("CLK2")) for cell in cells.values())
    if dual_count < args.min_dual_clock:
        raise AssertionError(f"seed {seed}: only {dual_count} live second clocks, "
                             f"expected at least {args.min_dual_clock}")

    def named_bits(name):
        net = top.get("netnames", {}).get(name, {})
        return set(net.get("bits", [])) if isinstance(net, dict) else set()

    bt = output / "top.bt"
    run([str(args.mistral_cv.resolve()), "decomp", args.device, str(rbf), str(bt)],
        output / "decomp.log", args.timeout)
    settings, route_pins = read_bitstream(bt)
    gnd_bits = named_bits("$PACKER_GND_NET")
    vcc_bits = named_bits("$PACKER_VCC_NET")
    sites = set()
    for name, cell in cells.items():
        site = site_name(cell)
        if site in sites:
            raise AssertionError(f"seed {seed}: duplicate M10K site {site}")
        sites.add(site)
        check_cell(name, cell, settings, route_pins, gnd_bits, vcc_bits)
    print(f"PASS: seed {seed}: {len(cells)} M10Ks, {dual_count} independent-clock cells, "
          f"selectors and routes consistent", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nextpnr", type=Path, required=True)
    parser.add_argument("--mistral-cv", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--qsf", type=Path, required=True)
    parser.add_argument("--sdc", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device", default=DEVICE)
    parser.add_argument("--top", default="top")
    parser.add_argument("--router", default="router1")
    parser.add_argument("--seed", type=int, action="append")
    parser.add_argument("--freq", type=float, default=74.25)
    parser.add_argument("--min-m10k", type=int, default=100)
    parser.add_argument("--min-dual-clock", type=int, default=100)
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--tmg-ripup", action="store_true")
    args = parser.parse_args()
    if args.min_m10k < 1 or args.min_dual_clock < 0 or args.timeout <= 0:
        parser.error("minimum counts must be non-negative (M10K count must be positive) and timeout positive")
    for seed in args.seed or [7]:
        check_route(args, seed, args.output.resolve() / f"seed{seed}")


if __name__ == "__main__":
    main()
