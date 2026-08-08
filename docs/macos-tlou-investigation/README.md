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

Current uncommitted/experimental work investigates performance rather than
boot correctness. It includes snapshot-reuse telemetry, write-generation and
dirty-region oracles, and macOS scheduler experiments. These diagnostics have
already ruled out several attractive but ineffective ideas; see
[`EXPERIMENTS.md`](EXPERIMENTS.md) for the result ledger and
[`STATE.md`](STATE.md) for the detailed current model and safety notes.

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
- Require warm compiler caches and reject windows containing SPU/RSX builds or
  `RsxKick` recovery.

The performance target is 30 FPS, but it is treated as a hypothesis to falsify,
not as an assumed outcome. The next architecture-level work is a causal ledger
for PPU/SPU channel and reservation waits, MFC/VM faults, RSX flush latency,
GPU readback waits, and transferred bytes. That evidence should decide whether
the next implementation belongs in Cell scheduling, 16 KiB host-page
coherence, or the RSX/MoltenVK feedback path.
