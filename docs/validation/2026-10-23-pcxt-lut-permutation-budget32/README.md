# PC/XT ordinary LUT permutation budget32 diagnostic

This frozen pair changes the trial cap from16 to32 at compiler134b8581, using the same original100MHz RTL and the prior two-round CPU repair. The candidate attempted23 trials and retained the same two permutations as budget16. All seven extra proposals were rejected by the existing native clock/coverage guard. No extra timing improvement was retained.

All17,199 endpoint identities, windows, flags and metadata are exact; all17,194 setup and hold margins are unchanged. Eight setup failures remain, with worst setup−434ps, zero related hold failures,105 unsupported obligations and `complete=false`. The complete12,397-cell/15,546-alias graph is unchanged. RBF, timing and endpoint files are byte-identical. Routed JSON differs only in five ROUTING-string tuple orders, with every wire/PIP/strength tuple exact.

Host admission retains45 native tests and6 CLI tests, complete87 evidence bindings and29 source files. These tests are retained receipts, not rerun during archive verification. The immutable structural reader was prepared and independently reviewed, but no reader execution was needed because no canonical tree changed. No rejected-state STA, RBF electrical equivalence, timing closure or hardware acceptance is claimed.

Both complete fresh routes, all failed proposals, original checker history, independent replay and the exact accounting50 journal are retained. The pair costs1796.4902071575634 seconds; cumulative historical route charge is50884.91650258190867 seconds at FES9caa2bd60e82dd41943b0df6c7c366d107d93ee2. The journal is checked arithmetically; this archive does not rerun historical routes.

Two explicit immutable archive dependencies supply the prior-node host/source control and the common CPU2+display route reference. Every dependency member is hash-verified at fresh extraction; selected payload mappings are explicit, with no fallback to original live paths. Executables and the401-member native object/header snapshot are omitted with exact identities. Offline verification performs no GPU, native reader or hardware operation.

Run `verify_capture.py CAPTURE.tar.gz EMPTY_OUTPUT PRIVATE_NODE.tar.gz COMBINED.tar.gz` with writable scratch outside nearly full `/tmp`. The verifier supplies its extraction parent as TMPDIR, checks the complete member inventory, then replays every graph, endpoint, ledger, native-port and no-change topology proof plus the independent byte comparison.
