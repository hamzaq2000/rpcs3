# CELLJOIN + MFCSLACK capture (`c25fb7dc2`)

Date: 2026-08-08

## Verdict

The bounded behavior-neutral oracle run is valid, and it stops both proposed
Cell/RSX coherence-broker directions under their predeclared gates.

CELLJOIN found a real, highly repeatable exact-plan SPU GET herd: 781 accepted
homogeneous cohorts contained 3,493 proven no-readback followers, or 4.472 per
strict-interior frame. That concurrency is not material critical-path saving.
After de-duplicating overlapping Q/tail intervals, the union is only
**0.652 ms/frame**, below the synchronous **STOP** threshold of 1.5 ms/frame.
The accepted multi-member herd also covers 4,274 of 6,408 handled read probes,
or **66.70%**, below the 70% synchronous GO share.

MFCSLACK closes all 4,274 matching candidates without loss, but every one is
the ordered outer command `0x45` (`MFC_GETLB_CMD`) and terminates with censor 5,
`outer_barrier_or_fence`. There are zero valid deferrable candidates and zero
safe hide, below the asynchronous **STOP** threshold of 7 ms/frame.

This is not an oracle failure. It is the intended distinction between large
inclusive/concurrent wait sums and non-overlapping time a broker could remove
or hide. Receipt v1 was safe but too late; the live-owner single-flight concept
was semantically reachable but too small; and the v1 MFCSLACK scope supplies no
usable evidence for the ordered list commands. Do not implement or repeat
either measured broker from this evidence. `GETLB` orders MFC commands but does
not by itself stop ordinary SPU computation, so a barrier-aware asynchronous
design remains unmeasured rather than disproved. The next architecture search
still returns to general, non-coherence bottlenecks.

## Identity and exact boundary

- Behavior source: `c25fb7dc2`
- Documentation present at launch: `30d2c3f11`
- Branch: `opt/macos-tlou-feedback-snapshot-reuse`
- Preserved app:
  `/Users/hamza/Documents/rpcs3-repro/binaries/rpcs3-c25fb7dc-celljoin-mfcs.app`
- Executable SHA-256:
  `aede4a855a00244c17ec68cbc17d610a5d46ee4d9659db726d041344c04b12f1`
- Executable size: `78,510,912` bytes
- Mach-O UUID: `6DC55168-1C7C-36CA-9DD3-A3B34369EBC1`
- Embedded identity: `RPCS3 v0.0.42-19720-c25fb7dc Alpha`
- Isolated profile:
  `/Users/hamza/Documents/rpcs3-repro/celljoin-c25fb7dc.nKJLr7/home`
- Original log:
  `/Users/hamza/Documents/rpcs3-repro/celljoin-c25fb7dc.nKJLr7/home/Library/Caches/rpcs3/RPCS3.log`
- Original log size: `41,037,583` bytes
- Original log SHA-256:
  `b595d91765364e1cb24c0778df292ac37ae7d85599a965d1befca2f8643fa7ad`
- Exact zero-based half-open byte interval: `[27906514, 38217332)`
- Exact interval size: `10,310,818` bytes
- Exact interval SHA-256:
  `a7f0c73e464021aefbd98900de5e0787cc3ad142ea7a7ce41f69ce48d0705ea7`
- External artifact directory:
  `/Users/hamza/Documents/rpcs3-repro/artifacts/celljoin-mfcs-2026-08-08-c25fb7dc`
- External `MANIFEST.md` SHA-256:
  `3bf1d371a48163d2d2f45e1dd70a90f7d75120f701ad2a8bc0c3ad13bc2634f0`
- Global configuration SHA-256:
  `40e2913ac211bc052aa5a2e5f7dd5ed23c92c82bd95c1d9cbdb22b9e2d956fe1`
- Title configuration SHA-256:
  `ec6a2bbb516b11ad577b4764df5365ddba26d285783745e6a0ad8f0457a25a4c`
- Patch configuration SHA-256:
  `f2a89abcad930220d7e0e6be6108e5f901430103ed58e064cd6a76750361e54c`
- Patch database SHA-256:
  `41c28ddf96e49864de2486deaf98b59c57b6ebfa82c0d6b6cfbcdcc4b92d88b3`
- Effective update EBOOT SHA-256:
  `eb303d226b664298d81bebd181106a90a267b93d5fb38f01998a92cbaaa52c5c`
- Warm-profile seed SHA-256:
  `46c2523c574393bb5eb2fd89018e38911f3dacc68c4565e8de03d6a3783ff34c`

The app ran windowed at verified nice level zero with Vulkan, 1280x720 at
100%, Strict Rendering Mode off, Force Framebuffer Feedback Copies on, the
debug overlay on, and the established supported patch/configuration set. The
user navigated manually to the steady bedroom scene.

The raw marker interval contains 799 package-`0x202` periods spanning
47.290224 seconds, or **16.89567 FPS**. This is overlay-on attribution context,
not a production timing result or an A/B comparison. The stricter cumulative
first-to-last summary interior contains **781 frames**; every per-frame oracle
rate and GO/STOP calculation below uses that denominator.

The interval starts with the included marker at emulator time
`0:03:00.302677` and ends before the excluded boundary marker at
`0:03:47.592901`; the last included marker is `0:03:47.529338`. The 799
included records plus the excluded end boundary define the 799 complete
boundary-to-boundary periods.

As a boundary cross-check, complete detailed records in the full marker window
contain 799 homogeneous SPU herds, 3,579 followers (4.479/window frame), and a
0.652139 ms/frame critical union (521,059.201 us / 799). The independent strict
batch interior closes at 4.472 followers/frame and 0.651551 ms/frame
(508,861.467 us / 781). Both calculations independently land below STOP; the
strict interior remains the cumulative-summary gate calculation.

The launch-side live counter was observed at 1,990 before collection and 2,793
after the drain, a difference of 803 rather than the 799 periods in the exact
raw interval. The four-period discrepancy is retained explicitly and is not
used to adjust either boundary. The byte-aligned raw records and their hash are
authoritative; they remain well above the required 600 periods, so this
capture-controller discrepancy does not invalidate the run.

## Integrity result

The interval passes the protocol's validity gates:

- zero CELLJOIN plan overflow, slot/member exhaustion, cohort/member record
  loss, stale terminal, unexplained live cohort, or queue-accounting failure;
- zero MFCSLACK ring loss, active/pending overflow, unexplained live candidate,
  or unjoined result after the required drain;
- exact terminal closure for every result used in the decision, with one
  covering materializer and proven no-op followers in every accepted cohort;
- zero ownership mismatch, poison, or mutation-accounting failure; and
- no SPU/RSX compilation, `RsxKick` recovery, audio-device switch, Vulkan
  device loss, or fatal event in the accepted interval.

One optional Sarah animation asset lookup (`sarah-tired-idle.stm`) returned
`CELL_ENOENT`. This periodic optional lookup has no material timing or integrity
effect and is the only noted disturbance.

## CELLJOIN result

The strict 781-frame interior contains:

| Quantity | Result |
| --- | ---: |
| handled read probes | 6,408 (8.205/frame) |
| exact-plan members | 5,055 (6.472/frame) |
| exact-plan leaders | 1,562 (2.000/frame) |
| accepted homogeneous SPU GET cohorts | 781 (1.000/frame) |
| accepted herd members | 4,274 (5.472/frame) |
| proven no-readback followers | 3,493 (4.472/frame) |
| de-duplicated critical Q/tail union | 0.651551 ms/frame |

The accepted herd is 84.55% of the already accepted exact-plan subset
(`4,274 / 5,055`). That is useful evidence that plan equality and conservative
completion proof worked, but it is not the predeclared population gate. Against
all 6,408 handled read attempts, the herd share is 66.70% (`4,274 / 6,408`).

The synchronous gates therefore close as follows:

| Predeclared gate | Observed | Result |
| --- | ---: | --- |
| multiplicity>=2 share: GO >=70% | 66.70% | GO not met |
| validated followers: GO >=4/frame; STOP <2/frame | 4.472/frame | GO component met |
| no-readback followers: GO >=80% | 3,493/3,493 (100%) | GO component met |
| critical interval union: GO >=2.5 ms/frame; STOP <1.5 | 0.651551 ms/frame | **STOP** |

The critical union is decisive. Earlier aggregate SPU fault, flush, and wait
totals counted many threads waiting concurrently on the same materializer.
Adding those inclusive waits suggested a much larger opportunity, but they
were never additive frame time. Exact interval union shows that a correct
single-flight implementation could at best remove a small overlapping tail in
this measured path. Replacing those faults with a broker, mutex, or condition
wait would add complexity and correctness risk without clearing the materiality
gate.

## MFCSLACK result

MFCSLACK joins all 4,274 accepted herd members to their CELLJOIN identity. All
4,274 candidates reach a terminal; there are no drops, live candidates,
active/pending overflows, lifecycle losses, or ambiguous joins. Their terminal
partition is exact:

| Quantity | Result |
| --- | ---: |
| exact joined candidates | 4,274 |
| valid candidates (`valid=1, censor=0`) | 0 |
| censor 5, `outer_barrier_or_fence` | 4,274 |
| outer command | `0x45` (`MFC_GETLB_CMD`) for all 4,274 |
| conservative safe hide | 0 ms/frame |

The detailed CELLJOIN member records expose the element's base GET transfer as
`mfc_cmd=0x40`; that does not remove the enclosing list command's ordering
flags. MFCSLACK follows the outer operation and correctly sees command `0x45`,
GETL with a barrier. The censor is therefore a deliberate v1 scope exclusion,
not missing instrumentation. Per the protocol, censored candidates cannot fill
dependency-coverage or time thresholds.

Eligible deferrable coverage is zero and safe hide is 0 ms/frame, so the
asynchronous route is below both the 70%/7 ms STOP band and far below the
10 ms GO or roughly 14 ms credible-30-FPS requirement. This formally stops a
behavioral prototype under the current oracle; it does not prove that ordinary
SPU computation cannot overlap an ordered GETLB. Establishing that narrower
possibility would require a revised barrier-aware proof/tracker that preserves
GETLB queue, tag, fence, and local-store-use semantics.

There is also a deliberately impossible upper bound that makes an oracle repair
unnecessary for this herd alone. Ignore censor 5 and optimistically delete every
candidate's entire candidate-registration-to-outer-GETLB-completion span. The
de-duplicated union is 1.620671 seconds total: 2.028 ms over 799 complete
boundary-to-boundary package periods, or 2.075 ms over the 781-frame strict
summary denominator.
That span is not safe hide—it includes work no implementation can simply
erase—but it is still below the 7 ms/frame asynchronous STOP threshold and far
below the roughly 14 ms/frame needed for a credible route to 30 FPS. Independent
cross-checks put the correlated full-fault union at 2.025 ms/package-frame and
the flush-wait union at 1.879 ms/frame.

Barrier-aware overlap therefore remains formally unknown, but this herd is
secondary-scale even under an unrealistically favorable bound. Do not repair
MFCSLACK solely to pursue it; a future broader architecture could revisit the
ordering proof only if independent evidence exposes materially more work. The
impossible bound covers the admitted cohort subset, not the conservative
unknown-generation read rejections, so it is a scale check on this measured
route rather than a proof about every readback in the game.

## Architecture decision

Stop the synchronous generation-keyed single-flight broker and the
current asynchronous MFC coherence-broker prototype direction. Do not loosen
exact-plan, generation, coverage, barrier, or fence rules to manufacture a
positive result, and do not repeat this bedroom capture. The one bounded run
met its integrity gates and landed in both predeclared STOP bands. Record
barrier-aware asynchronous viability as **unknown**, not negative; it would be
a new measurement problem rather than a reinterpretation of censored records.
The 2.075 ms/frame impossible upper bound means it is not worth opening that
measurement problem for this herd alone.

The receipt and broker work was not wheel-spinning: receipt v1 proved that a
post-flush receipt arrives too late, and this oracle proved both what can join
while the owner is live and how little non-overlapping time that join can save.
It converts the earlier large aggregate coherence totals into a bounded
negative result before a risky behavior-changing implementation was built.

The current source remains behavior-neutral: receipt v1 is removed and no
broker is implemented. Preserve the exact ownership/lifetime infrastructure
and the capture as evidence, but move active architecture work back to general
non-coherence bottlenecks. The leading concrete renderer question is whether
the dominant full-screen feedback workload can avoid repeated snapshots via a
semantically proven Vulkan design (for example, attachment ping-pong or a
narrowly valid feedback/interlock path). Guest PPU/SPU execution also remains
a separate general target. Any candidate must be selected by emulator-wide
shader, ownership, range, generation, and ordering semantics and then validated
in distinct gameplay scenes; no bedroom address, PC, size, identity, rank, or
cadence may become production policy.
