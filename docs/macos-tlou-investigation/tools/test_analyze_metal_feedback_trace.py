#!/usr/bin/env python3

import re
import unittest

from analyze_metal_feedback_trace import (
    Cell,
    Encoder,
    Row,
    RuntimeMapRecord,
    SignpostInterval,
    build_route_gpu_metrics,
    determine_status,
    extract_debug_group_intervals,
    extract_signpost_intervals,
    map_signposts_to_encoders,
    parse_runtime_map_lines,
    reconcile_runtime_map,
    union_ns,
)


def make_encoder(
    encoder_id: int,
    start: int,
    duration: int,
    *,
    thread_id: int = 7,
    frame_number: int = 1,
) -> Encoder:
    return Encoder(
        command_buffer_id=100,
        encoder_id=encoder_id,
        frame_number=frame_number,
        start_ns=start,
        duration_ns=duration,
        thread_id=thread_id,
        command_buffer_label="cb",
        encoder_label="blit",
    )


def make_signpost(serial: int, start: int, end: int, *, thread_id: int = 7) -> SignpostInterval:
    return SignpostInterval(
        start_ns=start,
        end_ns=end,
        thread_id=thread_id,
        identifier=serial + 1000,
        serial=serial,
        message=f"serial={serial}",
    )


def make_signpost_row(
    event_type: str,
    timestamp: int,
    identifier: int,
    serial: int,
    *,
    thread_id: int = 7,
) -> Row:
    def cell(raw: str | None = None, formatted: str | None = None) -> Cell:
        return Cell(tag="test", raw=raw, formatted=formatted)

    return Row(
        schema="os-signpost",
        cells={
            "time": cell(str(timestamp)),
            "thread": Cell(
                tag="thread",
                raw=None,
                formatted="thread",
                nested_pid=42,
                nested_tid=thread_id,
            ),
            "process": Cell(
                tag="process",
                raw=None,
                formatted="rpcs3 (42)",
                nested_pid=42,
            ),
            "event-type": cell(formatted=event_type),
            "identifier": cell(str(identifier)),
            "name": cell(formatted="RPCS3FeedbackEncode"),
            "subsystem": cell(formatted="org.rpcs3"),
            "message": cell(formatted=f"serial={serial}"),
        },
    )


def make_debug_group_row(
    start: int,
    duration: int,
    label: str,
    *,
    thread_id: int = 7,
    event_type: str = "Debug Groups",
) -> Row:
    def cell(raw: str | None = None, formatted: str | None = None) -> Cell:
        return Cell(tag="test", raw=raw, formatted=formatted)

    return Row(
        schema="metal-application-event-interval",
        cells={
            "start": cell(str(start)),
            "duration": cell(str(duration)),
            "thread": Cell(
                tag="thread",
                raw=None,
                formatted="thread",
                nested_pid=42,
                nested_tid=thread_id,
            ),
            "process": Cell(
                tag="process",
                raw=None,
                formatted="rpcs3 (42)",
                nested_pid=42,
            ),
            "event-type": cell(formatted=event_type),
            "event-label": cell(formatted=label),
        },
    )


class SignpostExtractionTests(unittest.TestCase):
    def test_pairs_begin_end_and_extracts_serial(self) -> None:
        rows = [
            make_signpost_row("Begin", 100, 500, 17),
            make_signpost_row("End", 200, 500, 17),
        ]
        intervals, diagnostics = extract_signpost_intervals(
            rows,
            "RPCS3FeedbackEncode",
            "org.rpcs3",
            re.compile(r"serial\s*=\s*(\d+)"),
        )
        self.assertEqual([(item.start_ns, item.end_ns, item.serial) for item in intervals], [(100, 200, 17)])
        self.assertEqual(diagnostics["complete_intervals"], 1)
        self.assertEqual(diagnostics["unique_serials"], 1)
        self.assertEqual(diagnostics["duplicate_serial_intervals"], 0)
        self.assertEqual(diagnostics["unmatched_begins"], 0)
        self.assertEqual(diagnostics["unmatched_ends"], 0)

    def test_duplicate_serial_and_unmatched_rows_are_visible(self) -> None:
        rows = [
            make_signpost_row("End", 50, 999, 3),
            make_signpost_row("Begin", 100, 500, 3),
            make_signpost_row("End", 150, 500, 3),
            make_signpost_row("Begin", 200, 501, 3),
            make_signpost_row("End", 250, 501, 3),
            make_signpost_row("Begin", 300, 502, 3),
        ]
        _, diagnostics = extract_signpost_intervals(
            rows,
            "RPCS3FeedbackEncode",
            None,
            re.compile(r"serial\s*=\s*(\d+)"),
        )
        self.assertEqual(diagnostics["complete_intervals"], 2)
        self.assertEqual(diagnostics["unique_serials"], 1)
        self.assertEqual(diagnostics["duplicate_serial_intervals"], 1)
        self.assertEqual(diagnostics["unmatched_begins"], 1)
        self.assertEqual(diagnostics["unmatched_ends"], 1)

    def test_trace_edge_unmatched_rows_are_trimmed_by_complete_serial_range(self) -> None:
        rows = [
            make_signpost_row("End", 50, 499, 2),
            make_signpost_row("Begin", 100, 500, 3),
            make_signpost_row("End", 150, 500, 3),
            make_signpost_row("Begin", 200, 501, 4),
        ]
        _, diagnostics = extract_signpost_intervals(
            rows,
            "RPCS3FeedbackEncode",
            None,
            re.compile(r"serial\s*=\s*(\d+)"),
        )
        self.assertEqual(diagnostics["unmatched_begins"], 0)
        self.assertEqual(diagnostics["unmatched_ends"], 0)
        self.assertEqual(diagnostics["edge_truncated_unmatched_begins"], 1)
        self.assertEqual(diagnostics["edge_truncated_unmatched_ends"], 1)

    def test_unparsed_serial_is_visible(self) -> None:
        rows = [make_signpost_row("Begin", 100, 500, 3)]
        rows[0].cells["message"] = Cell(
            tag="test",
            raw=None,
            formatted="serial is missing",
        )
        _, diagnostics = extract_signpost_intervals(
            rows,
            "RPCS3FeedbackEncode",
            None,
            re.compile(r"serial\s*=\s*(\d+)"),
        )
        self.assertEqual(diagnostics["unparsed_serial_rows"], 1)


class DebugGroupExtractionTests(unittest.TestCase):
    def test_extracts_only_matching_debug_group_intervals(self) -> None:
        rows = [
            make_debug_group_row(
                100,
                50,
                "Framebuffer feedback copy serial=17",
            ),
            make_debug_group_row(200, 25, "Unrelated serial=18"),
            make_debug_group_row(
                300,
                25,
                "Framebuffer feedback copy serial=19",
                event_type="Completion Handlers",
            ),
        ]
        intervals, diagnostics = extract_debug_group_intervals(
            rows,
            re.compile(r"(?i)feedback.*copy"),
            re.compile(r"serial\s*=\s*(\d+)"),
        )
        self.assertEqual(
            [(item.start_ns, item.end_ns, item.serial) for item in intervals],
            [(100, 150, 17)],
        )
        self.assertEqual(diagnostics["matching_rows"], 1)
        self.assertEqual(diagnostics["complete_intervals"], 1)


class RuntimeMapTests(unittest.TestCase):
    def test_parses_complete_frozen_grammar_and_summary(self) -> None:
        lines = [
            "FBPATH_MAP frame=12345 part=1/2 "
            "records=9001:2:0000000400082001:8294400:N",
            "FBPATH_MAP frame=12345 part=2/2 "
            "records=9002:a:0000000000082001:8294400:R",
            "FBPATH frames=1 unknown=0 orphan=0 pending_drops=0 "
            "reconciled=1 mismatches=0 map_records=2 map_drops=0 "
            "map_missing_serials=0 map_duplicate_serials=0 map_mismatches=0",
            "FBPATH_GATE unpartitioned_actual_copies=0 "
            "unpartitioned_actual_bytes=0",
        ]
        records, diagnostics = parse_runtime_map_lines(lines)
        self.assertEqual(set(records), {9001, 9002})
        self.assertEqual(records[9001].route_flags, 0x2)
        self.assertEqual(records[9002].reject_bits, 0x82001)
        self.assertTrue(diagnostics["parse_complete"])

    def test_missing_batch_part_and_summary_defect_are_invalid(self) -> None:
        lines = [
            "FBPATH_MAP frame=1 part=1/2 records=10:1:0000000000000000:4:N",
            "FBPATH frames=1 unknown=1 orphan=1 pending_drops=1 "
            "reconciled=0 mismatches=1 map_records=1 map_drops=1 "
            "map_missing_serials=0 map_duplicate_serials=0 map_mismatches=1",
            "FBPATH_GATE unpartitioned_actual_copies=1 "
            "unpartitioned_actual_bytes=4",
        ]
        _, diagnostics = parse_runtime_map_lines(lines)
        self.assertEqual(diagnostics["incomplete_part_frames"], 1)
        self.assertFalse(diagnostics["summary_defects_zero"])
        self.assertFalse(diagnostics["summary_all_frames_reconciled"])
        self.assertFalse(diagnostics["gate_defects_zero"])
        self.assertFalse(diagnostics["parse_complete"])

    def test_reconciliation_uses_exact_sparse_set_inside_trace_range(self) -> None:
        def record(serial: int) -> RuntimeMapRecord:
            return RuntimeMapRecord(serial, 1, 0, 0, 4, "N")

        records = {serial: record(serial) for serial in (1, 10, 12, 20)}
        interior, diagnostics = reconcile_runtime_map({10, 12}, records, True)
        self.assertEqual(set(interior), {10, 12})
        self.assertTrue(diagnostics["complete"])

        records[11] = record(11)
        _, diagnostics = reconcile_runtime_map({10, 12}, records, True)
        self.assertEqual(
            diagnostics["runtime_serials_missing_from_feedback_scopes_count"],
            1,
        )
        self.assertFalse(diagnostics["complete"])

    def test_route_metrics_partition_exclusive_and_mixed_encoders(self) -> None:
        encoder_mixed = make_encoder(10, 100, 20, frame_number=1)
        encoder_exclusive = make_encoder(11, 200, 20, frame_number=1)
        encoder_second_for_same_serial = make_encoder(
            12,
            300,
            20,
            frame_number=1,
        )
        mappings = [
            *map_signposts_to_encoders(
                [make_signpost(1, 105, 110), make_signpost(2, 112, 118)],
                [encoder_mixed],
            )[0],
            *map_signposts_to_encoders(
                [make_signpost(3, 190, 330)],
                [encoder_exclusive, encoder_second_for_same_serial],
            )[0],
        ]
        records = {
            1: RuntimeMapRecord(1, 1, 0x1, 0, 4, "N"),
            2: RuntimeMapRecord(2, 1, 0x0, 1, 8, "R"),
            3: RuntimeMapRecord(3, 1, 0x1, 0, 16, "N"),
        }
        result = build_route_gpu_metrics(
            records,
            mappings,
            {10: [(1000, 1010)], 11: [(1020, 1040)], 12: [(1050, 1080)]},
            [1, 2],
        )
        self.assertEqual(
            result["selected_window_totals"]["actual_copy_serial_count"],
            3,
        )
        self.assertEqual(result["selected_window_totals"]["logical_bytes"], 28)
        metrics = result["routes"]["local_strict"]
        self.assertEqual(metrics["candidate_encoder_count"], 3)
        self.assertEqual(
            metrics["candidate_serial_set_exclusive_encoder_count"],
            2,
        )
        self.assertEqual(
            metrics["mixed_candidate_and_noncandidate_encoder_count"],
            1,
        )
        self.assertEqual(metrics["candidate_encoder_gpu_active_union_ms"], 0.00006)
        self.assertEqual(metrics["candidate_serial_share"], 0.666667)
        self.assertEqual(metrics["candidate_logical_byte_share"], 0.714286)
        self.assertEqual(
            metrics["candidate_encoder_gpu_active_per_all_frame"]["count"],
            2,
        )


class EncoderMappingTests(unittest.TestCase):
    def test_signpost_inside_one_encoder(self) -> None:
        mappings, diagnostics = map_signposts_to_encoders(
            [make_signpost(1, 120, 180)],
            [make_encoder(10, 100, 100)],
        )
        self.assertEqual([item.encoder.encoder_id for item in mappings], [10])
        self.assertEqual(mappings[0].containment, "signpost_within_encoder")
        self.assertEqual(diagnostics["intervals_mapped"], 1)
        self.assertEqual(diagnostics["ambiguous_encoder"], 0)

    def test_one_logical_copy_can_contain_multiple_encoders(self) -> None:
        mappings, diagnostics = map_signposts_to_encoders(
            [make_signpost(1, 90, 230)],
            [make_encoder(10, 100, 40), make_encoder(11, 160, 40)],
        )
        self.assertEqual([item.encoder.encoder_id for item in mappings], [10, 11])
        self.assertTrue(all(item.containment == "encoder_within_signpost" for item in mappings))
        self.assertEqual(diagnostics["intervals_mapped"], 1)
        self.assertEqual(diagnostics["ambiguous_encoder"], 0)

    def test_multiple_serials_can_share_one_batched_encoder_without_double_counting(self) -> None:
        encoder = make_encoder(10, 100, 200)
        mappings, diagnostics = map_signposts_to_encoders(
            [make_signpost(1, 120, 140), make_signpost(2, 160, 180)],
            [encoder],
        )
        self.assertEqual(len(mappings), 2)
        self.assertEqual({item.encoder.encoder_id for item in mappings}, {10})
        self.assertEqual(diagnostics["intervals_mapped"], 2)
        interval = (encoder.start_ns, encoder.start_ns + encoder.duration_ns)
        self.assertEqual(union_ns([interval, interval]), encoder.duration_ns)

    def test_partial_overlap_is_rejected(self) -> None:
        mappings, diagnostics = map_signposts_to_encoders(
            [make_signpost(1, 150, 250)],
            [make_encoder(10, 100, 100)],
        )
        self.assertEqual(mappings, [])
        self.assertEqual(diagnostics["partial_overlap"], 1)
        self.assertEqual(diagnostics["ambiguous_encoder"], 1)

    def test_competing_inner_and_enclosing_intervals_are_rejected(self) -> None:
        mappings, diagnostics = map_signposts_to_encoders(
            [make_signpost(1, 90, 210)],
            [make_encoder(10, 50, 200), make_encoder(11, 100, 50)],
        )
        self.assertEqual(mappings, [])
        self.assertEqual(diagnostics["ambiguous_encoder"], 1)
        self.assertEqual(diagnostics["overlapping_same_thread_encoder_intervals"], 1)

    def test_multiple_earlier_enclosing_intervals_are_rejected(self) -> None:
        mappings, diagnostics = map_signposts_to_encoders(
            [make_signpost(1, 100, 200)],
            [make_encoder(10, 0, 300), make_encoder(11, 50, 200)],
        )
        self.assertEqual(mappings, [])
        self.assertEqual(diagnostics["ambiguous_encoder"], 1)

    def test_wrong_thread_does_not_map(self) -> None:
        mappings, diagnostics = map_signposts_to_encoders(
            [make_signpost(1, 120, 180, thread_id=8)],
            [make_encoder(10, 100, 100, thread_id=7)],
        )
        self.assertEqual(mappings, [])
        self.assertEqual(diagnostics["without_encoder"], 1)


class StatusTests(unittest.TestCase):
    def status(self, **overrides: object) -> str:
        arguments = {
            "has_feedback_scope_rows": True,
            "feedback_scope_attribution_complete": True,
            "feedback_label_visible": False,
            "has_feedback_gpu_intervals": True,
            "feedback_gpu_join_complete": True,
            "feedback_frame_assignment_complete": True,
            "runtime_log_provided": True,
            "runtime_reconciliation_complete": True,
            "has_consumer_pairs": True,
            "attribution_basis": "points_of_interest_signpost_pid_thread_containment",
        }
        arguments.update(overrides)
        return determine_status(**arguments)  # type: ignore[arg-type]

    def test_incomplete_signposts_take_priority(self) -> None:
        self.assertEqual(
            self.status(feedback_scope_attribution_complete=False),
            "feedback_signpost_attribution_incomplete",
        )

    def test_incomplete_debug_group_fallback_is_distinct(self) -> None:
        self.assertEqual(
            self.status(
                feedback_scope_attribution_complete=False,
                attribution_basis=(
                    "command_buffer_debug_group_pid_thread_containment_fallback"
                ),
            ),
            "feedback_debug_group_attribution_incomplete",
        )

    def test_missing_gpu_and_consumer_are_distinct(self) -> None:
        self.assertEqual(
            self.status(has_feedback_gpu_intervals=False),
            "feedback_attribution_found_but_no_joined_gpu_intervals",
        )
        self.assertEqual(
            self.status(feedback_gpu_join_complete=False),
            "feedback_attribution_found_but_gpu_join_incomplete",
        )
        self.assertEqual(
            self.status(feedback_frame_assignment_complete=False),
            "feedback_attribution_found_but_frame_assignment_incomplete",
        )
        self.assertEqual(
            self.status(runtime_log_provided=False),
            "feedback_timing_measured_necessary_condition_only_runtime_log_not_provided",
        )
        self.assertEqual(
            self.status(runtime_reconciliation_complete=False),
            "feedback_runtime_map_reconciliation_incomplete",
        )
        self.assertEqual(
            self.status(has_consumer_pairs=False),
            "feedback_gpu_intervals_runtime_reconciled_no_consumer_pairs",
        )

    def test_complete_signpost_and_label_fallback_statuses(self) -> None:
        self.assertEqual(
            self.status(),
            "feedback_signpost_and_candidate_consumer_intervals_measured",
        )
        self.assertEqual(
            self.status(
                has_feedback_scope_rows=False,
                feedback_label_visible=True,
                attribution_basis="encoder_label_regex_fallback",
            ),
            "feedback_gpu_intervals_measured_by_label_fallback",
        )
        self.assertEqual(
            self.status(
                attribution_basis=(
                    "command_buffer_debug_group_pid_thread_containment_fallback"
                )
            ),
            "feedback_gpu_intervals_measured_by_debug_group_fallback",
        )
        self.assertEqual(
            self.status(
                has_feedback_scope_rows=False,
                feedback_label_visible=False,
                has_feedback_gpu_intervals=False,
                has_consumer_pairs=False,
            ),
            "no_feedback_signpost_or_encoder_label_match",
        )


if __name__ == "__main__":
    unittest.main()
