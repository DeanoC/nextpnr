"""Pad-clock route check shared by the Cyclone V I/O register tests.

A DCMUX reached through its default TCLK input has no decoder route line.
The routed clock net must still name that pad's DCMUX, and any explicit pip
into it must come from TCLK in the same tile.

Up to four pads share one GPIO tile. The route ``GPIO.x.y.pad:CLK{IN,OUT}.0``
selects one of them. Another pad's clock net, even one that reaches a DCMUX
in the same tile, does not satisfy the check.
"""
import json
from pathlib import Path
import re
import sys
import tempfile

_IO_TYPES = {
    "MISTRAL_IO",
    "MISTRAL_IB",
    "MISTRAL_OB",
    "MISTRAL_SDRIN",
    "MISTRAL_SDROUT",
    "MISTRAL_SDRIO",
    "MISTRAL_DDRIN",
    "MISTRAL_DDROUT",
    "MISTRAL_DDRBIDIR",
}

# Port-0 DCMUX index by pad. libmistral p2r for sx50f and sx120f (the dies
# behind 5CSEBA6) uses one map on the top and bottom rows and another on the
# right-hand column. Right-column GPIO stays at y <= 25; the bottom row is
# y=61 (sx50f) or y=81 (sx120f).
_HORIZONTAL_CLKOUT = (14, 15, 1, 0)
_HORIZONTAL_CLKIN = (10, 11, 5, 4)
_RIGHT_CLKOUT = (12, 8, 4, 0)
_RIGHT_CLKIN = (14, 10, 6, 2)


def _field_int(text):
    try:
        return int(text)
    except ValueError:
        return None


def _horizontal_edge(y):
    return y == 0 or y >= 61


def _pad_dcmux_index(y, pad, clkin):
    """DCMUX z for GPIO ``pad`` port 0, or None when ``pad`` is not 0..3."""
    if pad < 0 or pad > 3:
        return None
    table = (_HORIZONTAL_CLKIN if clkin else _HORIZONTAL_CLKOUT) if _horizontal_edge(y) else (
        _RIGHT_CLKIN if clkin else _RIGHT_CLKOUT
    )
    return table[pad]


def _names_dcmux(routing, x, y, z):
    return re.search(rf"(?:^|[.;])DCMUX\.{x}\.{y}\.{z}(?!\d)", routing) is not None


def _pips_enter_dcmux_from_tclk(routing, x, y, z):
    parts = routing.split(";")
    for index in range(0, len(parts) - 2, 3):
        pip = parts[index + 1]
        if not pip:
            continue
        fields = pip.split(".")
        if len(fields) < 8 or fields[4] != "DCMUX":
            continue
        if _field_int(fields[5]) != x or _field_int(fields[6]) != y or _field_int(fields[7]) != z:
            continue
        if not (fields[0] == "TCLK" and _field_int(fields[1]) == x and _field_int(fields[2]) == y):
            return False
    return True


def clock_route_ok(route, text, routed_path):
    """Accept a default TCLK clock for this pad, and reject a non-TCLK pip into its DCMUX."""
    match = re.match(r"GPIO\.(\d+)\.(\d+)\.(\d+):CLK(IN|OUT)\.0\Z", route)
    if not match:
        return route in text
    x, y, pad = (int(match.group(i)) for i in range(1, 4))
    clkin = match.group(4) == "IN"
    dcmux_z = _pad_dcmux_index(y, pad, clkin)
    if dcmux_z is None:
        return False
    design = json.loads(Path(routed_path).read_text())["modules"]["top"]
    # Unidirectional input registers expose the CLKIN bel pin as port CLK.
    pins = ("CLKIN", "CLK") if clkin else ("CLK",)
    found = False
    for cell in design["cells"].values():
        if cell.get("type") not in _IO_TYPES:
            continue
        bel = str(cell.get("attributes", {}).get("NEXTPNR_BEL", ""))
        bel_fields = bel.split(".")
        if len(bel_fields) < 4:
            continue
        if (
            _field_int(bel_fields[1]) != x
            or _field_int(bel_fields[2]) != y
            or _field_int(bel_fields[3]) != pad
        ):
            continue
        bits = None
        for pin in pins:
            candidate = cell.get("connections", {}).get(pin)
            if candidate:
                bits = candidate
                break
        if not bits:
            continue
        net = next((item for item in design["netnames"].values() if item.get("bits") == bits), None)
        if net is None:
            continue
        routing = net.get("attributes", {}).get("ROUTING", "")
        if not _names_dcmux(routing, x, y, dcmux_z):
            continue
        found = True
        if not _pips_enter_dcmux_from_tclk(routing, x, y, dcmux_z):
            return False
    return found


def _cell(pad, pin, bits, x=89, y=8):
    connections = {}
    if pin is not None:
        connections[pin] = bits
    return {
        "type": "MISTRAL_IO",
        "attributes": {"NEXTPNR_BEL": f"MISTRAL_IO.{x}.{y}.{pad}"},
        "connections": connections,
    }


def _net(bits, routing):
    return {"bits": bits, "attributes": {"ROUTING": routing}}


def _routed(path, cells, nets):
    path.write_text(json.dumps({"modules": {"top": {"cells": cells, "netnames": nets}}}))
    return path


def _self_check():
    # Right edge, GPIO.89.8: pad 1 CLKOUT.0 is DCMUX z 8; pad 0 is z 12.
    assert _pad_dcmux_index(8, 1, False) == 8
    assert _pad_dcmux_index(8, 0, False) == 12
    assert _pad_dcmux_index(8, 1, True) == 10
    # Top edge, GPIO.64.0: pad 0 CLKIN.0 is DCMUX z 10.
    assert _pad_dcmux_index(0, 0, True) == 10
    assert _pad_dcmux_index(0, 1, False) == 15
    assert _pad_dcmux_index(0, 3, False) == 0
    assert _pad_dcmux_index(81, 2, True) == 5

    own = "TCLK.89.8.4;;1;DCMUX.89.8.8;;1"
    other = "TCLK.89.8.1;;1;DCMUX.89.8.12;;1"
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        # Two pads share the tile. Only the other pad has a clock route.
        shared = root / "shared.json"
        _routed(
            shared,
            {"pad0": _cell(0, "CLK", [1]), "pad1": _cell(1, None, None)},
            {"clk0": _net([1], other)},
        )
        assert clock_route_ok("GPIO.89.8.1:CLKOUT.0", "", shared) is False
        assert clock_route_ok("GPIO.089.008.1:CLKOUT.0", "", shared) is False

        # The other pad's route text names this pad's DCMUX, but this pad
        # still has no clock connection.
        borrowed = root / "borrowed.json"
        _routed(
            borrowed,
            {"pad0": _cell(0, "CLK", [1]), "pad1": _cell(1, None, None)},
            {"clk0": _net([1], other + ";" + own)},
        )
        assert clock_route_ok("GPIO.89.8.1:CLKOUT.0", "", borrowed) is False

        # The target pad's own DCMUX, reached from TCLK in the tile, passes.
        # A non-TCLK pip into the neighbouring DCMUX does not reject it.
        good = root / "good.json"
        routing = "TCLK.89.8.4;TCLK.89.8.4.DCMUX.89.8.8;3;DCMUX.89.8.8;;1;GOUT.89.8.1;GOUT.89.8.1.DCMUX.89.8.12;3"
        _routed(
            good,
            {"pad0": _cell(0, "CLK", [1]), "pad1": _cell(1, "CLK", [2])},
            {"clk0": _net([1], other), "clk1": _net([2], routing)},
        )
        assert clock_route_ok("GPIO.89.8.1:CLKOUT.0", "", good) is True

        # An explicit pip into this pad's DCMUX from outside TCLK fails.
        bad_pip = root / "bad-pip.json"
        _routed(
            bad_pip,
            {"pad1": _cell(1, "CLK", [2])},
            {"clk1": _net([2], "TDMUX.89.8.1;TDMUX.89.8.1.DCMUX.89.8.8;3;DCMUX.89.8.8;;1")},
        )
        assert clock_route_ok("GPIO.89.8.1:CLKOUT.0", "", bad_pip) is False
        foreign_tclk = root / "foreign-tclk.json"
        _routed(
            foreign_tclk,
            {"pad1": _cell(1, "CLK", [2])},
            {"clk1": _net([2], "TCLK.1.2.3;TCLK.1.2.3.DCMUX.89.8.8;3;DCMUX.89.8.8;;1")},
        )
        assert clock_route_ok("GPIO.89.8.1:CLKOUT.0", "", foreign_tclk) is False

        # Top-edge input: only the neighbour is clocked.
        top = root / "top.json"
        _routed(
            top,
            {
                "pad0": _cell(0, None, None, x=64, y=0),
                "pad1": _cell(1, "CLK", [3], x=64, y=0),
            },
            {"clk1": _net([3], "TCLK.64.0.2;;1;DCMUX.64.0.11;;1")},
        )
        assert clock_route_ok("GPIO.64.0.0:CLKIN.0", "", top) is False
        _routed(
            top,
            {"pad0": _cell(0, "CLK", [4], x=64, y=0)},
            {"clk0": _net([4], "TCLK.64.0.0;;1;DCMUX.64.0.10;;1")},
        )
        assert clock_route_ok("GPIO.064.000.0:CLKIN.0", "", top) is True

        # Data routes stay a literal bitstream search.
        assert clock_route_ok("GPIO.089.008.1:DATAOUT.0", "i GPIO.089.008.1:DATAOUT.0 1", good) is True
        assert clock_route_ok("GPIO.089.008.1:DATAOUT.0", "", good) is False
    print("PASS: pad clock check requires the target pad's DCMUX")


if __name__ == "__main__":
    try:
        _self_check()
    except AssertionError:
        print("FAIL: pad clock check", file=sys.stderr)
        raise
