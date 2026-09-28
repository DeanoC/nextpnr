# Command-enable Boolean cut diagnostic

`NEXTPNR_MISTRAL_COMMAND_CUT=<prefix>` runs after the ready-enable diagnostic.
Absent or empty prefixes return before identifier allocation. The single changed
cell is port1's old `slot_free_MISTRAL_ALUT4_C` root, which becomes ALUT5 mask
`ddddddd0` with A=existing cmd_valid, B=ready, C=skid, D=filler and E=the existing
from_core output. Its Q net and all 109 ENA consumers remain intact. No cell or
net is added or removed. Every other placement, parameter, pin state and port
connection remains unchanged, including both prior replicas and DDR1 changes.

The original seven-LUT cone, the new shared cut, masks and PIN_SIG states are
checked before mutation. Tests compare all 1,024 arbitrary boundary assignments.
The intentional direct ready load increases HPS fanout from two to three. The
load sidecar records all eight old/new root-input nets (seven change; skid does
not). The nine closure seeds also include the root output.

Combinational fanout closure is derived from those seeds, traversing ordinary
ALUT2..6 only. Cycles, clock consumers and unsupported endpoints are rejected.
The expected closure is 719 FF inputs (497 ENA, 197 DATAIN, 25 SCLR) plus HPS
cmd_valid_1. Its timing model must identify a registered input on cmd_port_clk_1,
rising edge, sharing the exact clock net of all 719 FF inputs. Every endpoint
must have finite, nonsentinel setup timing; fresh execution also requires the
memory clock constraint. The narrower 224-endpoint ready fanout is contained
within this guard.

The new root is tested for legality at its old BEL. Search considers free legal
BELs within Manhattan radius six of that original location, excluding protected
LABs; the first legal BEL by name represents each LAB. A fresh timing graph
scores each candidate against the original complete endpoint vector. No one of
720 endpoints may regress, and the worst of the 109 command endpoints must
improve by at least 1 ps. Ties use BEL-name order. A final fresh analysis checks
all endpoint slacks again, predicted hold nonregression and all occupied BELs.
Failure aborts before routing rather than weakening a guard.

The prefix receives snapshots, complete pin states, closure seeds, endpoint
slacks, protected LABs, trial metrics, load counts and an audit. Imported
preflight restores carry and pin metadata but not the complete clock context;
fresh in-process timing and final routed results are authoritative. Set
MISTRAL_COMMAND_CUT_TEST_PREFIX with the existing chain of optional test prefixes
to execute the imported preflight.
