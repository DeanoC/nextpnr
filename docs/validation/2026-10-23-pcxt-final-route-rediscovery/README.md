# PC/XT final-route rediscovery: no retained gain

The frozen `626e368b` final reroute option selected 21 nets from 24 failing ordinary sink rows. Two proposal generations logged 17 proposals, all negative; the best estimated gain was −20 ps. No proposal reached the unchanged 20 ps trial threshold, so no full trial ran and no repair was retained.

Both fresh routes retain eight setup failures, worst −434 ps, with zero related hold failures and minimum hold margin 822 ps. All 17,199 endpoint identities, domain windows, flags and metadata match, and all 17,194 setup and hold margins are exact. The 105 unsupported obligations and `complete=false` remain explicit. RBF, timing and endpoint files are byte-identical. Routed JSON differs only in 15 ROUTING tuple-order changes; every canonical tree, cell, pin map, LAB record and alias is unchanged across all 12,397 cells and 15,546 aliases.

No native reader execution was needed because no canonical tree changed. The retained compatibility scaffold and omitted 401-member native snapshot are clearly separate from an execution claim. The fresh disabled control reproduces the preceding source-exit-disabled route except the recorded JSON creator string. Complete prior repair decisions match before final discovery.

Host admission retains 111 evidence bindings and 34 exact committed sources, with nine new and 22 retained native tests plus seven new and two prior CLI tests. The archive verifies these receipts without rerunning tests. It preserves all original graph/checker outputs and their correction history, then replays eight complete outputs byte-for-byte. Only the standalone corruption-test fixture root is rebound to a hash-verified retained graph; production graph and timing checks retain their original code and archive-aware path handling.

The two fresh routes cost 2258.6285270126536 seconds. The committed 54-receipt journal at FES `5cb1677e95fe743a0be3caaddc021074dc1cc9ed` totals 55746.72936571435947 seconds and preserves the exact prior 52-receipt prefix. Historical routes are not rerun.

The single explicit dependency is the 418-member source-exit capture. Every dependency member and selected mapping is hash-verified, including the baseline, selection evidence, parent source/control bytes and corruption fixture. Run `verify_capture.py CAPTURE.tar.gz EMPTY_OUTPUT EXIT.tar.gz` with scratch outside `/tmp`. The fresh extractor requires an unchanged complete file inventory after replay and never falls back to live workspace paths.

This is host-route diagnostic evidence. It does not reconstruct unlogged or rejected proposal timing, establish physical configuration equivalence or timing closure, or claim hardware acceptance.
