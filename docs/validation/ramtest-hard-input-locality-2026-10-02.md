# Registered hard-input locality checkpoint

Local record date: 2026-10-02. This default-off experiment is implemented and
host-tested, but the two selected RAM-test placement runs retained no moves.
There is no hard-input routed gain or hardware acceptance at this checkpoint.

The generic pass moves an existing ordinary FF toward a directly connected
registered input of a fixed hard block, using its actual sink routing wire for
ranking. It preserves the graph, register initialization and controls, other
placements, and the accepted capture-pipeline and composed-copy prefix. It
requires at least 250 ps improvement in both native setup slack and the
true-wire estimate, strict incoming DATAIN/control and outgoing Q/feedback
nonregression, complete clock-pair coverage, passing or nonregressing holds,
global-clock guards, native legality and exact snapshot rollback. No design
names, pins or coordinates are special-cased. See the
[request, bounds and guard details](../mistral-hard-input-locality.md).

Closed host validation passed 17 hard-input native cases, eight hard-input CLI
methods, 43 prior native regression cases and three focused CTest regressions.
The native coverage includes connected controls, misleading hard-block display
coordinates, incoming-path and feedback guards, protected launch/LAB cases,
immutable preloaded requests and exception restoration. The inverted-clock
case proves rejection when native opposite-edge hold coverage is incomplete;
it does not claim a retained opposite-edge move.

Both selected runs used the same current C0 placed guide, device
`5CSEBA6U23I7`, seed 2 and radius 24, and stopped after placement with
`--no-route`.

| Hard-input budget | Discovered eligible FFs | Attempted FFs | Retained moves |
| --- | ---: | ---: | ---: |
| 8 | 315 | 8 | 0 |
| 64 | 315 | 64 | 0 |

Each selected run's `placed.json` and entire `timing.json` are byte-identical
to the hard-input-disabled prefix run. The independent prefix proof also
preserves the previous C0 lossless module, ordered PLL occurrences, exported
raw driver/user occurrence slots and entire timing JSON, allowing only the
creator header to differ. This proves preservation of exported prefix state;
it does not prove live pointers, allocation holes, architecture caches, full
native/device legality, calibrated numerical STA or runtime/GPU identity.

| Recorded identity | SHA-256 |
| --- | --- |
| Measured compiler ELF | `ce8eb37feaf6a06ef76e4d28b3ca8dd17af3d8a59b822bbd2f03f040719b81fe` |
| Independent prefix proof | `4c2c421641aef43231b878ff8e33395e3069608b4eaa4ba515790efc7b744ed4` |
| Budget-64 closed run | `a9de5ea379d45c17a4607187e3941a771265a817a68d87aa5c9515209d5f554e` |
| Prefix and both selected placed outputs | `d2df344eac84f2d31612d16b275c16b93b5b675e6aa2d969ea824188f53f16c4` |
| Prefix and both selected timing outputs | `ba18c96552bb3865b5ab2484c63d8b689e14539a07e3766a65dab83b423b6df9` |

The complete run/profile/source/helper/input/output bindings remain in local
records under
`out/ramtest-wide-reduction-264/capture-pipeline-264`: `proof-hard-input-prefix.json`,
`hard-input-c0-prefix-placed-v1`, `hard-input-c0-selected-placed-v1` and
`hard-input-c0-selected-placed-b64-v1`. These identities qualify the historical
selected bytes. This new checkpoint document changes no production code and
does not qualify a subsequently rebuilt binary or alter those frozen records.

Follow-up should identify the exact failing feedback or incoming-data endpoint
and global-clock rows for improving trials, then compare those constraints
with the address/burst/command launches covered by the first 64 attempts.
The logs show trials with target and wire improvement that fail endpoint or
clock guards. Zero retention within the selected budgets does not establish
that all 315 eligible FFs reject, or that the worst write-data launch received
a trial. No guard relaxation, full-route result, default-recipe change,
130 MHz closure or hardware result is claimed.
