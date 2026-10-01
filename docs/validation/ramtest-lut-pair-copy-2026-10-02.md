# Frozen composed-copy benchmark

Local record date: 2026-10-02. Machine-readable identities, complete measured
options, input hashes and seven clock windows are in [ramtest-lut-pair-copy-2026-10-02.json](ramtest-lut-pair-copy-2026-10-02.json).

The explicit ten-step remap plan plus one selected composed LUT-pair copy reached
**117.86892700195312 MHz**, compared with the accepted five-stage result of
117.77175903320312 MHz: **+0.09716796875 MHz**. The independent native and physical
comparison passed, all six required non-target clock windows remained positive,
and the final signoff reported no hold violations. This qualifies the measured
result under the confirmed target-clock rule.

| Clock | Accepted parent MHz | Measured MHz | Change MHz |
| --- | ---: | ---: | ---: |
| Memory | 117.77175903320312 | 117.86892700195312 | +0.09716796875 |
| Pixel | 93.6504898071289 | 93.57162475585938 | -0.07886505126953125 |
| Capture | 259.49139404296875 | 378.5823974609375 | +119.09100341796875 |

All-clock nonregression is false. Pixel and capture remain above their required
74.25006866455078 and 130.0052032470703 MHz constraints. Memory still misses the
130.0052032470703 MHz constraint: its complete path is 8.484000086784363 ns against
a 7.691999912261963 ns window, leaving -0.7920001745223999 ns headroom.

| Clock pair | Window ns | Complete delay ns | Headroom ns |
| --- | ---: | ---: | ---: |
| Async → memory rising | 13.468000411987305 | 4.637000098824501 | 8.831000313162804 |
| Memory falling → memory rising | 3.8459999561309814 | 1.7760000517591834 | 2.069999904371798 |
| Pixel rising → pixel rising | 13.468000411987305 | 10.687000192701817 | 2.781000219285488 |
| Pixel rising → memory rising | 13.468000411987305 | 4.035000130534172 | 9.433000281453133 |
| Memory rising → pixel rising | 7.691999912261963 | 3.899999901652336 | 3.7920000106096268 |
| Memory rising → memory rising | 7.691999912261963 | 8.484000086784363 | -0.7920001745223999 |
| Capture rising → memory falling | 5.0 | 1.7170000709593296 | 3.2829999290406704 |

The benchmark used device `5CSEBA6U23I7`, seed 2, HeAP timing weight 10 and critical
exponent 2, GPU router device 1, enable replication 4, and a 16,384-path report cap.
The complete report contained 15,290 paths across seven clock pairs. The command
retained the literal ten-step plan, existing reduction/combination/decomposition
options and diagnostic environment, then applied composed-copy candidate 0 from
the current ready1 guide. No driver-copy or post-plan pass ran. RBF generation
provided final signoff; the measured full route took 728.1713569071144 seconds.

The selected copy serves the complete two-FF ENA cohort while retaining the two
shared original LUTs and their other consumers. The proof checks all 8,221 original
FF BELs, complete original owner/port/alias ordering, paired PLL ports and raw
driver/user reconstruction. Its routed pin interpretation independently checks
the four-leaf composed function over 16 assignments, 27 affected whole LABs,
210 original LUT banks, 5,322 original FF control rows and all 5,232 introduced
route-through buffers. No private output ID frame changed in this run.

The final proof binds 3,037 artifacts and 1,911 runtime files. Compiler read audit
and the final original input/output/runtime hash checks passed. The individual
proof contains the complete ledgers; this summary preserves their identities.

| Frozen identity | SHA-256 / commit |
| --- | --- |
| Measured compiler commit | `cae5eb9a326507b688414ba934421286f648a43b` |
| Compiler ELF | `a2b5da93a9a91cd95d411fb5773e86136ff62e1fce7896d371c97c756004885e` |
| Source archive | `c64eb65c4e1ac01a277322bddc90172be836fddd60f2d3a0baf66bad62522dd2` |
| Current profile | `9cc80c5a73ee5e489acbc834be8593c394a1eadd59f5340f8df0283260d2b0de` |
| Current route run | `c0b1b098fc096ed5c069327a1058b4221bde35925f55a714c9f98a1672e18ec4` |
| Current native/physical/timing proof | `5d80a02f284ed7b73075cd1c76dcde5145c64f911b473844b6636a113f4e2287` |
| Accepted parent run | `5ed3c077a6beee28c651f6b8638969160abad05d1965f55fb4ae6b373abde8dd` |
| Accepted parent proof | `6dcf8c4bb6b4b20811334ab4a87d52163756085428fdc3dbb67b84a29c795e65` |

Frozen host validation passed **58 native cases and 51 CLI methods**. Native
coverage comprises 12 pair-placement, 11 composed-copy, 25 plan and 10 driver-copy
cases; CLI coverage comprises 17 pair, 24 plan and 10 driver methods. The host
record binds their actual command/log identities to the frozen compiler.

Publication code is synchronized onto main
`04a224483a3e01147523033351176b20ce41317c`, which includes the additional #108
placement-timeout fix. The publication build passed **60 native cases, 53 CLI
methods and four shared control-set cases**, including both LAB pin-map tests.
Those checks used source `02269d27e802cc13dc6ef85641c3aac8d0662391`; subsequent
documentation changes leave the tested production code unchanged. Publication
timing is not yet qualified. This benchmark does not claim that publication binaries match the
frozen measured ELF. The source additions are generic, explicitly requested
options and default off; the recipe above is an explicit benchmark configuration.

No default-recipe or current-pin acceptance, independent numerical STA, complete
route/device legality proof, unexported pre-route cache equality, bitstream decode
or hardware acceptance is claimed. The RBF digest records output identity only.
