#!/usr/bin/env python3
"""Check 45-degree 50 MHz phases from the board's 50 MHz reference."""
from pathlib import Path

from quadrature import main


if __name__ == "__main__":
    fixtures = Path(__file__).resolve().parent / "fixtures" / "phase-45"
    for profile, phases in (("dual315", (0, 17.5)),
                            ("triple225", (0, 0, 12.5)),
                            ("quadOdd", (0, 2.5, 7.5, 12.5)),
                            ("quadMixed", (0, 5, 15, 17.5))):
        main(phases, profile, 50, 50, fixtures / profile)
