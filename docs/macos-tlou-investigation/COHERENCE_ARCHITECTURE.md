# Cell/RSX coherence architecture

This document records the architecture-level performance direction selected
after the 2026-08-08 bedroom capture. It is a hypothesis with explicit causal
gates, not a promise that the base M3 will reach 30 FPS. The first live oracle
result is preserved in
[`FAULT_ORACLE_CAPTURE_F35963BE.md`](FAULT_ORACLE_CAPTURE_F35963BE.md); the
valid oracle-v2 result and exact artifact identity are in
[`FAULT_ORACLE_V2_CAPTURE_E0BE3532.md`](FAULT_ORACLE_V2_CAPTURE_E0BE3532.md).

## Why this is the leading CPU-side target

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

The checkpoint also adds a stable directory session and codifies the global
lock order: renderer-lifetime shared lock, texture-cache or ZCULL pages lock,
then the directory session innermost. No source lock may be acquired while the
stable session lives. Two integration tests use real `buffered_section`
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

Runtime Vulkan validation is now the gate. A bounded overlay run must first show
`ready_hit_n > 0`, visual correctness, plausible per-reason fallback counters,
and clean ownership/lifecycle state. Only then is an overlay-off comparison
meaningful, and it must show fewer handled GET faults/flush handoffs without
moving the same cost into MFC/channel waits. No runtime result exists yet.
Production code contains no bedroom-specific address, PC, transfer-size
signature, section identity, cadence, rank, or title key: the rule depends only
on emulator-wide range, ownership, lifetime, epoch, copied-range, and generation
semantics.
Distinct gameplay scenes remain mandatory before any game-general claim.

## Phase 3: asynchronous MFC coherence broker

If most faults are genuine data dependencies, merely replacing `SIGSEGV` while
retaining one submit/wait per command cannot recover the needed frame time. Use
the existing 16-entry MFC/tag machinery to make conflicting commands explicit
asynchronous coherence requests:

- leave conflicting commands pending without an artificial batching delay;
- coalesce overlapping ranges and texture sections already waiting;
- close and submit the primary command buffer once per natural batch;
- represent one combined readback dependency;
- complete through existing tag, barrier, and fence semantics;
- for GET, read back before copying guest memory into the owning SPU's local
  store;
- for PUT, stage the local-store payload at issue so asynchronous work cannot
  race later LS modification;
- keep atomic MFC transactions synchronous initially.

Oracle v2 finds repeated same-generation GET handoffs, but it does not yet prove
issue-to-consumption slack. Attempt the broker only if the synchronous ticket
materially reduces redundant handoffs and leaves true readbacks as the bound,
and only if the prototype reduces primary submissions/events. If the remaining
data are true, immediately consumed dependencies with no prediction or overlap
window, this is a real synchronization bound rather than an implementation
accident.

## Deterministic PPU fault

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
  broker;
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
The 6.65-per-frame no-readback GET herd is therefore important alongside the
true readbacks, but neither proves the remaining saving. Reaching 30 likely
also requires work on the framebuffer-feedback path, guest execution, or both.
In the later, nonstationary 14-FPS sample, main-PPU guest execution alone
occupied roughly 46 ms/frame; that is not a clean critical-path measurement,
but it rules out treating coherence as the entire problem. A plausible route
remains visible, but 30 FPS is not established until a correct semantic branch
reaches at most 40 ms/frame with a separately measured remaining cost and then
survives cross-scene validation.
