# Fault-oracle v2 capture at e0be3532

This report records the valid live run of the bounded Cell/RSX fault oracle at
`e0be35322`. The steady TLoU bedroom is used as a controlled microscope here;
the result selects a game-general coherence mechanism, not a bedroom-specific
address or guest-PC rule.

## Reproduction identity

- Source: `e0be35322`
- Candidate application:
  `/Users/hamza/Documents/rpcs3-repro/binaries/rpcs3-e0be3532-fault-oracle-v2.app`
- Candidate executable SHA-256:
  `91fc7261878e31cca83c9325c10b61874845e4f207bbf4879ff22af874c76cc9`
- Candidate executable size: 75,370,992 bytes
- Candidate Mach-O UUID: `CF89BB2E-33B2-36AD-AE1E-E8717A807605`
- Embedded build identity: `RPCS3 v0.0.42-19709-e0be3532 Alpha`
- External artifact directory:
  `/Users/hamza/Documents/rpcs3-repro/artifacts/cellfault-v2-2026-08-08-e0be3532`
- External manifest: `MANIFEST.md`, SHA-256
  `eedf2cb68c5cb8542f71fe5b29dc8ab9a022b9b9bec3d98cbc839722309ee494`
- Complete `RPCS3.log` SHA-256:
  `6358555ebbed0a91d5bf46910250e3848fe20fba1352ae925da9faa562e155b1`
- Exact interval file SHA-256:
  `f0730fe13f3e5dbaf3202fc2af764fa2402615fd0618f35a02ca0b07c19604a6`
- Post-window stack sample SHA-256:
  `35607eedf560d1ecdbd6eeef3a89a265ff5dfdf164fcc2dcca0a137aa5eb5331`
- Global configuration SHA-256:
  `40e2913ac211bc052aa5a2e5f7dd5ed23c92c82bd95c1d9cbdb22b9e2d956fe1`
- Title configuration SHA-256:
  `ec6a2bbb516b11ad577b4764df5365ddba26d285783745e6a0ad8f0457a25a4c`
- Patch configuration SHA-256:
  `f2a89abcad930220d7e0e6be6108e5f901430103ed58e064cd6a76750361e54c`
- Patch database SHA-256:
  `41c28ddf96e49864de2486deaf98b59c57b6ebfa82c0d6b6cfbcdcc4b92d88b3`
- MoltenVK dylib SHA-256:
  `05df2d2145b9ef5b86b08ed93a7a6e916815c2bbac244298a50b2d58061600d1`
- Exact log byte interval: `[12702865, 14931402)`
- Wall interval: `2026-08-08T23:13:29Z` through
  `2026-08-08T23:14:40Z`

The run used a fresh isolated HOME, the equal-RR47 source configuration, nice
level zero, a windowed 1280x720/100% title configuration, and the debug overlay
enabled. The exact launch identity, enabled patches, pre/post autosave hashes,
and isolated-HOME path are retained in the external manifest. The five-second
stack sample was taken after the log window and is not used in any timing below.
No separate visual verdict was recorded for this capture.

## Window boundaries and pacing

The full marker-to-marker window starts at the included package-`0x202` marker
at emulator time `0:02:42.818870` and ends at the excluded boundary marker at
`0:03:53.080875`. It contains 1,276 frame periods in 70.262005 seconds:
**18.1606 FPS**. Mean and median periods were 55.064 ms and 53.834 ms; p95 was
64.328 ms, p99 was 75.685 ms, and the maximum was 105.562 ms.

Fault attribution uses cumulative `CELLSTAT` deltas and the 67 fully contained
`CELLFAULT_SUM` intervals after the first in-slice drain. Its boundaries are
`0:02:43.161796` and `0:03:52.212842`: 1,254 frames in 69.051046 seconds, or
18.1605 FPS. The first summary is excluded because it includes events from
before the opening boundary. The fault counts, origin partitions, MFC-coverage
counts, and per-operation section/readback storage totals close exactly over
the 1,254-frame interior window. Timing counters differ slightly at snapshot
boundaries, and global readback counts/bytes intentionally include operations
that are not attached to a retained fault event.

This is a valid mechanism-attribution capture, not an overlay-on/off benchmark.
Its absolute FPS must not be used to claim observer cost or production speedup.

## Oracle-v2 validity gates

Every attribution gate passed over the interior window:

- zero ring drops and zero untracked semantic-site events;
- 14,615 of 14,615 SPU faults had valid MFC context, and every retained MFC
  interval contained its fault: **100% coverage**;
- zero missing SPU MFC contexts and zero MFC fault misses;
- zero section overflow, section/readback mismatch, pairing error, or readback
  outside its operation's locked range; and
- zero offloader, ZCULL, or unknown-origin events.

The optimized list-GET context and ordinal-paired two-section observations are
therefore live-validated. Unlike oracle v1, exact semantic-site shares and
section relations from this window are usable for mechanism selection.

## Per-frame mechanism ledger

Normalized over the 1,254-frame attribution window, the run recorded:

- exactly 1.000 main-PPU and 11.655 SPU handled texture faults per frame;
- 9.657 read faults and 2.998 write faults per frame;
- 8.657 MFC GET and 2.998 MFC PUT faults per frame;
- 3.003 readback-bearing faults and exactly four fault-attached readback
  operations per frame, totaling 1,428,288 bytes;
- exactly six global GPU readbacks per frame, totaling 1,934,912 bytes;
- 9.657 flush-queue waits totaling 14.990 ms/frame in aggregate; and
- six global GPU/readback waits totaling 14.943 ms/frame in aggregate.

The outer handled-fault timers total 12.584 ms/frame for the PPU and 38.046
ms/frame across the six SPUs. Those values, the flush timer, and the readback
timer are inclusive and overlap; they must not be added as independent
critical-path costs. The important causal result is that the roughly 15 ms of
GPU/readback wait is on deterministic, readback-bearing faults rather than on
bulk transfer throughput.

## Exact-range and repeated-GET evidence

The v2 section pairing resolves oracle v1's apparent multi-section ambiguity.
In aggregate, a small exact list GET causes one 230,400-byte collateral
readback and one 276,224-byte requested-range readback per frame. In 1,250 of
1,254 frames they belong to one two-section fault; in four frames, eight
one-section faults split the two operations. The 230,400-byte section is a
preceding non-overlapping texture selected only through the shared native
16 KiB protection page, while the 276,224-byte section contains the requested
range. This is direct evidence that native trap granularity is forcing
logically unrelated work. A production exact-range ticket must keep the sibling
section owned and protected rather than invalidate it merely because it shares
a host page.

The larger opportunity is a repeated GET herd. Two exact semantic list-GET
classes collectively take 8,343 no-readback faults, or **6.65 per frame**. They
account for 13.843 seconds, **73.65%** of all fault-attached flush-queue wait,
despite requiring no GPU readback at those events. The corresponding resources
have already been synchronized; repeated fault delivery and RSX handoff are
paying again to rediscover that fact.

The retained per-bucket exemplar intervals for the remaining true readbacks
never overlap, which supports near-serialization but is not an exhaustive trace
of every wait. The deterministic PPU read contributes about 7.97 ms/frame of
GPU wait, and the readback-bearing list GET contributes about 6.95 ms/frame.
Together they explain nearly all of the measured 14.94 ms/frame readback wait.
At 18.1605 FPS the frame period is 55.065 ms. Even removing or hiding all 15 ms
would leave about 40 ms/frame, so reaching 33.33 ms still requires roughly
**another 6--7 ms/frame** from the GET handoff herd, renderer feedback path,
guest execution, or another general mechanism. Coherence is a material route,
not a complete proof of 30 FPS.

## Architecture decision

The next implementation is a game-general, generation-keyed exact GET
ticket/shadow directory, not another bedroom profiling pass:

1. Publish exact RSX-owned intervals with content generation, synchronization
   generation, access class, and a lifetime epoch. Keep a native-page summary
   only as a cheap negative lookup hint.
2. Before an SPU GET touches `vm::g_base_addr`, resolve and pin exact logical
   owners. If the required generation already has a CPU-visible synchronized
   shadow, copy the exact bytes through the safe alias without signal delivery
   or another RSX flush-queue round trip.
3. On a genuine stale generation, synchronize only exact intersecting sections,
   publish the resulting shadow generation once, and allow later same-generation
   GETs to reuse it. Same-page non-intersecting siblings remain protected.
4. Fall back unchanged on an epoch race, unsupported mapping, ambiguous
   ownership, partial-write hazard, atomic MFC command, ZCULL, or offloader
   path. The fast negative path must remain allocation-free and a few local
   loads because ordinary MFC traffic is orders of magnitude hotter than the
   fault path.

Addresses, guest/host PCs, bedroom cadence, origin IDs, and observed rank may
identify tests but may not control production behavior. The rule is valid only
when derived from exact range, direction, ownership and content generation,
synchronization generation, and MFC tag/barrier ordering.

The prototype must first prove invariants with focused tests, then demonstrate
correctness and reduced faults/handoffs in the bedroom, and finally survive
distinct TLoU scenes with different Cell/RSX traffic. Performance claims require
thermally conditioned, overlay-off A/B windows. A bedroom-only win or any stale
shadow, sibling-protection loss, or ordering violation rejects the design.
