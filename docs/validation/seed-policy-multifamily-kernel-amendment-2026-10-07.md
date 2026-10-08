# Cross-family screen: explicit kernel amendment, 2026-10-07

Deano requested a cohort-boundary pause for a VM restart, then authorized
continuing on the new minor kernel after discussing its likely impact. This
is an explicit protocol amendment, not an unchanged-environment replay claim.
The original plan remains intact with canonical digest
`a33edab985c2b6395e8119b7e641e148010d6da39675588804bfe5219f710ca2`.

Before reboot, eleven complete cohorts (50 attempts) were authenticated. The
runner stopped after RAM-test exponent 2, before any exponent-5 attempt. The
pause barrier contains no collected data and is archived recoverably on resume.
No active PNR was killed, and no completed attempt is rerun. The remaining
54 attempts stay within the original 104-invocation limit and original deadline
(04:45:14 EEST on October 8). The frozen training ranking remains 8, 2, 5, 7.

The runtime evidence differs only in `platform.release`:

| Kernel | Native runtime environment ID |
| --- | --- |
| `6.8.0-136-generic` | `sha256:5464dab34fe9d4221a210e094f662d131fc12e16b4d1119590a30869737efa1f` |
| `6.8.0-142-generic` | `sha256:036c9fcd51f00300264b8db273f489f923e49478bd6a5a63943502be27d601bc` |

Recorded binary/dependency bytes, CPU and native execution metadata match;
the local GPU UUID is unchanged. That makes a large outcome change unlikely,
but does not prove label equivalence or eliminate operating-system effects on
runtime. No new repeat-control runs are claimed or silently added.

The screen layer loads a separate `kernel-amendment.json`, bound to the original
plan, and permits only these exact runtime-ID/kernel pairs. It rechecks that
the full runtime manifests differ solely in kernel release and that the backend
matches. Unknown runtime IDs, changed binary/library/CPU/backend records and
digest tampering still fail closed. The general collector/portfolio reader's
strict per-cohort validation is unchanged. Each cohort remains internally
homogeneous; only inter-cohort screening accepts the declared kernel strata.

Amendment file SHA-256:
`f191d9072f1d8b8b2fc92b9e509cffbe209621fcfb4d39e9912a31cbb2bb543e`.
It is retained beside `plan.json` in the experiment evidence directory.

RAM-test exponents 7, 8 and 2 ran before reboot; exponent 5 runs after it.
Its cross-policy runtime comparisons are therefore confounded and descriptive:
do not turn small measured differences into speedup claims. Training is entirely
pre-reboot; Pong and C64 held-out cohorts are entirely post-reboot. The final
evaluation records the amendment, kernel releases per family/policy and an
explicit cross-kernel cost-comparison flag. Historical results and timestamps
remain unchanged. Reboot downtime is not PNR execution or saved compute.

Validation: 17 multi-family tests, 21 portfolio tests and 89 foundation tests
pass. Six new amendment tests cover undeclared transitions, exact kernel-only
acceptance, unlisted IDs, binary/backend changes and digest tampering. Recovery
checks verified the current runtime against the old record, the unchanged
selection and the data-free pause barrier before resuming.
