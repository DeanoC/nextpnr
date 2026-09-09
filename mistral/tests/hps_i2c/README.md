# HPS I2C route check

`quartus-routes.txt` records the four relevant lines emitted by
`mistral-cv routes 5CSEBA6U23I7` for the retained Quartus Pong reference.
That reference is only a placement, pin, and hard-block endpoint oracle: its
route listing sends the HPS outputs to GPIO `DATAOUT` and does not expose an
output-enable route, so those lines alone do not prove open-drain behavior.

`expected-routes.txt` records the electrically explicit implementation used by
this test.  Both GPIO data inputs feed the HPS feedback inputs, each HPS output
controls the matching GPIO output enable, and both GPIO data values are tied to
zero.  The test checks the netlist connections as well as the routed endpoints,
which proves each pin can only drive low or release.

The reference RBF is not redistributed here.  Its SHA-256 and recorded routing
are checked by `check.py --reference-rbf` when that optional diagnostic input
is supplied; this does not establish the reference build's source provenance.  The retained oracle digest is
`1567e5ea4db1f18b9f23b48e7a4b7604024a998ddf1378bf77fe5968e00c64d1`.
