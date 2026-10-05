"""Shared description of the lab_ff4 oracle cases (Quartus 17.0.2, LAB X30 Y20).

Each case lists the registers placed in the LAB.  A register is
(name, quartus_n, data, clock, edge, aclr, ena):
  quartus_n  FF_X30_Y20_N<n>; ALM a, FF slot i: n = 6a + {1, 2, 4, 5}[i]
  data       input port, or 'lut:<name>' for a LUT in the same half
  clock      clock port, edge 'pos'/'neg'
  aclr       async-clear port (active high) or None
  ena        clock-enable port or None
Quartus uses: FF0 -> FFT0 (TPKREG1), FF1 -> FFT1/FFT1L (TPKREG0), FF2 -> FFB0
(BPKREG1), FF3 -> FFB1/FFB1L (BPKREG0); TCLK_SEL/TCLR_SEL serve FF0+FF3 and
BCLK_SEL/BCLR_SEL serve FF1+FF2.
"""

N_TO_SLOT = {1: 0, 2: 1, 4: 2, 5: 3}


def site(n):
    """Quartus FF N number -> (alm, ff slot)."""
    return n // 6, N_TO_SLOT[n % 6]


P4 = ["V12", "AF7", "W12", "AF8"]
Q4 = ["Y15", "AC24", "AA15", "AD26"]


def regs(spec, clocks=None, aclrs=None, enas=None, ns=(1, 2, 4, 5)):
    out = []
    for i, n in enumerate(ns):
        clock, edge = (clocks or 'a' * len(ns))[i], 'pos'
        clk = {'a': 'clka', 'b': 'clkb', 'p': 'clka', 'n': 'clka'}[clock]
        if clock == 'n':
            edge = 'neg'
        aclr = None if not aclrs else {'a': 'rst0', 'b': 'rst1', 'o': None}[aclrs[i]]
        ena = None if not enas else {'a': 'e0', 'b': 'e1', 'c': 'e2'}[enas[i]]
        out.append((f"r[{i}]", n, f"d[{i}]", clk, edge, aclr, ena))
    return out


CASES = {
    # four fabric-fed registers in one ALM, one clock
    "loc4": dict(flags=["--mistral-ff4"], regs=regs("aaaa"), luts={},
                 pins=dict(zip(["d[0]", "d[1]", "d[2]", "d[3]"], P4)) | dict(zip(["q[0]", "q[1]", "q[2]", "q[3]"], Q4))
                 | {"clka": "V11"}),
    # LUT-fed primary plus packed secondary register in each half
    "lutpk": dict(flags=["--mistral-ff4"],
                  regs=[("r0", 1, "lut:f", "clka", "pos", None, None), ("r1", 2, "c", "clka", "pos", None, None),
                        ("r2", 4, "lut:g", "clka", "pos", None, None), ("r3", 5, "h", "clka", "pos", None, None)],
                  luts={"f": (0, "and", "a", "b"), "g": (1, "xor", "d", "e")},
                  pins={"a": "V12", "b": "AF7", "c": "W12", "d": "AF8", "e": "Y8", "h": "AB4",
                        "q[0]": "Y15", "q[1]": "AC24", "q[2]": "AA15", "q[3]": "AD26", "clka": "V11"}),
    # two global clocks: FF0+FF3 on clka, FF1+FF2 on clkb
    "g_abba": dict(flags=["--mistral-ff4", "--mistral-clkb"], regs=regs("abba", clocks="abba"), luts={},
                   pins=dict(zip(["d[0]", "d[1]", "d[2]", "d[3]"], P4)) | dict(zip(["q[0]", "q[1]", "q[2]", "q[3]"], Q4))
                   | {"clka": "V11", "clkb": "Y13"}),
    # both edges of one clock: FF1+FF2 on the falling edge
    "i_pnnp": dict(flags=["--mistral-ff4", "--mistral-clkb"], regs=regs("pnnp", clocks="pnnp"), luts={},
                   pins=dict(zip(["d[0]", "d[1]", "d[2]", "d[3]"], P4)) | dict(zip(["q[0]", "q[1]", "q[2]", "q[3]"], Q4))
                   | {"clka": "V11"}),
    # two fabric async clears split by control group
    "r_abba": dict(flags=["--mistral-ff4"], regs=regs("aaaa", aclrs="abba"), luts={},
                   pins=dict(zip(["d[0]", "d[1]", "d[2]", "d[3]"], P4)) | dict(zip(["q[0]", "q[1]", "q[2]", "q[3]"], Q4))
                   | {"clka": "V11", "rst0": "W8", "rst1": "Y4"}),
    # an open (no clear) control group next to a cleared one
    "r_aooa": dict(flags=["--mistral-ff4"], regs=regs("aaaa", aclrs="aooa"), luts={},
                   pins=dict(zip(["d[0]", "d[1]", "d[2]", "d[3]"], P4)) | dict(zip(["q[0]", "q[1]", "q[2]", "q[3]"], Q4))
                   | {"clka": "V11", "rst0": "W8"}),
    # two clock enables split by control group
    "e_abba": dict(flags=["--mistral-ff4"], regs=regs("aaaa", enas="abba"), luts={},
                   pins=dict(zip(["d[0]", "d[1]", "d[2]", "d[3]"], P4)) | dict(zip(["q[0]", "q[1]", "q[2]", "q[3]"], Q4))
                   | {"clka": "V11", "e0": "W8", "e1": "Y4"}),
    # two global clocks in one LAB, one per ALM (top-half register pairs)
    "clk2_alm": dict(flags=["--mistral-ff4", "--mistral-clkb"], regs=regs("aabb", clocks="aabb", ns=(1, 2, 7, 8)),
                     luts={},
                     pins=dict(zip(["d[0]", "d[1]", "d[2]", "d[3]"], P4)) | dict(zip(["q[0]", "q[1]", "q[2]", "q[3]"], Q4))
                     | {"clka": "V11", "clkb": "Y13"}),
    # clk and ~clk in one LAB, one edge per ALM
    "clkinv_alm": dict(flags=["--mistral-ff4", "--mistral-clkb"], regs=regs("ppnn", clocks="ppnn", ns=(1, 2, 7, 8)),
                       luts={},
                       pins=dict(zip(["d[0]", "d[1]", "d[2]", "d[3]"], P4)) | dict(zip(["q[0]", "q[1]", "q[2]", "q[3]"], Q4))
                       | {"clka": "V11"}),
    # three clock+enable pairs, two clocks
    "ena3_alm": dict(flags=["--mistral-ff4", "--mistral-clkb"],
                     regs=[(f"r[{i}]", n, f"d[{i}]", "clkb" if i >= 4 else "clka", "pos", None, ["e0", "e1", "e2"][i // 2])
                           for i, n in enumerate((1, 2, 7, 8, 13, 14))],
                     luts={},
                     pins={"clka": "V11", "clkb": "Y13", "e0": "W8", "e1": "Y4", "e2": "Y5",
                           "d[0]": "V12", "d[1]": "AF7", "d[2]": "W12", "d[3]": "AF8", "d[4]": "Y8", "d[5]": "AB4",
                           "q[0]": "Y15", "q[1]": "AC24", "q[2]": "AA15", "q[3]": "AD26", "q[4]": "AG28", "q[5]": "AF28"}),
}
