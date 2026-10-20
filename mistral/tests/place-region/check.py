#!/usr/bin/env python3
"""PLACE_REGION instance assignments fence core logic into grid rectangles.

top.json (from top.v) holds two shift-register hierarchies, left and right.
The most specific assignment wins per cell: an exact cell name, then the
longest hierarchy or wildcard target, then the later line. I/O and clock
buffers under a fenced hierarchy keep their own placement. Bad rectangles
and targets fail before placement with a message naming them.
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

nextpnr = sys.argv[1]
here = Path(__file__).parent
pins = (here / "pins.qsf").read_text()


def place(assignments, temporary):
    qsf = Path(temporary) / "top.qsf"
    placed = Path(temporary) / "placed.json"
    qsf.write_text(pins + "".join(
        f'set_instance_assignment -name PLACE_REGION "{rect}" -to {target}\n' for rect, target in assignments))
    run = subprocess.run([nextpnr, "--device", "5CSEBA6U23I7", "--json", str(here / "top.json"), "--qsf", str(qsf),
                          "--placer", "heap", "--no-route", "--write", str(placed)], capture_output=True, text=True)
    cells = {}
    if run.returncode == 0:
        for name, cell in json.loads(placed.read_text())["modules"]["top"]["cells"].items():
            bel = cell["attributes"]["NEXTPNR_BEL"].split(".")
            cells[name] = (cell["type"], int(bel[1]), int(bel[2]))
    return run, cells


def inside(cell, rect):
    x0, y0, x1, y1 = (int(field[1:]) for field in rect.split())
    return x0 <= cell[1] <= x1 and y0 <= cell[2] <= y1


LEFT, RIGHT, TAIL, EXACT = "X20 Y10 X30 Y20", "X50 Y10 X60 Y20", "X40 Y30 X45 Y35", "X34 Y24 X36 Y26"
with tempfile.TemporaryDirectory(prefix="nextpnr-place-region-") as temporary:
    run, cells = place([(LEFT, "left"), (RIGHT, "right"), (TAIL, '"left.tail*"'),
                        (EXACT, "left.tail_MISTRAL_FF_Q_1")], temporary)
    if run.returncode != 0:
        raise SystemExit(f"placement with PLACE_REGION failed:\n{run.stdout}{run.stderr}")
    for name, cell in cells.items():
        if cell[0] in ("MISTRAL_IB", "MISTRAL_OB", "MISTRAL_CLKBUF", "MISTRAL_CONST"):
            continue
        if name == "left.tail_MISTRAL_FF_Q_1":
            expected = EXACT
        elif name.startswith("left.tail"):
            expected = TAIL
        elif name.startswith("left."):
            expected = LEFT
        elif name.startswith("right."):
            expected = RIGHT
        else:
            continue
        if not inside(cell, expected):
            raise SystemExit(f"{name} placed at X{cell[1]} Y{cell[2]}, outside {expected}")
    for io in ("left.d_MISTRAL_IB_O", "right.d_MISTRAL_IB_O", "left.clk_MISTRAL_CLKBUF_Q"):
        if inside(cells[io], LEFT) or inside(cells[io], RIGHT):
            raise SystemExit(f"{io} was fenced with core logic")

    for assignments, expected in (
            ([("X60 Y40 X70 Y50", "right")], "has no MISTRAL_FF location for cell 'right."),
            ([(LEFT, "middle")], "PLACE_REGION target 'middle' matches no core logic cell."),
            ([("X20 Y10 X30", "left")], "must be one rectangle"),
            ([("X30 Y10 X20 Y20", "left")], "is empty or outside"),
            ([("X20 Y10 X300 Y20", "left")], "is empty or outside")):
        run, _ = place(assignments, temporary)
        output = run.stdout + run.stderr
        if run.returncode == 0 or expected not in output:
            raise SystemExit(f"{assignments} was not rejected with {expected!r}:\n{output}")
print("PLACE_REGION checks passed")
