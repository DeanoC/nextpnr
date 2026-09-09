#!/usr/bin/env python3
"""Check 100 MHz quarter phases from the board's 50 MHz reference."""
from pathlib import Path

from quadrature import main


if __name__ == "__main__":
    fixtures = Path(__file__).resolve().parent / "fixtures" / "phase-100"
    for profile, phases in (("dual100", (0, 7.5)),
                            ("triple100", (0, 0, 7.5)),
                            ("quad100", (0, 2.5, 5, 7.5))):
        main(phases, profile, 50, 100, fixtures / profile)
