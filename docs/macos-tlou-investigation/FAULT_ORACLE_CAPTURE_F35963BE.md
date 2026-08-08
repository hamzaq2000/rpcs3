# Fault-oracle capture at f35963be

This report records the first live run of the behavior-preserving Cell/RSX
fault oracle. It is a controlled microscope on the steady TLoU bedroom, not a
production benchmark or proof of whole-game benefit.

## Reproduction identity

- Source: `f35963bea`
- Candidate executable SHA-256:
  `506d1455d543cc9c0b16c51c89fa0e92b01162c81f3c70f827c8f5a2ae28dbed`
- External artifact directory:
  `/Users/hamza/Documents/rpcs3-repro/artifacts/cellfault-2026-08-08-f35963be`
- External manifest: `MANIFEST.md`, SHA-256
  `c29b1e314e824bc917291016919e3a0fbfe6c50dc4bf3a94a149832d8e96d422`
- Preserved `RPCS3.log` SHA-256:
  `065cb3beb69482b23c165211fb84a1064681fd740c5617faa8d8c5a131958ffd`
- Post-window stack sample SHA-256:
  `4539faedf9906ae520bba1aa5e85b0dc8f457059832d20f5bcd11c06c659cf6d`
- Title configuration SHA-256:
  `ec6a2bbb516b11ad577b4764df5365ddba26d285783745e6a0ad8f0457a25a4c`
- Autosave SHA-256:
  `b064f3881077e9022623526d8179bf2e3adbafa6d9c1891bc1fc410affc9a6b8`
- MoltenVK dylib SHA-256:
  `05df2d2145b9ef5b86b08ed93a7a6e916815c2bbac244298a50b2d58061600d1`
- Exact log byte interval: `[12205127, 14023723)`
- Wall interval: `2026-08-08T22:23:37Z` through
  `2026-08-08T22:24:47Z`

The five-second stack sample was taken after the exact log interval and is not
included in any timing below. The first in-slice one-second summary was also
excluded because its drain interval began before the byte boundary. The 68
following summaries are bounded by two in-slice drains and exactly match the
first-to-last cumulative `CELLSTAT` delta.

## Observed pacing

The complete-bucket window contains 1,310 validated package-`0x202` frames in
69.8359 seconds: **18.758 FPS**. Mean and median frame intervals were 53.312 ms
and 52.603 ms; p95 was 62.458 ms, p99 was 73.925 ms, and the maximum was
107.535 ms. Of 1,309 intervals, 34 exceeded 66.67 ms, 11 exceeded 75 ms, and
one exceeded 100 ms. Full one-second bins ranged from 13 to 20 frames.

The window contains one tiny size-22 SPU block compilation and one Cubeb
output-device reinitialization. They are recorded as incidental covariates, not
grounds to discard roughly 70 seconds of settled behavior. There was no RSX
shader/pipeline compilation, `RsxKick`, device loss, audio underrun, fatal
error, or crash. The largest sustained dip, around 3:30 in the emulator log,
was not adjacent to either incidental event.

## Aggregate mechanism evidence

The ring lost no events. Over the 1,310 frames it recorded:

- 16,729 handled texture faults: 1,310 PPU, exactly one per frame, and 15,419
  SPU, or 11.77 per frame;
- 12,809 reads and 3,920 writes;
- 3,930 faults with readback, exactly three per frame;
- 5,240 fault-attached readback operations, exactly four per frame, moving
  1,871,057,280 bytes, or 1,428,288 bytes per frame;
- 12,809 fault-attached flush waits totaling 19.556 seconds, or 14.928 ms of
  aggregate wait per frame; and
- 23.133 seconds of fault-attached GPU/readback wait, or 17.659 ms per frame.

The cumulative ledger saw six readbacks per frame and 1,934,912 bytes per
frame. Thus fault records attached only four of six operations and 73.8% of
the bytes, but they captured 99.98% of global GPU/readback wait time. The two
unattached operations per frame were large enough to matter to bandwidth but
were effectively wait-free. At one-second resolution, slower buckets
correlated with more flush-wait time per frame (`r = 0.75`); the worst sustained
bucket reached about 50.2 ms of aggregate flush wait per frame.

These timings overlap across actors. GPU-event and readback timers wrap the
same wait, and PPU/SPU fault durations include nested waits. They must not be
added or interpreted as a predicted frame-time saving.

## Oracle-v1 limits

The aggregate counts above are usable because the ring had zero drops and all
sequence, origin, direction, and cumulative-delta invariants closed. Exact
signature and range percentages are not yet usable:

- the 64-entry signature table saturated in every bucket and excluded 3,883
  events, 23.21% of the interval, from signature aggregation;
- exact MFC context covered only 3,920 of 15,419 SPU faults, 25.42% rather than
  the required 95%; every captured command was a PUT and no GET was captured;
- all 3,920 captured MFC ranges contained their fault, so the retained context
  itself was internally consistent;
- 1,310 of 3,930 section-bearing faults were multi-section cases that overflowed
  the single retained section, exactly one per frame; and
- the matching 1,310 `readback_outside_locked` results compare the combined
  readback against only the first retained section and are an observer artifact,
  not evidence of genuine out-of-range access.

There were no section/readback count mismatches and no offloader-attributed
faults. Nevertheless, the overflow and directionally missing MFC context make
`CELLFAULT_TOP` site shares and exact overlap-policy predictions invalid.

## Decision and generalization policy

The capture preserves an architecture-scale lead: this scene pays one PPU
coherence fault per frame, nearly all expensive readback wait is attached to
handled faults, and pacing degradation tracks flush-queue waiting. Cell/RSX
coherence therefore remains a plausible route to a material gain. The capture
does not establish a recoverable millisecond total, a path to 30 FPS by itself,
or benefit outside this bedroom.

The bedroom is only a repeatable microscope. Production code must be governed
by semantic state such as exact requested range, access direction, ownership,
generation, and MFC ordering. Guest addresses, guest or host PCs, observed
section identities, frame cadence, and bedroom-specific signatures may select
diagnostics but may never become production rules. Any optimization must pass
correctness and performance validation in multiple TLoU scenes with different
Cell/RSX traffic before it can be generalized.

## Oracle-v2 gate

Before an emulation-policy prototype:

1. retain bounded, allocation-free capture while grouping exact semantic sites
   without fragmenting them by moving addresses or origin IDs;
2. include origin identity and recover exact GET/read context, including the
   raw-SPU proxy path, until at least 95% of SPU handled faults have a containing
   MFC range;
3. retain a small fixed array of selected sections and classify each section,
   rather than collapsing a common two-section access into `multi_unknown`;
4. make validation explicitly multi-section-aware; and
5. rerun the bedroom with an otherwise identical overlay-off observer control,
   then repeat any promising semantic result in distinct scenes.

Only proceed to an exact-access ticket or asynchronous coherence broker if a
valid capture shows a stable semantic class explaining at least 80% of
readback wait or a dry-run policy predicts at least 5 ms/frame of
non-overlapping critical-path savings.
