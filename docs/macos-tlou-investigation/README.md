# The Last of Us on Apple ARM investigation

This directory records a focused investigation of RPCS3 running *The Last of
Us* 1.11 (`BCES01584`) on a base Apple M3 MacBook Air. It exists to preserve
causal evidence across long experiments and to keep experimental work distinct
from fixes that are ready to review.

The work has produced two separately preserved fixes:

- `983c69d5e`: Apple ARM SPU runtime compilation coordination and executable
  cache publication. This removes the original cold-cache black-screen/livelock
  failure and allows the game to boot with the supported title configuration.
- `f782cdd3c`: a narrow framebuffer-feedback copy workaround. It removes the
  flickering black squares seen through MoltenVK without enabling all of Strict
  Rendering Mode, but it remains expensive in gameplay.

Current experimental work investigates performance rather than boot
correctness. It includes snapshot-reuse telemetry, write-generation and
dirty-region oracles, macOS scheduler experiments, and a causal Cell/RSX
coherence ledger. These diagnostics have already ruled out several attractive
but ineffective ideas; see
[`EXPERIMENTS.md`](EXPERIMENTS.md) for the result ledger and
[`STATE.md`](STATE.md) for the detailed current model and safety notes. The
coherence architecture record and its correctness gates are in
[`COHERENCE_ARCHITECTURE.md`](COHERENCE_ARCHITECTURE.md). The first live
fault-oracle capture and its exact validity boundary are recorded in
[`FAULT_ORACLE_CAPTURE_F35963BE.md`](FAULT_ORACLE_CAPTURE_F35963BE.md); the
live-validated oracle-v2 capture and architecture decision are in
[`FAULT_ORACLE_V2_CAPTURE_E0BE3532.md`](FAULT_ORACLE_V2_CAPTURE_E0BE3532.md).
The accepted live proof of the behavior-preserving ownership summary is in
[`OWNERSHIP_DIRECTORY_CAPTURE_CBE0B796.md`](OWNERSHIP_DIRECTORY_CAPTURE_CBE0B796.md).
The decisive zero-hit runtime result for the first collateral-GET receipt is in
[`COLLATERAL_GET_RECEIPT_V1_CAPTURE_6FD9D496.md`](COLLATERAL_GET_RECEIPT_V1_CAPTURE_6FD9D496.md).
The exact source design and predeclared decision protocol for its behavior-
neutral successor are in
[`CELLJOIN_MFCSLACK_ORACLE_PROTOCOL.md`](CELLJOIN_MFCSLACK_ORACLE_PROTOCOL.md).
The completed capture and decisive STOP result for both broker directions are
in
[`CELLJOIN_MFCSLACK_CAPTURE_C25FB7DC.md`](CELLJOIN_MFCSLACK_CAPTURE_C25FB7DC.md).

Important measurement rules:

- Use the official wiki settings and recommended patches recorded in
  `STATE.md`.
- Keep all test launches windowed and use a fresh APFS-cloned profile per run.
- Disable debug/profiler overlays for timing runs and verify process nice level
  zero (`NO_BG_NICE` in zsh launch wrappers).
- Treat `sys_rsx_attribute(packageId=0x202)` as the validated TLoU frame proxy;
  title-bar FPS is a short-window, latched value.
- Condition or record thermal state. This is a fanless machine, and a recent
  link can halve observed throughput.
- Prefer warm compiler caches and record SPU/RSX builds, audio changes, and
  `RsxKick` recovery. Exclude the affected interval or reject the run only when
  the event is recurrent, material, or explains the result; one isolated tiny
  event does not invalidate a long stable window.

The performance target is 30 FPS, but it is treated as a hypothesis to falsify,
not as an assumed outcome. The causal ledger shows that the bedroom issues
hundreds of Cell-to-RSX coherence faults and roughly six synchronous GPU
readbacks per frame while transferring only about 34--36 MB/s back to the CPU.
The first `f35963bea` fault-oracle capture measured 18.758 FPS and attached
99.98% of readback-wait time to handled faults, preserving the coherence path
as an architecture-scale lead. It also exposed observer defects: signature
overflow, only 25.42% exact MFC coverage, no GET context, and one multi-section
overflow per frame. Commit `e0be35322` implements oracle v2's signal-visible
GET context, exact semantic-site table, and paired two-section observations.
Its valid 1,254-frame interior window then achieved 100% containing SPU MFC
coverage with zero ring, site, section, pairing, or offloader loss. It exposed
one 230,400-byte native-page collateral readback per frame and a 6.65-per-frame
herd of already-synchronized GET faults responsible for 73.65% of flush wait.
The first behavior-preserving part of that direction is implemented in
`44f581fb1`: a fixed 16 KiB summary of texture-cache `NO` owners, maintained
from the common buffered-section lifecycle and checked against an exact
debug-only recount. In the accepted `cbe0b7960` marker window, the clean
first-to-last `CELLDIR` interior covered 8,412 accepted read probes and nine
exact recounts with zero new inconclusive probes, clear results, mismatch,
poison, mutation error, or cache-busy scan. Its final recount matched 5,548
owner references across 4,983 granules exactly. Across the whole run, four of
more than 25,000 probes conservatively fell back as inconclusive; none produced
a stable clear. Another bedroom ownership-summary run is not needed.

The stable probe remains only a conservative negative hint about texture-cache
ownership; it does not prove VM or ZCULL safety and does not pin a section or
protection lifetime. Commit `6fda0daf0` closes those behavior-neutral
prerequisites with a renderer epoch, a nontexture `NO` plane, conservative
prelock handoff, and the renderer-lifetime -> texture-cache -> directory lock
order.

Commit `6fd9d4968` adds the first deliberately narrow behavioral use. After a
real linear, exclusion-free Vulkan framebuffer flush has copied bytes into Cell
backing, it records the exact copied range, source content generation, renderer
epoch, and an intrusive RTT lifetime pin. A later ordinary SPU GET can reuse
those exact bytes only when the request has no current logical owner, a locked
native-page sibling explains the trap, exactly one logical section/receipt is
relevant, and all VM, renderer, cache, nontexture, epoch, and current-generation
proofs remain pinned. It initiates no readback and changes no legacy protection,
discard, predictor, or `flushed` state. Receipts expire at frame end and are
also cleared on unmap, new DMA, rebind, teardown, and memory pressure.

The Release+ThinLTO app links; 20/20 focused tests, 100 shuffled repetitions of
the pin/lifecycle subset, and 215/215 enabled full-suite tests pass, with two
existing tests disabled. Two independent source audits also pass. The bounded
Vulkan validation is now complete, and it falsifies the prototype's timing
assumption: `ready_hit_n` remained zero while `ready_exact_owner_n` increased
by 7,467 and `ready_directory_n` by 1,599. Readers arrive while the exact
logical owner is still live; a receipt published after the legacy flush is too
late for that concurrent group. The branch was safe but inert, makes no
performance claim, and does not need another bedroom run. The preserved
executable is
`/Users/hamza/Documents/rpcs3-repro/binaries/rpcs3-6fd9d496-cell-get-receipt.app`
(SHA-256
`78161278460f618b18beb356fc0fcfafb4bda9978cd0c72e5116a7b45e6b8232`).

Commit `c25fb7dc2`, pushed on `opt/macos-tlou-feedback-snapshot-reuse`, removes
receipt v1's inert copy/receipt behavior and implements only a debug-overlay-
gated CELLJOIN/MFCSLACK oracle. CELLJOIN groups exact ordered Vulkan plans by
renderer/directory/section/producer/content semantics, including safe cross-
page grouping with per-member coverage proof,
then records Q (flush-queue-reference release), D (data ready), U (unprotect
complete), and conservative terminal outcomes. MFCSLACK follows matched direct
SPU GETL members through tag update, publication, first RdTagStat demand, and
return; immediate, ambiguous, ordered, stalled, unsupported, or incomplete
paths are censored rather than credited. The same-owner SPU thread-group
restart lifetime is generation-bound and covered by focused tests. The focused
suite passes 52/52, the root suite passes 247/247 enabled with two disabled,
the Release+ThinLTO full app link passes, and all audited blockers are resolved.
This checkpoint supplied the bounded behavior-neutral capture without changing
an access outcome.

The run passes every integrity gate and closes a strict 781-frame interior.
CELLJOIN finds 781 exact homogeneous SPU GET cohorts and 3,493 proven
no-readback followers (4.472/frame), but their de-duplicated critical Q/tail
union is only 0.652 ms/frame, below the synchronous 1.5 ms/frame STOP line.
Only 4,274 of 6,408 handled read attempts (66.70%) enter the accepted herd,
also below the 70% GO share. MFCSLACK exactly joins all 4,274 candidates, but
every enclosing command is ordered `MFC_GETLB_CMD` (`0x45`) and censored as
`outer_barrier_or_fence`; valid deferrable work and safe hide are both zero.

No optimization is implemented. Both the synchronous single-flight and
asynchronous MFC coherence-broker directions are stopped, and another bedroom
run is not warranted. Earlier aggregate SPU wait totals were real but heavily
concurrent, so they were not additive recoverable frame time. Active
architecture work now returns to general non-coherence bottlenecks, led by the
dominant framebuffer-feedback path and guest execution. The overlay-on
16.89567-FPS context is attribution only, not a performance comparison.
The asynchronous STOP is a protocol decision, not proof that GETLB blocks
ordinary SPU computation: barrier-aware overlap remains unmeasured and would
require a revised tracker that preserves queue, tag, fence, and local-store-use
semantics. It is not worth repairing the tracker for this herd alone: even
unrealistically deleting every complete candidate-to-outer-completion span
produces only a 2.075 ms/strict-interior-frame union, still below the 7 ms/frame
asynchronous STOP line and far below the roughly 14 ms/frame needed for 30 FPS.
That scale bound covers the admitted cohort subset, not every conservative
unknown-generation read rejection or readback in the game.

The bedroom is a controlled microscope, not the optimization specification.
Production decisions must be semantic—exact range, direction, ownership,
generation, and ordering—never hard-coded guest addresses, PCs, or observed
bedroom signatures or measurements. A promising rule must pass cross-scene
correctness and performance validation before it can support a general
performance claim.
