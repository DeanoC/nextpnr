# Mistral fork branches

`main` is the integration branch for the Cyclone V compiler work in
DeanoC/nextpnr: feature PRs target `main`. Until 2026-09-26 that line was
called `mistral-stable` (renamed from `feat/mistral-cyclonev-dsp-bel` after
PR #10); it was merged into `main` and deleted, so every commit the FES
toolchain lock has ever pinned remains reachable from `main`. The branch is
the accumulated development line; individual features retain the
validation limits documented in their tests.

The integration line started at the locked upstream commit
`7d4f72c0aabc15da932748a54e82a6ff7b41921e`; it does not silently follow
upstream YosysHQ/nextpnr changes. Toolchain consumers select immutable
commit hashes, not branch names. Updating misteross locks or FES pins is a
separate integration change after review and the appropriate validation.

Prepare upstream PRs on separate branches based on current YosysHQ/nextpnr
`main`, carrying only the relevant change and a reproducible regression.
Do not target upstream with the entire accumulated Cyclone V branch.
Shared timing fixes were prepared first; backend features can follow in
focused changes once their implementation and validation are suitable.

## Mistral library revision

`main` builds against the DeanoC/mistral `master` line with the
rnode_index routing API (`rnode_coords` / `pnode_coords` / `xycoords`
labels, `rnode_index` handles, `rc2ri`/`ri2rc`), master `7ed06e21` or
later: that revision implements `rnode_unlink` (`MISTRAL_RNODE_UNLINK`)
and carries the timing target-position and pnode/rnode lookup fixes the
index conversion needed (DeanoC/mistral#10). The arch CI pins it.
Revisions before the index conversion (`18db248` and earlier, the
`rnode_t` API) no longer build. `WireId`/`PipId` carry `rnode_coords`, the
same packed type/x/y/z label as before, so nextpnr-created wires (type
>= 128), wire names and hashing are unchanged; the index is looked up
where the library wants one (timing, inversion, mux linking).

