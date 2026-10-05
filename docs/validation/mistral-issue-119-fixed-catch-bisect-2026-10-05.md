# Issue 119 fixed-Catch bisect

Date: 2026-10-05

This report follows up issue #119 without changing synthesis inputs between
nextpnr revisions. It is a host-only CUDA place-and-route experiment; it does
not program or validate FPGA hardware.

## Frozen cohort

- Catch mapped netlist SHA-256:
  `711932035fa2569f2e38a7f40f046b605decf193fc169582202f837c9f305918`
- QSF SHA-256:
  `b0d9cd097b0552ed45f048b92e2a13d1567150bea881cee5fd21f4b777c00b64`
- SDC SHA-256:
  `3ba280ad420fee63a546a7c5cda15fc6c480de58cad398c67af2e62d1e296041`
- Mistral: `7ed06e21c18b047ec5c6d6a7e85e5ea2c8827039`
- Device: `5CSEBA6U23I7`
- GPU router: CUDA, NVIDIA RTX 3090, device 0, driver 595.84
- Seeds: 1 through 8, sequential execution
- Per-run timeout: 120 seconds

The command kept the mapped netlist, constraints, device, requested 74.25 MHz
frequency, router, GPU and output options fixed. Only the nextpnr revision and
seed changed. Every run exited normally, completed legal routing and passed
final analogue setup timing for both `core.game.clk` and `audio.clk`. These
historical reports do not expose hold analysis, so hold remains unavailable.

## Bisect

The controlled endpoints reproduce the earlier Powerboat HIP comparison
exactly at seed 1: `a93fe013` reaches 132.7669 MHz and `3d4a5b35` reaches
129.7353 MHz on `core.game.clk`. A deterministic bisect of the 112-commit
ancestry path classified the exact endpoint midpoint, 131.0 MHz. It identified:

```text
4e5ace3c493cd38d9f1fa2a8bd4337b131b04646
Fix Mistral HeAP placement timeout with joint placement and control affinity
```

Its immediate parent `3f4acc0e` produces 132.6260 MHz at seed 1. The commit
produces 129.7353 MHz, identical to the later `3d4a5b35` endpoint. All five
tested bisect midpoints before the commit retained the parent result; the first
tested merge after it retained the new result.

## Eight-seed confirmation

Final `core.game.clk` analogue Fmax in MHz:

| Seed | Parent `3f4acc0e` | Commit `4e5ace3c` | Commit with joint placement disabled |
| ---: | ---: | ---: | ---: |
| 1 | 132.626 | 129.735 | 135.135 |
| 2 | 132.415 | 132.118 | 134.354 |
| 3 | 143.431 | 137.741 | 131.079 |
| 4 | 130.548 | 127.162 | 124.285 |
| 5 | 129.668 | 120.250 | 137.118 |
| 6 | 125.016 | 122.294 | 130.617 |
| 7 | 126.952 | 133.743 | 123.001 |
| 8 | 133.511 | 145.688 | 138.427 |
| **Median** | **131.482** | **130.927** | **132.717** |
| **Mean** | **131.771** | **131.091** | **131.752** |
| **Minimum** | **125.016** | **120.250** | **123.001** |
| **Maximum** | **143.431** | **145.688** | **138.427** |

The commit regresses six of eight paired seeds. Its paired median change is
-2.18%, while the cohort median changes by -0.42% and the maximum improves.
This is a placement-trajectory and distribution change, not a uniform loss.
For seed 1 the parent critical path contains 3.567 ns logic and 3.419 ns
routing; the commit changes it to 2.666 ns logic and 4.515 ns routing.

## Change isolation

The commit combines ordinary-design control-set affinity with
`placeAllAtOnce`, which jointly places movable LUTs and FFs. Seed-1 variants at
the boundary give:

| Variant | Game Fmax | Audio Fmax |
| --- | ---: | ---: |
| Parent | 132.626 MHz | 207.039 MHz |
| Joint placement + affinity | 129.735 MHz | 228.833 MHz |
| Joint placement, affinity disabled | 115.553 MHz | 217.061 MHz |
| Separate placement + affinity | 135.135 MHz | 219.974 MHz |
| Separate placement, affinity disabled | 132.626 MHz | 207.039 MHz |

The final variant reproduces the parent's compressed RBF and timing JSON
byte-for-byte. Control affinity mitigates the seed-1 loss under joint
placement; enabling joint placement is the necessary change for this boundary
regression. Across eight seeds, retaining affinity while disabling joint
placement improves the median over both the parent and the committed default,
but still changes individual trajectories.

The same commit fixed issue #99's C64 placement timeout. Its validation records
that neither joint placement nor affinity alone completed the historical
600-second placement; both were required. A blanket revert would therefore
trade this small mixed Catch cohort shift for a known severe placement-runtime
regression.

## Conclusion

The nextpnr-only part of issue #119 begins at `4e5ace3c`, specifically with
ordinary Mistral joint LUT/FF placement. On the fixed original Catch netlist it
is much smaller than the original cross-toolchain headline: -0.42% cohort
median here versus -9.09% when synthesis and the mapped netlist also changed.
The original result cannot be assigned wholly to nextpnr.

No default change is justified by this cohort alone. A follow-up should expose
or derive a bounded selection rule between separate and joint placement, then
validate it on both the issue #99 large-design timeout fixture and held-out
multi-seed timing cohorts. Seed racing or choosing the best observed maximum
must not conceal a regression in the complete distribution.
