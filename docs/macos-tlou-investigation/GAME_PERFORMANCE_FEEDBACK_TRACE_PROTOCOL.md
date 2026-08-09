# Game Performance feedback-copy timing protocol

## Purpose

This is a behavior-neutral feasibility gate for the snapshot-free framebuffer-feedback route. It attributes RPCS3 feedback-copy CPU scopes to Metal encoders, joins those encoders to GPU execution intervals, and measures per-frame unions without changing rendering behavior.

Use one Xcode **Game Performance** recording. Do not use Metal System Trace for this oracle: Xcode 26's Metal System Trace template filters out application PointsOfInterest signposts.

## Required RPCS3 marker

Emit an Apple PointsOfInterest interval tightly around `copy_transfer_regions_impl`, after the render-pass-ending layout transition:

- name: `RPCS3FeedbackEncode`
- message: `serial=<copy_serial>`
- one begin/end pair for every FBPATH copy serial

The serial must be the same monotonically increasing serial printed by the behavior-neutral framebuffer-feedback path telemetry. Do not rely on Vulkan debug-utils labels for attribution.

## Enable and preflight

Both instrumentation gates must be active **before renderer and Vulkan-instance creation**:

1. launch RPCS3 with `RPCS3_FRAMEBUFFER_FEEDBACK_ORACLE=1` in its environment;
2. enable RPCS3's Debug Overlay before booting the game.

Before navigating to the bounded capture interval, make one brief preflight recording and confirm both that the RPCS3 log contains current `FBPATH` output and that Game Performance exports at least one `RPCS3FeedbackEncode` PointsOfInterest interval for the RPCS3 PID. Stop immediately and fix the gates if either is absent. Do not discover a silent empty-marker trace after the representative scene run.

## Why Game Performance

A controlled Xcode 26 probe established the following:

- `Game Performance` exports the generic `os-signpost` PointsOfInterest begin/end rows, including PID, thread ID, interval identifier, name, and the dynamic `serial=N` message.
- The same trace exports `metal-application-encoders-list`, `metal-command-buffer-frame-assignment`, `metal-gpu-intervals`, `metal-object-label`, command-buffer submissions/completions, and driver intervals.
- In the control, the signpost interval `[1.349103708, 1.354102625]` strictly contained the matching Metal encoder CPU interval `[1.349183583, 1.354088166]`. The exported encoder ID then joined directly to its GPU interval.
- `Metal System Trace` did not export a generic PointsOfInterest interval from the same program.
- Direct Metal `MTLCommandBuffer.pushDebugGroup` before encoder creation did export as a `metal-application-event-interval` row with event type `Debug Groups`, PID/thread, start/duration, and its serial-bearing label. This validates the MoltenVK debug-utils inheritance route as a fallback. Encoder-local `pushDebugGroup` and `insertDebugSignpost` strings did not export as independent intervals in the control. Ordinary command-buffer and encoder object labels did appear.

## Recording

Settle the target scene before starting. Record one representative bounded interval; do not restart merely because a small number of frames differ from the median.

```sh
xcrun xctrace record \
  --template 'Game Performance' \
  --attach RPCS3_PID \
  --time-limit 60s \
  --output /absolute/path/rpcs3-feedback.trace \
  --no-prompt
```

Record the exact RPCS3 commit, binary hash, configuration, scene/navigation interval, trace SHA-256, and matching runtime-log interval. Treat the trace as attribution evidence, not as a replacement for the clean FPS run: Instruments can perturb performance.

Keep the RPCS3 log unabridged from oracle enablement through the capture. After `xctrace` stops, keep RPCS3 and log collection running until the next cumulative `FBPATH` summary appears, so its final `map_records` denominator includes every preceding `FBPATH_MAP` line in the log.

The frozen map grammar is:

```text
FBPATH_MAP frame=<u64> part=<u32>/<u32> records=<serial_dec>:<route_hex>:<reject_hex16>:<bytes_dec>:<N|R>,...
```

Each line contains at most 32 records. Route bits are `0x1` local-strict, `0x2` local-potential, `0x4` ping-strict, and `0x8` ping-potential. The cumulative summary must contain the frame/reconciliation fields plus `map_records`, `map_drops`, `map_missing_serials`, `map_duplicate_serials`, and `map_mismatches`; the matching cumulative `FBPATH_GATE` line must contain `unpartitioned_actual_copies` and `unpartitioned_actual_bytes`.

## Analysis

Run the parser with Python 3.13:

```sh
uv run --python 3.13 python \
  docs/macos-tlou-investigation/tools/analyze_metal_feedback_trace.py \
  /absolute/path/rpcs3-feedback.trace \
  --pid RPCS3_PID \
  --runtime-log /absolute/path/RPCS3.log
```

The parser exports XML into a temporary directory, leaves the trace unchanged, and emits JSON to stdout. Its primary join is:

1. exact RPCS3 PID;
2. exact signpost name;
3. begin/end pairing by thread and signpost identifier;
4. serial extraction from `serial=N`;
5. exact same-thread interval containment between the signpost and either all Metal encoder CPU intervals wholly inside it, or one enclosing Metal encoder when MoltenVK batches several copies;
6. stable encoder ID to `metal-gpu-intervals`;
7. command-buffer ID to Instruments frame assignment.

If no PointsOfInterest rows exist, the analyzer can apply the same PID/thread/containment join to serial-bearing command-buffer `Debug Groups` intervals. It never substitutes that fallback for present-but-incomplete PointsOfInterest data. Encoder-label regex matching is a final diagnostic fallback and cannot satisfy runtime serial reconciliation.

One logical copy can create several Metal blit encoders, and one Metal blit encoder can contain several logical copies. Both are valid. Partial overlaps, multiple competing enclosing encoders, and simultaneous inner-plus-enclosing candidates are rejected. The parser reports mapping edges separately from complete signpost intervals and unique encoders, then counts each encoder once in all CPU/GPU unions.

## Capture validity

The attribution and runtime reconciliation are valid only when all of these are true:

- `feedback_scope_mapping_coverage` is `1.0`;
- complete interval count equals unique runtime FBPATH serial count for the selected interval;
- mapped unique serial count equals complete signpost unique serial count;
- duplicate serial, unparsed serial, unmatched begin/end, malformed row, no-encoder, and ambiguous-encoder counts are all zero;
- all multipart `FBPATH_MAP` batches are complete, their serials are unique, and their frozen record grammar is exact;
- the final cumulative `map_records` equals the unique map records parsed from the full log, while `map_drops`, `map_missing_serials`, `map_duplicate_serials`, and `map_mismatches` are all zero;
- final `unknown`, `orphan`, `pending_drops`, and `mismatches` are zero, `reconciled == frames`, and final `FBPATH_GATE` `unpartitioned_actual_copies/bytes` are both zero—a syntactically complete route-zero orphan record must never count as valid route coverage;
- the exact set of runtime-map serials within the minimum-to-maximum complete trace serial window equals the complete trace serial set—numeric continuity is not required because failed copies can burn serial IDs without producing a map record;
- every attributed feedback encoder that contributes to timing has joined GPU rows;
- every attributed feedback encoder has an Instruments frame assignment; otherwise it silently falls out of per-frame medians.

Anything else is a partial oracle. Preserve it, diagnose the exact loss, and do not extrapolate missing rows.

A lone unmatched End whose parsed serial is below the minimum complete trace serial, or a lone unmatched Begin whose serial is above the maximum complete trace serial, is reported and trimmed as capture-edge truncation. Unmatched rows on the interior side, malformed rows, and unparsed serial rows remain invalid. This boundary rule avoids rerunning a sound capture over an interval cut while preserving strict interior loss detection.

## Reported measurements

Per Instruments frame, the JSON reports:

- feedback signpost count and unique serial count;
- unique attributed Metal encoder count;
- merged feedback signpost CPU time;
- merged attributed Metal encoder CPU encoding time;
- merged attributed GPU active time;
- next same-command-buffer render candidate count;
- merged feedback-plus-render GPU active time;
- merged wall-clock span from feedback GPU start through the candidate render end;
- command-buffer submission and commit-to-completion unions.

GPU rows are merged before summing. This prevents simultaneous Vertex/Fragment channel rows for one render encoder from being double-counted, and prevents a batched feedback encoder from being counted once per serial.

The JSON reports separate all-Instruments-frame and active-feedback-frame distributions. Use the all-frame median/p90/p95 for economic decisions so zero-feedback frames remain in the denominator; the active-only distribution is diagnostic. Consumer-span distributions remain conditional on paired frames.

For each semantic route bit, runtime records are joined back through serial→scope→encoder. The JSON partitions candidate encoders into:

- candidate-serial-set-exclusive: every feedback serial mapped to the encoder has that route bit;
- mixed: at least one mapped feedback serial has the bit and at least one does not.

One serial may contribute multiple encoders and multiple serials may share one encoder. Encoder IDs and GPU intervals are deduplicated globally. “Exclusive” describes the mapped feedback serial set only; an encoder can still contain unrelated, unlabeled blits, so both exclusive and mixed GPU unions remain upper bounds.

Route output includes selected-window total actual-copy serials/logical bytes and each route's serial and byte share, so dominance can be evaluated without reparsing the runtime log.

## What the trace does not prove

The exported tables do not expose:

- per-encoder tile store/load or TBDR counters;
- render-pass load/store action boundaries;
- the cost of ending and later recreating a render encoder;
- the image or subresource read by a render encoder;
- a resource dependency from a copy to a later draw;
- per-copy GPU duration when multiple copies share one Metal encoder;
- which fraction of a feedback-attributed encoder is avoidable if it also contains unrelated blits.

Without an explicit consumer label, the parser's "consumer" is only the first later encoder in the same command buffer that has a Vertex or Fragment GPU channel. It is a render candidate, not proof that the draw sampled the snapshot.

Accordingly:

- attributed feedback-encoder GPU time is an **upper bound** on feedback-copy GPU work when batching is present;
- feedback-to-consumer span is a still looser **serialization-window upper bound**, because it includes intervening work and idle gaps;
- neither number is removable time until the independent renderer semantic oracle proves that the relevant feedback class can legally avoid the snapshot.

Without `--runtime-log` and a complete exact reconciliation, analyzer status is explicitly necessary-condition-only; it cannot support a positive architecture gate or semantic-route attribution.

The default Game Performance recording's counter set is null. In the controlled probe, the only exported GPU counter was global `RT Unit Active`, without encoder or command-buffer IDs; the counter profile and APS stream were empty. That is not a usable measurement of framebuffer tile traffic or render-pass boundary cost.

## Economic gate

Use interior stable frames and report median, p90, and p95.

- A feedback-blit upper bound below about 5 ms/frame rules out **copy-engine work alone** as the mechanism for closing the gap. It does not stop the architecture: snapshot removal may also avoid a render-encoder break and tile store/load/reload work outside the attributed blit interval.
- Do not infer those pass-boundary savings from the broad feedback-to-render span; that span includes intervening work and idle gaps.
- If the independent semantic oracle covers the dominant legal event/byte class, use a tightly bounded, legal behavior-preserving A/B prototype to measure the whole frame/critical-path delta, including pass-boundary effects. That end-to-end delta—not the blit interval alone—is the decisive economic gate.
- A standalone route to 30 FPS still needs roughly 20–25 ms/frame of genuine critical-path savings. Stop only when a measurement that includes the relevant pass-boundary costs shows the legal architecture cannot supply enough of that budget, or when the semantic oracle rules out the dominant class.

This gate is game-general: attribution is by renderer operation and Metal execution, never by scene address, shader hash, draw ordinal, or bedroom-specific signature.
