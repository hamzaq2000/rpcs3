# Cell/RSX coherence architecture

This document records the architecture-level performance direction selected
after the 2026-08-08 bedroom capture. It is a hypothesis with explicit causal
gates, not a promise that the base M3 will reach 30 FPS. The first live oracle
result is preserved in
[`FAULT_ORACLE_CAPTURE_F35963BE.md`](FAULT_ORACLE_CAPTURE_F35963BE.md).

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
second that timer is itself perturbative, despite cache-line sharding. Remove
or sparsely sample it before the next attribution capture. The lower-rate
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
control. Oracle v2 must compare otherwise identical overlay-on and overlay-off
windows. Reject or redesign the observer if throughput changes by more than
3--5%, pacing changes materially, or faults per frame move by more than about
3%.

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
`CELLFAULT_SUM` and bounded `CELLFAULT_TOP` aggregates. Multiple selected
readback sections deliberately become `multi_unknown`; unknown or ambiguous
cases never alter the existing fallback.

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

## Phase 2: logical coherence directory and exact access ticket

If the oracle confirms a useful pattern, separate logical ownership from trap
granularity:

- Track exact section ranges with conservative 4 KiB read/write conflict bits.
- Maintain a native-16-KiB summary only as the hot negative lookup hint. One
  normal MFC command is at most 16 KiB, so the common path should inspect at
  most two summaries and perform no map scan, allocation, or renderer call.
- Protect directory lifetime with an epoch/refcount scheme and update it on
  section protection, resize, discard, flush, unmap, reuse, and ZCULL changes.
  A stale false negative would silently corrupt guest memory.

An RAII `cell_access_ticket{EA, size, direction}` then either:

1. returns the normal base pointer when no protected native page is involved;
2. resolves and pins exact logical owners, synchronizes only required sections,
   and returns the sudo alias; or
3. falls back unchanged when the epoch changes or semantics are unsupported.

Same-native-page sibling sections are keepers: they remain logically owned and
physically protected while exact requested bytes are accessed through the sudo
alias. Simply enabling strict bounds in the current invalidator is unsafe
because non-target siblings can be erased from the re-protection plan. Partial
PUTs must preserve untouched GPU-owned bytes.

Coverage must include normal GET/PUT/SDCRZ, inline and fused list transfers,
and separate synchronous treatment for GETLLAR/PUTLLC/PUTLLUC. Raw-SPU MMIO
and unsupported mappings retain existing behavior.

The synchronous ticket is an enabling prototype. Continue only if it removes
roughly half of handled covered-MFC faults or improves a clean A/B by about 10%.

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

The broker is worthwhile only if the oracle finds repeated/concurrent ranges or
meaningful issue-to-consumption slack and a prototype materially reduces
primary submissions/events. If the data are true, immediately consumed
dependencies with no prediction or overlap window, this is a real
synchronization bound rather than an implementation accident.

## Deterministic PPU fault

The one-per-frame PPU fault is independently valuable: removing its measured
11.35 ms in the clean ledger, or 14.79 ms in the instrumented oracle interval,
would be material if the time is fully serialized. The differing observations
are not interchangeable absolute benchmarks, and neither is a guaranteed
saving. Aggregate it by semantic access and ownership state as well as address
and guest PC in oracle v2.

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

The clean ledger measured about 56.3 ms/frame and the later oracle interval
about 53.3 ms/frame; 30 FPS requires 33.3 ms, so roughly 20--23 ms must be
removed from the critical path. The one-per-frame PPU fault is a material lead,
and oracle-v1 attached nearly all expensive readback wait to handled faults,
but those timers overlap across nested and parallel actors. They do not prove a
20 ms saving. Reaching 30 likely also requires hiding or removing SPU/RSX
coherence work and, depending on scene, additional guest-compute improvements.
In the later, nonstationary 14-FPS sample, main-PPU guest execution alone
occupied roughly 46 ms/frame; that is not a clean critical-path measurement,
but it rules out treating coherence as the entire problem. A plausible route
remains visible, but 30 FPS is not established until a correct semantic branch
reaches at most 40 ms/frame with a separately measured remaining cost and then
survives cross-scene validation.
