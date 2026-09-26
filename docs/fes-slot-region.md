# FES slot placement region and capacity report

`FES_RESERVED_RECT` in the cart QSF reserves every BEL of a tile rectangle for
`FES_SLOT` cells (see [fes-slot-clock.md](fes-slot-clock.md) for the merge
flow). Reservation alone only *rejects* placements: neither HeAP nor the
simulated-annealing placer knew where the socket was, so both sampled the
whole device and hit the socket about one time in a hundred. HeAP then cycled
evictions between flip-flops with different control sets for tens of minutes
before its global attempt limit reported anything.

After the scaffold is locked, `--fes-cart` placement now:

1. Creates the region `$FES_SLOT` from the reserved BELs and constrains every
   `FES_SLOT` cell to it. HeAP clamps its solver and legaliser to the region.
   `fes_placement_allowed()` remains the hard gate, so frozen shell LABs and
   BELs outside the rectangle are still refused. `--placer sa` is refused for
   cart placement: it moves cluster members one at a time under a soft
   penalty and can finish with an unrepaired LUT/FF pair (its initial
   placement now samples inside a region's bounding box, which helps
   region-constrained designs in general but is not enough here).
2. Clusters cart carry chains (`constrain_carries` for unbound slot cells) and
   pairs each cart flip-flop with the slot LUT that drives its `DATAIN`. A
   paired FF sits in its LUT's ALM half and needs no route-through LUT or E/F
   input, the scarce resource inside a small rectangle.
3. Enables HeAP's control-set aware legalisation for the run, keyed on the
   signals a Cyclone V LAB has exactly one of (clock, synchronous clear,
   synchronous load), so the legaliser first looks for a LAB already holding
   the same set. Enables and asynchronous clears have several LAB lines and
   are left to the full LAB validity check, which still decides legality.
4. Fails fast when the same cell is re-legalised more than 500 times in one
   pass (`placerHeap/cellRipupLimit`), naming that cell.
5. Prints a `FES slot capacity` report before placement and stops with the
   violated bound when the cart provably cannot fit:

```
FES slot capacity: 41 usable LABs (3 frozen), longest vertical run 11.
FES slot capacity: comb 617/820, FF 271/820 (...), carry chains 17 (longest 2 LAB rows).
FES slot capacity: 18 FF control-set groups by SCLR need at least 29 LABs (...).
FES slot capacity: LAB inputs need at least 37 LABs at 42 unique inputs each (...).
```

The bounds are necessary, not sufficient: LABs holding a locked shell cell are
excluded, only two FF BELs per ALM are legal, a chain root needs ALM 0 of a
LAB, a chain of more than twenty cells needs vertically adjacent usable LABs
in one column, a LAB with a synchronous clear keeps one enable line
because Mistral routes the clock through a LAB `DATAIN` line as well, and
`check_lab_input_count` admits 42 unique ALM inputs per LAB (the report's
input bound assumes the best case of two shared inputs per LUT pair, so a real
placement needs noticeably more LABs than that line says).

The focused regression synthesizes a shell with two frozen socket FFs, routes
it, then places carts in `FES_RESERVED_RECT "24 1 28 3"`: a six-control-set
cart must place with HeAP inside the rectangle and away from the frozen LABs
while SA is refused, a fourteen-clear cart must be refused by the control-set
bound, and a 64-cell adder must be refused by the chain-height bound:

```
python3 mistral/tests/fes_slot_region.py --yosys /path/to/yosys \
  --nextpnr /path/to/nextpnr-mistral --output /tmp/fes-slot-region
```

None of this claims routing, timing closure, CRAM overlay equivalence or
hardware acceptance for a consumer shell; those remain separate checks.
