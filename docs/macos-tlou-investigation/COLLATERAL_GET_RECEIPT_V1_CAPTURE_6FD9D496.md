# Collateral GET receipt-v1 capture (`6fd9d4968`)

Date: 2026-08-08

## Verdict

The first behavioral Cell/RSX coherence prototype was safe but inert in this
bounded bedroom run. The fast path produced zero hits. Of the 9,066 resolver
outcomes in the exact counter interior, 7,467 rejected a still-live exact
logical owner and 1,599 rejected directory state; every other receipt-result
delta was zero.

This falsifies receipt v1's timing assumption. It does not falsify
generation-keyed coalescing while an exact owner is live, and it does not
support any performance claim. No repeat of this receipt-only bedroom run is
needed.

## Identity and exact boundary

- Behavior source: `6fd9d496897f4c63921cea8e32de14ad75cd11e3`
- Documentation present at launch: `0767c32f1`
- Preserved app:
  `/Users/hamza/Documents/rpcs3-repro/binaries/rpcs3-6fd9d496-cell-get-receipt.app`
- Executable SHA-256:
  `78161278460f618b18beb356fc0fcfafb4bda9978cd0c72e5116a7b45e6b8232`
- Original log:
  `/Users/hamza/Documents/rpcs3-repro/cellget-6fd9d496.fNlqef/home/Library/Caches/rpcs3/RPCS3.log`
- Original log size: `15,522,351` bytes
- Original log SHA-256:
  `46c2523c574393bb5eb2fd89018e38911f3dacc68c4565e8de03d6a3783ff34c`
- Exact zero-based half-open byte interval: `[13549702, 15053344)`
- Exact interval size: `1,503,642` bytes
- Exact interval SHA-256:
  `ff9f023f66c471bdd9665a7de1e614f41e14455ba9145821840eb95da32a6b54`

The interval is externally preserved at
`/Users/hamza/Documents/rpcs3-repro/artifacts/cellget-receipt-v1-2026-08-08-6fd9d496`.
Its `MANIFEST.md` SHA-256 is
`bb60f71dfac37899ee9d37d914991b5fcee9b6a111eb119cda696f8bf523156c`.
The executable's embedded revision banner is stale; the executable hash and
full source commit above are authoritative.

The slice contains 876 package-`0x202` present markers. Its 875 complete
first-to-last periods span 44.990309 seconds: 19.448633 FPS, with
51.417/50.905/55.899/59.627/75.729 ms mean/p50/p95/p99/maximum gaps. The debug
overlay was enabled, so this is context for the functional experiment rather
than a comparable absolute-FPS result.

## Exact counter result

The first-to-last `CELLDIR`/`CELLSTAT` interior spans 44.155817 seconds.
Cumulative deltas are:

| Counter | Delta |
| --- | ---: |
| `ready_hit_n` | 0 |
| `ready_exact_owner_n` | 7,467 |
| `ready_directory_n` | 1,599 |
| all other ready-result counters | 0 |
| `read_probe_n` | 8,207 |
| `read_handled_n` / `handled_maybe_n` | 8,205 / 8,205 |
| PPU / SPU renderer faults | 859 / 9,916 |
| flush waits | 8,205; 11.171823 aggregate seconds |
| GPU-event/readback waits | 5,154; about 17.777 aggregate seconds |
| readback bytes | 1,662,089,408 |
| exact recounts | 8; 1,750 aggregate microseconds |

Ownership mismatch, missing/excess reference, poison, underflow, overflow,
abandoned-mutation, and sequence-error deltas were all zero. There were also
zero stable-clear or handled-inconclusive probes.

One SPU block (`size=22`) compiled late in the window, at emulator time
`0:03:33.114837`. There were no `RsxKick` recoveries, device-loss events,
audio-device switches, or fatal log lines. The isolated compile prevents using
the window as a clean performance comparison, but it cannot explain a
cumulative zero-hit result. Repeating a stable run for that event would not
change the receipt reachability decision.

## What failed

Receipt v1 could reuse Cell backing only after a real Vulkan flush had copied
the relevant bytes, published a matching generation receipt, and removed the
exact logical owner from the requested range. Its rejected timing assumption
was that later members of a herd would reach the helper after a leading member
had completed that transition.

The run shows the opposite ordering. Roughly 8.53 exact-owner rejections occur
per frame: readers reach the preflight while the exact owner still exists, so
they all enter the legacy fault/flush path. Any receipt published by that path
arrives too late for the already-concurrent group. Loosening the exact-owner
check would be unsafe because the requested bytes have not yet been proven
current in Cell backing.

The run is safe in the relevant bounded sense: the new local-store copy branch
never executed, all ownership integrity counters stayed clean, and every
candidate retained legacy behavior. The earlier source reviews and tests still
cover the branch itself; this zero-hit run cannot add runtime evidence for a
branch it never took.

## Decision and next gate

Do not tune or broaden the post-flush receipt rule, and do not run an
overlay-off A/B for it. Preserve the ownership, lifetime, hook-coverage, and
counter infrastructure, but treat receipt v1 as a negative experiment.

Before implementing a live-owner broker, add a behavior-neutral oracle that
measures both:

1. **Exact-owner joinability.** For each semantic owner/generation and exact
   GET range, count a prospective leader and same-generation followers, their
   arrival spread, the legacy synchronization they share, and how many
   submissions/handoffs a single-flight operation could actually avoid.
2. **MFC issue-to-consumption slack.** Measure issue to the first tag, barrier,
   or fence completion demand for the affected command, preserving the existing
   MFC ordering model. This establishes whether a real readback can overlap
   useful Cell work rather than merely moving the same wait.

The oracle must remain observational and use only emulator-wide semantics—not
the bedroom's addresses, PCs, sizes, cadence, or title identity. Implement a
generation-keyed single-flight broker only if it predicts material
critical-path savings and exposes enough tag slack for the asynchronous phase.
The prior oracle-v2 evidence still leaves an architecture-scale route worth
measuring, but receipt v1 contributes zero realized saving toward 30 FPS.
