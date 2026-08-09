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
15. A first content-generation snapshot cache is checkpointed on branch
    `opt/macos-tlou-feedback-snapshot-reuse`. Every
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
27. A causal Cell/RSX ledger was added behind the existing debug-overlay gate
   and validated with the full test suite. It distinguishes confirmed
   renderer-handled PPU/SPU faults from rejected Vulkan probes, times the RSX
   flush-queue handoff and GPU-event readback wait, records actual readback
   bytes, and measures inclusive SPU channel/event slow paths and MFC command
   submission paths. The hot MFC counters use 64 independent 128-byte shards
   because this M3 reports 128-byte cache lines. Manual SPU stack escapes finish
   their active timer explicitly, and the signal-handler path remains lock-free
   and allocation-free. These are lifetime-cumulative, inclusive counters: they
   must be differenced over a window and never summed as independent wall time.
28. The clean CELLSTAT-differenced bedroom window lasted 58.716 seconds and
   contained 1,043 validated package-0x202 presents (17.76 FPS, p50 55.4 ms,
   p95 62.9 ms, maximum 82.3 ms), with no SPU/RSX compilation, `RsxKick` timeout, audio
   switch, or device loss. It recorded 1,043 confirmed PPU and 12,086 confirmed
   SPU renderer faults: 223.6 handled Cell-to-RSX faults per second, 92.1% of
   them originating on SPUs. The Vulkan handler saw 16,188 probes, of which
   3,059 (18.9%) were rejected. There were 10,002 RSX producer waits
   (170.3/s, 1.63 ms average) and 6,258 GPU readback waits (106.6/s, 2.22 ms
   average), but only 2,018,113,216 readback bytes, about 34.4 MB/s. The event
   and readback timers currently wrap the same single wait and therefore are an
   integrity check, not additive costs.
29. Normalized per frame, the chain is unusually deterministic: exactly one
   confirmed PPU renderer fault averaging 11.35 ms, 11.59 SPU faults, exactly
   six GPU readbacks totaling 1,934,912 bytes, 9.59 flush-queue waits, and
   164.69 SPU channel/event slow paths. The inclusive times overlap across
   parallel actors, but the fixed cadence gives the next fault-side ring a
   strong attribution target. The current telemetry cannot yet identify which
   addresses dominate, associate individual readbacks with PPU versus SPU
   faults, or distinguish an MFC GET that requires old GPU data from a
   full-cover PUT that may legally discard it.
30. This changes the leading optimization model. The readback volume is modest,
   yet the pipeline pays hundreds of fault, handoff, and GPU-wait transitions
   per second. The likely leverage is eliminating or batching the serialized
   CPU/SPU -> RSX -> GPU coherence protocol, not reducing transfer bandwidth or
   adding another NEON instruction. SPU MFC commands know their exact EA and
   size before direct memory access, so a cheap page-state preflight could avoid
   signal delivery and coalesce the rare slow path. It cannot call the renderer
   unconditionally: the ledger saw about 3.57 million MFC submissions per
   second but only about 206 confirmed SPU renderer faults per second. First,
   an exact-range fault oracle must establish how many slow paths are genuine
   logical overlaps versus macOS 16 KiB page rounding/false sharing. The MFC
   timer itself executes at that 3.57-million/s rate, so this diagnostic run is
   not a clean absolute-FPS benchmark even though its fault/readback direction
   is strong. Before relying on exact MFC timing, compare this build against an
   otherwise identical build with only the per-command MFC timer disabled. If
   throughput changes by more than 3--5%, replace it with deterministic sparse
   sampling or owner-local counters.
31. The separate 5.265-second stack sample occurred later, after the exact
   ledger window, when throughput had fallen to about 14.1 FPS and fault
   pressure had warmed down; do not merge its percentages with the earlier
   interval. It nevertheless localizes the mechanism: all 1,209 sampled SPU
   renderer-fault stacks occurred inside `process_mfc_cmd`. Across the six SPUs,
   mutually exclusive occupancy was 57.2% guest JIT execution, 18.2% actual
   channel blocking, 16.5% non-fault/non-wait MFC host work, 3.8% renderer
   faults, and 4.0% yields. The main PPU remained 64.7% guest compute and 3.9%
   renderer fault; RSX CPU was not saturated, while Metal's serial command
   queue dispatch consumed about 45% of one host core. This validates MFC as the
   place to attach exact command context for the fault oracle, not as permission
   to add a heavyweight check to every MFC submission.
32. Commit `18092fc7c` implements the first-stage fault attribution oracle
    without changing emulation decisions. It removes the perturbative
    per-command MFC timer, carries a lightweight TLS context only around actual
    DMA/list/atomic memory execution, and publishes confirmed renderer faults
    into a bounded 4,096-entry MPSC ring. Each event records the CPU origin,
    host and guest PCs, fault direction/width evidence, exact MFC EA/size/class,
    renderer path, nested handoff/readback intervals, actual readback range, and
    texture-section full/confirmed/native-page-expanded ranges captured under
    the texture-cache lock. The RSX thread emits one-second summaries and
    bounded top signatures. All 195 enabled tests pass, including seven new
    queue/context/classification tests; two pre-existing tests remain disabled.
    The live capture is valid only when ring drops, signature overflow,
    multi-section overflow/mismatch, MFC-context misses, and offloader events
    are zero or immaterial. Because the hot path still performs one relaxed
    enable load per executed MFC transfer, a debug-on/off observer A/B remains
    mandatory before using its absolute FPS.
33. The first live oracle interval at `f35963bea` is preserved and fully
    reported in `FAULT_ORACLE_CAPTURE_F35963BE.md`. Its 68 complete buckets
    contain 1,310 frames in 69.8359 seconds (18.758 FPS, p95 62.46 ms, maximum
    107.54 ms), 1,310 PPU and 15,419 SPU handled faults, and zero ring drops.
    The scene issued exactly six global readbacks per frame; four were attached
    to handled faults, yet those four captured 99.98% of global readback-wait
    time. This keeps coherence/flush serialization viable at architecture
    scale. Exact attribution is not ready: the first-64 signature table
    excluded 23.21% of events, MFC context covered only 25.42% of SPU faults
    and no GETs, and one multi-section case overflowed per frame. The bedroom
    remains a controlled microscope. Production behavior must use semantic
    range, direction, ownership/generation, and ordering evidence and must pass
    cross-scene validation; observed addresses, PCs, and cadence are never
    production rules.
34. Commit `e0be35322` implements oracle v2 without changing emulation policy.
    Compiler signal fences retain list-GET TLS context through asynchronous
    access violations without emitting a hardware fence. Events identify
    normal DMA, optimized list, atomic, and raw-SPU-proxy sources; retain two
    ordinal-paired section/readback observations; validate each operation
    against its own locked range; and aggregate an exact 128-entry semantic-site
    table without keying on moving addresses or origin IDs. Separate
    `spu_mfc_valid`, `spu_mfc_missing`, and `spu_mfc_fault_miss` counters make
    the 95% SPU-origin gate unambiguous. The bounded ring is 2 MiB.
    Release+ThinLTO linked successfully and all 198 enabled tests passed; two
    existing tests remain disabled. The preserved app binary has SHA-256
    `91fc7261878e31cca83c9325c10b61874845e4f207bbf4879ff22af874c76cc9`.
35. The preserved oracle-v2 capture at `e0be35322` passes every causal gate.
    Its full marker window was 1,276 periods in 70.262005 seconds (18.1606
    FPS); its exact attribution window was 1,254 frames in 69.051046 seconds.
    Ring drops, untracked sites, missing/non-containing MFC context, section
    overflow, mismatches, pairing errors, out-of-range readbacks, offloader
    cases, and ZCULL cases were all zero. All 14,615 SPU handled faults carried
    a containing MFC range. Per frame it measured one PPU plus 11.655 SPU
    faults, four fault-attached of six global readbacks, 1,934,912 global
    readback bytes, 9.657 flush waits totaling 14.990 ms, and six GPU/readback
    waits totaling 14.943 ms. The paired evidence identifies one exact
    230,400-byte native-page-collateral section per frame. More importantly,
    two semantic list-GET classes take 6.65 no-readback faults/frame and 73.65%
    of aggregate flush wait after the data are already synchronized. The
    detailed artifact boundary and architecture decision are in
    `FAULT_ORACLE_V2_CAPTURE_E0BE3532.md`.
36. Commit `44f581fb1` is a behavior-preserving Cell-access ownership-directory
    checkpoint. It summarizes texture-cache `NO` owners in fixed 16 KiB
    granules. Common buffered-section protect, confirmed-range protect, range
    expansion, and discard transitions maintain conservative per-granule owner
    counts. A stable probe is only a negative texture-owner hint: `clear` does not prove
    that VM, ZCULL, or another subsystem permits access, and it neither pins a
    section nor establishes a protection/lifetime epoch. With the debug overlay
    enabled, read faults record summary/handler attribution and a cache-try-lock
    exact recount runs at most every five seconds. `CELLDIR` reports recount
    mismatches, missing/excess references, poison and mutation errors, cache-
    busy skips, and total/maximum recount duration. The directory is not yet
    consulted by MFC or any other behavioral path. Its eight focused tests pass;
    all 206 enabled tests pass, with two existing tests disabled.
37. The accepted ownership-summary capture at `cbe0b7960` passes the bounded
    live gate. Its 45.116754-second interval contains 825 frame periods
    (18.2859 FPS). The first-to-last `CELLDIR` interior spans 815 frames in
    44.560866 seconds, with 8,412 accepted read probes and nine exact recounts.
    It adds zero handled inconclusive or clear probes and records zero recount
    mismatch, missing/excess reference, poison, expected-count overflow,
    mutation error, sequence error, or cache-busy scan. The final recount
    matches 31 live sections, 5,548 owner references, and 4,983 granules
    exactly. The recounts averaged 364.444 us and peaked at 441 us in the
    interval (645 us lifetime maximum). Four of more than 25,000 probes over
    the complete run fell back
    conservatively as inconclusive—two before and two after the interval—with
    zero stable-clear result, mismatch, or poison. This does not invalidate the
    accepted window or require another bedroom-only summary run. Full artifact
    identity and boundaries are in `OWNERSHIP_DIRECTORY_CAPTURE_CBE0B796.md`.
38. Commit `6fda0daf0` completes the behavior-neutral lifetime prerequisites.
    At the proven boundary after the fully derived renderer exists but before
    backend initialization creates protected sections or Cell execution
    resumes, it clears the texture and nontexture ownership planes, resets
    validation state, and advances a renderer-lifetime epoch. The new
    nontexture `NO` plane accounts for ZCULL page protection and transient
    texture-cache prelocks. Its exact-coverage handoff removes a transient
    owner only after a texture owner covering the complete prelock range has
    been published; failure is nonfatal, retains the nontexture owner, poisons
    the directory, and therefore forces any future behavioral user to the old
    path. At that historical checkpoint, a stable session pinned directory
    ownership and epoch under a renderer-lifetime shared lock followed by the
    texture-cache or ZCULL pages lock and the directory lock innermost; no
    source lock was acquired while that session lived. Two integration tests
    exercise real `buffered_section`
    confirmed-range expansion/unprotect and physical-unlock/discard lifecycles.
    All 13 focused tests and all 211 enabled tests pass, with two existing tests
    disabled, and Release+ThinLTO `rpcs3_emu` builds. Nothing in the checkpoint
    changes an access decision.
39. Commit `6fd9d4968` is the first behavior-changing Cell/RSX coherence
    checkpoint, but its scope is intentionally narrower than a general ready-
    snapshot ticket. A normal Vulkan framebuffer flush may publish a receipt
    only after it actually copied a linear, exclusion-free range into Cell
    backing and the source RTT still has the captured nonzero content
    generation. The receipt records that exact copied range, generation, and
    renderer epoch; it owns an intrusive RTT reference so a flush-to-discard
    transition cannot retire or reuse the producer while the proof survives.
    A later ordinary SPU GET may copy through the sudo alias only for native-
    page collateral: the exact requested range has no current locked logical
    owner, a protected native-page sibling still exists, exactly one logical
    section and one covering receipt explain the request's intersection with
    that section, and the pinned RTT remains live at the receipt generation.
    Bytes in a request gap outside that sole logical section remain ordinary
    guest-authoritative Cell backing. Every ambiguity or failed VM, lifetime,
    epoch, nontexture, sibling, exact-owner, receipt, or generation check falls
    back to the old fault path before local store is modified.

    Receipt lifetime is bounded deliberately. A receipt survives the immediate
    flush-to-discard handoff, but reset, rebind, destroy, a new DMA transfer, an
    overlapping VM unmap, renderer teardown, memory pressure, or frame end
    clears it. Frame expiry bounds retained RTT lifetime and stale-section scan
    cost. The memory-pressure path now clears receipts and performs base texture-
    cache purging under the same exclusive cache lock; a re-entrant callback
    that cannot acquire that lock skips collection instead of racing a ticket
    or self-deadlocking. The implementation never unprotects or discards a
    sibling, advances a predictor, initiates a readback, or mutates the legacy
    buffered-section `flushed` state.

    Hook coverage includes the normal C++ GET path, each whole element in the
    fused six-element list fast path, the per-item inline list path, list
    fallback through normal C++, and LLVM direct GET/GETB/GETF. Accurate DMA,
    MFC debug, PUT/SDCRZ, atomic commands, Raw-SPU/MMIO, RSX local memory,
    zero/oversize/wrapping transfers, 64 KiB-crossing ranges, unreadable VM,
    unsupported renderers, exact owners, nontexture ownership, and ambiguous
    sections all retain existing behavior. MFC barrier/tag completion and
    `last_faddr` cleanup remain on their existing control-flow boundaries.

    The Release+ThinLTO app links, all 20 focused tests pass, the pin/lifecycle
    subset passes 100 shuffled repetitions, and the root full suite passes all
    215 enabled tests with two existing tests disabled. Two independent source
    audits report no remaining blocking lifetime, resolver, or MFC-hook issue.
    This proves only the bounded source checkpoint; the subsequent runtime
    result is recorded separately below.
40. The bounded receipt-v1 runtime capture at `6fd9d4968` rejects the
    prototype's timing assumption. Its exact slice contains 876 package-`0x202`
    markers defining 875 complete periods in 44.990309 seconds (19.448633 FPS
    with the debug overlay enabled). In the 44.155817-second counter interior,
    `ready_hit_n` increased by zero, `ready_exact_owner_n` by 7,467, and
    `ready_directory_n` by 1,599; every other ready-result delta was zero.
    There were 8,205 handled reads, 9,916 SPU and 859 PPU renderer faults,
    8,205 flush waits totaling 11.171823 aggregate seconds, and 5,154
    GPU/readback waits totaling about 17.777 aggregate seconds. Eight exact
    ownership recounts completed with zero mismatch, missing/excess reference,
    poison, mutation, or sequence error.

    Receipt v1 assumed that later readers would arrive after a leader's legacy
    flush had copied the bytes, retired the exact owner, and published a
    receipt. Instead, readers preflight while that owner is still live and all
    fall back before the receipt can exist. The branch was safe but inert: no
    new local-store copy executed, no performance effect was possible, and the
    overlay window supports no absolute-FPS comparison. One late SPU compile
    cannot explain zero cumulative hits, so another receipt-only bedroom run is
    not justified. The exact boundary and artifact identity are in
    `COLLATERAL_GET_RECEIPT_V1_CAPTURE_6FD9D496.md`.
41. Commit **`c25fb7dc2`**, pushed on
    `opt/macos-tlou-feedback-snapshot-reuse`, removes receipt v1's
    inert SPU copy path, receipt metadata/counters, and receipt-specific Vulkan
    lifetime behavior. In their place it adds only a debug-overlay-gated,
    behavior-neutral CELLJOIN + MFCSLACK oracle. It neither copies new bytes nor
    changes protection, invalidation, readback scheduling, MFC completion, tag
    values, or guest-visible ordering.

    CELLJOIN captures the exact ordered Vulkan plan for a deferred read fault:
    renderer epoch, stable directory sequence, section/session/producer and all
    relevant content/transfer/staged/synchronization/write generations, ranges,
    state, geometry, format, and execution role/rank. Same semantic plans may
    group faults from different native pages and cache revisions, but every
    member retains its own fault/invalidation range and completion is accepted
    only when the sole proven materializer covers them all. Q is queue-reference
    release, D is completion of the actual flush/data work, and U is completion
    of unprotect; success requires exact execution and generation proof with
    `semantic >= U >= D`, while a follower no-op requires an empty unchanged
    replan after the materializer. All other outcomes are explicit terminals.

    MFCSLACK attaches the same cohort/member key to a direct unordered SPU GETL
    and follows its enclosing-list completion through WrTagUpdate, actual tag
    publication, first RdTagStat demand, and return. Only `ALL` or single-bit
    `ANY` observations with a consistent mode/mask/publication/return and no
    ordering edge are valid. Immediate, multi-bit ANY, early/overwritten query,
    barrier/fence, later same-tag work, stall/resume, unsupported AsmJit tag
    path, lifecycle, overflow, missing-publication, and deadline cases are
    censored, never converted to slack. Fixed capacities are 16 plan sections,
    64 cohort slots, 64 members/cohort, 1,024 cohort and member records, 16
    active list candidates, 64 pending candidates, and a 4,096-result MFCSLACK
    ring.

    The current lock order is texture-cache or ZCULL pages lock, then the
    ownership-directory stable-session lock innermost. The receipt-only
    renderer-lifetime shared-lock/session layer is deleted; the quiescent
    renderer epoch remains as identity. The focused suite passes 52/52, the
    root suite passes 247/247 enabled with two disabled, and the Release+ThinLTO
    full app link passes. All findings from two independent source audits are
    resolved, including explicit generation binding and a regression test for
    same-owner SPU thread-group restart. This checkpoint is launch-ready for
    the bounded diagnostic run. Exact terminal/censor mappings, completion
    gates, artifact identity, and the predeclared live protocol are in
    `CELLJOIN_MFCSLACK_ORACLE_PROTOCOL.md`.

## Current blocker

Receipt v1 is removed from the current source and must not be repeated. The
current blocker is one behavior-neutral CELLJOIN/MFCSLACK oracle run from the
preserved `c25fb7dc2` app with the overlay on. Target
30--60 seconds and at least 600 complete package-`0x202` periods, then allow two
seconds for pending tag candidates to drain.

The interval is usable only with zero plan/slot/member exhaustion, cohort/member
record loss, MFCSLACK ring loss and active/pending overflow, and with no stale
terminal, incomplete queue proof, unexplained interior live candidate,
ownership poison/mismatch, kick timeout, device loss, or fatal error. Claimed
join time comes only from complete homogeneous-SPU-GET cohorts with exactly one
proven materializer, coverage of every member fault, and all other members
proven no-op. Claimed slack comes only from matched `valid=1, censor=0` records.

Synchronous GO requires all four predeclared gates: at least 70% of exact-owner
attempts in multiplicity-at-least-two cohorts, at least four validated followers
per frame, at least 80% of followers resolving no-readback under the same proven
closure, and at least 2.5 ms/frame in the offline union of critical Q/tail
intervals. STOP that route below 1.5 ms/frame or below two validated followers
per frame. A value in between is not implementation authorization.

Asynchronous GO requires at least 85% definitive dependency coverage and at
least 10 ms/frame of conservatively de-duplicated safe hide. A credible 30-FPS
line further requires about 14 ms/frame hide, predicted frame time `Tpred <=
35 ms`, and an optimistic demand envelope at or below 33.3 ms. STOP that route
if coverage is below 70%, safe hide is below 7 ms/frame, or even optimistic
`Tpred` exceeds 35 ms. Repair the oracle, without inferring zero opportunity,
if a validity gate fails.

Overlay-on timing is attribution only. `CELLJOIN *_interval_sum_us` and
`MFCSLACK *_slack_sum_us` are overlapping arithmetic sums, not wall time, FPS,
or additive critical-path savings. Even an offline union is a counterfactual
bound until a later behavior-changing overlay-off A/B. No broker or other
optimization is implemented in this checkpoint.

The oracle has no hard-coded title/bedroom address, PC, observed size, identity,
cadence, or rank key. Current live section/producer identity and execution rank
participate only in exact plan equality; no fixed bedroom value selects a
group. Policy inputs are general emulator semantics: ordered plan, owner/session/
producer generations, range coverage, renderer/directory lifetime, MFC GET/tag,
and guest ordering. A bedroom-only positive result still requires distinct-
scene validation after a behavior-changing prototype exists.

Direct PPU JIT accesses still rely on host protection and remain a later
producer-scheduled-shadow problem. Keep official 1280x720/100% settings and
treat the removed historical WCB/WDB patch only as a canary. The v2 window's
roughly 15 ms/frame of true readback wait is material, but removing all of it
would still leave about 6--7 ms/frame to reach 30 FPS.

## Current source work

- Apple ARM mapping-reservation protection fix plus focused tests.
- Per-SPU-program LLVM compile claim plus concurrency tests.
- Exact ARM executable-range cache flushes.
- Apple ARM runtime compile gate that performs a short quiescence handshake,
  compiles outside the global CPU lock on the requester, serializes concurrent
  misses, and safely releases peers on normal/abnormal exit.
- Checkpointed renderer performance work on
  `opt/macos-tlou-feedback-snapshot-reuse`: per-render-target content tracking,
  safe snapshot reuse, exact-region refresh, and feedback-copy counters. A
  no-benefit proactive disjoint-write extension was tested and removed.
- Equal-RR47 macOS scheduler correction in `ec2da5b59`; it removes the observed
  PPU/SPU-to-RSX priority inversion and improves pacing, but is not a 30-FPS
  throughput fix.
- Debug-overlay-gated causal Cell/RSX coherence ledger in `5670a849f`, including
  handled-fault origins, flush/readback waits and bytes, and sharded SPU
  channel/MFC timing.
- Bounded Cell/RSX fault attribution oracle in `18092fc7c`, including exact MFC
  execution context, signal-safe fault capture, locked texture-section range
  classification, and compact `CELLFAULT_SUM`/`CELLFAULT_TOP` output. It also
  removes the perturbative exact-MFC timer from the capture build.
- Oracle-v2 attribution repair in `e0be35322`, including signal-visible list
  context, semantic exact-site aggregation, raw-proxy source identity, paired
  two-section observations, and origin-clean coverage gates.
- Preserved `f35963bea` live-capture report and artifact identity in
  `FAULT_ORACLE_CAPTURE_F35963BE.md`; the report separates trustworthy
  aggregate evidence from the oracle-v1 attribution failures and records the
  oracle-v2 and cross-scene gates.
- Preserved valid `e0be35322` capture report in
  `FAULT_ORACLE_V2_CAPTURE_E0BE3532.md`; it closes every v2 gate and selects a
  generation-keyed exact GET ticket/shadow directory as the next implementation.
- Behavior-preserving Cell-access ownership summary in `44f581fb1`: fixed
  16 KiB conservative `NO`-owner counts maintained by common buffered-section
  lifecycle hooks, stable read-fault attribution, and an exact five-second debug-only recount.
  Busy-scan cadence and recount duration are explicit telemetry. It changes no
  access decision and supplies neither VM/ZCULL safety nor a section lifetime
  pin.
- Accepted `cbe0b7960` ownership-summary capture in
  `OWNERSHIP_DIRECTORY_CAPTURE_CBE0B796.md`: 8,412 accepted reads, nine exact
  recounts, and an exact 5,548-reference/4,983-granule final match with no
  interval mismatch, poison, mutation error, clear, or inconclusive result.
  Four of more than 25,000 whole-run probes conservatively fell back as
  inconclusive outside the interval; no repeat run is required.
- Behavior-neutral Cell-access lifetime preparation in `6fda0daf0`: a
  quiescent renderer reset/epoch, nontexture ownership for ZCULL and transient
  texture prelocks, conservative exact-coverage handoff, and an innermost
  stable directory session with explicit lock order. Two real
  `buffered_section` lifecycle tests cover confirmed-range expansion/unprotect
  and physical-unlock/discard.
- Conservative collateral-GET receipt v1 in `6fd9d4968`: an actual linear,
  exclusion-free Vulkan framebuffer flush can publish its exact Cell-backing
  range, source generation, renderer epoch, and an intrusive RTT lifetime pin.
  An ordinary or optimized-list SPU GET can use that backing only when it has no
  current exact owner, a protected native sibling explains the trap, exactly
  one receipt covers the logical intersection, and every VM/lifetime/cache/
  directory/generation proof remains pinned. Frame end, unmap, new DMA, rebind,
  teardown, and memory pressure bound the receipt lifetime. All failure classes
  fall back without changing legacy invalidation state.
- Negative `6fd9d4968` receipt-v1 capture in
  `COLLATERAL_GET_RECEIPT_V1_CAPTURE_6FD9D496.md`: zero hits against 7,467
  current-exact-owner and 1,599 directory rejections. The post-flush receipt is
  too late for the concurrent reader herd. Do not broaden or performance-test
  this inert branch; its behavioral path is removed in the current working tree.
- Launch-ready behavior-neutral CELLJOIN + MFCSLACK checkpoint `c25fb7dc2`:
  fixed exact-plan cohorts join same renderer/directory/section/producer/
  generation semantics
  across same or different native fault pages, then conservatively prove Q/D/U,
  one materializer, no-op followers, range coverage, and queue release. Matched
  direct SPU GETL candidates follow tag update/publication/RdTagStat boundaries;
  every ambiguous or ordered path is explicitly censored. The receipt copy path
  is gone and no broker is implemented. The focused tests pass 52/52, the root
  suite passes 247/247 enabled with two disabled, and the Release+ThinLTO full
  app link passes. All audited blockers, including same-owner thread-group
  restart lifetime binding, are resolved. The preserved app is
  `/Users/hamza/Documents/rpcs3-repro/binaries/rpcs3-c25fb7dc-celljoin-mfcs.app`;
  exact artifact identity, protocol, and numeric enum maps are in
  `CELLJOIN_MFCSLACK_ORACLE_PROTOCOL.md`.

At `6fd9d4968`, the affected Release+ThinLTO build and full app link pass, all
20 focused Cell-access tests pass, and the pin/lifecycle subset passes 100
shuffled repetitions. The root full suite passes 215/215 enabled tests with two
existing tests disabled, and two independent source audits pass. The earlier
13/211 result belongs to lifetime preparation, eight/206 to the ownership
summary, 198 to oracle v2, and 195 to historical v1. Runtime Vulkan validation
is complete and negative: none of these source results or the zero-hit overlay
capture is a gameplay-performance or FPS claim.

The boot fix is preserved on branch `fix/macos-arm-spu-runtime`, commit
`983c69d5e`, and pushed to `git@github.com:hamzaq2000/rpcs3.git`. The renderer
workaround is preserved separately on branch
`fix/macos-tlou-feedback-copies`, commit `f782cdd3c`, and pushed to the same
fork. It contains only the hidden switch and the two minimal tested
texture-cache decisions; it is not part of the isolated boot-fix commit.

## Safety/state

- No RPCS3 test process should remain after an experiment.
- The clean coherence capture is
  `/Users/hamza/Documents/rpcs3-repro/cell-ledger-correct.2AmIiU/home/Library/Caches/rpcs3/RPCS3.log`
  (SHA-256 `ad39db152b22c01199ac9cc78a69aa852fb21737e42cc23f12d354e2f2b81979`).
  The matching stack sample is durably preserved at
  `/Users/hamza/Documents/rpcs3-repro/artifacts/cellstat-2026-08-08/rpcs3-cellstat-bedroom-d46e343.sample.txt` (SHA-256
  `0d5655cf330555b7dd4434565bf2c0af9dc0c3e4a7ea4e3a8dbdb70c4f76fcf0`).
  The exact 60-second log byte interval is `12120418..12981849`.
- The first fault-oracle capture is externally preserved at
  `/Users/hamza/Documents/rpcs3-repro/artifacts/cellfault-2026-08-08-f35963be`.
  Its manifest SHA-256 is
  `c29b1e314e824bc917291016919e3a0fbfe6c50dc4bf3a94a149832d8e96d422`,
  full `RPCS3.log` SHA-256 is
  `065cb3beb69482b23c165211fb84a1064681fd740c5617faa8d8c5a131958ffd`,
  stack-sample SHA-256 is
  `4539faedf9906ae520bba1aa5e85b0dc8f457059832d20f5bcd11c06c659cf6d`,
  and exact analyzed byte interval is `[12205127, 14023723)`. The sample was
  taken after the interval and must remain separate from its timing.
- The oracle-v2 Release+ThinLTO app is externally preserved at
  `/Users/hamza/Documents/rpcs3-repro/binaries/rpcs3-e0be3532-fault-oracle-v2.app`.
  Its executable SHA-256 is
  `91fc7261878e31cca83c9325c10b61874845e4f207bbf4879ff22af874c76cc9`,
  and its embedded build identity is `19709-e0be3532`.
- The collateral-GET receipt-v1 Release+ThinLTO app is externally preserved at
  `/Users/hamza/Documents/rpcs3-repro/binaries/rpcs3-6fd9d496-cell-get-receipt.app`.
  Its executable is 75,406,304 bytes, has SHA-256
  `78161278460f618b18beb356fc0fcfafb4bda9978cd0c72e5116a7b45e6b8232`,
  and has Mach-O UUID `10C46989-D70B-339D-9F63-83878437D9BB`.
- The corresponding negative receipt-v1 capture is externally preserved at
  `/Users/hamza/Documents/rpcs3-repro/artifacts/cellget-receipt-v1-2026-08-08-6fd9d496`.
  Its manifest SHA-256 is
  `bb60f71dfac37899ee9d37d914991b5fcee9b6a111eb119cda696f8bf523156c`,
  full-log SHA-256 is
  `46c2523c574393bb5eb2fd89018e38911f3dacc68c4565e8de03d6a3783ff34c`,
  exact-interval SHA-256 is
  `ff9f023f66c471bdd9665a7de1e614f41e14455ba9145821840eb95da32a6b54`,
  and its exact source byte range is `[13549702, 15053344)`.
- The valid oracle-v2 capture is externally preserved at
  `/Users/hamza/Documents/rpcs3-repro/artifacts/cellfault-v2-2026-08-08-e0be3532`.
  Its manifest SHA-256 is
  `eedf2cb68c5cb8542f71fe5b29dc8ab9a022b9b9bec3d98cbc839722309ee494`,
  full `RPCS3.log` SHA-256 is
  `6358555ebbed0a91d5bf46910250e3848fe20fba1352ae925da9faa562e155b1`,
  exact interval SHA-256 is
  `f0730fe13f3e5dbaf3202fc2af764fa2402615fd0618f35a02ca0b07c19604a6`,
  and post-window stack-sample SHA-256 is
  `35607eedf560d1ecdbd6eeef3a89a265ff5dfdf164fcc2dcca0a137aa5eb5331`.
- The accepted ownership-summary capture is externally preserved at
  `/Users/hamza/Documents/rpcs3-repro/artifacts/cellown-2026-08-08-cbe0b796`.
  Its manifest SHA-256 is
  `05a185f6ec92d944eec218900484d97badd38b101efcb09a761d707f3be6a096`,
  full `RPCS3.log` SHA-256 is
  `0d7574333f25f0f5f96025c93b30c24496c6d71a98fdcef6e7321dfa87f598c7`,
  exact interval SHA-256 is
  `712fa924c1de170d9616ddf11a4e765bfb2d37268042d1b9dea6c58ce0ff4dbf`,
  and its exact source byte range is `[13408737, 14870766)`.
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
