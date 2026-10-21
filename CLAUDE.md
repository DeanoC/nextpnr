# Claude Code instructions

Follow the repository-level agent guidance in [AGENTS.md](AGENTS.md), including its HIP/CUDA/CPU routing equivalence rule.

## Rules for all agents

Never rebase, force-push, or amend pushed commits on any PR branch or main. Update a PR branch by merging main into it. Don't push to a PR branch you weren't asked to work on.

All GPU work goes through the `gpu-run` wrapper. Never set GPU_LEASE_IDS and never pin a GPU vendor.

Nobody kills a GPU job, and nobody touches Deano's harness (~/harnesses on Powerboat) or its processes.

HIP, CUDA and CPU route/build results count as equivalent evidence (they are bit-identical); don't require or discount one backend.

Claim a MiSTer kit in ~/kit-status on Powerboat (kitA-STATUS.md / kitB-STATUS.md) before use, log what you change, restore the kit's designated baseline afterwards, and release the claim.

No broad pkill, killall or `pkill -f` on Powerboat; kill only PIDs you started.

Never relax a seal or timing check; stop and report instead.

Delete superseded code instead of keeping compatibility shims or "kept for older kits" branches.

## PR scope

- One scope per PR. Don't bundle unrelated fixes.
- The PR body says `Closes #N` for the issue it resolves. Agents may open that issue themselves when opening the PR.
- Small, self-explanatory PRs may skip the issue and the `Closes #N` line.
- Before opening a PR, check for an open PR that already covers the change, and don't duplicate it.
- Stay inside the repo and the scope the task names.
