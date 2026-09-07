#!/usr/bin/env python3
"""Check additional PLL phase references and output rates against Quartus oracles."""
from pathlib import Path

from quadrature import main


if __name__ == "__main__":
    fixtures = Path(__file__).resolve().parent / "fixtures" / "phase-rates"
    for reference_mhz, output_mhz in ((25, 25), (100, 25), (50, 50)):
        quarter_ns = 250 // output_mhz
        for name, phases in (("dual", (0, 3)), ("triple", (0, 0, 3)), ("quad", (0, 1, 2, 3))):
            profile = f"ref{reference_mhz}-{name}{output_mhz}"
            main(tuple(phase * quarter_ns for phase in phases), profile,
                 reference_mhz, output_mhz, fixtures / profile)
