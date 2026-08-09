#!/usr/bin/env python3
"""Analyze framebuffer-feedback encoder timing from an xctrace trace.

The script is deliberately read-only with respect to the input trace. It asks
``xctrace export`` for the relevant XML tables in a temporary directory, joins
rows by the exported Metal command-buffer and encoder IDs, and writes JSON to
stdout.

The intended input is one ``Game Performance`` trace. Its PointsOfInterest rows
carry a feedback-copy serial on a CPU interval; strict PID/thread/containment
maps that interval to a stable Metal encoder ID, which then joins to GPU rows.
Encoder-label matching is only a diagnostic fallback.

This is an oracle, not a resource-dependency tracer. In the absence of an
explicitly labelled consumer, the optional "next render" association is only
the first later encoder in the same command buffer for which the GPU table has
a Vertex or Fragment channel. It does not prove that encoder sampled the copied
image.
"""

from __future__ import annotations

import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
from dataclasses import dataclass
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile
from typing import Iterable, Sequence
import xml.etree.ElementTree as ET


SCHEMAS = (
    "metal-application-command-buffer-submissions",
    "metal-application-encoders-list",
    "metal-application-event-interval",
    "metal-application-intervals",
    "metal-command-buffer-completed",
    "metal-command-buffer-frame-assignment",
    "metal-driver-event-intervals",
    "metal-driver-intervals",
    "metal-gpu-intervals",
    "metal-gpu-submission-to-command-buffer-id",
    "metal-object-label",
    "os-signpost",
)

REQUIRED_SCHEMAS = {
    "metal-application-encoders-list",
    "metal-command-buffer-frame-assignment",
    "metal-gpu-intervals",
}

RENDER_GPU_CHANNELS = {"vertex", "fragment"}

RUNTIME_ROUTE_BITS = {
    "local_strict": 0x1,
    "local_potential": 0x2,
    "ping_strict": 0x4,
    "ping_potential": 0x8,
}

RUNTIME_MAP_LINE_PATTERN = re.compile(
    r"\bFBPATH_MAP\s+frame=(\d+)\s+part=(\d+)/(\d+)\s+records=([^\s]+)"
)
RUNTIME_MAP_RECORD_PATTERN = re.compile(
    r"([1-9]\d*):([0-9a-f]+):([0-9a-f]{16}):(\d+):([NR])"
)
RUNTIME_SUMMARY_FIELDS = (
    "frames",
    "unknown",
    "orphan",
    "pending_drops",
    "reconciled",
    "mismatches",
    "map_records",
    "map_drops",
    "map_missing_serials",
    "map_duplicate_serials",
    "map_mismatches",
)

RUNTIME_SUMMARY_ZERO_FIELDS = (
    "unknown",
    "orphan",
    "pending_drops",
    "mismatches",
    "map_drops",
    "map_missing_serials",
    "map_duplicate_serials",
    "map_mismatches",
)

RUNTIME_GATE_ZERO_FIELDS = (
    "unpartitioned_actual_copies",
    "unpartitioned_actual_bytes",
)


class AnalysisError(RuntimeError):
    pass


@dataclass(frozen=True)
class Cell:
    tag: str
    raw: str | None
    formatted: str | None
    nested_pid: int | None = None
    nested_tid: int | None = None

    @property
    def text(self) -> str | None:
        return self.formatted or self.raw

    def as_int(self) -> int | None:
        if self.raw is None:
            return None
        value = self.raw.replace(",", "").strip()
        if not value:
            return None
        try:
            return int(value, 0)
        except ValueError as exc:
            raise AnalysisError(f"Expected an integer cell, got {self.raw!r}") from exc


@dataclass(frozen=True)
class Row:
    schema: str
    cells: dict[str, Cell]

    def cell(self, mnemonic: str) -> Cell | None:
        return self.cells.get(mnemonic)

    def text(self, mnemonic: str) -> str | None:
        cell = self.cell(mnemonic)
        return cell.text if cell else None

    def integer(self, mnemonic: str) -> int | None:
        cell = self.cell(mnemonic)
        return cell.as_int() if cell else None

    @property
    def pid(self) -> int | None:
        direct = self.integer("pid")
        if direct is not None:
            return direct
        for mnemonic in ("process", "thread"):
            cell = self.cell(mnemonic)
            if cell and cell.nested_pid is not None:
                return cell.nested_pid
        return None

    @property
    def tid(self) -> int | None:
        direct = self.integer("tid")
        if direct is not None:
            return direct
        cell = self.cell("thread")
        return cell.nested_tid if cell else None


class ExportedTable:
    def __init__(self, path: Path):
        self.path = path
        try:
            self.root = ET.parse(path).getroot()
        except (OSError, ET.ParseError) as exc:
            raise AnalysisError(f"Cannot parse exported table {path}: {exc}") from exc

        schema_node = self.root.find(".//schema")
        if schema_node is None or not schema_node.get("name"):
            raise AnalysisError(f"Export {path} contains no table schema")
        self.schema = schema_node.get("name") or ""
        self.columns = [
            col.findtext("mnemonic") or ""
            for col in schema_node.findall("col")
        ]
        if not self.columns or any(not name for name in self.columns):
            raise AnalysisError(f"Export {path} has an invalid column definition")

        self._ids = {
            element.get("id"): element
            for element in self.root.iter()
            if element.get("id")
        }
        self.rows = self._parse_rows()

    def _resolve(self, element: ET.Element) -> ET.Element:
        seen: set[str] = set()
        while reference := element.get("ref"):
            if reference in seen:
                raise AnalysisError(
                    f"Reference cycle at id {reference!r} in {self.path}"
                )
            seen.add(reference)
            try:
                element = self._ids[reference]
            except KeyError as exc:
                raise AnalysisError(
                    f"Unresolved XML reference {reference!r} in {self.path}"
                ) from exc
        return element

    def _find_nested_int(
        self,
        element: ET.Element,
        tag: str,
        visited: set[int] | None = None,
    ) -> int | None:
        visited = visited or set()
        element = self._resolve(element)
        identity = id(element)
        if identity in visited:
            return None
        visited.add(identity)

        if element.tag == tag:
            raw = (element.text or "").strip().replace(",", "")
            if raw:
                try:
                    return int(raw, 0)
                except ValueError:
                    return None
        for child in element:
            value = self._find_nested_int(child, tag, visited)
            if value is not None:
                return value
        return None

    def _cell(self, element: ET.Element) -> Cell:
        element = self._resolve(element)
        if element.tag == "sentinel":
            return Cell(tag=element.tag, raw=None, formatted=None)
        raw = (element.text or "").strip() or None
        return Cell(
            tag=element.tag,
            raw=raw,
            formatted=element.get("fmt"),
            nested_pid=self._find_nested_int(element, "pid"),
            nested_tid=self._find_nested_int(element, "tid"),
        )

    def _parse_rows(self) -> list[Row]:
        rows: list[Row] = []
        for number, row_node in enumerate(self.root.findall(".//row"), start=1):
            values = list(row_node)
            if len(values) != len(self.columns):
                raise AnalysisError(
                    f"{self.path}: row {number} has {len(values)} values for "
                    f"{len(self.columns)} columns"
                )
            rows.append(
                Row(
                    schema=self.schema,
                    cells={
                        mnemonic: self._cell(value)
                        for mnemonic, value in zip(self.columns, values, strict=True)
                    },
                )
            )
        return rows


@dataclass(frozen=True)
class Encoder:
    command_buffer_id: int
    encoder_id: int
    frame_number: int | None
    start_ns: int
    duration_ns: int
    thread_id: int | None
    command_buffer_label: str
    encoder_label: str


@dataclass(frozen=True)
class EncoderPair:
    feedback: Encoder
    consumer: Encoder
    basis: str


@dataclass(frozen=True)
class SignpostInterval:
    start_ns: int
    end_ns: int
    thread_id: int
    identifier: int
    serial: int
    message: str


@dataclass(frozen=True)
class SignpostMapping:
    signpost: SignpostInterval
    encoder: Encoder
    containment: str


@dataclass(frozen=True)
class RuntimeMapRecord:
    serial: int
    frame: int
    route_flags: int
    reject_bits: int
    logical_bytes: int
    outcome: str


def run_checked(command: Sequence[str]) -> None:
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if result.returncode:
        output = result.stdout.strip()
        raise AnalysisError(
            f"Command failed ({result.returncode}): {' '.join(command)}"
            + (f"\n{output}" if output else "")
        )


def export_trace(trace: Path, run_number: int, directory: Path) -> tuple[Path, dict[str, Path]]:
    toc = directory / "toc.xml"
    run_checked(
        [
            "xcrun",
            "xctrace",
            "export",
            "--input",
            str(trace),
            "--toc",
            "--output",
            str(toc),
        ]
    )

    try:
        toc_root = ET.parse(toc).getroot()
    except (OSError, ET.ParseError) as exc:
        raise AnalysisError(f"Cannot parse xctrace table of contents: {exc}") from exc
    run_node = toc_root.find(f"./run[@number='{run_number}']")
    if run_node is None:
        available = [node.get("number") for node in toc_root.findall("./run")]
        raise AnalysisError(
            f"Trace has no run {run_number}; available runs: {', '.join(available)}"
        )
    available_schemas = {
        table.get("schema")
        for table in run_node.findall("./data/table")
        if table.get("schema")
    }
    missing = sorted(REQUIRED_SCHEMAS - available_schemas)
    if missing:
        raise AnalysisError(
            "Trace is missing required Game Performance Metal tables: "
            + ", ".join(missing)
        )

    exports: dict[str, Path] = {}
    for schema in SCHEMAS:
        if schema not in available_schemas:
            continue
        output = directory / f"{schema}.xml"
        xpath = (
            f'/trace-toc/run[@number="{run_number}"]/data/'
            f'table[@schema="{schema}"]'
        )
        run_checked(
            [
                "xcrun",
                "xctrace",
                "export",
                "--input",
                str(trace),
                "--xpath",
                xpath,
                "--output",
                str(output),
            ]
        )
        exports[schema] = output
    return toc, exports


def trace_metadata(toc: Path, run_number: int) -> dict[str, object]:
    root = ET.parse(toc).getroot()
    run_node = root.find(f"./run[@number='{run_number}']")
    if run_node is None:
        raise AnalysisError(f"Trace has no run {run_number}")
    summary = run_node.find("./info/summary")
    device = run_node.find("./info/target/device")
    return {
        "run": run_number,
        "start_date": summary.findtext("start-date") if summary is not None else None,
        "end_date": summary.findtext("end-date") if summary is not None else None,
        "duration_seconds": float(summary.findtext("duration"))
        if summary is not None and summary.findtext("duration")
        else None,
        "end_reason": summary.findtext("end-reason") if summary is not None else None,
        "instruments_version": summary.findtext("instruments-version")
        if summary is not None
        else None,
        "template": summary.findtext("template-name") if summary is not None else None,
        "device_model": device.get("model") if device is not None else None,
        "device_os": device.get("os-version") if device is not None else None,
    }


def resolve_pid(toc: Path, run_number: int, requested_pid: int | None, process_name: str) -> tuple[int, str | None]:
    root = ET.parse(toc).getroot()
    run_node = root.find(f"./run[@number='{run_number}']")
    if run_node is None:
        raise AnalysisError(f"Trace has no run {run_number}")

    process_nodes = [
        *run_node.findall("./info/target/process"),
        *run_node.findall("./processes/process"),
    ]
    processes: dict[int, str | None] = {}
    for node in process_nodes:
        raw_pid = node.get("pid")
        if raw_pid:
            processes[int(raw_pid)] = node.get("name")

    if requested_pid is not None:
        return requested_pid, processes.get(requested_pid)

    wanted = process_name.casefold()
    candidates = {
        pid: name
        for pid, name in processes.items()
        if name and name.casefold() == wanted
    }
    if len(candidates) == 1:
        return next(iter(candidates.items()))
    if not candidates:
        raise AnalysisError(
            f"No process named {process_name!r} appears in run {run_number}; pass --pid explicitly"
        )
    rendered = ", ".join(f"{pid} ({name})" for pid, name in sorted(candidates.items()))
    raise AnalysisError(
        f"Multiple processes named {process_name!r} appear in the trace: {rendered}; pass --pid"
    )


def valid_interval(row: Row, start_key: str = "start", duration_key: str = "duration") -> tuple[int, int] | None:
    start = row.integer(start_key)
    duration = row.integer(duration_key)
    if start is None or duration is None or duration < 0:
        return None
    return start, start + duration


def merge_intervals(intervals: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    ordered = sorted((start, end) for start, end in intervals if end >= start)
    if not ordered:
        return []
    merged = [ordered[0]]
    for start, end in ordered[1:]:
        previous_start, previous_end = merged[-1]
        if start <= previous_end:
            merged[-1] = (previous_start, max(previous_end, end))
        else:
            merged.append((start, end))
    return merged


def union_ns(intervals: Iterable[tuple[int, int]]) -> int:
    return sum(end - start for start, end in merge_intervals(intervals))


def ns_to_ms(value: int) -> float:
    return round(value / 1_000_000.0, 6)


def distribution(values: Sequence[int]) -> dict[str, float | int] | None:
    if not values:
        return None
    ordered = sorted(values)

    def percentile(fraction: float) -> int:
        if len(ordered) == 1:
            return ordered[0]
        index = round((len(ordered) - 1) * fraction)
        return ordered[index]

    return {
        "count": len(values),
        "mean_ms": round(statistics.fmean(values) / 1_000_000.0, 6),
        "median_ms": ns_to_ms(int(statistics.median(values))),
        "p90_ms": ns_to_ms(percentile(0.90)),
        "p95_ms": ns_to_ms(percentile(0.95)),
        "min_ms": ns_to_ms(ordered[0]),
        "max_ms": ns_to_ms(ordered[-1]),
    }


def count_distribution(values: Sequence[int]) -> dict[str, float | int] | None:
    if not values:
        return None
    ordered = sorted(values)
    p90_index = round((len(ordered) - 1) * 0.90)
    return {
        "count": len(ordered),
        "mean": round(statistics.fmean(ordered), 6),
        "median": statistics.median(ordered),
        "p90": ordered[p90_index],
        "min": ordered[0],
        "max": ordered[-1],
    }


def top_counts(values: Iterable[str], limit: int = 24) -> list[dict[str, object]]:
    return [
        {"value": value, "count": count}
        for value, count in Counter(values).most_common(limit)
    ]


def parse_runtime_map_lines(
    lines: Sequence[str],
) -> tuple[dict[int, RuntimeMapRecord], dict[str, object]]:
    parts_by_frame: dict[int, dict[int, tuple[int, list[RuntimeMapRecord]]]] = (
        defaultdict(dict)
    )
    matching_map_lines = 0
    malformed_map_lines = 0
    malformed_records = 0
    duplicate_parts = 0
    inconsistent_part_totals = 0
    unknown_route_flag_records = 0
    parsed_record_entries = 0

    for line in lines:
        if "FBPATH_MAP" not in line:
            continue
        match = RUNTIME_MAP_LINE_PATTERN.search(line)
        if match is None:
            malformed_map_lines += 1
            continue
        matching_map_lines += 1
        frame = int(match.group(1))
        part = int(match.group(2))
        total = int(match.group(3))
        raw_records = match.group(4)
        if total < 1 or part < 1 or part > total:
            malformed_map_lines += 1
            continue

        records: list[RuntimeMapRecord] = []
        for raw_record in raw_records.split(","):
            record_match = RUNTIME_MAP_RECORD_PATTERN.fullmatch(raw_record)
            if record_match is None:
                malformed_records += 1
                continue
            route_flags = int(record_match.group(2), 16)
            if route_flags & ~sum(RUNTIME_ROUTE_BITS.values()):
                unknown_route_flag_records += 1
            records.append(
                RuntimeMapRecord(
                    serial=int(record_match.group(1)),
                    frame=frame,
                    route_flags=route_flags,
                    reject_bits=int(record_match.group(3), 16),
                    logical_bytes=int(record_match.group(4)),
                    outcome=record_match.group(5),
                )
            )
            parsed_record_entries += 1

        frame_parts = parts_by_frame[frame]
        if part in frame_parts:
            duplicate_parts += 1
            continue
        if frame_parts and any(
            existing_total != total
            for existing_total, _ in frame_parts.values()
        ):
            inconsistent_part_totals += 1
        frame_parts[part] = (total, records)

    incomplete_part_frames = 0
    records_by_serial: dict[int, RuntimeMapRecord] = {}
    duplicate_serial_records = 0
    for frame_parts in parts_by_frame.values():
        totals = {total for total, _ in frame_parts.values()}
        if len(totals) != 1:
            incomplete_part_frames += 1
            continue
        total = next(iter(totals))
        if set(frame_parts) != set(range(1, total + 1)):
            incomplete_part_frames += 1
            continue
        for part in range(1, total + 1):
            for record in frame_parts[part][1]:
                if record.serial in records_by_serial:
                    duplicate_serial_records += 1
                else:
                    records_by_serial[record.serial] = record

    summary_rows: list[dict[str, int]] = []
    for line in lines:
        if "FBPATH " not in line or "map_records=" not in line:
            continue
        values: dict[str, int] = {}
        for field in RUNTIME_SUMMARY_FIELDS:
            match = re.search(rf"\b{re.escape(field)}=(\d+)\b", line)
            if match is None:
                values = {}
                break
            values[field] = int(match.group(1))
        if values:
            summary_rows.append(values)

    nonmonotonic_summary_rows = 0
    for previous, current in zip(summary_rows, summary_rows[1:]):
        if any(current[field] < previous[field] for field in RUNTIME_SUMMARY_FIELDS):
            nonmonotonic_summary_rows += 1
    final_summary = summary_rows[-1] if summary_rows else None
    summary_defects_zero = bool(final_summary) and all(
        final_summary[field] == 0 for field in RUNTIME_SUMMARY_ZERO_FIELDS
    )
    summary_all_frames_reconciled = bool(final_summary) and (
        final_summary["reconciled"] == final_summary["frames"]
    )
    summary_record_count_matches = bool(final_summary) and (
        final_summary["map_records"] == len(records_by_serial)
    )

    gate_rows: list[dict[str, int]] = []
    for line in lines:
        if "FBPATH_GATE " not in line:
            continue
        values: dict[str, int] = {}
        for field in RUNTIME_GATE_ZERO_FIELDS:
            match = re.search(rf"\b{re.escape(field)}=(\d+)\b", line)
            if match is None:
                values = {}
                break
            values[field] = int(match.group(1))
        if values:
            gate_rows.append(values)
    final_gate = gate_rows[-1] if gate_rows else None
    gate_defects_zero = bool(final_gate) and all(
        final_gate[field] == 0 for field in RUNTIME_GATE_ZERO_FIELDS
    )
    parse_complete = (
        matching_map_lines > 0
        and malformed_map_lines == 0
        and malformed_records == 0
        and duplicate_parts == 0
        and inconsistent_part_totals == 0
        and incomplete_part_frames == 0
        and duplicate_serial_records == 0
        and unknown_route_flag_records == 0
        and bool(summary_rows)
        and nonmonotonic_summary_rows == 0
        and summary_defects_zero
        and summary_all_frames_reconciled
        and summary_record_count_matches
        and bool(gate_rows)
        and gate_defects_zero
    )
    diagnostics: dict[str, object] = {
        "matching_map_lines": matching_map_lines,
        "malformed_map_lines": malformed_map_lines,
        "frames_with_map_parts": len(parts_by_frame),
        "incomplete_part_frames": incomplete_part_frames,
        "duplicate_parts": duplicate_parts,
        "inconsistent_part_totals": inconsistent_part_totals,
        "parsed_record_entries": parsed_record_entries,
        "unique_records": len(records_by_serial),
        "duplicate_serial_records": duplicate_serial_records,
        "malformed_records": malformed_records,
        "unknown_route_flag_records": unknown_route_flag_records,
        "summary_rows": len(summary_rows),
        "nonmonotonic_summary_rows": nonmonotonic_summary_rows,
        "final_summary": final_summary,
        "summary_defects_zero": summary_defects_zero,
        "summary_all_frames_reconciled": summary_all_frames_reconciled,
        "summary_record_count_matches": summary_record_count_matches,
        "gate_rows": len(gate_rows),
        "final_gate": final_gate,
        "gate_defects_zero": gate_defects_zero,
        "parse_complete": parse_complete,
    }
    return records_by_serial, diagnostics


def load_runtime_map(
    path: Path,
) -> tuple[dict[int, RuntimeMapRecord], dict[str, object]]:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        raise AnalysisError(f"Cannot read runtime log {path}: {exc}") from exc
    return parse_runtime_map_lines(lines)


def reconcile_runtime_map(
    feedback_serials: set[int],
    runtime_records: dict[int, RuntimeMapRecord],
    runtime_parse_complete: bool,
) -> tuple[dict[int, RuntimeMapRecord], dict[str, object]]:
    if feedback_serials:
        minimum_feedback_serial = min(feedback_serials)
        maximum_feedback_serial = max(feedback_serials)
        interior_runtime_records = {
            serial: record
            for serial, record in runtime_records.items()
            if minimum_feedback_serial <= serial <= maximum_feedback_serial
        }
    else:
        minimum_feedback_serial = None
        maximum_feedback_serial = None
        interior_runtime_records = {}

    interior_runtime_serials = set(interior_runtime_records)
    feedback_serials_missing = sorted(feedback_serials - interior_runtime_serials)
    runtime_serials_missing = sorted(interior_runtime_serials - feedback_serials)
    serial_set_equal = bool(feedback_serials) and (
        not feedback_serials_missing and not runtime_serials_missing
    )
    diagnostics: dict[str, object] = {
        "feedback_serial_min": minimum_feedback_serial,
        "feedback_serial_max": maximum_feedback_serial,
        "feedback_scope_unique_serial_count": len(feedback_serials),
        "runtime_records_in_feedback_serial_window": len(
            interior_runtime_records
        ),
        "runtime_records_before_feedback_serial_window": sum(
            minimum_feedback_serial is not None
            and serial < minimum_feedback_serial
            for serial in runtime_records
        ),
        "runtime_records_after_feedback_serial_window": sum(
            maximum_feedback_serial is not None
            and serial > maximum_feedback_serial
            for serial in runtime_records
        ),
        "feedback_serials_missing_from_runtime_map_count": len(
            feedback_serials_missing
        ),
        "feedback_serials_missing_from_runtime_map_sample": (
            feedback_serials_missing[:64]
        ),
        "runtime_serials_missing_from_feedback_scopes_count": len(
            runtime_serials_missing
        ),
        "runtime_serials_missing_from_feedback_scopes_sample": (
            runtime_serials_missing[:64]
        ),
        "interior_serial_set_equal": serial_set_equal,
        "complete": runtime_parse_complete and serial_set_equal,
    }
    return interior_runtime_records, diagnostics


def extract_signpost_intervals(
    rows: Sequence[Row],
    name: str,
    subsystem: str | None,
    serial_pattern: re.Pattern[str],
) -> tuple[list[SignpostInterval], dict[str, int]]:
    relevant = [
        row
        for row in rows
        if row.text("name") == name
        and (subsystem is None or row.text("subsystem") == subsystem)
    ]
    relevant.sort(key=lambda row: row.integer("time") or -1)

    open_intervals: dict[tuple[int, int], list[tuple[int, int, str]]] = defaultdict(list)
    intervals: list[SignpostInterval] = []
    unparsed_serial = 0
    malformed_rows = 0
    unmatched_end_serials: list[int | None] = []

    for row in relevant:
        timestamp = row.integer("time")
        thread_id = row.tid
        identifier = row.integer("identifier")
        event_type = (row.text("event-type") or "").casefold()
        message = row.text("message") or ""
        if timestamp is None or thread_id is None or identifier is None:
            malformed_rows += 1
            continue
        key = (thread_id, identifier)
        if event_type == "begin":
            match = serial_pattern.search(message)
            if match is None:
                unparsed_serial += 1
                continue
            try:
                serial = int(match.group(1), 0)
            except (IndexError, ValueError):
                unparsed_serial += 1
                continue
            open_intervals[key].append((timestamp, serial, message))
        elif event_type == "end":
            if not open_intervals[key]:
                match = serial_pattern.search(message)
                if match is None:
                    unparsed_serial += 1
                    unmatched_end_serials.append(None)
                else:
                    try:
                        unmatched_end_serials.append(int(match.group(1), 0))
                    except (IndexError, ValueError):
                        unparsed_serial += 1
                        unmatched_end_serials.append(None)
                continue
            start, serial, begin_message = open_intervals[key].pop()
            if timestamp < start:
                malformed_rows += 1
                continue
            intervals.append(
                SignpostInterval(
                    start_ns=start,
                    end_ns=timestamp,
                    thread_id=thread_id,
                    identifier=identifier,
                    serial=serial,
                    message=begin_message,
                )
            )
        else:
            malformed_rows += 1

    unmatched_begin_serials = [
        serial
        for entries in open_intervals.values()
        for _, serial, _ in entries
    ]
    serial_counts = Counter(interval.serial for interval in intervals)
    if serial_counts:
        minimum_complete_serial = min(serial_counts)
        maximum_complete_serial = max(serial_counts)
        edge_truncated_unmatched_ends = sum(
            serial is not None and serial < minimum_complete_serial
            for serial in unmatched_end_serials
        )
        edge_truncated_unmatched_begins = sum(
            serial > maximum_complete_serial for serial in unmatched_begin_serials
        )
    else:
        minimum_complete_serial = None
        maximum_complete_serial = None
        edge_truncated_unmatched_ends = 0
        edge_truncated_unmatched_begins = 0
    unmatched_ends = len(unmatched_end_serials) - edge_truncated_unmatched_ends
    unmatched_begins = len(unmatched_begin_serials) - edge_truncated_unmatched_begins
    diagnostics = {
        "matching_rows": len(relevant),
        "complete_intervals": len(intervals),
        "unique_serials": len(serial_counts),
        "duplicate_serial_intervals": sum(count - 1 for count in serial_counts.values()),
        "unparsed_serial_rows": unparsed_serial,
        "unmatched_begins": unmatched_begins,
        "unmatched_ends": unmatched_ends,
        "edge_truncated_unmatched_begins": edge_truncated_unmatched_begins,
        "edge_truncated_unmatched_ends": edge_truncated_unmatched_ends,
        "minimum_complete_serial": minimum_complete_serial,
        "maximum_complete_serial": maximum_complete_serial,
        "malformed_rows": malformed_rows,
    }
    intervals.sort(key=lambda item: (item.start_ns, item.end_ns, item.serial))
    return intervals, diagnostics


def extract_debug_group_intervals(
    rows: Sequence[Row],
    label_pattern: re.Pattern[str],
    serial_pattern: re.Pattern[str],
) -> tuple[list[SignpostInterval], dict[str, int]]:
    relevant = [
        row
        for row in rows
        if (row.text("event-type") or "").casefold() == "debug groups"
        and label_pattern.search(row.text("event-label") or "")
    ]

    intervals: list[SignpostInterval] = []
    unparsed_serial = 0
    malformed_rows = 0
    for index, row in enumerate(relevant):
        interval = valid_interval(row)
        thread_id = row.tid
        label = row.text("event-label") or ""
        if interval is None or thread_id is None:
            malformed_rows += 1
            continue
        match = serial_pattern.search(label)
        if match is None:
            unparsed_serial += 1
            continue
        try:
            serial = int(match.group(1), 0)
        except (IndexError, ValueError):
            unparsed_serial += 1
            continue
        start, end = interval
        intervals.append(
            SignpostInterval(
                start_ns=start,
                end_ns=end,
                thread_id=thread_id,
                identifier=-(index + 1),
                serial=serial,
                message=label,
            )
        )

    serial_counts = Counter(interval.serial for interval in intervals)
    diagnostics = {
        "matching_rows": len(relevant),
        "complete_intervals": len(intervals),
        "unique_serials": len(serial_counts),
        "duplicate_serial_intervals": sum(
            count - 1 for count in serial_counts.values()
        ),
        "unparsed_serial_rows": unparsed_serial,
        "unmatched_begins": 0,
        "unmatched_ends": 0,
        "edge_truncated_unmatched_begins": 0,
        "edge_truncated_unmatched_ends": 0,
        "minimum_complete_serial": min(serial_counts) if serial_counts else None,
        "maximum_complete_serial": max(serial_counts) if serial_counts else None,
        "malformed_rows": malformed_rows,
    }
    intervals.sort(key=lambda item: (item.start_ns, item.end_ns, item.serial))
    return intervals, diagnostics


def map_signposts_to_encoders(
    signposts: Sequence[SignpostInterval],
    encoders: Sequence[Encoder],
) -> tuple[list[SignpostMapping], dict[str, int]]:
    encoders_by_thread: dict[int, list[Encoder]] = defaultdict(list)
    for encoder in encoders:
        if encoder.thread_id is not None:
            encoders_by_thread[encoder.thread_id].append(encoder)

    encoder_starts_by_thread: dict[int, list[int]] = {}
    encoder_prefix_max_end_by_thread: dict[int, list[int]] = {}
    overlapping_same_thread_encoder_intervals = 0
    for thread_id, thread_encoders in encoders_by_thread.items():
        thread_encoders.sort(key=lambda item: (item.start_ns, item.encoder_id))
        encoder_starts_by_thread[thread_id] = [item.start_ns for item in thread_encoders]
        prefix_max_end: list[int] = []
        maximum_end = -1
        for encoder in thread_encoders:
            maximum_end = max(maximum_end, encoder.start_ns + encoder.duration_ns)
            prefix_max_end.append(maximum_end)
        encoder_prefix_max_end_by_thread[thread_id] = prefix_max_end
        for previous, current in zip(thread_encoders, thread_encoders[1:]):
            if previous.start_ns + previous.duration_ns > current.start_ns:
                overlapping_same_thread_encoder_intervals += 1

    mappings: list[SignpostMapping] = []
    without_encoder = 0
    ambiguous_encoder = 0
    partial_overlap = 0
    intervals_mapped = 0
    for signpost in signposts:
        thread_encoders = encoders_by_thread.get(signpost.thread_id, [])
        starts = encoder_starts_by_thread.get(signpost.thread_id, [])
        prefix_max_end = encoder_prefix_max_end_by_thread.get(
            signpost.thread_id,
            [],
        )
        first_candidate = bisect_right(prefix_max_end, signpost.start_ns)
        after_candidate = bisect_right(starts, signpost.end_ns)
        inner: dict[int, Encoder] = {}
        enclosing: dict[int, Encoder] = {}
        partial: dict[int, Encoder] = {}
        for encoder in thread_encoders[first_candidate:after_candidate]:
            encoder_end = encoder.start_ns + encoder.duration_ns
            if signpost.start_ns <= encoder.start_ns and encoder_end <= signpost.end_ns:
                inner[encoder.encoder_id] = encoder
            if encoder.start_ns <= signpost.start_ns and signpost.end_ns <= encoder_end:
                enclosing[encoder.encoder_id] = encoder
            if (
                encoder.start_ns < signpost.end_ns
                and signpost.start_ns < encoder_end
                and encoder.encoder_id not in inner
                and encoder.encoder_id not in enclosing
            ):
                partial[encoder.encoder_id] = encoder

        if partial:
            partial_overlap += 1
            ambiguous_encoder += 1
            continue

        distinct_enclosing = {
            encoder_id: encoder
            for encoder_id, encoder in enclosing.items()
            if encoder_id not in inner
        }
        if inner and distinct_enclosing:
            ambiguous_encoder += 1
            continue
        if inner:
            selected = [
                (encoder, "encoder_within_signpost")
                for encoder in sorted(
                    inner.values(),
                    key=lambda item: (item.start_ns, item.encoder_id),
                )
            ]
        elif len(enclosing) == 1:
            selected = [
                (next(iter(enclosing.values())), "signpost_within_encoder")
            ]
        elif len(enclosing) > 1:
            ambiguous_encoder += 1
            continue
        else:
            without_encoder += 1
            continue

        intervals_mapped += 1
        for encoder, containment in selected:
            mappings.append(
                SignpostMapping(
                    signpost=signpost,
                    encoder=encoder,
                    containment=containment,
                )
            )

    return mappings, {
        "intervals_mapped": intervals_mapped,
        "without_encoder": without_encoder,
        "ambiguous_encoder": ambiguous_encoder,
        "partial_overlap": partial_overlap,
        "overlapping_same_thread_encoder_intervals": overlapping_same_thread_encoder_intervals,
    }


def build_route_gpu_metrics(
    runtime_records: dict[int, RuntimeMapRecord],
    mappings: Sequence[SignpostMapping],
    gpu_intervals_by_encoder: dict[int, list[tuple[int, int]]],
    frame_numbers: Sequence[int],
) -> dict[str, object]:
    serials_by_encoder: dict[int, set[int]] = defaultdict(set)
    frame_by_encoder: dict[int, int] = {}
    for mapping in mappings:
        serials_by_encoder[mapping.encoder.encoder_id].add(mapping.signpost.serial)
        if mapping.encoder.frame_number is not None:
            frame_by_encoder[mapping.encoder.encoder_id] = (
                mapping.encoder.frame_number
            )

    def gpu_union(encoder_ids: set[int]) -> int:
        return union_ns(
            interval
            for encoder_id in encoder_ids
            for interval in gpu_intervals_by_encoder.get(encoder_id, [])
        )

    def per_frame_distribution(encoder_ids: set[int]) -> dict[str, float | int] | None:
        values = [
            union_ns(
                interval
                for encoder_id in encoder_ids
                if frame_by_encoder.get(encoder_id) == frame
                for interval in gpu_intervals_by_encoder.get(encoder_id, [])
            )
            for frame in frame_numbers
        ]
        return distribution(values)

    total_logical_bytes = sum(
        record.logical_bytes for record in runtime_records.values()
    )
    route_metrics: dict[str, object] = {}
    for route_name, route_bit in RUNTIME_ROUTE_BITS.items():
        candidate_serials = {
            serial
            for serial, record in runtime_records.items()
            if record.route_flags & route_bit
        }
        candidate_encoders: set[int] = set()
        candidate_exclusive_encoders: set[int] = set()
        mixed_encoders: set[int] = set()
        for encoder_id, encoder_serials in serials_by_encoder.items():
            if not (encoder_serials & candidate_serials):
                continue
            candidate_encoders.add(encoder_id)
            if encoder_serials <= candidate_serials:
                candidate_exclusive_encoders.add(encoder_id)
            else:
                mixed_encoders.add(encoder_id)

        candidate_records = [
            runtime_records[serial] for serial in sorted(candidate_serials)
        ]
        route_metrics[route_name] = {
            "route_bit": f"0x{route_bit:x}",
            "candidate_serial_count": len(candidate_serials),
            "candidate_logical_bytes": sum(
                record.logical_bytes for record in candidate_records
            ),
            "candidate_serial_share": round(
                len(candidate_serials) / len(runtime_records),
                6,
            )
            if runtime_records
            else 0.0,
            "candidate_logical_byte_share": round(
                sum(record.logical_bytes for record in candidate_records)
                / total_logical_bytes,
                6,
            )
            if total_logical_bytes
            else 0.0,
            "candidate_outcomes": dict(
                sorted(Counter(record.outcome for record in candidate_records).items())
            ),
            "candidate_reject_bits": top_counts(
                f"0x{record.reject_bits:016x}" for record in candidate_records
            ),
            "candidate_encoder_count": len(candidate_encoders),
            "candidate_serial_set_exclusive_encoder_count": len(
                candidate_exclusive_encoders
            ),
            "mixed_candidate_and_noncandidate_encoder_count": len(mixed_encoders),
            "candidate_encoder_gpu_active_union_ms": ns_to_ms(
                gpu_union(candidate_encoders)
            ),
            "candidate_serial_set_exclusive_encoder_gpu_active_union_ms": ns_to_ms(
                gpu_union(candidate_exclusive_encoders)
            ),
            "mixed_candidate_and_noncandidate_encoder_gpu_active_union_ms": ns_to_ms(
                gpu_union(mixed_encoders)
            ),
            "candidate_encoder_gpu_active_per_all_frame": per_frame_distribution(
                candidate_encoders
            ),
            "candidate_serial_set_exclusive_encoder_gpu_active_per_all_frame": (
                per_frame_distribution(candidate_exclusive_encoders)
            ),
            "mixed_candidate_and_noncandidate_encoder_gpu_active_per_all_frame": (
                per_frame_distribution(mixed_encoders)
            ),
            "qualification": (
                "Exclusive means every feedback serial mapped to that encoder has "
                "this route bit. The Metal encoder may still contain unrelated, "
                "unlabelled blits, so its GPU duration remains an upper bound."
            ),
        }
    return {
        "selected_window_totals": {
            "actual_copy_serial_count": len(runtime_records),
            "logical_bytes": total_logical_bytes,
            "outcomes": dict(
                sorted(Counter(record.outcome for record in runtime_records.values()).items())
            ),
        },
        "routes": route_metrics,
    }


def determine_status(
    *,
    has_feedback_scope_rows: bool,
    feedback_scope_attribution_complete: bool,
    feedback_label_visible: bool,
    has_feedback_gpu_intervals: bool,
    feedback_gpu_join_complete: bool,
    feedback_frame_assignment_complete: bool,
    runtime_log_provided: bool,
    runtime_reconciliation_complete: bool,
    has_consumer_pairs: bool,
    attribution_basis: str,
) -> str:
    if has_feedback_scope_rows and not feedback_scope_attribution_complete:
        if attribution_basis == "command_buffer_debug_group_pid_thread_containment_fallback":
            return "feedback_debug_group_attribution_incomplete"
        return "feedback_signpost_attribution_incomplete"
    if not has_feedback_scope_rows and not feedback_label_visible:
        return "no_feedback_signpost_or_encoder_label_match"
    if not has_feedback_gpu_intervals:
        return "feedback_attribution_found_but_no_joined_gpu_intervals"
    if not feedback_gpu_join_complete:
        return "feedback_attribution_found_but_gpu_join_incomplete"
    if not feedback_frame_assignment_complete:
        return "feedback_attribution_found_but_frame_assignment_incomplete"
    if not runtime_log_provided:
        return "feedback_timing_measured_necessary_condition_only_runtime_log_not_provided"
    if not runtime_reconciliation_complete:
        return "feedback_runtime_map_reconciliation_incomplete"
    if not has_consumer_pairs:
        return "feedback_gpu_intervals_runtime_reconciled_no_consumer_pairs"
    if attribution_basis == "command_buffer_debug_group_pid_thread_containment_fallback":
        return "feedback_gpu_intervals_measured_by_debug_group_fallback"
    if attribution_basis == "encoder_label_regex_fallback":
        return "feedback_gpu_intervals_measured_by_label_fallback"
    return "feedback_signpost_and_candidate_consumer_intervals_measured"


def analyze(
    toc: Path,
    export_paths: dict[str, Path],
    trace: Path,
    run_number: int,
    pid: int,
    process_name: str | None,
    feedback_pattern: re.Pattern[str],
    consumer_pattern: re.Pattern[str] | None,
    signpost_name: str,
    signpost_subsystem: str | None,
    signpost_serial_pattern: re.Pattern[str],
    runtime_log: Path | None,
) -> dict[str, object]:
    tables = {
        schema: ExportedTable(path)
        for schema, path in export_paths.items()
    }

    def target_rows(schema: str) -> list[Row]:
        table = tables.get(schema)
        if table is None:
            return []
        return [row for row in table.rows if row.pid == pid]

    encoder_rows = target_rows("metal-application-encoders-list")
    frame_rows = target_rows("metal-command-buffer-frame-assignment")
    gpu_rows = target_rows("metal-gpu-intervals")
    submission_rows = target_rows("metal-application-command-buffer-submissions")
    object_label_rows = target_rows("metal-object-label")
    application_interval_rows = target_rows("metal-application-intervals")
    application_event_rows = target_rows("metal-application-event-interval")
    driver_rows = target_rows("metal-driver-intervals")
    driver_event_rows = target_rows("metal-driver-event-intervals")
    signpost_rows = target_rows("os-signpost")

    frame_by_command_buffer: dict[int, int] = {}
    commit_by_command_buffer: dict[int, int] = {}
    for row in frame_rows:
        command_buffer_id = row.integer("cmdbuffer-id")
        frame_number = row.integer("frame-number")
        if command_buffer_id is None:
            continue
        if frame_number is not None and frame_number != 0xFFFFFFFF:
            frame_by_command_buffer[command_buffer_id] = frame_number
        commit = row.integer("commit-time")
        if commit is not None:
            commit_by_command_buffer[command_buffer_id] = commit

    parsed_encoders: list[Encoder] = []
    malformed_encoder_rows = 0
    for row in encoder_rows:
        command_buffer_id = row.integer("cmdbuffer-id")
        encoder_id = row.integer("encoder-id")
        start = row.integer("start")
        duration = row.integer("duration")
        if None in (command_buffer_id, encoder_id, start, duration):
            malformed_encoder_rows += 1
            continue
        assert command_buffer_id is not None
        assert encoder_id is not None
        assert start is not None
        assert duration is not None
        frame = frame_by_command_buffer.get(command_buffer_id)
        parsed_encoders.append(
            Encoder(
                command_buffer_id=command_buffer_id,
                encoder_id=encoder_id,
                frame_number=frame,
                start_ns=start,
                duration_ns=duration,
                thread_id=row.tid,
                command_buffer_label=row.text("cmdbuffer-label") or "",
                encoder_label=row.text("encoder-label") or "",
            )
        )

    encoder_by_id: dict[int, Encoder] = {}
    duplicate_encoder_rows = 0
    conflicting_encoder_ids: set[int] = set()
    for encoder in parsed_encoders:
        previous = encoder_by_id.get(encoder.encoder_id)
        if previous is None:
            encoder_by_id[encoder.encoder_id] = encoder
        else:
            duplicate_encoder_rows += 1
            if previous != encoder:
                conflicting_encoder_ids.add(encoder.encoder_id)
    encoders = sorted(
        encoder_by_id.values(),
        key=lambda item: (item.start_ns, item.command_buffer_id, item.encoder_id),
    )

    gpu_intervals_by_encoder: dict[int, list[tuple[int, int]]] = defaultdict(list)
    gpu_channels_by_encoder: dict[int, set[str]] = defaultdict(set)
    non_active_gpu_rows = 0
    malformed_gpu_rows = 0
    for row in gpu_rows:
        state = (row.text("state") or "").casefold()
        if state and state != "active":
            non_active_gpu_rows += 1
            continue
        encoder_id = row.integer("encoder-id")
        interval = valid_interval(row)
        if encoder_id is None or interval is None:
            malformed_gpu_rows += 1
            continue
        gpu_intervals_by_encoder[encoder_id].append(interval)
        channel = row.text("channel-name")
        if channel:
            gpu_channels_by_encoder[encoder_id].add(channel)

    label_feedback_encoders = [
        encoder for encoder in encoders if feedback_pattern.search(encoder.encoder_label)
    ]

    points_of_interest_scopes, points_of_interest_diagnostics = extract_signpost_intervals(
        signpost_rows,
        signpost_name,
        signpost_subsystem,
        signpost_serial_pattern,
    )
    debug_group_scopes, debug_group_diagnostics = extract_debug_group_intervals(
        application_event_rows,
        feedback_pattern,
        signpost_serial_pattern,
    )
    if points_of_interest_diagnostics["matching_rows"]:
        feedback_scopes = points_of_interest_scopes
        feedback_scope_diagnostics = points_of_interest_diagnostics
        feedback_scope_source = "points_of_interest_signpost"
        scope_attribution_basis = "points_of_interest_signpost_pid_thread_containment"
    elif debug_group_diagnostics["matching_rows"]:
        feedback_scopes = debug_group_scopes
        feedback_scope_diagnostics = debug_group_diagnostics
        feedback_scope_source = "metal_command_buffer_debug_group"
        scope_attribution_basis = (
            "command_buffer_debug_group_pid_thread_containment_fallback"
        )
    else:
        feedback_scopes = []
        feedback_scope_diagnostics = points_of_interest_diagnostics
        feedback_scope_source = "none"
        scope_attribution_basis = "none"

    signpost_mappings, encoder_mapping_diagnostics = map_signposts_to_encoders(
        feedback_scopes,
        encoders,
    )
    signposts_without_encoder = encoder_mapping_diagnostics["without_encoder"]
    signposts_with_ambiguous_encoders = encoder_mapping_diagnostics[
        "ambiguous_encoder"
    ]
    overlapping_same_thread_encoder_intervals = encoder_mapping_diagnostics[
        "overlapping_same_thread_encoder_intervals"
    ]

    signpost_feedback_encoder_ids = {
        mapping.encoder.encoder_id for mapping in signpost_mappings
    }
    feedback_scope_rows_present = bool(
        feedback_scope_diagnostics["matching_rows"]
    )
    if feedback_scope_rows_present:
        attribution_basis = scope_attribution_basis
        feedback_encoders = [
            encoder
            for encoder in encoders
            if encoder.encoder_id in signpost_feedback_encoder_ids
        ]
    else:
        attribution_basis = "encoder_label_regex_fallback"
        feedback_encoders = label_feedback_encoders

    encoders_by_command_buffer: dict[int, list[Encoder]] = defaultdict(list)
    for encoder in encoders:
        encoders_by_command_buffer[encoder.command_buffer_id].append(encoder)
    for command_buffer_encoders in encoders_by_command_buffer.values():
        command_buffer_encoders.sort(key=lambda item: (item.start_ns, item.encoder_id))

    pairs: list[EncoderPair] = []
    no_later_encoder = 0
    no_render_consumer = 0
    for feedback in feedback_encoders:
        ordered = encoders_by_command_buffer[feedback.command_buffer_id]
        later = [
            encoder
            for encoder in ordered
            if (encoder.start_ns, encoder.encoder_id)
            > (feedback.start_ns, feedback.encoder_id)
        ]
        if not later:
            no_later_encoder += 1
            continue

        consumer: Encoder | None = None
        basis: str
        if consumer_pattern is not None:
            consumer = next(
                (
                    encoder
                    for encoder in later
                    if consumer_pattern.search(encoder.encoder_label)
                ),
                None,
            )
            basis = "explicit_consumer_encoder_label"
        else:
            consumer = next(
                (
                    encoder
                    for encoder in later
                    if {
                        channel.casefold()
                        for channel in gpu_channels_by_encoder.get(encoder.encoder_id, set())
                    }
                    & RENDER_GPU_CHANNELS
                ),
                None,
            )
            basis = "next_same_command_buffer_vertex_or_fragment_encoder_candidate"
        if consumer is None:
            no_render_consumer += 1
            continue
        pairs.append(EncoderPair(feedback=feedback, consumer=consumer, basis=basis))

    target_command_buffers = {encoder.command_buffer_id for encoder in encoders}
    completion_table = tables.get("metal-command-buffer-completed")
    completion_by_command_buffer: dict[int, int] = {}
    if completion_table:
        for row in completion_table.rows:
            command_buffer_id = row.integer("cmdbuffer-id")
            timestamp = row.integer("timestamp")
            if (
                command_buffer_id in target_command_buffers
                and timestamp is not None
            ):
                completion_by_command_buffer[command_buffer_id] = timestamp

    submissions_by_frame: dict[int, list[tuple[int, int]]] = defaultdict(list)
    for row in submission_rows:
        command_buffer_id = row.integer("cmdbuffer-id")
        interval = valid_interval(row)
        if command_buffer_id is None or interval is None:
            continue
        frame = frame_by_command_buffer.get(command_buffer_id, row.integer("frame-number"))
        if frame is not None:
            submissions_by_frame[frame].append(interval)

    encoders_by_frame: dict[int, list[Encoder]] = defaultdict(list)
    feedback_by_frame: dict[int, list[Encoder]] = defaultdict(list)
    pairs_by_frame: dict[int, list[EncoderPair]] = defaultdict(list)
    signpost_mappings_by_frame: dict[int, list[SignpostMapping]] = defaultdict(list)
    signpost_mappings_by_encoder: dict[int, list[SignpostMapping]] = defaultdict(list)
    for encoder in encoders:
        if encoder.frame_number is not None:
            encoders_by_frame[encoder.frame_number].append(encoder)
    for encoder in feedback_encoders:
        if encoder.frame_number is not None:
            feedback_by_frame[encoder.frame_number].append(encoder)
    for pair in pairs:
        if pair.feedback.frame_number is not None:
            pairs_by_frame[pair.feedback.frame_number].append(pair)
    for mapping in signpost_mappings:
        signpost_mappings_by_encoder[mapping.encoder.encoder_id].append(mapping)
        if mapping.encoder.frame_number is not None:
            signpost_mappings_by_frame[mapping.encoder.frame_number].append(mapping)

    per_frame: list[dict[str, object]] = []
    for frame in sorted(encoders_by_frame):
        frame_encoders = encoders_by_frame[frame]
        frame_feedback = feedback_by_frame.get(frame, [])
        frame_pairs = pairs_by_frame.get(frame, [])
        frame_signpost_mappings = signpost_mappings_by_frame.get(frame, [])
        frame_signposts = set(
            mapping.signpost for mapping in frame_signpost_mappings
        )
        command_buffers = {encoder.command_buffer_id for encoder in frame_encoders}

        all_cpu = [
            (encoder.start_ns, encoder.start_ns + encoder.duration_ns)
            for encoder in frame_encoders
        ]
        feedback_cpu = [
            (encoder.start_ns, encoder.start_ns + encoder.duration_ns)
            for encoder in frame_feedback
        ]
        feedback_signpost_cpu = [
            (signpost.start_ns, signpost.end_ns)
            for signpost in frame_signposts
        ]
        all_gpu = [
            interval
            for encoder in frame_encoders
            for interval in gpu_intervals_by_encoder.get(encoder.encoder_id, [])
        ]
        feedback_gpu = [
            interval
            for encoder in frame_feedback
            for interval in gpu_intervals_by_encoder.get(encoder.encoder_id, [])
        ]
        consumer_gpu = [
            interval
            for pair in frame_pairs
            for interval in gpu_intervals_by_encoder.get(pair.consumer.encoder_id, [])
        ]
        feedback_plus_consumer_gpu = [*feedback_gpu, *consumer_gpu]
        pair_spans: list[tuple[int, int]] = []
        for pair in frame_pairs:
            copy_intervals = gpu_intervals_by_encoder.get(pair.feedback.encoder_id, [])
            consume_intervals = gpu_intervals_by_encoder.get(pair.consumer.encoder_id, [])
            if copy_intervals and consume_intervals:
                pair_spans.append(
                    (
                        min(start for start, _ in copy_intervals),
                        max(end for _, end in consume_intervals),
                    )
                )

        lifetimes = [
            (commit_by_command_buffer[command_buffer_id], completion_by_command_buffer[command_buffer_id])
            for command_buffer_id in command_buffers
            if command_buffer_id in commit_by_command_buffer
            and command_buffer_id in completion_by_command_buffer
            and completion_by_command_buffer[command_buffer_id]
            >= commit_by_command_buffer[command_buffer_id]
        ]

        per_frame.append(
            {
                "frame": frame,
                "command_buffer_count": len(command_buffers),
                "encoder_count": len(frame_encoders),
                "feedback_encoder_count": len(frame_feedback),
                "feedback_scope_interval_count": len(frame_signposts),
                "feedback_scope_mapping_edge_count": len(
                    frame_signpost_mappings
                ),
                "feedback_scope_unique_serial_count": len(
                    {signpost.serial for signpost in frame_signposts}
                ),
                "feedback_encoders_with_multiple_signposts": sum(
                    len(signpost_mappings_by_encoder.get(encoder.encoder_id, [])) > 1
                    for encoder in frame_feedback
                ),
                "max_signposts_per_feedback_encoder": max(
                    (
                        len(signpost_mappings_by_encoder.get(encoder.encoder_id, []))
                        for encoder in frame_feedback
                    ),
                    default=0,
                ),
                "paired_consumer_count": len(frame_pairs),
                "all_encoder_cpu_union_ms": ns_to_ms(union_ns(all_cpu)),
                "feedback_encoder_cpu_union_ms": ns_to_ms(union_ns(feedback_cpu)),
                "feedback_scope_cpu_union_ms": ns_to_ms(
                    union_ns(feedback_signpost_cpu)
                ),
                "command_buffer_submission_cpu_union_ms": ns_to_ms(
                    union_ns(submissions_by_frame.get(frame, []))
                ),
                "all_gpu_active_union_ms": ns_to_ms(union_ns(all_gpu)),
                "feedback_gpu_active_union_ms": ns_to_ms(union_ns(feedback_gpu)),
                "paired_consumer_gpu_active_union_ms": ns_to_ms(union_ns(consumer_gpu)),
                "feedback_plus_consumer_gpu_active_union_ms": ns_to_ms(
                    union_ns(feedback_plus_consumer_gpu)
                ),
                "feedback_to_consumer_span_union_ms": ns_to_ms(union_ns(pair_spans)),
                "command_buffer_commit_to_completion_union_ms": ns_to_ms(
                    union_ns(lifetimes)
                ),
            }
        )

    object_feedback_matches = [
        row
        for row in object_label_rows
        if feedback_pattern.search(row.text("label") or "")
    ]
    application_feedback_matches = [
        row
        for row in application_interval_rows
        if feedback_pattern.search(row.text("event-label") or "")
    ]
    driver_feedback_matches = [
        row
        for row in driver_rows
        if feedback_pattern.search(row.text("event-label") or "")
    ]
    driver_feedback_intervals = [
        interval
        for row in driver_feedback_matches
        if (interval := valid_interval(row)) is not None
    ]

    feedback_gpu_all_frames_ns = [
        round(float(frame["feedback_gpu_active_union_ms"]) * 1_000_000)
        for frame in per_frame
    ]
    feedback_gpu_active_frames_ns = [
        round(float(frame["feedback_gpu_active_union_ms"]) * 1_000_000)
        for frame in per_frame
        if int(frame["feedback_encoder_count"]) > 0
    ]
    feedback_scope_cpu_all_frames_ns = [
        round(float(frame["feedback_scope_cpu_union_ms"]) * 1_000_000)
        for frame in per_frame
    ]
    feedback_scope_cpu_active_frames_ns = [
        round(float(frame["feedback_scope_cpu_union_ms"]) * 1_000_000)
        for frame in per_frame
        if int(frame["feedback_scope_interval_count"]) > 0
    ]
    span_per_frame_ns = [
        round(float(frame["feedback_to_consumer_span_union_ms"]) * 1_000_000)
        for frame in per_frame
        if int(frame["paired_consumer_count"]) > 0
    ]

    consumer_basis = (
        "explicit_consumer_encoder_label"
        if consumer_pattern is not None
        else "next_same_command_buffer_vertex_or_fragment_encoder_candidate"
    )
    feedback_label_visible = bool(label_feedback_encoders)
    mapped_serials = [mapping.signpost.serial for mapping in signpost_mappings]
    mapping_containment_counts = Counter(
        mapping.containment for mapping in signpost_mappings
    )
    feedback_scope_attribution_complete = bool(feedback_scopes) and (
        encoder_mapping_diagnostics["intervals_mapped"] == len(feedback_scopes)
        and len(set(mapped_serials)) == feedback_scope_diagnostics["unique_serials"]
        and feedback_scope_diagnostics["duplicate_serial_intervals"] == 0
        and feedback_scope_diagnostics["unparsed_serial_rows"] == 0
        and feedback_scope_diagnostics["unmatched_begins"] == 0
        and feedback_scope_diagnostics["unmatched_ends"] == 0
        and feedback_scope_diagnostics["malformed_rows"] == 0
        and encoder_mapping_diagnostics["without_encoder"] == 0
        and encoder_mapping_diagnostics["ambiguous_encoder"] == 0
        and encoder_mapping_diagnostics["partial_overlap"] == 0
        and not conflicting_encoder_ids
    )

    feedback_encoders_with_gpu_intervals = sum(
        bool(gpu_intervals_by_encoder.get(encoder.encoder_id))
        for encoder in feedback_encoders
    )
    feedback_gpu_join_complete = bool(feedback_encoders) and (
        feedback_encoders_with_gpu_intervals == len(feedback_encoders)
    )
    feedback_encoders_with_frame_assignment = sum(
        encoder.frame_number is not None for encoder in feedback_encoders
    )
    feedback_frame_assignment_complete = bool(feedback_encoders) and (
        feedback_encoders_with_frame_assignment == len(feedback_encoders)
    )
    frames_by_feedback_scope: dict[SignpostInterval, set[int]] = defaultdict(set)
    for mapping in signpost_mappings:
        if mapping.encoder.frame_number is not None:
            frames_by_feedback_scope[mapping.signpost].add(
                mapping.encoder.frame_number
            )
    feedback_scopes_with_multiple_frame_assignments = sum(
        len(frames) > 1 for frames in frames_by_feedback_scope.values()
    )
    feedback_frame_assignment_complete = (
        feedback_frame_assignment_complete
        and feedback_scopes_with_multiple_frame_assignments == 0
    )

    runtime_records: dict[int, RuntimeMapRecord] = {}
    runtime_diagnostics: dict[str, object] = {
        "parse_complete": False,
        "reason": "runtime_log_not_provided",
    }
    if runtime_log is not None:
        runtime_records, runtime_diagnostics = load_runtime_map(runtime_log)
    feedback_scope_serials = {scope.serial for scope in feedback_scopes}
    interior_runtime_records, runtime_reconciliation = reconcile_runtime_map(
        feedback_scope_serials,
        runtime_records,
        bool(runtime_log) and bool(runtime_diagnostics.get("parse_complete")),
    )
    runtime_reconciliation_complete = bool(runtime_reconciliation["complete"])
    route_gpu_metrics = (
        build_route_gpu_metrics(
            interior_runtime_records,
            signpost_mappings,
            gpu_intervals_by_encoder,
            sorted(encoders_by_frame),
        )
        if (
            runtime_reconciliation_complete
            and feedback_scope_attribution_complete
            and feedback_gpu_join_complete
            and feedback_frame_assignment_complete
        )
        else None
    )

    result = {
        "schema_version": 3,
        "trace": {
            "path": str(trace.resolve()),
            **trace_metadata(toc, run_number),
        },
        "selection": {
            "pid": pid,
            "process_name": process_name,
            "feedback_attribution_basis": attribution_basis,
            "feedback_scope_source": feedback_scope_source,
            "signpost_name": signpost_name,
            "signpost_subsystem": signpost_subsystem,
            "signpost_serial_regex": signpost_serial_pattern.pattern,
            "feedback_regex": feedback_pattern.pattern,
            "consumer_regex": consumer_pattern.pattern if consumer_pattern else None,
            "consumer_association_basis": consumer_basis,
            "runtime_log": str(runtime_log.resolve()) if runtime_log else None,
        },
        "measurability": {
            "cpu_encoder_intervals_joined_by_encoder_id": bool(encoder_rows),
            "gpu_intervals_joined_by_encoder_id": bool(gpu_rows),
            "frame_assignment_joined_by_command_buffer_id": bool(frame_rows),
            "points_of_interest_feedback_scopes_exported": bool(
                points_of_interest_diagnostics["matching_rows"]
            ),
            "points_of_interest_feedback_scopes_strictly_mapped": (
                feedback_scope_source == "points_of_interest_signpost"
                and feedback_scope_attribution_complete
            ),
            "command_buffer_debug_group_feedback_scopes_exported": bool(
                debug_group_diagnostics["matching_rows"]
            ),
            "command_buffer_debug_group_feedback_scopes_strictly_mapped": (
                feedback_scope_source == "metal_command_buffer_debug_group"
                and feedback_scope_attribution_complete
            ),
            "selected_feedback_scopes_strictly_mapped": feedback_scope_attribution_complete,
            "feedback_encoder_label_exported": feedback_label_visible,
            "feedback_object_label_exported": bool(object_feedback_matches),
            "feedback_gpu_interval_join_complete": feedback_gpu_join_complete,
            "feedback_frame_assignment_complete": feedback_frame_assignment_complete,
            "runtime_map_reconciliation_complete": runtime_reconciliation_complete,
            "nested_debug_group_column_exported": False,
            "per_encoder_tile_store_load_counters_exported": False,
            "render_pass_load_store_actions_exported": False,
            "render_encoder_break_cost_directly_measured": False,
            "resource_read_dependency_exported": False,
            "next_render_is_proven_feedback_consumer": False,
            "driver_interval_stable_encoder_id_join": False,
            "notes": [
                "GPU rows are merged before summing so simultaneous Vertex/Fragment channel rows are not double-counted.",
                "PointsOfInterest scopes are mapped only by exact PID, thread, and interval containment; overlaps without containment are rejected.",
                "Multiple feedback serials may map to one batched Metal encoder. That encoder is counted once in GPU unions.",
                "One feedback serial may map to multiple Metal encoders wholly contained by its PointsOfInterest scope. Mapping edges and unique encoders are reported separately.",
                "A feedback-attributed Metal encoder can include unlabeled work batched into that encoder; its GPU interval is therefore an upper bound for feedback-copy work.",
                "Default Game Performance exports do not expose per-encoder tile store/load counters or render-pass break cost; those costs can fall outside the feedback blit encoder interval.",
                "The feedback-to-consumer span includes intervening GPU work and idle gaps; it is a serialization-window upper bound, not removable copy time.",
                "The exported schemas do not prove which texture a later render encoder reads.",
                "Without an exact FBPATH_MAP serial-set reconciliation, timing is a necessary-condition oracle only.",
            ],
        },
        "counts": {
            "target_encoder_rows": len(encoder_rows),
            "parsed_target_encoder_rows": len(parsed_encoders),
            "unique_target_encoders": len(encoders),
            "duplicate_target_encoder_rows": duplicate_encoder_rows,
            "conflicting_target_encoder_ids": len(conflicting_encoder_ids),
            "target_gpu_rows": len(gpu_rows),
            "frames": len(per_frame),
            "feedback_points_of_interest_rows": points_of_interest_diagnostics[
                "matching_rows"
            ],
            "feedback_points_of_interest_intervals": len(
                points_of_interest_scopes
            ),
            "feedback_debug_group_rows": debug_group_diagnostics["matching_rows"],
            "feedback_debug_group_intervals": len(debug_group_scopes),
            "feedback_scope_intervals": len(feedback_scopes),
            "feedback_scope_unique_serials": feedback_scope_diagnostics[
                "unique_serials"
            ],
            "feedback_scope_mapped_intervals": encoder_mapping_diagnostics[
                "intervals_mapped"
            ],
            "feedback_scope_mapping_edges": len(signpost_mappings),
            "feedback_scope_mapped_unique_serials": len(set(mapped_serials)),
            "feedback_scope_mapping_coverage": round(
                encoder_mapping_diagnostics["intervals_mapped"]
                / len(feedback_scopes),
                6,
            )
            if feedback_scopes
            else 0.0,
            "feedback_scope_unique_encoder_count": len(
                signpost_feedback_encoder_ids
            ),
            "feedback_scope_without_encoder": signposts_without_encoder,
            "feedback_scope_ambiguous_encoder": signposts_with_ambiguous_encoders,
            "feedback_scope_partial_overlap": encoder_mapping_diagnostics[
                "partial_overlap"
            ],
            "feedback_scope_duplicate_serial_intervals": feedback_scope_diagnostics[
                "duplicate_serial_intervals"
            ],
            "feedback_scope_unparsed_serial_rows": feedback_scope_diagnostics[
                "unparsed_serial_rows"
            ],
            "feedback_scope_unmatched_begins": feedback_scope_diagnostics[
                "unmatched_begins"
            ],
            "feedback_scope_unmatched_ends": feedback_scope_diagnostics[
                "unmatched_ends"
            ],
            "feedback_scope_edge_truncated_unmatched_begins": (
                feedback_scope_diagnostics["edge_truncated_unmatched_begins"]
            ),
            "feedback_scope_edge_truncated_unmatched_ends": (
                feedback_scope_diagnostics["edge_truncated_unmatched_ends"]
            ),
            "feedback_scope_malformed_rows": feedback_scope_diagnostics[
                "malformed_rows"
            ],
            "overlapping_same_thread_encoder_intervals": overlapping_same_thread_encoder_intervals,
            "feedback_encoders": len(feedback_encoders),
            "feedback_encoders_with_multiple_signposts": sum(
                len(mappings) > 1
                for mappings in signpost_mappings_by_encoder.values()
            ),
            "feedback_encoders_with_gpu_intervals": feedback_encoders_with_gpu_intervals,
            "feedback_encoders_without_gpu_intervals": (
                len(feedback_encoders) - feedback_encoders_with_gpu_intervals
            ),
            "feedback_encoders_with_frame_assignment": feedback_encoders_with_frame_assignment,
            "feedback_encoders_without_frame_assignment": (
                len(feedback_encoders) - feedback_encoders_with_frame_assignment
            ),
            "feedback_scopes_with_multiple_frame_assignments": feedback_scopes_with_multiple_frame_assignments,
            "feedback_consumer_pairs": len(pairs),
            "feedback_without_later_same_command_buffer_encoder": no_later_encoder,
            "feedback_without_matching_consumer": no_render_consumer,
            "object_label_feedback_matches": len(object_feedback_matches),
            "application_interval_feedback_matches": len(application_feedback_matches),
            "driver_interval_feedback_matches": len(driver_feedback_matches),
            "driver_event_rows": len(driver_event_rows),
            "malformed_encoder_rows": malformed_encoder_rows,
            "malformed_gpu_rows": malformed_gpu_rows,
            "non_active_gpu_rows_ignored": non_active_gpu_rows,
            "command_buffers_with_completion": len(completion_by_command_buffer),
        },
        "distributions": {
            "feedback_scope_cpu_union_per_all_frame": distribution(
                feedback_scope_cpu_all_frames_ns
            ),
            "feedback_scope_cpu_union_per_active_feedback_frame": distribution(
                feedback_scope_cpu_active_frames_ns
            ),
            "feedback_gpu_active_per_all_frame": distribution(
                feedback_gpu_all_frames_ns
            ),
            "feedback_gpu_active_per_active_feedback_frame": distribution(
                feedback_gpu_active_frames_ns
            ),
            "feedback_to_consumer_span_per_paired_frame": distribution(span_per_frame_ns),
        },
        "runtime_reconciliation": {
            "provided": runtime_log is not None,
            "parser": runtime_diagnostics,
            **runtime_reconciliation,
        },
        "route_gpu_metrics": route_gpu_metrics,
        "driver_feedback_label_matched_cpu_union_ms": ns_to_ms(
            union_ns(driver_feedback_intervals)
        ),
        "feedback_scope_mapping_diagnostics": {
            "containment": dict(sorted(mapping_containment_counts.items())),
            "signposts_per_mapped_encoder": count_distribution(
                [len(mappings) for mappings in signpost_mappings_by_encoder.values()]
            ),
        },
        "label_diagnostics": {
            "encoder_labels": top_counts(encoder.encoder_label for encoder in encoders),
            "command_buffer_labels": top_counts(
                encoder.command_buffer_label for encoder in encoders
            ),
            "gpu_channels": top_counts(
                channel
                for channels in gpu_channels_by_encoder.values()
                for channel in channels
            ),
        },
        "per_frame": per_frame,
    }

    result["status"] = determine_status(
        has_feedback_scope_rows=feedback_scope_rows_present,
        feedback_scope_attribution_complete=feedback_scope_attribution_complete,
        feedback_label_visible=feedback_label_visible,
        has_feedback_gpu_intervals=any(
            gpu_intervals_by_encoder.get(item.encoder_id)
            for item in feedback_encoders
        ),
        feedback_gpu_join_complete=feedback_gpu_join_complete,
        feedback_frame_assignment_complete=feedback_frame_assignment_complete,
        runtime_log_provided=runtime_log is not None,
        runtime_reconciliation_complete=runtime_reconciliation_complete,
        has_consumer_pairs=bool(pairs),
        attribution_basis=attribution_basis,
    )
    return result


def compile_regex(value: str, option: str) -> re.Pattern[str]:
    try:
        return re.compile(value)
    except re.error as exc:
        raise AnalysisError(f"Invalid {option} regular expression {value!r}: {exc}") from exc


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Export and join Xcode Game Performance tables for a framebuffer-feedback "
            "timing feasibility gate. JSON is written to stdout."
        )
    )
    parser.add_argument("trace", type=Path, help="Input .trace bundle")
    parser.add_argument("--run", type=int, default=1, help="Trace run number (default: 1)")
    parser.add_argument("--pid", type=int, help="Exact RPCS3 PID in the trace")
    parser.add_argument(
        "--runtime-log",
        type=Path,
        help=(
            "Full unabridged RPCS3 log containing FBPATH_MAP batches and a final "
            "cumulative FBPATH summary"
        ),
    )
    parser.add_argument(
        "--process-name",
        default="rpcs3",
        help="Exact process name used when --pid is omitted (default: rpcs3)",
    )
    parser.add_argument(
        "--signpost-name",
        default="RPCS3FeedbackEncode",
        help="Exact PointsOfInterest interval name (default: RPCS3FeedbackEncode)",
    )
    parser.add_argument(
        "--signpost-subsystem",
        help="Optional exact subsystem filter for the feedback signpost",
    )
    parser.add_argument(
        "--signpost-serial-regex",
        default=r"serial\s*=\s*(\d+)",
        help="Regex with capture group 1 for the copy serial in the signpost message",
    )
    parser.add_argument(
        "--feedback-regex",
        default=r"(?i)feedback.*copy|copy.*feedback",
        help=(
            "Regex matched against command-buffer Debug Group labels and, only when "
            "no feedback scope rows exist, encoder labels"
        ),
    )
    parser.add_argument(
        "--consumer-regex",
        help=(
            "Optional regex for an explicitly labelled consuming encoder. Without it, "
            "the script selects the next same-command-buffer encoder with a Vertex or "
            "Fragment GPU channel and marks the association as unproven."
        ),
    )
    parser.add_argument(
        "--compact",
        action="store_true",
        help="Write compact rather than indented JSON",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    if args.run < 1:
        print("error: --run must be positive", file=sys.stderr)
        return 2
    if not args.trace.exists():
        print(f"error: trace does not exist: {args.trace}", file=sys.stderr)
        return 2
    if args.runtime_log is not None and not args.runtime_log.is_file():
        print(
            f"error: runtime log does not exist or is not a file: {args.runtime_log}",
            file=sys.stderr,
        )
        return 2

    try:
        feedback_pattern = compile_regex(args.feedback_regex, "--feedback-regex")
        signpost_serial_pattern = compile_regex(
            args.signpost_serial_regex,
            "--signpost-serial-regex",
        )
        consumer_pattern = (
            compile_regex(args.consumer_regex, "--consumer-regex")
            if args.consumer_regex
            else None
        )
        with tempfile.TemporaryDirectory(prefix="rpcs3-metal-trace-export-") as temporary:
            toc, exports = export_trace(args.trace, args.run, Path(temporary))
            pid, process_name = resolve_pid(
                toc,
                args.run,
                args.pid,
                args.process_name,
            )
            result = analyze(
                toc=toc,
                export_paths=exports,
                trace=args.trace,
                run_number=args.run,
                pid=pid,
                process_name=process_name,
                feedback_pattern=feedback_pattern,
                consumer_pattern=consumer_pattern,
                signpost_name=args.signpost_name,
                signpost_subsystem=args.signpost_subsystem,
                signpost_serial_pattern=signpost_serial_pattern,
                runtime_log=args.runtime_log,
            )
    except AnalysisError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    json.dump(
        result,
        sys.stdout,
        indent=None if args.compact else 2,
        sort_keys=False,
    )
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
