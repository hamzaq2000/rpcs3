# The Last of Us / RPCS3 Apple Silicon investigation

Last updated: 2026-08-08

## Acceptance criterion

The work is not complete until BCES01584 boots in a window and reaches visibly
interactive gameplay using the current official RPCS3 recommendations and
applicable game patches. Passing compilation, reaching a later assertion, or
merely eliminating the old hot loop is not sufficient.

Do not launch RPCS3 in full-screen mode. Do not overwrite the installed
`/Applications/RPCS3.app` or the user's normal RPCS3 profile while testing.

## Paths

- Source: `/Users/hamza/Documents/rpcs3`
- Test app: `/Users/hamza/Documents/rpcs3/build-baseline/bin/rpcs3.app`
- Disc root: `/Users/hamza/Documents/The Last of Us/The Last of Us + Left Behind/The.Last.of.Us.PS3-DUPLEX/BCES01584-[The Last of Us]`
- Isolated profiles: `/Users/hamza/Documents/rpcs3-repro/home-*`
- Decrypted update executable used for read-only disassembly:
  `/tmp/rpcs3-time-audit.xcHRq0/EBOOT.elf`

## Proven findings

1. The original cold-cache black-screen/high-CPU failure was reproduced and
   localized. A stalled SPU consumer read a recycled all-zero work descriptor,
   combined it with stale local-store metadata, and issued a valid-sized MFC PUT
   to guest EA 0.
2. On Apple ARM64, the guest address reservation was initially host-RW. The
   synthetic page-fault probe therefore succeeded without changing guest page
   state and `vm::range_lock_internal` retried forever. A narrow
   `is_memory_mapping ? PROT_NONE : RW` fix restores the expected fault boundary.
3. Cold compilation also exposed missing cross-core ARM instruction-cache
   publication. Explicit cache invalidation is required for LLVM code sections,
   AsmJit code, raw branch patchpoints, ubertrampolines, and in-place patches.
4. Identical SPU LLVM blocks were being compiled concurrently by several SPURS
   workers. Per-item compile claiming removes that compile storm, although it did
   not by itself cure the descriptor-lifetime bug.
5. A prototype that quiesces registered PPU/SPU guest execution during each
   first-time SPU compile eliminated the original zero-EA failure. A cold run
   built 11,411 SPU functions without the old access violation, illegal
   instruction, or hot `range_lock_internal` loop.
6. That run later asserted at `ndlib/anim/anim-mgr.cpp:1798`. Decrypted-EBOOT
   disassembly proves this is not a time deadline: the PPU polls animation-job
   completion up to 10,000,000 iterations, decrementing a counter from
   `0x00989680`, then asserts. The stale `f3 ~= 10.38` value is not the loop
   condition. Do not treat a timebase-only patch as the immediate fix.
7. A supported warm-cache run with the current wiki configuration and eight
   selected patches reached the menu and ran for 4:31.78. It exercised 11,410
   precompiled SPU blocks and 261 runtime compile gates with no EA-0 fault,
   `SIGILL`, animation assertion, or access violation. The original boot blocker
   is therefore fixed in this control.
8. The menu has an independent renderer artifact: rapidly moving pure-black
   blocks confined to guest rows 648--719 and aligned to 64-pixel tile
   boundaries. MSL fast math Off and RSX memory tiling Off did not change it.
   Strict Rendering Mode On eliminated it in repeated observations. This exactly
   matches upstream macOS-ARM issue #17913 and points to framebuffer-feedback
   handling, not the CPU/SPU patches.
9. MoltenVK 1.4.2 does not advertise
   `VK_EXT_attachment_feedback_loop_layout`. RPCS3's non-Strict fast path still
   samples a live render target in `VK_IMAGE_LAYOUT_GENERAL`, a feedback-loop
   usage explicitly outside the Vulkan specification. A new targeted diagnostic
   that snapshots only framebuffer feedback resources, while leaving global
   Strict mode Off, removes the tiles and restores the observed menu range from
   about 18 FPS to roughly 20--30 FPS.
10. Actual bedroom gameplay with the targeted copy enabled averages roughly
    7--10 FPS. A steady-state process sample proves this is not compilation:
    the SPU LLVM and both RSX pipeline compiler workers were idle, the last SPU
    runtime build was over five minutes old, and the runtime-compile gate did
    not appear in any stack. The main PPU instead repeatedly enters RSX texture
    cache fault/readback handling, including a serialized Vulkan-event wait;
    the RSX thread contends on the same cache while MoltenVK submits work.
11. Disabling all targeted copies restores the black tiles and raises
    instantaneous CPU-side throughput, but produces much worse pacing. In
    matched 20-second intervals the safe path delivered 189 flips (9.45/s),
    no stalls over one second, and a 0.508-second maximum gap; the unsafe path
    delivered 97 flips (4.85/s), six stalls over one second, and a 1.961-second
    maximum gap. The unsafe control also logged three manual `RsxKick`
    recoveries. Feedback-loop UB therefore causes both corruption and hangs.
12. A clean `Release` (`-O3`) + ThinLTO build does not recover the missing
    performance. With all four targeted safeguards it delivered 40 flips in a
    steady five-second bedroom interval (8.0 FPS), with no compilation or kick
    timeout. This rules out the former `RelWithDebInfo` build as the cause.
13. Routing only the primary live-ROP snapshot site through the hidden flag
    improved the same Release+LTO interval to 50 flips (10.0 FPS), but the user
    confirmed that the black squares returned. The primary snapshot alone is
    insufficient. A fresh run with the primary snapshot plus the edge-clamped
    direct-view safeguard, while leaving cyclic-Z and attachment flags at
    normal non-Strict behavior, removed the black squares. The user observed
    about 8 FPS and chose this as the final stopping point. This establishes the
    two texture-cache sites as the minimal tested correctness combination; it
    does not solve their performance cost.
14. The green debug HUD's `Internal Resolution: 3072x1024` is not the output
    or swapchain resolution. The effective title configuration and log remain
    `1280x720` at 100% scale. RPCS3's HUD reports the largest active guest
    framebuffer attachment seen during that frame, after resolution scaling;
    TLoU is creating a large offscreen surface/atlas. One 3072x1024 RGBA8
    snapshot is 12 MiB, so the surface is performance-relevant without being a
    configuration error.
15. A first content-generation snapshot cache is implemented on branch
    `opt/macos-tlou-feedback-snapshot-reuse` but is not committed yet. Every
    render-target write advances a logical content generation, cached dynamic
    snapshots remember the generation they copied, and unchanged sources reuse
    the snapshot. Unknown owners still copy. Cached refreshes now preserve the
    descriptor's nonzero x/y and coordinate transform, fixing an independent
    wrong-region bug; cache identity now includes the component remap.
16. Corrected per-frame counters in the bedroom show the first optimization is
    conservative: about 161.85 forced feedback requests, 152.85 actual copies,
    and exactly 9 generation reuses per frame. It avoids roughly 33 MiB while
    still copying roughly 725 MiB of logical image data per frame. This is a
    real optimization but cannot by itself explain or deliver a 3x speedup.
17. Short title-bar readings of 20--25 FPS are not long-window throughput. The
    title updates from a roughly 0.5-second window and latches the result;
    longer windows include large pacing gaps. `sys_rsx_attribute` package
    `0x202` is a validated one-to-one frame proxy in this scene: every group of
    ten FBSTAT frames contained exactly ten such events.
18. Two benchmark confounds were found. zsh's default `BG_NICE` silently gave
    watchdog-launched RPCS3 processes nice +5; all future harnesses must use
    `setopt NO_BG_NICE` and verify NI=0. More importantly, macOS PPM logs show
    the fanless M3 Air's thermal budget varied by more than 2x across runs. The
    historical ~8-FPS run followed a Release/LTO link and was severely
    throttled. Absolute FPS claims require thermal conditioning/covariates and
    long A-B-B-A windows.
19. A Vulkan-only proactive disjoint-write experiment was implemented with
    conservative coordinate guards (`live_rop`, logical SPP1, and half-open
    scaled scissor rectangles). It passed focused tests, but the bedroom run
    advanced only one cached generation per frame and eliminated no additional
    copies: 164.12 requests, 155.12 copies, 9.00 generation reuses, and about
    707 MiB of logical copy traffic per frame. It was removed because it added
    per-draw cache scanning without benefiting this workload.
20. Fixed-capacity copy-signature telemetry found a single dominant operation
    in the steady bedroom: a live `1280x720`, 8-byte-per-pixel framebuffer at
    guest address `0xc2790000` is copied about 99 times per frame. Over a
    30-second window it accounted for roughly 698 MiB of the measured 732 MiB
    logical copy traffic per frame (over 95%). Same-generation copies differing
    only by channel remap were zero. The next optimization target is therefore
    partial refresh of this persistent snapshot after small intervening draw
    writes, not remap sharing or generic atlas caching.
21. A strict metadata-only cross-frame snapshot oracle was run for 4,321
    rendered frames in the same bedroom. It found zero exact prior-frame
    structural matches and therefore zero same-generation copy-reuse
    opportunities. Only about five eligible snapshots survived to each frame
    boundary; overlapping surface invalidations removed them before a matching
    request in the next frame. Persisting snapshots across frame boundaries is
    not the next copy-elimination path for this workload. The result is a
    conservative lower bound for allocation reuse because invalidated storage
    could theoretically be retained and refreshed, but it cannot avoid the
    dominant image copies.
22. A 64-event write journal then measured the exact conservative union of
    Vulkan draw scissors between same-frame snapshot refreshes without changing
    rendering. In a steady 300-frame bedroom window it classified 28,255
    refreshes (94.18/frame): every one was complete and full-surface, with zero
    partial, disjoint, unknown, overflow, or broken-chain cases. Full, union,
    and bounding-box traffic were identical at 208,318,464,000 bytes over the
    window, about 662.2 MiB/frame. Partial refresh is therefore also a dead end
    for this dominant path; the next experiment must avoid the snapshot itself
    or prove a legal alternative feedback implementation.
23. Disabling only the official `Enable GPU Lighting` patch was a useful
    architecture control. The same bedroom fell to roughly 2.3 FPS while the
    feedback workload remained essentially unchanged (about 163 requests and
    153 copies in the HUD, including about 99 live copies). The patch therefore
    is not the source of the framebuffer-copy chain; moving Naughty Dog's
   lighting path back onto emulated SPUs is catastrophically slower on this
   M3. This is direct evidence that Cell/SPU execution and synchronization are
   also a major bottleneck, not merely the renderer workaround.
24. A new scheduler audit found a plausible broad macOS regression worth an
   immediate one-change A/B. Since commit `3819b9d57` (2025-12-20),
   `thread_base::start()` creates every RPCS3 `named_thread` with both
   `QOS_CLASS_USER_INTERACTIVE` and `SCHED_RR` priority 99. On this M3/macOS,
   the resulting emulator threads are non-timesharing and commonly appear at
   Mach priority 63; this includes PPU/SPU/RSX as well as compiler, profiler,
   logging, and helper threads. The old RPCS3 build documented in the user's
   preserved configuration screenshot predates this change. This is not yet a
   proven performance regression, but equal maximum-priority oversubscription
   is especially suspicious on a 4P+4E fanless machine. Preserve the current
   binary, then compare it against a build that removes only the `SCHED_RR` /
   priority-99 attributes while retaining interactive QoS.
25. Live policy inspection made the scheduler issue concrete. The main PPU and
   six SPUs run fixed `SCHED_RR` at Mach priority 63, while the supposedly
   elevated RSX thread is accidentally lowered to RR 47 by its lifetime
   `scoped_priority(+1)` call; Metal workers remain ordinary timeshare threads.
   The synchronized SPU -> RSX -> Metal path therefore crosses a real priority
   inversion. QoS-only removed the inversion and eliminated long frame gaps in
   a preliminary 30-second window, but throughput fell to 10.24 FPS versus
   12.05 FPS for the immediately following max-RR control (which suffered one
   1.06-second gap). A separate warm max-RR sample reached 15.03 FPS. QoS-only
   is not a performance fix by itself. The final equal-RR47 oracle removed the
   inversion while retaining fixed scheduling: after compilation ceased it ran
   15.08 FPS for 30 seconds with p95 78.1 ms and no >200 ms gap, then 13.96 FPS
   for a thermally drifting 60-second window with p95 82.7 ms, maximum 109 ms,
   and still no >200 ms gap. It also reached `hom-const` in 1:41 versus 2:13
   for the immediately preceding max-RR control. Equal RR47 is a sound pacing
   improvement candidate, but it does not materially raise the best sustained
   throughput and is not the route from 15 to 30 FPS. Stop tuning priorities.
26. An 8-second steady max-RR sample quantified the current Cell-side shape.
   Across the six SPUs, 47.7% of wall snapshots were guest JIT execution and
   52.1% were channel/MFC/fault/wait paths; 12.5% were direct VM/RSX faults,
   and 72.7% of those faults terminated in a Vulkan or mutex wait. The main PPU
   was 56.3% guest compute, 22.7% VM/RSX fault handling, and 21.0% waiting.
   RSX was almost exactly half active and half waiting, while all compiler
   workers were idle. This validates an architecture-first target: reduce Cell
   scheduling and PPU/SPU <-> RSX coherence serialization, rather than looking
   for more compilation or isolated instruction wins.

## Current blocker

The minimal two-site feedback snapshot is correct, but the first whole-surface
content-generation cache only avoids 9 of roughly 164 requests per bedroom
frame, simple disjoint-write generation roll-forward removed no additional
copies, an exact cross-frame oracle found zero matches, and the write-region
oracle found that every dominant refresh follows a full-surface write. The next
leading paths are now architectural: the bounded equal-priority macOS
scheduler oracle is complete, so build a causal Cell/RSX frame ledger next; in parallel,
determine whether the dominant draws can safely render through ping-pong
attachments or a narrowly legal Apple feedback mechanism, thereby avoiding
the full snapshot. Keep the
official 1280x720/100% configuration while measuring; use 50% scale only as a
diagnostic of GPU/copy bandwidth. Separately evaluate the removed historical
WCB/WDB performance patch only as a canary: its ten writes match this
executable, but it repurposes live code and was removed from the official patch
database in February 2026 without a published reason.

## Current source work

- Apple ARM mapping-reservation protection fix plus focused tests.
- Per-SPU-program LLVM compile claim plus concurrency tests.
- Exact ARM executable-range cache flushes.
- Apple ARM runtime compile gate that performs a short quiescence handshake,
  compiles outside the global CPU lock on the requester, serializes concurrent
  misses, and safely releases peers on normal/abnormal exit.
- Uncommitted renderer performance work on
  `opt/macos-tlou-feedback-snapshot-reuse`: per-render-target content tracking,
  safe snapshot reuse, exact-region refresh, and feedback-copy counters. A
  no-benefit proactive disjoint-write extension was tested and removed.

All 191 enabled tests passed after these changes. Re-run after subsequent edits.

The boot fix is preserved on branch `fix/macos-arm-spu-runtime`, commit
`983c69d5e`, and pushed to `git@github.com:hamzaq2000/rpcs3.git`. The renderer
workaround is preserved separately on branch
`fix/macos-tlou-feedback-copies`, commit `f782cdd3c`, and pushed to the same
fork. It contains only the hidden switch and the two minimal tested
texture-cache decisions; it is not part of the isolated boot-fix commit.

## Safety/state

- No RPCS3 test process should remain after an experiment.
- The final renderer profile is
  `home-release-lto-feedback-copy-edge`; it ran windowed with Strict Off,
  `Force Framebuffer Feedback Copies` On, and the Release+ThinLTO binary.
- The previous acceptance log in
  `home-gate-accept-spuclean/Library/Caches/rpcs3/RPCS3.log` was accidentally
  replaced by a read-only decrypt invocation. The important timings and crash
  evidence above were preserved before loss; do not rely on that file as the
  original long-run log.
- Every cold test must use a fresh profile with warm PPU objects only and no SPU
  persistence (`spu-safe-*.dat` must not be copied).
