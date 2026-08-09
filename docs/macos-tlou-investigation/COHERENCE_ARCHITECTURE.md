# Cell/RSX coherence architecture

This document records the architecture-level performance direction selected
after the 2026-08-08 bedroom capture. It is a hypothesis with explicit causal
gates, not a promise that the base M3 will reach 30 FPS. The first live oracle
result is preserved in
[`FAULT_ORACLE_CAPTURE_F35963BE.md`](FAULT_ORACLE_CAPTURE_F35963BE.md); the
valid oracle-v2 result and exact artifact identity are in
[`FAULT_ORACLE_V2_CAPTURE_E0BE3532.md`](FAULT_ORACLE_V2_CAPTURE_E0BE3532.md).
The later exact joinability/slack gate is complete and stops both proposed
coherence brokers; its boundary and decision are in
[`CELLJOIN_MFCSLACK_CAPTURE_C25FB7DC.md`](CELLJOIN_MFCSLACK_CAPTURE_C25FB7DC.md).

Cell/RSX coherence was the leading CPU-side hypothesis because the aggregate
wait volume was real and large. It is no longer the active implementation
direction. Exact offline union proves that the joinable wait tail is only
0.652 ms/frame, and all matched list GETs are ordered `GETLB` commands outside
MFCSLACK v1's creditable scope. Even the impossible union of their complete
candidate-to-outer-completion spans is only 2.075 ms/strict-interior frame.
The remaining architecture search returns to general non-coherence renderer
and guest-execution bottlenecks.

## Why this became the leading CPU-side target

The clean 58.716-second CELLSTAT interval rendered 1,043 frames at 17.76 FPS
with no compiler activity or recovery timeout. Per frame it contained:

- exactly one confirmed main-PPU renderer fault, averaging 11.35 ms;
- 11.59 confirmed SPU renderer faults, all sampled occurrences nested inside
  `process_mfc_cmd`;
- 9.59 RSX flush-queue producer waits;
- exactly six synchronous GPU readbacks, totaling 1,934,912 bytes and 13.3 ms
  of aggregate wait time;
- roughly 201,000 MFC command submissions.

The CPU-visible readback rate was only about 34.4 MB/s. The expensive part is
therefore the repeated CPU/SPU -> signal handler -> RSX -> Vulkan -> GPU event
handoff, not copying two gigabytes over a minute. This is separate from the
renderer workaround's roughly 703 MiB/frame of GPU-to-GPU framebuffer
snapshots.

The first diagnostic timed every MFC submission. At 3.57 million calls per
second that timer was itself perturbative despite cache-line sharding, so
`18092fc7c` removed it before the fault-oracle captures. The lower-rate
handled-fault, flush, readback, and byte counters remain useful.

## Current architectural coupling

RPCS3 currently uses native `mprotect` state for two distinct responsibilities:

1. logical ownership of exact guest ranges by the RSX texture cache; and
2. trapping direct CPU accesses so stale GPU-owned data cannot be observed.

On this Mac the native page is 16 KiB, so four logical 4 KiB lanes share one
trap and protection state. A direct SPU DMA already knows its exact EA, length,
and direction, but accesses the protected `vm::g_base_addr` mapping and learns
about RSX ownership only by taking `SIGSEGV`. Texture-cache lookup then starts
from the rounded native page and can include other logical ranges on that page.

The important existing paths are:

- `spu_thread::process_mfc_cmd` -> `do_dma_transfer` and `do_list_transfer` in
  `rpcs3/Emu/Cell/SPUThread.cpp`;
- the renderer access-violation callback in `Utilities/Thread.cpp`;
- `VKGSRender::on_access_violation` and its `m_flush_requests.producer_wait`;
- native-page rounding and overlap selection in
  `rpcs3/Emu/RSX/Common/texture_cache.cpp` and `texture_cache.h`;
- `cached_texture_section::imp_flush` and its Vulkan event wait;
- the always-RW `vm::g_sudo_addr` alias exposed by `vm::get_super_ptr`, already
  used by renderer DMA.

## Phase 0: observer control

Commit `5670a849f` preserves the exact-MFC-timer arm. Commit `18092fc7c`
removes that 3.57-million-calls/s timer and retains the lower-rate ledger plus a
single relaxed enable check on each executed MFC transfer. The first live
capture exercised the observer but did not include an adjacent overlay-off
control. The v2 capture closes the mechanism-attribution gates but remains an
overlay-on diagnostic, so its 18.1606 FPS is not an observer-cost claim. Do not
repeat bedroom profiling solely to perfect that control. Disable the oracle in
production timing builds and use an overlay-off A/B once the exact-ticket
prototype can change an outcome; reject the prototype if the A/B merely moves
time into MFC/channel waits.

## Phase 1: fault-side causal ring

Commit `18092fc7c` implements this behavior-neutral stage. It records only the
roughly 224 confirmed faults per second in a fixed, preallocated, signal-safe
4,096-entry ring. Each event contains:

- frame/sequence and timestamp;
- origin class and thread/SPU ID;
- fault address, R/W direction, decoded access width, and PPU/SPU guest PC when
  safely available;
- active MFC EA, size, direction, command/list/atomic class, tag, and SPU PC;
- native 16 KiB page and touched logical 4 KiB lanes;
- the actual readback texture section's full, confirmed, and native-page-
  expanded locked ranges, context, protection, synchronized state, and tags,
  captured while the texture-cache lock still protects its lifetime;
- selected readback range and bytes plus nested flush-queue and GPU-event wait
  intervals;
- classification as exact logical overlap, another-lane overlap, padding only,
  invalidation-chain only, ZCULL, or unknown;
- synchronization, last-write, and post-wait ROP tags for correlation. The ROP
  tag is not exact selection-time proof.

The signal handler performs no allocation or logging. It publishes fixed event
records, and the RSX frame-end path emits cumulative `CELLSTAT` plus one-second
`CELLFAULT_SUM` and bounded `CELLFAULT_TOP` aggregates. In oracle v1, multiple
selected readback sections deliberately became `multi_unknown`; unknown or
ambiguous cases never altered the existing fallback.

A capture is invalid for causal percentages when ring drops or signature
overflow are nonzero, or when section overflow/mismatch, missing MFC context,
or offloader handoff is material. Require at least 95% of SPU handled faults to
carry an exact MFC range containing the fault. Timing fields are inclusive:
GPU-event and readback waits currently wrap the same call, and neither may be
added to the outer fault/MFC time.

Implementation work proceeds only if a small stable set of ranges/pages
explains at least 80% of readback wait or a dry-run policy predicts at least
5 ms/frame of non-overlapping critical-path savings.

### First live result and oracle v2

The `f35963bea` bedroom interval delivered 1,310 frames in 69.8359 seconds
(18.758 FPS). The aggregate ring had zero drops and recorded exactly one PPU
fault, three readback-bearing faults, four fault-attached readbacks, and six
global readbacks per frame. Fault records captured 99.98% of cumulative
GPU/readback wait time. One-second pacing degradation also tracked flush-wait
time per frame (`r = 0.75`). This preserves coherence as an architecture-scale
lead; it does not turn inclusive wait totals into recoverable frame time.

Oracle v1 failed its exact-attribution gates. Every one-second signature table
saturated, excluding 23.21% of events from signature aggregation. Exact MFC
context covered only 25.42% of SPU faults and contained PUTs but no GETs.
One-third of section-bearing faults selected multiple sections, overflowing
the one retained record; the corresponding `readback_outside_locked` results
are a validation artifact, not proof of genuine range escape.

Commit `e0be35322` implements oracle v2 as a behavior-preserving, bounded
diagnostic:

- use a bounded exact semantic-site key that excludes moving addresses and IDs,
  with an explicit untracked-site gate instead of silently dropping new sites;
- emit origin identity and make GET/read context explicit, including raw-SPU
  proxy transfers;
- retain a small fixed array of selected sections with per-section relation
  evidence instead of collapsing a common two-section case; and
- pair each retained readback with its corresponding section and validate that
  operation against that section's locked range.

Require zero aggregate-ring loss, zero untracked semantic sites, at least 95%
containing MFC context on SPU handled faults, and no unexplained section or
offloader cases before using site shares to select a policy.

The preserved `e0be35322` run passes those gates. Its exact interior contains
1,254 frames in 69.051046 seconds. Ring/site loss, missing or non-containing MFC
context, section overflow/mismatch/pairing error, out-of-range readback, and
offloader/ZCULL attribution are all zero. All 14,615 SPU faults have a
containing MFC interval. Per frame, the run records one PPU and 11.655 SPU
faults, four fault-attached of six global readbacks, 14.990 ms of aggregate
flush wait, and 14.943 ms of global GPU/readback wait.

The exact relations select two general mechanisms. First, the small list GET
causes one 230,400-byte non-overlapping sibling readback per frame in aggregate,
solely through native-page collateral. In 1,250 frames it is paired with the
requested section in one two-section fault; four frames split the two readbacks
across eight one-section faults. Second, two semantic list-GET classes take
6.65 no-readback faults per frame and 73.65% of all flush wait after the
relevant data are already synchronized. This clears the implementation gate:
no additional bedroom-only attribution pass is required before phase 2.

## Phase 2: generation-keyed logical directory, shadow, and exact GET ticket

### Ownership, lifetime, and first behavioral checkpoints

Commit `44f581fb1`, the first phase-2 checkpoint, deliberately stops before
changing an access decision. It maintains a fixed 16 KiB conservative count of
texture-cache sections whose buffered-section protection is `NO`. Updates are
centralized in the common protect, confirmed-range protect, range-expansion, and discard
lifecycle, rather than duplicated in Vulkan and OpenGL paths. A stable probe
requires only one or two granule loads for an MFC-sized range.

This summary has a narrow meaning. `clear` says only that no texture-cache
`NO` owner is summarized for the requested range. It does not establish VM or
ZCULL permission, prove that no other native protection source exists, pin a
texture section, or bind the result to a physical-protection/lifetime epoch.
The current checkpoint is therefore validation-only and changes no emulation
behavior.

When the debug overlay is enabled, the existing common read-fault path records
whether a stable probe was maybe/clear/inconclusive and whether the texture
cache handled the fault. At most every five seconds, frame end tries to take a
shared cache lock without blocking, enumerates the exact live `NO` sections,
and compares their expected granule reference counts with the summary while
directory mutation is excluded. `CELLDIR` exposes completed and mismatching
recounts, missing/excess references, poisoned granules and global poison,
expected-count overflow, mutation underflow/overflow, abandoned mutations,
sequence errors, cache-busy skips, and total/maximum recount duration.

The bounded live proof at `cbe0b7960` passes. Its marker window contains 825
periods in 45.116754 seconds (18.2859 FPS). The clean first-to-last `CELLDIR`
interior spans 815 frames in 44.560866 seconds, with 8,412
texture-handler-accepted read probes, all classified `maybe_texture`, and nine
exact recounts. It adds zero handled clear or inconclusive probes and records
zero mismatch, missing/excess reference, poison, expected-count overflow,
owner underflow/overflow, abandoned mutation, sequence error, or cache-busy
scan. The final recount matches 31 sections, 5,548 owner references, and 4,983
granules exactly. Recounts averaged 364.444 us and peaked at 441 us in the
interval.

Across the complete run, four of more than 25,000 probes conservatively fell
back as inconclusive—two before and two after the accepted window. No stable
clear, mismatch, or poison occurred. Inconclusive preserves the old path, so
these rare observations validate the required fallback rather than creating a
false negative. The exact boundary and artifact identity are in
`OWNERSHIP_DIRECTORY_CAPTURE_CBE0B796.md`; another bedroom-only summary run is
not required.

Commit `6fda0daf0` completes the behavior-neutral lifetime prerequisite. At the
quiescent boundary after construction of the fully derived renderer, but before
backend initialization creates protected sections or Cell execution resumes,
it clears both ownership planes, resets validation state, and advances a
renderer-lifetime epoch. A second, nontexture `NO` plane now covers ZCULL pages
and the texture cache's transient physical prelocks. Handoff retires a transient
owner only if an already-published texture owner covers its complete range; an
incomplete handoff is nonfatal, retains the conservative nontexture owner, and
poisons the directory so future behavioral use falls back.

That historical checkpoint also added a stable directory session and used a
renderer-lifetime shared lock followed by the texture-cache or ZCULL pages lock
and the directory session innermost. No source lock could be acquired while the
stable session lived. Two integration tests use real `buffered_section`
instances: one covers confirmed-range protection, expansion, and unprotect;
the other covers discard after the caller physically unlocks the range. All 13
focused tests and all 211 enabled tests pass, with two existing tests disabled,
and Release+ThinLTO `rpcs3_emu` builds. The earlier `44f581fb1` result was 8/8
focused and 206/206 enabled. Neither checkpoint changes an access decision.

The architecture still separates logical ownership and synchronized content
state from trap granularity:

- Track exact section ranges with conservative 4 KiB read/write conflict bits,
  RSX content generation, CPU-shadow synchronization generation, and lifetime
  epoch.
- Maintain a native-16-KiB summary only as the hot negative lookup hint. One
  normal MFC command is at most 16 KiB, so the common path should inspect at
  most two summaries and perform no map scan, allocation, or renderer call.
- Use the quiescent renderer epoch as one part of lifetime identity, but also
  pin the exact VM range/mapping and each exact cache owner for the duration of
  a behavioral copy. The summary and epoch alone cannot prevent VM unmap or
  section reuse. A stale false negative would silently corrupt guest memory.
- Publish reusable CPU-visible state only after an exact synchronization is
  complete. Its generation is reusable by later readers only while the RSX
  content generation and all associated lifetimes remain current.

Commit `6fd9d4968` implements a still narrower v1 than the originally proposed
general snapshot ticket: a receipt for bytes already copied into ordinary Cell
backing by the existing Vulkan framebuffer flush. The receipt is metadata, not
a second shadow buffer and not permission to synchronize. Publication occurs
only after `imp_flush()` really completes a linear, exclusion-free copy, and
only when the nonzero content generation captured at DMA setup still equals the
source RTT generation. `finish_flush()` then records the exact copied range,
generation, and renderer epoch and acquires an intrusive RTT reference while
the section's existing locked reference is still live. A skipped flush never
publishes a receipt.

The intrusive receipt reference closes the flush-to-discard lifetime gap. A
receipt can outlive that immediate discard, but reset, rebind, destroy, a new
DMA transfer, overlapping VM unmap, renderer teardown, memory pressure, and
every Vulkan frame end clear it. Frame expiry deliberately bounds retained RTT
lifetime and stale-section lookup cost. Memory-pressure collection now clears
receipts and runs the base purge while holding the same exclusive cache lock;
if an OOM callback re-enters while that lock is held, collection returns without
self-deadlocking. The RTT content generation is atomic because the SPU validates
it outside the RSX thread.

The read-side acquisition and proof are conservative and ordered:

1. accept only an ordinary main-memory GET of 1--16 KiB whose 32-bit range does
   not wrap or cross a 64 KiB pin boundary; reject accurate-DMA/debug modes,
   RSX local memory, Raw-SPU/MMIO, and unreadable VM;
2. require the allocation-free 16 KiB summary to report `maybe_texture`, then
   pin the exact VM range before acquiring renderer or cache state;
3. hold the published Vulkan renderer lifetime and its epoch, take the texture-
   cache reader lock, and acquire the stable directory session innermost;
4. require the same epoch, no nontexture owner, and stable `maybe_texture`;
5. reject any current locked logical owner overlapping the exact request, and
   require at least one still-protected native-page sibling to explain why the
   direct guest access would fault;
6. require exactly one logical section and exactly one receipt. Only
   `request intersection section_full_range` must be inside the receipt; bytes
   in a request gap outside the sole logical section remain guest-authoritative;
7. require a live framebuffer-storage section, its receipt-pinned RTT, and an
   exact receipt-generation match with that RTT's current generation; and
8. while every pin remains held, copy the requested bytes through the sudo alias
   into local store. Any failed or ambiguous proof returns to the legacy path
   before local store is changed.

This deliberately handles native-page collateral only. It will not bypass a
genuine exact GPU owner, initiate a readback, unprotect/discard a sibling,
advance the cache predictor, or set, clear, or repurpose the legacy
buffered-section `flushed` state. Partial or stale receipts, multiple logical
sections, missing renderer, epoch changes, nontexture ownership, missing
siblings, and directory uncertainty each have an explicit cumulative
`CELLDIR` fallback counter beside `ready_hit_n`.

Same-native-page sibling sections are keepers: they remain logically owned and
physically protected while exact requested bytes are accessed through the sudo
alias. Simply enabling strict bounds in the current invalidator is unsafe
because non-target siblings can be erased from the re-protection plan. Partial
PUTs must preserve untouched GPU-owned bytes.

Hook coverage is normal C++ GET, one attempt per whole element in the fused six-
element list fast path (including its multi-chunk 256/512-byte cases), per-item
inline list GET, list fallback through the normal C++ path, and LLVM direct
GET/GETB/GETF after their existing barrier and MMIO gates. PUT/SDCRZ, atomics,
unsupported renderers/mappings, and all rejected semantic cases retain existing
behavior. MFC context attribution wraps the helper and fallback, while tag/
barrier completion and `last_faddr` cleanup remain observationally unchanged.

Source verification passes: the affected Release+ThinLTO build and full app
link, 20/20 focused tests, 100 shuffled repetitions of the pin/lifecycle subset,
and 215/215 enabled root-suite tests with two existing tests disabled. Two
independent source audits found no remaining blocking lifetime, resolver, or
MFC-hook issue. The preserved app has SHA-256
`78161278460f618b18beb356fc0fcfafb4bda9978cd0c72e5116a7b45e6b8232`.

The bounded Vulkan validation is complete and negative. In the exact
44.155817-second counter interior, `ready_hit_n` stayed zero while
`ready_exact_owner_n` increased by 7,467 and `ready_directory_n` by 1,599; all
other receipt-result deltas and every ownership safety delta were zero. The
post-flush receipt's timing assumption is wrong for this workload. Readers
preflight while the current exact owner still exists, enter the legacy path,
and only then can that path flush and publish a receipt. The resulting receipt
arrives too late for the already-concurrent group.

This is a safe but inert negative result, not a performance result. The debug-
overlay slice measured 875 complete frame periods in 44.990309 seconds, but one
late SPU compile and, more fundamentally, zero branch hits make an overlay-off
A/B meaningless. Do not repeat the receipt-only bedroom test and do not loosen
the exact-owner rejection: doing so would copy bytes before current Cell
backing is proven. Preserve the general ownership/lifetime/hook infrastructure.
The exact result and artifact identity are in
`COLLATERAL_GET_RECEIPT_V1_CAPTURE_6FD9D496.md`.

Commit `c25fb7dc2` removes that inert receipt copy path, receipt metadata/
counters, and receipt-specific Vulkan lifetime behavior. The ownership
directory and quiescent renderer-lifetime epoch remain, but the receipt-only
renderer-lifetime shared-lock/session layer is deleted. The current lock order
is texture-cache or ZCULL pages lock, then the directory stable-session lock
innermost; no source lock is acquired while that session lives. No current path
consumes a receipt, no successor copy optimization is present, and the
historical `6fd9d4968` verification/artifact must not be attributed to
`c25fb7dc2`.

## Phase 3: live-owner joinability oracle and stopped coherence brokers

The 7,467 exact-owner rejections establish candidate volume and ordering, but
not joinability or recoverable time. Commit `c25fb7dc2` therefore implements a
bounded, debug-overlay-gated CELLJOIN/MFCSLACK oracle. It observes
the unchanged legacy path and does not alter bytes, protection, invalidation,
readback scheduling, MFC completion, tags, or guest ordering.

CELLJOIN captures an allocation-free plan of at most 16 sections under the
texture-cache lock. Its semantic identity contains the renderer epoch, stable
even directory sequence, exact ordered section/session/producer identities,
content/staged/transfer/synchronization/write generations, full/confirmed/
locked ranges, state, geometry, format, swizzle, exclusion, and execution role.
Members group only when the renderer/directory identity and whole ordered plan
match. Cache-revision drift and each member's fault/invalidation ranges remain
telemetry rather than group keys, which deliberately permits an identical plan
to span different native pages. Completion still requires the sole successful
materializer's range to cover every member fault; cross-page grouping never
authorizes broadened invalidation.

Each member records submission plus three non-interchangeable milestones: Q is
release of its actually posted flush-queue reference, D is return from the real
flush/data work with exact completion proof, and U is completion of unprotect.
Success requires exact execution/readback/generation proof and `semantic >= U
>= D`; a follower no-op requires a prior materializer and an exact empty,
cache-unchanged replan with no transfer, readback, readback-wait, flush,
unprotect, or discard evidence. The ordinary queue handoff remains allowed. An
exact-complete cohort has exactly one materializer, zero abandonments, all
remaining members proven no-op, all terminals present, full member-range
coverage, and complete queue-reference accounting. Other outcomes remain
explicit mismatch/offloader/abandon terminals. Storage is fixed at 64 active
cohorts, 64 members/cohort, and 1,024 cohort plus 1,024 member records.

MFCSLACK attaches the same cohort/member key only to direct, successfully
completed, unordered SPU GETL work. Fixed per-thread state holds 16 active-list
candidates and 64 candidates awaiting consumption; the result ring holds 4,096
records and the runtime deadline is two seconds. C++ and LLVM hooks follow the
candidate through its enclosing-list completion, tag mask, `WrTagUpdate`,
actual publication, first `RdTagStat` demand (including the conservatively early
`RCHCNT` observation), and returned bits. `ALL` and single-bit `ANY` are valid
only when the live transaction's mode/mask matches publication and returned
bits equal published bits; query and publication generations are independent
trace IDs, not values required to equal one another. Immediate, ambiguous,
queued/resumed, stalled, barrier/fence, later-same-tag, early/overwritten,
missing-publication, unsupported AsmJit, overflow, lifecycle, and deadline cases
are censored rather than credited.
The nominal two-second deadline is emitted on the next tracker hook rather than
by a timer.
Exact CELLJOIN terminal values and MFCSLACK censor values are recorded in
[`CELLJOIN_MFCSLACK_ORACLE_PROTOCOL.md`](CELLJOIN_MFCSLACK_ORACLE_PROTOCOL.md).

The focused suite passes 52/52, the root suite passes 247/247 enabled with two
disabled, and the Release+ThinLTO full app link passes. All findings from two
independent source audits are resolved, including explicit lifecycle-generation
binding and regression coverage for a same-owner SPU thread-group restart. The
checkpoint is committed, pushed, and supplied the completed bounded capture.
Its preserved executable is
`/Users/hamza/Documents/rpcs3-repro/binaries/rpcs3-c25fb7dc-celljoin-mfcs.app`
(SHA-256
`aede4a855a00244c17ec68cbc17d610a5d46ee4d9659db726d041344c04b12f1`).

The predeclared protocol required exactly one bounded overlay-on run. Validity
requires zero loss/exhaustion and only exact-complete cohorts plus
`valid=1, censor=0` slack records.
Synchronous GO requires >=70% of exact-owner attempts in cohorts
of multiplicity >=2, >=4 validated followers/frame, >=80% of followers no-
readback under the same closure, and >=2.5 ms/frame in the offline critical
Q/tail union; STOP below 1.5 ms/frame or below two followers/frame. Async GO
requires >=85% definitive dependency coverage and >=10 ms/frame safe hide. A
credible 30-FPS line further requires about 14 ms/frame hide, `Tpred <= 35 ms`,
and an optimistic demand envelope <=33.3 ms; STOP below 70% coverage, below
7 ms/frame safe hide, or when even optimistic `Tpred > 35 ms`.

Raw CELLJOIN interval sums and MFCSLACK slack sums can overlap and duplicate
shared time. They are attribution, not wall time, FPS, or realized saving;
offline union/de-duplication is still a counterfactual bound. Only stable
semantic groups clearing the predeclared gates authorize a behavioral branch.
Hard-coded bedroom address, guest/host PC, observed transfer-size signature,
previously observed section identity/rank, cadence, and title identity are not
policy keys. Current live identity/rank remains part of exact plan equality.

The bounded run passes every integrity gate and lands in both STOP bands. Its
strict first-to-last summary interior contains 781 frames and 6,408 handled
read probes. CELLJOIN accepts 5,055 exact-plan members and 1,562 leaders. The
781 homogeneous SPU GET cohorts contain 4,274 members and 3,493 proven
no-readback followers, or 4.472 followers/frame. This is 84.55% of the already
accepted exact-plan subset but only 66.70% of all handled reads, below the 70%
GO share. More importantly, offline de-duplication reduces the critical Q/tail
union to **0.652 ms/frame**, below the 1.5 ms/frame synchronous STOP line.

MFCSLACK exactly joins and terminalizes all 4,274 candidates with zero loss,
overflow, or unexplained live state. Every outer command is `0x45`
(`MFC_GETLB_CMD`) and every result is censor 5,
`outer_barrier_or_fence`. The element-level base transfer appears as GET
`0x40`, but the enclosing list command carries the guest ordering edge. There
are zero valid deferrable candidates and **0 ms/frame safe hide**, below the
7 ms/frame asynchronous STOP line.

Do not implement either broker. A synchronous leader/follower mechanism would
replace a real herd with more lifetime, cancellation, and wakeup machinery for
less than the predeclared materiality floor. An asynchronous request would need
a new barrier-aware proof: GETLB orders MFC commands but does not by itself
block ordinary SPU computation, so censor 5 leaves that narrower viability
unknown. It is nevertheless immaterial for this herd. Even if every candidate's
entire candidate-to-outer-completion span were unrealistically erased, its
de-duplicated union is 1.620671 seconds total, 2.028 ms per 799 package periods
or 2.075 ms per 781 strict-interior frames. Correlated full-fault and flush-wait
unions independently close at about 2.025 and 1.879 ms/frame. All are far below
the 7 ms asynchronous STOP line and roughly 14 ms needed for 30 FPS.

Loosening exact plan, coverage, generation, barrier, or fence proof to
manufacture a positive result would be a correctness regression. Do not repair
MFCSLACK solely for this secondary-scale herd. The capture is complete; no
repeat bedroom run is warranted. This impossible bound covers the admitted
cohort subset, not conservative unknown-generation read rejections, so it is a
scale check on the measured route rather than a proof about every game readback.

## Historical separate PPU-fault hypothesis

The one-per-frame PPU fault is independently valuable. In the valid v2 window,
its outer duration is 12.58 ms/frame and its true GPU/readback wait is about
7.97 ms/frame. The differing older observations are not interchangeable
absolute benchmarks, and none is a guaranteed saving. The PPU path should use
the same range/ownership/content-generation model, not an observed address or
guest PC.

If it is a stable report/ZCULL/texture read, schedule readback at the producer's
known RSX synchronization point rather than waiting for the consumer fault. If
it is only another-lane collision, the existing ARM64 memory decoder may allow
a supported scalar load/store to use the sudo alias while preserving sibling
protection; SIMD, pair, and ambiguous operations retain the old fault path.

This remains a historical semantic hypothesis, not the current implementation
step. The completed SPU herd gate makes coherence a secondary-scale direction;
new work first returns to the dominant renderer-feedback and guest-execution
bottlenecks rather than extending the broker architecture from this bedroom
capture.

## Production generalization rule

The steady bedroom is a repeatable microscope for data collection, not the
specification for an optimization. Addresses, guest or host PCs, observed
section identities, frame cadence, and signature ranks may select diagnostic
groups but may never select production behavior. Production decisions must be
derived from semantic inputs: exact requested range, access direction,
ownership and generation, synchronization state, and MFC tag/barrier ordering.

A candidate must preserve correctness in multiple TLoU scenes with different
Cell/RSX traffic and improve thermally conditioned overlay-off A/B windows
before it can be generalized. If a rule works only for the bedroom's stable
addresses or cadence, discard it rather than hard-code it.

## Correctness risks

- stale directory epochs bypassing a newly created RSX owner;
- losing sibling protection while resolving exact ranges;
- partial PUTs overwriting untouched GPU data;
- RSX reuse before an access ticket releases;
- lock-order deadlocks among VM reservations, I/O mapping, texture cache,
  flush queue, and offloader paths;
- local-store races or broken MFC tag/barrier semantics in the asynchronous
  broker or its behavior-neutral oracle;
- pause, savestate, cancellation, or shutdown with pending requests;
- accidentally bypassing ZCULL or VM allocation protection.

Stop immediately on visual corruption, report/ZCULL errors, alias ambiguity,
or any overwrite optimization lacking exact range and generation proof. Stop
if time merely moves from faults into channel, MFC, or GPU waits.

## Performance decision gate

The valid v2 window measured 55.065 ms/frame; 30 FPS requires 33.333 ms, so the
gap is 21.73 ms/frame. It exposes roughly 14.94 ms/frame of true GPU/readback
wait on deterministic faults. Even the impossible upper bound of eliminating
all of that wait leaves about 40.1 ms/frame and another 6.8 ms/frame to recover.
That aggregate evidence justified measuring the 6.65-per-frame no-readback GET
herd, but it did not establish recoverable time. Receipt v1 realizes none of
the bound because it waits until after exact-owner retirement. The completed
CELLJOIN gate now reduces the exact herd's non-overlapping critical union to
0.652 ms/frame. Even the impossible candidate-to-outer-completion union is only
2.075 ms/strict-interior frame, below the asynchronous STOP floor and about
one-seventh of the roughly 14 ms/frame hide needed for a credible 30-FPS line.

Coherence brokers therefore do not close the 21.73 ms/frame gap and are no
longer the active route. Reaching 30, if feasible, requires materially larger
general wins in the framebuffer-feedback path, guest execution, or both. The
dominant full-screen feedback workload should be audited for a semantically
valid snapshot-free Vulkan design; guest execution should be re-profiled
without treating concurrent coherence sums as additive opportunity.
In the later, nonstationary 14-FPS sample, main-PPU guest execution alone
occupied roughly 46 ms/frame; that is not a clean critical-path measurement,
but it rules out treating coherence as the entire problem. A plausible route
to further improvement remains visible, but 30 FPS itself is not established.
Any new branch must show materially reduced overlay-off frame time and then
survive cross-scene validation; a bedroom-specific rule does not qualify.
