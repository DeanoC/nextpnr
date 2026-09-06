# Mistral fork branches

`mistral-stable` is the integration branch for the Cyclone V DSP/PLL compiler
work. Feature PRs in DeanoC/nextpnr target this branch. It was renamed from
`feat/mistral-cyclonev-dsp-bel` after PR #10, without changing its history.
The branch is the accumulated development line; individual features retain
the validation limits documented in their tests.

`main` remains the upstream reference. The integration line started at the
locked upstream commit `7d4f72c0aabc15da932748a54e82a6ff7b41921e`; it does not
silently follow upstream changes. Toolchain consumers select immutable commit
hashes, not branch names. Updating misteross locks or FES pins is a separate
integration change after review and the appropriate validation.

Prepare upstream PRs on separate branches based on current YosysHQ/nextpnr
`main`, carrying only the relevant change and a reproducible regression.
Do not target upstream with the entire accumulated Cyclone V branch.
Shared timing fixes were prepared first; backend features can follow in
focused changes once their implementation and validation are suitable.
