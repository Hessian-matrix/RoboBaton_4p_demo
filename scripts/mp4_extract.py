#!/usr/bin/env python3
"""Decode a RoboBaton H.264 MP4 session into timestamp-named JPEG files."""

from __future__ import annotations

import argparse
import csv
import ctypes
import errno
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time
from typing import Any


SCHEMA = "robobaton_mp4_dataset_v1"
SESSION_SCHEMA = "robobaton_h264_mp4_session_v1"
RECEIPT_SCHEMA = "robobaton_h264_mp4_publication_receipt_v1"
DEFAULT_TOOL_TIMEOUT_SECONDS = 1800.0
TOOL_TERMINATE_GRACE_SECONDS = 2.0
TIMESTAMP_COLUMNS = ("frame_index", "timestamp_ns", "key_frame", "encoded_bytes")


class Mp4ExtractError(RuntimeError):
    """A deterministic input, tool, or output contract failure."""


def rename_no_replace(source: Path, destination: Path) -> None:
    """Atomically promote a directory without replacing a concurrent destination."""
    libc = ctypes.CDLL(None, use_errno=True)
    renameat2 = getattr(libc, "renameat2", None)
    if renameat2 is None:
        raise Mp4ExtractError("renameat2(RENAME_NOREPLACE) is unavailable")
    renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
                          ctypes.c_char_p, ctypes.c_uint]
    renameat2.restype = ctypes.c_int
    result = renameat2(-100, os.fsencode(source), -100, os.fsencode(destination), 1)
    if result != 0:
        error_number = ctypes.get_errno()
        if error_number == errno.EEXIST:
            raise Mp4ExtractError(f"output already exists: {destination}")
        raise Mp4ExtractError(
            f"renameat2(RENAME_NOREPLACE) failed: {os.strerror(error_number)}"
        )


def parse_camera_ids(text: str) -> list[int]:
    try:
        values = [int(part) for part in text.split(",") if part != ""]
    except ValueError as exc:
        raise argparse.ArgumentTypeError("camera IDs must be comma-separated integers") from exc
    if not values or len(values) != len(set(values)) or any(value < 0 or value > 3 for value in values):
        raise argparse.ArgumentTypeError("camera IDs must be unique values in 0..3")
    return values


def positive_timeout_seconds(text: str) -> float:
    try:
        value = float(text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("tool timeout must be a number") from exc
    if not value > 0.0:
        raise argparse.ArgumentTypeError("tool timeout must be positive")
    return value


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise Mp4ExtractError(f"required tool not found: {name}")
    return path


def process_group_exists(process_group: int) -> bool:
    try:
        os.killpg(process_group, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def terminate_process_group(process: subprocess.Popen[str]) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    deadline = time.monotonic() + TOOL_TERMINATE_GRACE_SECONDS
    while time.monotonic() < deadline and process_group_exists(process.pid):
        time.sleep(0.01)
    if process_group_exists(process.pid):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=TOOL_TERMINATE_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def run_tool(command: list[str], timeout_seconds: float) -> str:
    process = subprocess.Popen(
        command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        start_new_session=True,
    )
    try:
        stdout, stderr = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired as exc:
        terminate_process_group(process)
        try:
            stdout, stderr = process.communicate(timeout=TOOL_TERMINATE_GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            stdout, stderr = "", ""
        raise Mp4ExtractError(
            f"tool timed out after {timeout_seconds:g}s: {' '.join(command)}"
        ) from exc
    if process_group_exists(process.pid):
        terminate_process_group(process)
        raise Mp4ExtractError(f"tool left descendant processes: {' '.join(command)}")
    if process.returncode != 0:
        detail = stderr.strip() or stdout.strip()
        raise Mp4ExtractError(
            f"tool failed rc={process.returncode}: {' '.join(command)}"
            + (f": {detail}" if detail else "")
        )
    return stdout


def read_json_file(path: Path, label: str) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise Mp4ExtractError(f"cannot read {label}: {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise Mp4ExtractError(f"{label} must be a JSON object: {path}")
    return payload


def require_session_status(session: Path) -> dict[str, Any]:
    path = session / "session_status.json"
    if not path.is_file():
        raise Mp4ExtractError("missing session_status.json")
    status = read_json_file(path, "session status")
    if status.get("schema") != SESSION_SCHEMA:
        raise Mp4ExtractError("session_status.json has an unsupported schema")
    outcome = status.get("outcome")
    if outcome not in {
        "published_complete", "published_partial", "aborted", "cleanup_incomplete"
    }:
        raise Mp4ExtractError("session_status.json has an unsupported outcome")
    publication_id = status.get("publication_id")
    if (
        not isinstance(publication_id, str)
        or re.fullmatch(r"[0-9a-f]{32}", publication_id) is None
    ):
        raise Mp4ExtractError("session_status.json missing valid publication_id")
    if not isinstance(status.get("data_complete"), bool):
        raise Mp4ExtractError("session_status.json missing boolean data_complete")
    outcome_is_complete = outcome == "published_complete"
    if status["data_complete"] is not outcome_is_complete:
        raise Mp4ExtractError(
            "session_status.json has inconsistent outcome/data_complete"
        )
    return status


def read_matching_receipt(session: Path, status: dict[str, Any]) -> dict[str, Any] | None:
    path = session / "publication_receipt.json"
    if not path.is_file():
        return None
    receipt = read_json_file(path, "publication receipt")
    expected = {
        "schema": RECEIPT_SCHEMA,
        "session_schema": status["schema"],
        "publication_id": status["publication_id"],
        "outcome": status["outcome"],
        "data_complete": status["data_complete"],
        "session_status": "session_status.json",
    }
    mismatched = [
        key for key, value in expected.items() if receipt.get(key) != value
    ]
    if mismatched:
        raise Mp4ExtractError(
            "publication receipt does not match session_status.json: "
            + ",".join(mismatched)
        )
    return receipt


def read_timestamp_index(path: Path, camera_id: int) -> list[dict[str, int]]:
    try:
        handle = path.open("r", encoding="utf-8", newline="")
    except OSError as exc:
        raise Mp4ExtractError(f"cannot open {path}: {exc}") from exc
    with handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None or tuple(reader.fieldnames) != TIMESTAMP_COLUMNS:
            raise Mp4ExtractError(
                f"camera{camera_id} timestamp header must be {','.join(TIMESTAMP_COLUMNS)}"
            )
        rows: list[dict[str, int]] = []
        last_timestamp = -1
        for line_number, raw in enumerate(reader, start=2):
            try:
                row = {name: int(raw[name]) for name in TIMESTAMP_COLUMNS}
            except (KeyError, TypeError, ValueError) as exc:
                raise Mp4ExtractError(
                    f"camera{camera_id} invalid timestamp row at line {line_number}"
                ) from exc
            if row["frame_index"] != len(rows):
                raise Mp4ExtractError(
                    f"camera{camera_id} frame_index must be contiguous from zero"
                )
            if row["timestamp_ns"] <= last_timestamp:
                raise Mp4ExtractError(
                    f"camera{camera_id} timestamps must be strictly increasing"
                )
            if row["key_frame"] not in (0, 1) or row["encoded_bytes"] <= 0:
                raise Mp4ExtractError(
                    f"camera{camera_id} invalid key_frame/encoded_bytes at line {line_number}"
                )
            last_timestamp = row["timestamp_ns"]
            rows.append(row)
    if not rows:
        raise Mp4ExtractError(f"camera{camera_id} timestamp index is empty")
    return rows


def probe_video(
    ffprobe: str,
    path: Path,
    camera_id: int,
    expected_start_timestamp_ns: int,
    expected_timestamp_index: str,
    tool_timeout_seconds: float,
) -> dict[str, Any]:
    output = run_tool([
        ffprobe,
        "-v", "error",
        "-select_streams", "v:0",
        "-show_entries", (
            "stream=codec_name,width,height:"
            "format_tags=start_abs_timestamp_ns,timestamp_index,frame_rate"
        ),
        "-of", "json",
        str(path),
    ], tool_timeout_seconds)
    try:
        document = json.loads(output)
        streams = document["streams"]
        stream = streams[0]
        tags = document["format"]["tags"]
        codec = stream["codec_name"]
        width = int(stream["width"])
        height = int(stream["height"])
        start_timestamp_ns = int(tags["start_abs_timestamp_ns"])
        timestamp_index = str(tags["timestamp_index"])
        frame_rate = int(tags["frame_rate"])
    except (KeyError, IndexError, TypeError, ValueError, json.JSONDecodeError) as exc:
        raise Mp4ExtractError(
            f"camera{camera_id} MP4 timestamp metadata is incomplete"
        ) from exc
    if codec != "h264" or width <= 0 or height <= 0:
        raise Mp4ExtractError(
            f"camera{camera_id} unsupported video codec={codec} size={width}x{height}"
        )
    if (
        start_timestamp_ns != expected_start_timestamp_ns
        or timestamp_index != expected_timestamp_index
        or frame_rate <= 0
    ):
        raise Mp4ExtractError(
            f"camera{camera_id} MP4 timestamp metadata does not match "
            f"{expected_timestamp_index}"
        )
    return {
        "codec": codec,
        "width": width,
        "height": height,
        "start_abs_timestamp_ns": start_timestamp_ns,
        "timestamp_index": timestamp_index,
        "frame_rate": frame_rate,
    }


def decode_camera(
    ffmpeg: str,
    video: Path,
    rows: list[dict[str, int]],
    camera_id: int,
    output_dir: Path,
    tool_timeout_seconds: float,
) -> None:
    output_dir.mkdir(mode=0o755)
    pattern = output_dir / ".frame-%012d.jpg"
    run_tool([
        ffmpeg,
        "-v", "error",
        "-i", str(video),
        "-map", "0:v:0",
        "-vsync", "0",
        "-q:v", "2",
        str(pattern),
    ], tool_timeout_seconds)
    used_names: set[str] = set()
    for row in rows:
        temporary = output_dir / f".frame-{row['frame_index'] + 1:012d}.jpg"
        if not temporary.is_file():
            raise Mp4ExtractError(
                f"camera{camera_id} frame/timestamp count mismatch: missing decoded frame "
                f"{row['frame_index']}"
            )
        timestamp = row["timestamp_ns"]
        name = f"{timestamp}.jpg"
        if name in used_names:
            name = f"{timestamp}_{row['frame_index']}.jpg"
        if name in used_names:
            raise Mp4ExtractError(f"camera{camera_id} duplicate output image name: {name}")
        temporary.replace(output_dir / name)
        used_names.add(name)
    extra = output_dir / f".frame-{len(rows) + 1:012d}.jpg"
    if extra.exists():
        raise Mp4ExtractError(
            f"camera{camera_id} frame/timestamp count mismatch: extra decoded frames"
        )


def extract_session(
    session: Path, output: Path, camera_ids: list[int],
    tool_timeout_seconds: float = DEFAULT_TOOL_TIMEOUT_SECONDS,
) -> dict[str, Any]:
    session = session.resolve()
    output = output.resolve()
    if not session.is_dir():
        raise Mp4ExtractError(f"input session is not a directory: {session}")
    if output.exists():
        raise Mp4ExtractError(f"output directory already exists: {output}")
    for name in ("imu.csv", "camera_params.yaml"):
        if not (session / name).is_file():
            raise Mp4ExtractError(f"missing session file: {name}")
    status = require_session_status(session)
    receipt = read_matching_receipt(session, status)
    source_is_complete = (
        status["outcome"] == "published_complete" and status["data_complete"] is True
    )
    recovery_only = session.name.endswith(".partial")
    publication_incomplete = os.path.lexists(session / ".publication_incomplete")
    if source_is_complete and receipt is None and not recovery_only:
        raise Mp4ExtractError(
            "published_complete source requires a matching publication receipt"
        )
    full_camera_ids = [0, 1, 2, 3]
    expected_mp4_files = [f"camera{camera_id}.mp4" for camera_id in full_camera_ids]
    expected_timestamp_indexes = [
        f"camera{camera_id}_timestamps.csv" for camera_id in full_camera_ids
    ]
    inventory_complete = (
        camera_ids == full_camera_ids
        and status.get("camera_mask") == 0x0F
        and receipt is not None
        and receipt.get("mp4_files") == expected_mp4_files
        and receipt.get("timestamp_indexes") == expected_timestamp_indexes
    )
    if (source_is_complete and receipt is not None and not recovery_only
            and not publication_incomplete and not inventory_complete):
        raise Mp4ExtractError(
            "published_complete source requires exact four-camera publication inventory"
        )
    # `.partial` and incomplete-marker sessions are recovery-only and can never be promoted.
    source_data_complete = (
        source_is_complete and receipt is not None and not recovery_only
        and not publication_incomplete and inventory_complete
    )

    ffmpeg = require_tool("ffmpeg")
    ffprobe = require_tool("ffprobe")
    timestamp_rows: dict[int, list[dict[str, int]]] = {}
    probes: dict[int, dict[str, Any]] = {}
    for camera_id in camera_ids:
        video = session / f"camera{camera_id}.mp4"
        index = session / f"camera{camera_id}_timestamps.csv"
        if not video.is_file() or not index.is_file():
            raise Mp4ExtractError(f"missing camera{camera_id} MP4 or timestamp index")
        rows = read_timestamp_index(index, camera_id)
        probe = probe_video(
            ffprobe, video, camera_id, rows[0]["timestamp_ns"], index.name,
            tool_timeout_seconds,
        )
        timestamp_rows[camera_id] = rows
        probes[camera_id] = probe

    output.parent.mkdir(parents=True, exist_ok=True)
    staging = output.parent / f".{output.name}.tmp-{os.getpid()}"
    if staging.exists():
        raise Mp4ExtractError(f"staging directory already exists: {staging}")
    try:
        staging.mkdir(mode=0o755)
        for camera_id in camera_ids:
            decode_camera(
                ffmpeg,
                session / f"camera{camera_id}.mp4",
                timestamp_rows[camera_id],
                camera_id,
                staging / f"camera{camera_id}",
                tool_timeout_seconds,
            )
            probes[camera_id]["decoded_frames"] = len(timestamp_rows[camera_id])
            shutil.copy2(
                session / f"camera{camera_id}_timestamps.csv",
                staging / f"camera{camera_id}_timestamps.csv",
            )
        shutil.copy2(session / "imu.csv", staging / "imu.csv")
        shutil.copy2(session / "camera_params.yaml", staging / "camera_params.yaml")
        shutil.copy2(session / "session_status.json", staging / "session_status.json")
        if receipt is not None:
            shutil.copy2(
                session / "publication_receipt.json",
                staging / "publication_receipt.json",
            )
        images_by_camera = {
            f"camera{camera_id}": len(timestamp_rows[camera_id]) for camera_id in camera_ids
        }
        first_by_camera = {
            f"camera{camera_id}": timestamp_rows[camera_id][0]["timestamp_ns"]
            for camera_id in camera_ids
        }
        last_by_camera = {
            f"camera{camera_id}": timestamp_rows[camera_id][-1]["timestamp_ns"]
            for camera_id in camera_ids
        }
        summary = {
            "schema": SCHEMA,
            "source_session": str(session),
            "source_outcome": status["outcome"],
            "source_publication_id": status["publication_id"],
            "source_data_complete": source_data_complete,
            "source_publication_incomplete_marker": publication_incomplete,
            "source_inventory_complete": inventory_complete,
            "output_directory": str(output),
            "cameras": camera_ids,
            "images_by_camera": images_by_camera,
            "total_images": sum(images_by_camera.values()),
            "timestamp_unit": "nanoseconds",
            "first_timestamp_ns": min(first_by_camera.values()),
            "last_timestamp_ns": max(last_by_camera.values()),
            "first_timestamp_by_camera": first_by_camera,
            "last_timestamp_by_camera": last_by_camera,
            "video_streams": {
                f"camera{camera_id}": probes[camera_id] for camera_id in camera_ids
            },
            "files": {
                "imu": "imu.csv",
                "camera_parameters": "camera_params.yaml",
                "session_status": "session_status.json",
                "publication_receipt": (
                    "publication_receipt.json" if receipt is not None else None
                ),
                "timestamp_indexes": {
                    f"camera{camera_id}": f"camera{camera_id}_timestamps.csv"
                    for camera_id in camera_ids
                },
            },
        }
        (staging / "conversion_summary.json").write_text(
            json.dumps(summary, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        rename_no_replace(staging, output)
        return summary
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Decode four H.264 MP4 files into JPEGs named by the exact nanosecond "
            "timestamps stored in each camera sidecar."
        )
    )
    parser.add_argument("session_dir", type=Path, help="MP4 recording session directory")
    parser.add_argument("output_dir", type=Path, help="new output dataset directory")
    parser.add_argument(
        "--tool-timeout-seconds",
        type=positive_timeout_seconds,
        default=DEFAULT_TOOL_TIMEOUT_SECONDS,
        help="per ffmpeg/ffprobe process timeout, default 1800 seconds",
    )
    parser.add_argument(
        "--expected-cameras",
        type=parse_camera_ids,
        default=[0, 1, 2, 3],
        metavar="IDS",
        help="comma-separated camera IDs, default 0,1,2,3",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        summary = extract_session(
            args.session_dir, args.output_dir, args.expected_cameras,
            args.tool_timeout_seconds,
        )
    except Mp4ExtractError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(
        f"mp4 extraction complete: {summary['total_images']} images -> "
        f"{summary['output_directory']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
