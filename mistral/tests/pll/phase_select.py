#!/usr/bin/env python3
"""Check arbitrary quarter-cycle phase selections against mandatory Quartus oracles."""
from quadrature import main


if __name__ == "__main__":
    for name, phases in (("tripleLate", (0, 0, 30)),
                         ("quadPermuted", (0, 30, 10, 20)),
                         ("quadRepeated", (0, 20, 20, 0))):
        main(phases, name)
