# Cell-access ownership-summary capture at cbe0b796

This report records the accepted live validation of the behavior-preserving
Cell-access ownership summary at source HEAD `cbe0b7960`. The steady TLoU
bedroom is a controlled microscope for this proof; the summary and every future
access rule remain game-general and semantic.

## Reproduction identity

- Source HEAD/banner: `cbe0b7960` / `RPCS3 v0.0.42-19713-cbe0b796`
- Ownership-summary commit: `44f581fb1`
- Branch: `opt/macos-tlou-feedback-snapshot-reuse`
- Candidate application:
  `/Users/hamza/Documents/rpcs3-repro/binaries/rpcs3-cbe0b796-cell-ownership.app`
- Candidate executable SHA-256:
  `1d98979d92c57b5278bc198b092c72e86e2e29cb048bd2964c7c806d30ce5603`
- Candidate executable size: 76,439,040 bytes
- Candidate Mach-O UUID: `C3018665-0D1F-3616-BCA6-0006D2FB1328`
- External artifact directory:
  `/Users/hamza/Documents/rpcs3-repro/artifacts/cellown-2026-08-08-cbe0b796`
- External manifest: `MANIFEST.md`, SHA-256
  `05a185f6ec92d944eec218900484d97badd38b101efcb09a761d707f3be6a096`
- Complete `RPCS3.log` size: 17,625,052 bytes
- Complete `RPCS3.log` SHA-256:
  `0d7574333f25f0f5f96025c93b30c24496c6d71a98fdcef6e7321dfa87f598c7`
- Exact interval size: 1,462,029 bytes
- Exact interval file SHA-256:
  `712fa924c1de170d9616ddf11a4e765bfb2d37268042d1b9dea6c58ce0ff4dbf`
- Global configuration SHA-256:
  `40e2913ac211bc052aa5a2e5f7dd5ed23c92c82bd95c1d9cbdb22b9e2d956fe1`
- Title configuration SHA-256:
  `ec6a2bbb516b11ad577b4764df5365ddba26d285783745e6a0ad8f0457a25a4c`
- Patch configuration SHA-256:
  `f2a89abcad930220d7e0e6be6108e5f901430103ed58e064cd6a76750361e54c`
- Patch database SHA-256:
  `41c28ddf96e49864de2486deaf98b59c57b6ebfa82c0d6b6cfbcdcc4b92d88b3`
- Bundled Vulkan loader SHA-256:
  `ad33747ad8ec2425b05831c4a12bf2c6a404d755e34fd00baf5c584a380eaf70`
- MoltenVK 1.4.2 dylib SHA-256:
  `05df2d2145b9ef5b86b08ed93a7a6e916815c2bbac244298a50b2d58061600d1`

The host was the base Apple M3 (4P+4E, 16 GiB) on macOS 26.5.2. The raw bundle
executable ran directly with `--no-gui`, windowed and at verified nice level
zero. The user navigated manually to the steady bedroom scene. The isolated
profile at
`/Users/hamza/Documents/rpcs3-repro/cellown-cbe0b796.6NgYOn/home` used Vulkan,
1280x720 at 100%, Strict Rendering Mode off, Force Framebuffer Feedback Copies
on, the debug overlay on, and exactly the expected eight game patches. The full
launch identity and isolated-profile paths are in the external manifest.

## Exact window and pacing

The preserved interval is the zero-based half-open byte range
`[13408737, 14870766)` from the complete log. It starts with the included
package-`0x202` marker at emulator time `0:02:59.144063` and ends before the
excluded marker at `0:03:44.260817`; its last included marker is
`0:03:44.201477`. The file inode remained unchanged, the source log was not
rotated or truncated, and both interval boundaries are complete
newline-terminated records. The wall-clock capture ran from
`2026-08-09T00:28:47Z` through `2026-08-09T00:29:32Z`.

The interval contains 825 frame periods in 45.116754 seconds: **18.2859 FPS**.
Mean, p50, p95, p99, and maximum frame gaps are 54.687, 54.327, 58.915, 63.860,
and 65.782 ms. No gap reaches 70 ms. This overlay-on proof run validates the
observer; it is not a production-performance comparison.

## Ownership-summary proof

The interval contains 44 complete `CELLDIR` snapshots. Their clean first-to-last
interior spans 815 frames in 44.560866 seconds. Over that interior, cumulative
deltas are:

- 8,412 read probes and 8,412 texture-handler-accepted reads;
- 8,412 handled `maybe_texture`, zero handled clear, and zero newly handled
  inconclusive probes;
- zero unhandled `maybe_texture` probes;
- nine completed exact recounts and zero recount mismatches or cache-busy
  attempts;
- 3,280 us of recount time, averaging 364.444 us, with a 441 us maximum in the
  interval and a 645 us lifetime maximum; and
- zero count underflow/overflow, abandoned mutation, or sequence error.

The final exact recount has 31 live sections, 5,548 expected and observed owner
references, and 4,983 expected and observed granules. Missing, excess,
mismatched, or poisoned entries are all zero. Global poison and expected-count
overflow are also zero. The accepted-read partition closes exactly:
8,412 `maybe_texture` + 0 clear + 0 inconclusive = 8,412 accepted reads.

There are no SPU builds, `RsxKick` recoveries, audio-device switches, or Vulkan
device losses in the interval. One optional `sarah-tired-idle.stm` lookup
returns `CELL_ENOENT`; its containing frame is a normal 54.517 ms and the same
periodic probe occurs in earlier accepted captures. It does not invalidate the
window.

## Conservative fallback outside the window

Two handled probes had already returned inconclusive during navigation, and
two more did so after the accepted interval. The interval itself added none.
Across the complete run there are four inconclusive fallbacks in more than
25,000 read probes, with zero stable-clear result, recount mismatch, or poison.
An inconclusive probe conservatively retains the existing path; it cannot cause
a false clear. This rare fallback is expected to remain in the behavioral
ticket, so the observations outside the exact window do not justify another
bedroom ownership-summary run.

## Decision

The ownership summary passes its bounded live gate for this workload. Together
with the focused 8/8 and full 206/206 enabled test results, the run validates it
as a conservative texture-owner hint. It is still behavior-neutral: no access
decision changed, and `clear` remains neither a general VM/ZCULL safety proof
nor a section-lifetime pin.

No further bedroom-only validation of this summary is required. Before it can
control behavior, add a quiescent reset/rebuild at a proven renderer lifecycle
boundary and real buffered-section transition tests for protection, confirmed
range protection, expansion, and discard. Then implement the smallest
game-general synchronous exact-GET ticket: resolve and pin exact owners,
preserve same-native-page sibling protection, reuse only a correctly generated
CPU-visible shadow, and fall back unchanged for VM/ZCULL, atomic, Raw-SPU,
unsupported, or ambiguous cases.

The bedroom can validate function and support a later overlay-off A/B, but the
optimization must then pass correctness and performance checks in distinct
TLoU scenes. No production rule may depend on a bedroom address, PC, transfer
size, section identity, or cadence.
