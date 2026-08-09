# CELLJOIN + MFCSLACK oracle checkpoint and live protocol

Date: 2026-08-08

Source identity: **`c25fb7dc2` (`rsx: measure exact Cell coherence
joinability`)**, committed and pushed. All audited blockers are fixed; this is
the launch-ready behavior-neutral oracle checkpoint.

Preserved Release+ThinLTO app:
`/Users/hamza/Documents/rpcs3-repro/binaries/rpcs3-c25fb7dc-celljoin-mfcs.app`

- executable SHA-256:
  `aede4a855a00244c17ec68cbc17d610a5d46ee4d9659db726d041344c04b12f1`;
- executable size: 78,510,912 bytes;
- Mach-O UUID: `6DC55168-1C7C-36CA-9DD3-A3B34369EBC1`;
- embedded git identity: `19720-c25fb7dc`;
- branch/build marker: `opt/macos-tlou-feedback-snapshot-reuse`,
  `local_build`.

## Status and purpose

This checkpoint is behavior-neutral. It removes the inert receipt-v1 SPU copy
path, receipt metadata, ready-result counters, and receipt-specific Vulkan/OOM
lifetime changes. No replacement optimization is implemented: Cell bytes,
local store, texture ownership/protection, invalidation choices, readback
scheduling, MFC completion, and tag values follow the legacy paths. The new
code observes those paths only while the debug overlay enables the existing
coherence diagnostics.

The current ownership lock order is texture-cache or ZCULL pages lock, then the
ownership-directory stable-session lock innermost; no source lock is acquired
while the directory session lives. The deleted receipt-v1 renderer-lifetime
shared-lock/session layer is not part of this checkpoint. The quiescent
renderer-lifetime epoch reset remains as observational lifetime identity.

The oracle answers two narrower questions before a broker is authorized:

1. **CELLJOIN:** do concurrent read faults resolve to the same exact semantic
   Vulkan plan, and how much duplicated queue/terminal tail remains after the
   first proven materializer?
2. **MFCSLACK:** for CELLJOIN members that are SPU list GETs, how much time lies
   between completion of the enclosing direct GETL and the guest's first
   unambiguous tag-update/RdTagStat consumption boundary?

Neither stream changes an access outcome. Overlay-on frame timing is attribution
context only, never an absolute-FPS comparison.

## CELLJOIN exact-plan model

The initial deferred read-fault scan captures a fixed, allocation-free plan of
at most 16 texture sections while the texture-cache lock is held. Each section
token includes its object identity, section-session generation, producer
identity, content/staged/transfer/synchronization/write generations, full/
confirmed/locked ranges, context, protection and read flags, geometry, pitch,
format, swizzle state, flush-exclusion state, role, raw rank, and execution
rank. Flush sections are ordered with the real oldest-write-first execution
comparator; unprotect and exclude sections preserve their real vector order.
The plan also records renderer epoch, a stable even ownership-directory
sequence, cache revision, fault range, and native invalidation range.

Grouping is deliberately semantic rather than address-signature based.
Members join an active cohort only when renderer epoch, directory sequence, and
the entire ordered section-token plan match. Per-scan cache revision is retained
as drift telemetry but does not split an otherwise identical plan. Fault and
invalidation ranges also do not split the group, so faults on different native
pages can expose one cross-page plan. Their ranges remain attached to each
member, and a cohort is exact-complete only when the first successful
materializer's invalidation range covers every member fault range. The output
separately reports same-range followers, different-range followers, cross-page
pairs, and leader/materializer coverage; cross-page grouping is therefore not
permission to broaden a future invalidation.

Plans are rejected, rather than approximated, for zero or more than 16 sections,
an invalid/odd directory sequence, invalid fault-to-invalidation coverage,
missing renderer/cache/producer/generation/geometry identity, synchronized
state without staged or synchronization generation, any flush exclusion, a
pre-plan discard/mutation, or an unsupported backend. Storage is bounded at 64
active slots, 64 members per cohort, and 1,024 cohort plus 1,024 member records.
The corresponding hard validity counters are `plan_overflow`,
`slot_exhaustion`, `member_exhaustion`, `completion_drops`, and `member_drops`.

### Q, D, U, and terminal proof

The detailed records use four distinct milestones:

- `submission_us`: the faulting producer has passed the existing primary-
  submission/flush-queue handoff and may enter `flush_all`.
- **Q** (`q_us`): that member's posted flush-queue reference is actually
  removed. A zero Q is legitimate when the path posted no reference.
- **D** (`d_us`): `flush_set` has returned after the existing transfer/event/
  Cell-backing work, and exact staged completion has been checked.
- **U** (`u_us`): `unprotect_set` has completed. `semantic_us` is the later
  under-lock point at which the execution or empty replan is fully classified.

Q may precede D: queue-reference release occurs after transfer command setup/
cleanup but before the per-section flush loop. It proves queue-consumer release,
not Cell-data readiness. Every Q/D/U-family field is an absolute monotonic
microsecond timestamp, not an elapsed duration.

A requested `success` terminal is accepted only when the execution plan exactly
matches the captured ticket, completion is verified section-by-section, every
flush section produced a recorded readback, readback bytes are nonzero, and
`semantic >= U >= D > 0`. Completion verification rechecks section/session/
producer identity, execution order, geometry/format/swizzle, token equality
(`captured content generation == captured transfer generation`), nonzero
resulting staged generation equal to that captured transfer generation, and the
legacy flushed state. It does not claim a separate post-flush current-transfer-
generation read.

A requested `resolved_noop` is accepted only after one proven materializer,
with an empty cache-unchanged replan, no transfer/readback/readback-wait
evidence, no flush/unprotect/discard, an exact semantic proof, and a semantic-
ready timestamp no earlier than the materializer's. An ordinary queue wait/
handoff is allowed and remains measured. Any failed success/noop proof is
converted to `replan_mismatch`; offloader and other abandon paths remain
explicit.

A cohort is `complete=1` only with exactly one successful materializer, zero
abandons, all other members proven no-op, every registered member terminal, and
materializer coverage of every member fault. Per-cohort materializer/leader
tails are emitted only under those gates. Q-tail additionally requires valid
queue instrumentation, a posted materializer reference, and a release timestamp
for every posted reference.

`materializer_final_tail` is the last successful/no-op semantic resolution
minus the first materializer resolution. `leader_final_tail` is emitted only
when member 0 is that successful covering materializer. `queue_tail` is last Q
minus materializer Q; `leader_queue_tail` adds the same valid-leader condition.
A timestamp without a corresponding post invalidates Q-tail. Origin/MFC fields
are not cohort keys; homogeneous-SPU-GET membership is a separate live-analysis
gate.

`CELLJOIN_MEMBER terminal` numeric mapping:

| Value | Terminal | Meaning |
| ---: | --- | --- |
| 0 | `success` | Exact plan and D/U completion proof passed |
| 1 | `resolved_noop` | A prior materializer made an exact empty replan safe |
| 2 | `replan_mismatch` | Requested success/no-op or actual replan failed proof |
| 3 | `offloader` | Vulkan offloader path; excluded from join timing |
| 4 | `other_abandon` | Other explicit lifecycle/control-flow abandonment |

## MFCSLACK state machine and bounds

CELLJOIN sends only SPU GET members, keyed by the same `(cohort serial, member)`,
to a per-thread fixed-state tracker. A candidate must occur inside the enclosing
direct, successfully completed, unordered GETL. The tracker holds at most 16
candidates in the active list and 64 completed-list candidates awaiting tag
consumption; its MPSC result ring holds 4,096 records and its runtime deadline
is two seconds. Runtime binding includes the owning SPU identity and an explicit
thread-group lifecycle generation; a change censors active, pending, and query
state before later work can reuse it.

The candidate timestamp is the nested fault observation. `complete_tsc` is the
end of the enclosing direct GETL, after its current synchronous work has
finished. The oracle then follows the relevant tag mask, `WrTagUpdate`, actual
tag-status publication, first `RdTagStat` demand, and returned bits across both
the C++ and LLVM paths. `RCHCNT(MFC_RdTagStat)` counts as first demand, which can
only shorten the reported window and is therefore conservative. AsmJit's
optimized tag path is explicitly censored rather than silently treated as
covered.

The nominal two-second deadline is event-driven: an expired candidate is
terminalized by the next tracker hook, not a background timer. The live
protocol therefore uses a two-second active-game drain and still requires no
unexplained interior candidate left live.

The tracked window is bounded on both sides. A later global barrier/fence, a
same-tag barrier/fence, or any later command using the same tag censors the
older candidate before it can be credited. Only the `MFC_RdTagStat` channel is
used as the guest-consumption boundary; unrelated channels do not close a
successful record. A `WrTagUpdate` alone is not enough: the matching mode,
mask, publication record/bits, first demand, and returned bits must all be
observed. Thus the window cannot extend through unseen same-tag work or infer a
completion from a tag update that was never published and consumed.

`update_slack` is `GETL outer completion -> WrTagUpdate`; `demand_slack` is
`GETL outer completion -> first RdTagStat demand`. These are lower-bound guest
ordering windows, not time already saved. Immediate mode has no deferrable
window. `ANY` is accepted only for a single-bit mask; multi-bit `ANY` cannot
identify which candidate satisfied the query. `ALL` may cover all selected
candidate tags. The live transaction's mode and mask must match publication,
and returned bits must equal published bits. Query and publication generations
are independent trace identifiers; they need not have equal numeric values. A
new query, missing publication, early query, list stall/resume, later barrier/
fence or same-tag work, lifecycle change, or deadline censors the affected
candidate. Query modes in member output are `0=immediate`, `1=any`, and `2=all`.

`MFCSLACK_MEMBER censor` numeric mapping:

| Value | Reason | Meaning |
| ---: | --- | --- |
| 0 | `none` | Valid, unambiguous completion-to-consumption observation |
| 1 | `no_outer_list` | CELLJOIN candidate was not inside a tracked outer list |
| 2 | `not_get_list` | Outer/context command, SPU, or tag was not the same GETL |
| 3 | `queued_or_resumed` | GETL was queued/resumed rather than the direct outer command |
| 4 | `list_stall` | Stall-and-notify interrupted the outer list |
| 5 | `outer_barrier_or_fence` | The candidate's enclosing GETL was already ordered |
| 6 | `later_barrier_or_fence` | A later global or same-tag ordering edge closed the window |
| 7 | `later_same_tag_work` | A later command reused the candidate's tag before consumption |
| 8 | `immediate_query` | Immediate tag update leaves no asynchronous slack |
| 9 | `ambiguous_any` | Multi-bit `ANY` cannot identify the completing tag |
| 10 | `query_overwrite` | A new/mismatched query replaced the tracked query |
| 11 | `missing_publication` | Query/publication/return sequence could not be paired |
| 12 | `query_precedes_completion` | `WrTagUpdate` occurred before outer GETL completion |
| 13 | `unsupported_optimized_tag_path` | Decoder path lacks complete tag hooks (currently AsmJit) |
| 14 | `active_overflow` | More than 16 candidates appeared in one active list |
| 15 | `pending_overflow` | More than 64 candidates awaited tag consumption |
| 16 | `lifecycle_rebind` | SPU/TLS/list/thread-group lifetime changed before a safe terminal |
| 17 | `deadline` | No safe terminal appeared within two seconds |

Censors are data, not optimistic zeros. Structural censors may describe real
guest behavior; only `valid=1, censor=0` records contribute slack. Overflow,
ring loss, or unexplained unterminated state invalidates coverage claims.

## Source verification

The focused CELLJOIN/MFCSLACK suite passes **52/52**, the root suite passes
**247/247 enabled** with two existing tests disabled, and the Release+ThinLTO
full app link passes. The focused tests cover exact and cross-page grouping,
semantic drift and replan mismatch, one-materializer/no-op completion, Q/D/U
and queue-release gates, record/slot/member exhaustion, terminal races, tag
modes and masks,
publication/return matching, early and overwritten queries, barriers/fences,
stalls/resume, lifecycle/deadline handling, fixed-capacity overflow, and
unsupported optimized tag paths.

All findings from two independent source audits are resolved. In particular,
the tracker now binds an explicit SPU thread-group lifecycle generation so a
same-owner restart censors old active, pending, and query state; the focused
lifecycle test covers that restart and subsequent clean reuse. Commit
`c25fb7dc2` is source-stable and launch-ready for the bounded diagnostic run.

## One bounded live run: validity and GO/STOP rule

Run once in the steady bedroom with the debug overlay enabled, windowed, NI=0,
and the established package-`0x202` frame proxy. Target 30--60 seconds and at
least 600 complete frame periods after compilation settles. Allow a two-second
post-window drain so pending MFCSLACK candidates can reach a terminal, then use
only cohorts/candidates whose complete records lie inside the accepted marker
window.

The capture is invalid for an architecture decision unless all of these hold:

- zero CELLJOIN plan overflow, slot/member exhaustion, multiple materializers
  in accepted exact-complete cohorts, stale terminals, incomplete queue timing,
  queue timestamp without post, and cohort/member record drops;
- zero MFCSLACK result-ring drops, active/pending overflow, and unexplained live
  candidates in the drained interior;
- every claimed cohort is `complete=1`, coverage-valid, and homogeneous SPU GET;
  every claimed slack member is `valid=1, censor=0` and joins by serial/member;
- no ownership mismatch/poison, kick timeout, device loss, fatal error, or
  recurrent material compile/audio disturbance.

Predeclared synchronous **GO** requires all four gates after exact record
joining and offline interval de-duplication:

- at least **70%** of exact-owner attempts belong to cohorts of multiplicity
  two or greater;
- at least **4 validated followers/frame**;
- at least **80%** of followers resolve with no readback under the same proven
  materializer closure; and
- at least **2.5 ms/frame** in the union of critical Q/tail intervals.

Synchronous **STOP** applies below **1.5 ms/frame** of that critical union or
below **2 validated followers/frame**. A result between the GO and STOP bands
does not authorize a broker from this capture.

Predeclared asynchronous **GO** requires at least **85% definitive dependency
coverage** and at least **10 ms/frame** of conservatively de-duplicated safe
hide, with each operation bounded by its matched readback duration and valid
completion-to-demand window. A credible route to 30 FPS further requires about
**14 ms/frame** of hide, predicted frame time **`Tpred <= 35 ms`**, and an
optimistic demand envelope of at most **33.3 ms**. Asynchronous **STOP** applies
when definitive coverage is below **70%**, safe hide is below **7 ms/frame**,
or even optimistic `Tpred` exceeds **35 ms**. Censored members cannot fill any
coverage or time threshold.

Stop and repair the oracle, without inferring zero opportunity, if a validity
gate fails. These thresholds gate only whether a behavior-changing prototype is
worth building; they do not turn the oracle into an optimization or an FPS
measurement.

The `CELLJOIN_SUM` fields named `*_interval_sum_us` are arithmetic sums of
per-cohort intervals. Cohorts can overlap, so those sums are not wall time.
Likewise `MFCSLACK *_slack_sum_us` can count the same guest interval once per
candidate or tag and is not wall time. Neither family is additive with
CELLSTAT, convertible directly to FPS, or evidence of realized savings. The
offline union/de-duplication above is still only a counterfactual upper bound.
Only a later behavior-changing, overlay-off A/B can establish performance.

No production branch may use a hard-coded bedroom address, PC, observed transfer
size, previously observed section identity/rank, cadence, or title ID. The exact
token necessarily compares the current live section/producer identities and
execution rank as lifetime and plan-equality proof; no fixed value learned from
this scene selects a group. The policy inputs are general emulator semantics:
owner/session/producer generations, ordered section plan, range coverage,
renderer/directory lifetime, MFC direction/tag, and guest ordering.
