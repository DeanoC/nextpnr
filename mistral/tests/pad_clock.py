"""Pad-clock route check shared by the Cyclone V I/O register tests.

A DCMUX reached through its default TCLK input has no decoder route line.
The routed clock net must still name that DCMUX, and any explicit pip into
it must come from TCLK in the same tile.
"""
import json
from pathlib import Path
import re

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


def _field_int(text):
    try:
        return int(text)
    except ValueError:
        return None


def _pips_enter_dcmux_from_tclk(routing, x, y):
    parts = routing.split(";")
    for index in range(0, len(parts) - 2, 3):
        pip = parts[index + 1]
        if not pip:
            continue
        fields = pip.split(".")
        if len(fields) < 8 or fields[4] != "DCMUX":
            continue
        if _field_int(fields[5]) != x or _field_int(fields[6]) != y:
            continue
        if not (fields[0] == "TCLK" and _field_int(fields[1]) == x and _field_int(fields[2]) == y):
            return False
    return True


def clock_route_ok(route, text, routed_path):
    """Accept a default TCLK pad clock, and reject a non-TCLK pip into its DCMUX."""
    match = re.match(r"GPIO\.(\d+)\.(\d+)\.\d+:CLK(?:IN|OUT)\.0", route)
    if not match:
        return route in text
    x, y = int(match.group(1)), int(match.group(2))
    design = json.loads(Path(routed_path).read_text())["modules"]["top"]
    # Unidirectional input registers expose the CLKIN bel pin as port CLK.
    pins = ("CLKIN", "CLK") if "CLKIN" in route else ("CLK",)
    tile = f"DCMUX.{x}.{y}."
    found = False
    for cell in design["cells"].values():
        if cell.get("type") not in _IO_TYPES:
            continue
        bel = str(cell.get("attributes", {}).get("NEXTPNR_BEL", ""))
        bel_fields = bel.split(".")
        if len(bel_fields) < 3 or _field_int(bel_fields[1]) != x or _field_int(bel_fields[2]) != y:
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
        if tile not in routing:
            continue
        found = True
        if not _pips_enter_dcmux_from_tclk(routing, x, y):
            return False
    return found
