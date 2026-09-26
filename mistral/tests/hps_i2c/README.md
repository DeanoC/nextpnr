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

Bitgen must also leave the input buffer enabled when a `MISTRAL_IO` consumes
its `O` feedback.  The check decompiles the emitted RBF and verifies that these
two bidirectional pins retain the database input default.  A derived fixture
uses one output-only and one input-only buffer to prove that only the former
selects `IOCSR_STD=DIS`.

The reference RBF is not redistributed here.  Its SHA-256 and recorded routing
are checked by `check.py --reference-rbf` when that optional diagnostic input
is supplied; this does not establish the reference build's source provenance.  The retained oracle digest is
`1567e5ea4db1f18b9f23b48e7a4b7604024a998ddf1378bf77fe5968e00c64d1`.

The check also removes the primitive's RTL `BEL` attribute and applies the
Quartus `HPS_LOCATION HPSINTERFACEPERIPHERALI2C_X52_Y60_N111` assignment from
QSF. The routed JSON must still select
`cyclonev_hps_interface_peripheral_i2c.52.60.0`, proving that an internal HPS
location can come from the Quartus constraint file.
