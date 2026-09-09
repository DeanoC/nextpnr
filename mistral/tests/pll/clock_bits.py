"""Check routed FF clock source and polarity using corrected Mistral fields."""
import re


def check_ff_clock(bt, routed, cell_prefix, inverted):
    cells = routed["modules"]["top"]["cells"]
    matches = [(name, cell) for name, cell in cells.items()
               if name.startswith(cell_prefix + "_MISTRAL_FF_") and cell["type"] == "MISTRAL_FF"]
    assert len(matches) == 1, matches
    name, cell = matches[0]
    _, x, y, z = cell["attributes"]["NEXTPNR_BEL"].split(".")
    # Each ALM imports two LUT BELs followed by four FF BELs.
    alm, slot = divmod(int(z), 6)
    assert slot >= 2, (name, z)
    half = "T" if slot < 4 else "B"
    pos = f"{int(x):03d}.{int(y):03d}"
    blocks = re.findall(rf"^r \S+ ((?:LAB|MLAB)\.{pos}):CLKIN\.0$", bt, re.M)
    assert len(blocks) == 1, (name, "missing physical CLKIN.0 route", blocks)
    block = blocks[0]
    settings = dict(re.findall(rf"^s {re.escape(block)}:(\S+) (\S+)$", bt, re.M))
    clock = settings.get(f"{half}CLK_SEL.{alm}", "OFF")
    assert clock in ("CLK0", "CLK1", "CLK2"), (name, settings)
    assert settings.get(clock + "_SEL", "CLKA") == "CLKA", (name, "wrong clock source", settings)
    assert settings.get(clock + "_INV", "0") == str(int(inverted)), (name, "wrong clock polarity", settings)
    assert settings.get("CLKA_SEL", "CIN0" if block.startswith("LAB.") else "CLKI0") in ("CIN0", "CLKI0")
    assert settings.get("EN" + clock[-1] + "_EN", "1") == "0", (name, "clock unexpectedly gated", settings)
