# Agent instructions

## Routing equivalence

The GPU router's HIP and CUDA backends and its CPU reference backend give bit-identical route results. HIP, CUDA and CPU-reference route evidence is interchangeable for seed and replay comparisons; this does not combine the distinct router2/router1-fallback cohort. Never require a separate HIP run or discount CUDA evidence; use whichever GPU is free via gpu-run. Never set GPU_LEASE_IDS per job (it is a box inventory override).

## Rules for all agents

Never rebase, force-push, or amend pushed commits on any PR branch or main. Update a PR branch by merging main into it. Don't push to a PR branch you weren't asked to work on.

Never pin a GPU vendor. All GPU work goes through the `gpu-run` wrapper (see above); never set GPU_LEASE_IDS.

Nobody kills a GPU job, and nobody touches Deano's harness (~/harnesses on Powerboat) or its processes.

Claim a MiSTer kit in ~/kit-status on Powerboat (kitA-STATUS.md / kitB-STATUS.md) before use, log what you change, restore the kit's designated baseline afterwards, and release the claim.

No broad pkill, killall or `pkill -f` on Powerboat; kill only PIDs you started.

Never relax a seal or timing check; stop and report instead.

Delete superseded code instead of keeping compatibility shims or "kept for older kits" branches.

A core's locked nextpnr is only its last known good, not a required pin. If a newer nextpnr closes that core's timing, move the lock forward instead of changing RTL to fit the old version. Timing and seal checks still apply.

## PR scope

- One scope per PR. Don't bundle unrelated fixes.
- The PR body says `Closes #N` for the issue it resolves. Agents may open that issue themselves when opening the PR.
- Small, self-explanatory PRs may skip the issue and the `Closes #N` line.
- Before opening a PR, check for an open PR that already covers the change, and don't duplicate it.
- Stay inside the repo and the scope the task names.
