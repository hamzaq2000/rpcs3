# Experiment ledger

Keep each experiment bounded by a watchdog, run RPCS3 windowed, and record the
effective log configuration rather than only the intended YAML values.

| Experiment | Result | Interpretation |
| --- | --- | --- |
| Baseline cold SPU cache | Black screen; high CPU in `range_lock_internal`; EA 0, size `0x4000` | Original failure reproduced |
| Apple mapping reservations start `PROT_NONE` | Infinite host loop becomes an honest unmapped-access pause | Correct containment/host invariant; not sufficient to boot |
| Dynamic/interpreter control | Did not enter the same EA-0 range-lock failure | JIT timing/code path is material |
| Descriptor DMA probe at PC `0x4364` | Guest source `0x30df6440` was already 16 zero bytes before C++ copied it | LQX/ROTQBY and DMA copy exonerated |
| Warm valid descriptor + forced 3-second consumer delay | Descriptor changed from valid to zero | Descriptor has a finite lifetime and can be recycled during host compile stalls |
| Compile deduplication | Successful duplicate builds collapsed; zero descriptor remained | Real performance bug, not sole root cause |
| Stop-world compile without explicit I-cache flush | Deterministic ARM `SIGILL` in valid newly generated code | Cross-core code publication was missing |
| Stop-world compile plus broad cache flush | 359 valid descriptor reads; no EA 0 or `SIGILL` | Coordination and cache publication jointly fix original failure |
| Production exact-range flushes plus compile gate | 455 valid descriptors in probe run | Precise flush coverage works |
| Clean cold-SPU acceptance | 11,411 SPU functions; original failure absent; later animation-job assertion | Original bug fixed, game still not accepted |
| Assertion disassembly | Fixed 10,000,000-iteration completion poll; no clock read in loop | Pivot from clock theory to scheduler/job completion |
| Official-config audit | Previous profile had all six required title overrides off, no patches, `Stub PPU Traps=0`, and fullscreen start on | Previous later assertion is not a supported-config result |
| Supported warm profile | Reached menu; 4:31.78 clean, including 261 runtime gates | Original boot and animation-poll blockers cleared with supported settings |
| Menu video geometry | Black blocks occur only in guest y=648--719 on 64-pixel x boundaries | Tiled render-target/copy/feedback signature |
| Disable MSL Fast Math | Artifact remained | Fast-math translation is not the cause |
| Disable RSX memory tiling | Artifact remained after full restart | Tiling option is not the direct cause |
| Strict Rendering Mode | Artifact absent in repeated menu observations | Exact upstream workaround confirmed; framebuffer feedback is leading cause |
| Strict Off + targeted framebuffer feedback copies | Artifact absent; live menu about 20--30 FPS instead of roughly 18 with global Strict | Narrow path is sufficient and avoids most global Strict overhead |
| Targeted-copy bedroom gameplay, steady state | Roughly 7--10 FPS; LLVM/SPU/RSX compiler workers all idle; main PPU repeatedly performs RSX cache fault/readback waits | Not compilation; CPU emulation plus CPU/GPU coherency serialization |
| Unsafe live-feedback bedroom control | Black tiles return; somewhat higher instantaneous FPS but frequent dips/hangs and much higher host CPU; matched long-window flip rate remains near 10/s | Feedback UB causes corruption and poor pacing; safe copies cost performance but are not the entire deficit |
| Targeted-copy RSX sample | 26/410 RSX samples in MoltenVK CopyImage encoding; repeated dynamic snapshot cache hits unconditionally refresh contents and can break render passes | Per-draw snapshot refresh/tile-store boundaries are the leading incremental cost |
| Release + ThinLTO, all four safeguards | 40 flips/5 s (8.0 FPS), no compilation or kick timeout | Debug-oriented build flags were not the missing performance factor |
| Release + ThinLTO, live-ROP snapshot only | 50 flips/5 s (10.0 FPS), but black squares present | Primary feedback copy is necessary but not sufficient; another safeguard is required |
| Release + ThinLTO, live-ROP + edge-clamped safeguards | Black squares absent; user observed about 8 FPS | Minimal tested correctness combination; retained as the final workaround despite substantial copy cost |
| Historical ZEROx WCB/WDB patch audit | All ten writes match the exact 1.11 PPU hash; patch disables depth reload and conditionally skips a graphics block, but was removed from the official DB in 2026 with no rationale | Plausible readback-stall canary only; not a default or direct feedback-loop fix |
| Content-generation snapshot reuse | Bedroom counters: ~161.85 feedback requests/frame, 152.85 copies, 9 generation skips; ~725 MiB logical copy traffic remains and ~33 MiB is avoided | Correct but conservative; repeated writes anywhere on a large RT invalidate every cached subregion |
| Resolution HUD audit | Effective output is 1280x720/100%; HUD's 3072x1024 is the largest active offscreen guest framebuffer | Do not change the recommended output setting; large offscreen surfaces amplify snapshot cost |
| Frame-proxy validation | Every 10-frame FBSTAT interval contained exactly 10 packageId=0x202 events | Use 0x202 for long-window TLoU FPS; title-bar FPS is short-window/latched |
| Harness priority audit | Background zsh launch silently assigned RPCS3 nice +5 | All future launch wrappers must set `NO_BG_NICE` and verify NI=0 |
| Thermal audit | Historical ~8-FPS run had PPM thermal budget ~6.2k after a large link; later runs ranged roughly 8.5k--14.2k | Fanless M3 thermal state is a major covariate; condition and record it for A/B |
| Proactive disjoint-write generation advance | Reviewed/tested SPP1 live-ROP-only rule advanced 1 cached snapshot/frame, but bedroom remained 155.12 copies/frame, 9.00 real reuses/frame, and ~707 MiB logical copied/frame | Safe but ineffective here; removed to avoid per-draw cache scanning with zero copy reduction |
| Feedback copy signature telemetry | One live 1280x720x8-byte source at 0xc2790000 is copied ~99 times/frame and contributes ~698 of ~732 MiB/frame; remap-only duplicates are zero | Over 95% of logical bytes have one target: keep its snapshot and refresh only regions written between samples |
| One-frame cross-frame snapshot oracle | 4,321 rendered frames; zero exact structural matches, zero same-generation reuse, and zero would-refresh matches after normal invalidation | Frame-boundary persistence cannot eliminate this workload's dominant copies; proceed to partial dirty-region refresh instead |
| Same-frame dirty-region oracle | Steady 300 bedroom frames: 28,255 candidates, all complete/full-coverage; union=bbox=full at 208,318,464,000 bytes (~662.2 MiB/frame), with no fallback/overflow | Full scissor writes make partial refresh worthless; investigate snapshot-free ping-pong or legal feedback paths |
| Official GPU Lighting patch disabled | Same bedroom dropped to roughly 2.3 FPS, while HUD feedback load stayed near 163 requests/153 copies with ~99 live copies | Patch does not create the feedback-copy chain; SPU lighting path is drastically slower, proving Cell/SPU execution remains a separate major bottleneck |
| macOS named-thread scheduler audit | Current code requests interactive QoS and SCHED_RR priority 99 for every named thread; live process shows many emulator threads at fixed priority 63, while the preserved old RPCS3 revision predates this change | High-priority one-change A/B: remove only RR/99, retain QoS, and measure the same scene before designing more local optimizations |
| QoS-only scheduler oracle | PPU/SPU/RSX became equal timeshare threads; preliminary steady window was 10.24 FPS with no >200 ms gap. Immediately following max-RR control was 12.05 FPS but had one 1.06 s gap; a separate clean max-RR sample reached 15.03 FPS | Removing fixed RR smooths tails but loses throughput; do not promote it. Test one final equal-RR47 arm, then stop scheduler work if no substantial win |
| Equal-RR47 scheduler oracle | All PPU/SPU/RSX threads verified RR47. Clean 30-second window: 15.08 FPS, p95 78.1 ms, max 92.1 ms; following 60-second thermal-drift window: 13.96 FPS, p95 82.7 ms, max 109.3 ms; no >200 ms gap in either. Reached hom-const at 1:41 vs 2:13 for adjacent max-RR control | Removes the real priority inversion and improves pacing/startup, but does not beat the best max-RR throughput. Preserve as a candidate; stop priority tuning and pivot to coherence ledger |
| Max-RR Cell/RSX sample | Six SPUs: 47.7% guest compute, 52.1% channel/MFC/fault/wait; direct VM/RSX faults 12.5%, 72.7% ending in Vulkan/mutex waits. Main PPU: 56.3% compute, 22.7% faults, 21.0% waits. Compilers idle | The deficit is split between guest execution and synchronization/coherence; architecture-level causal tracing is justified |

## Next experiments

1. Build a causal Cell/RSX ledger for PPU/SPU CPU time, channel and reservation
   waits, MFC/VM faults, RSX flush-queue latency, and GPU readback waits.
2. Add a fixed-ring fault oracle that classifies 16 KiB native-page faults as
   logical hits, other-lane/padding hits, or chain-only selections, and records
   actual readback bytes plus unioned wait intervals per frame.
3. Audit a snapshot-free Vulkan path for the dominant full-screen live-feedback
   draws: ping-pong attachments first, then a narrowly proven same-pixel
   interlock/framebuffer-fetch path if the shaders qualify. Before changing
   rendering, record shader/primitive/blend/depth state and prove full overwrite
   rather than relying on full scissor alone.
4. Run long, thermally conditioned A-B-B-A windows at NI=0 with debug overlay
   off. Use `hom-const` as the scene trigger and packageId=0x202 as the frame
   proxy; keep audio device and window visibility stable.
5. Only after the supported renderer path is measured, isolate-test the removed
   ten-write WCB/WDB performance patch; require exact patch-log verification and
   visual/depth regression coverage.
6. Keep renderer and title-patch work separate from boot commit `983c69d5e`.
