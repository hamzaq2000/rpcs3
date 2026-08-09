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
selected architecture and its correctness gates are in
[`COHERENCE_ARCHITECTURE.md`](COHERENCE_ARCHITECTURE.md). The first live
fault-oracle capture and its exact validity boundary are recorded in
[`FAULT_ORACLE_CAPTURE_F35963BE.md`](FAULT_ORACLE_CAPTURE_F35963BE.md); the
live-validated oracle-v2 capture and architecture decision are in
[`FAULT_ORACLE_V2_CAPTURE_E0BE3532.md`](FAULT_ORACLE_V2_CAPTURE_E0BE3532.md).
The accepted live proof of the behavior-preserving ownership summary is in
[`OWNERSHIP_DIRECTORY_CAPTURE_CBE0B796.md`](OWNERSHIP_DIRECTORY_CAPTURE_CBE0B796.md).

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
protection lifetime. No result in this checkpoint changes emulation behavior.
The immediate blocker is a quiescent lifecycle reset plus real buffered-section
transition tests; after those pass, implement the game-general,
generation-keyed synchronous exact GET ticket/shadow mechanism.

The bedroom is a controlled microscope, not the optimization specification.
Production decisions must be semantic—exact range, direction, ownership,
generation, and ordering—never hard-coded guest addresses, PCs, or observed
bedroom signatures. A promising rule must pass cross-scene correctness and
performance validation before it can support a general performance claim.
