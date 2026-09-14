#!/usr/bin/env python3
"""Convert a sensor_demo tee capture session into viewable JPEG images.

Input contract (produced by sensor_demo --capture-dir):
  session.json                        capture session metadata
  camN/frames.jsonl                   per-frame NV12/AU index
  camN/raw/NNNNN.nv12                 compact NV12 planes (Y then interleaved UV)
  camN/au/NNNNN.h264                  encoded access units (Annex-B)
  camN/prefix.h264                    decode prefix starting at a key frame

Output:
  camN/raw/NNNNN.jpg                  raw NV12 frames converted losslessly into JPEG
  camN/encoded/NNNNN.jpg              encoded access units decoded into JPEG
  conversion_summary.json             conversion accounting and warnings
"""

from __future__ import annotations

import argparse
import ctypes
import errno
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
from typing import Any


SCHEMA = "robobaton_tee_extract_v1"
SESSION_SCHEMA = "robobaton_tee_capture_session_v1"
DEFAULT_TOOL_TIMEOUT_SECONDS = 1800.0
TOOL_TERMINATE_GRACE_SECONDS = 2.0


class TeeExtractError(RuntimeError):
    """A deterministic input, tool, or output contract failure."""


def rename_no_replace(source: Path, destination: Path) -> None:
    """Atomically promote a directory without replacing a concurrent destination."""
    libc = ctypes.CDLL(None, use_errno=True)
    renameat2 = getattr(libc, "renameat2", None)
    if renameat2 is None:
        raise TeeExtractError("renameat2(RENAME_NOREPLACE) is unavailable")
    renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
                          ctypes.c_char_p, ctypes.c_uint]
    renameat2.restype = ctypes.c_int
    result = renameat2(-100, os.fsencode(source), -100, os.fsencode(destination), 1)
    if result != 0:
        error_number = ctypes.get_errno()
        if error_number == errno.EEXIST:
            raise TeeExtractError(f"output already exists: {destination}")
        raise TeeExtractError(
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
        raise TeeExtractError(f"required tool not found: {name}")
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
        raise TeeExtractError(f"tool timed out: {' '.join(command)}") from exc
    if process.returncode != 0:
        detail = stderr.strip().splitlines()
        raise TeeExtractError(
            f"tool failed (exit {process.returncode}): {' '.join(command)}"
            + (f": {detail[-1]}" if detail else "")
        )
    return stdout


def read_session(capture_dir: Path) -> dict[str, Any]:
    path = capture_dir / "session.json"
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise TeeExtractError(f"cannot read {path}: {exc}") from exc
    if document.get("schema") != SESSION_SCHEMA:
        raise TeeExtractError(f"unsupported capture session schema: {path}")
    for key in ("camera_mask", "capture_frame_count", "fps", "rotate_degrees", "codec"):
        if key not in document:
            raise TeeExtractError(f"capture session metadata missing {key}: {path}")
    if str(document["codec"]) not in ("h264", "h265"):
        raise TeeExtractError(f"unsupported capture codec: {path}")
    return document


def read_frames_index(path: Path, camera_id: int) -> list[dict[str, Any]]:
    """Read camN/frames.jsonl and validate contiguous frame indices."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise TeeExtractError(f"cannot open {path}: {exc}") from exc
    rows: list[dict[str, Any]] = []
    for line_number, line in enumerate(lines, start=1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise TeeExtractError(
                f"camera{camera_id} invalid frames.jsonl line {line_number}"
            ) from exc
        if row.get("index") != len(rows):
            raise TeeExtractError(
                f"camera{camera_id} frame index must be contiguous from zero"
            )
        for key in ("raw_file", "width", "height", "stride", "vstride",
                    "group_timestamp_ns"):
            if key not in row:
                raise TeeExtractError(
                    f"camera{camera_id} frames.jsonl line {line_number} missing {key}"
                )
        width = int(row["width"])
        height = int(row["height"])
        if width <= 0 or height <= 0 or (width & 1) != 0 or (height & 1) != 0:
            raise TeeExtractError(
                f"camera{camera_id} invalid frame size {width}x{height}"
            )
        rows.append(row)
    if not rows:
        raise TeeExtractError(f"camera{camera_id} frames.jsonl is empty")
    return rows


def convert_raw_frames(
    ffmpeg: str,
    camera_id: int,
    rows: list[dict[str, Any]],
    camera_dir: Path,
    output_dir: Path,
    tool_timeout_seconds: float,
) -> int:
    """Concatenate compact NV12 frames and convert the batch into JPEG files."""
    width = int(rows[0]["width"])
    height = int(rows[0]["height"])
    expected_bytes = width * height * 3 // 2
    merged = output_dir / "merged_raw.nv12"
    with merged.open("wb") as handle:
        for row in rows:
            if int(row["width"]) != width or int(row["height"]) != height:
                raise TeeExtractError(
                    f"camera{camera_id} raw frames must share one size"
                )
            source = camera_dir / row["raw_file"]
            if not source.is_file():
                raise TeeExtractError(f"missing raw frame file: {source}")
            if source.stat().st_size != expected_bytes:
                raise TeeExtractError(
                    f"raw frame size mismatch for {source}: expected "
                    f"{expected_bytes} bytes of compact NV12"
                )
            with source.open("rb") as frame:
                shutil.copyfileobj(frame, handle)
    images_dir = output_dir / "raw"
    images_dir.mkdir(parents=True, exist_ok=True)
    run_tool([
        ffmpeg, "-v", "error",
        "-f", "rawvideo", "-pix_fmt", "nv12",
        "-video_size", f"{width}x{height}",
        "-i", str(merged),
        "-frames:v", str(len(rows)),
        "-start_number", "0",
        "-q:v", "2",
        str(images_dir / "raw_%05d.jpg"),
    ], tool_timeout_seconds)
    merged.unlink()
    return len(rows)

def convert_encoded_frames(
    ffmpeg: str,
    codec: str,
    camera_id: int,
    rows: list[dict[str, Any]],
    camera_dir: Path,
    output_dir: Path,
    tool_timeout_seconds: float,
) -> dict[str, Any]:
    """Decode access units with full reference cascade, mapped by raw index.

    A P-frame AU can only decode when every reference frame is in the stream, so
    the stream for index i is prefix + every AU up to i (missing AUs leave a
    reference gap that the decoder reports by dropping the frame). The decoder
    frame count is probed per index and only an exact +1 increment promotes the
    last decoded frame as encoded_NNNNN.jpg with N = the raw index. A dropped
    frame is counted as skipped: it never shifts the index mapping.
    """
    demuxer = "hevc" if codec == "h265" else "h264"
    prefix = camera_dir / f"prefix.{codec}"
    has_prefix = prefix.is_file() and prefix.stat().st_size > 0
    images_dir = output_dir / "encoded"
    images_dir.mkdir(parents=True, exist_ok=True)
    workdir = output_dir / ".encoded_work"
    workdir.mkdir(parents=True, exist_ok=True)

    def decode_frame_count(merged: Path) -> int:
        progress = run_tool([
            ffmpeg, "-v", "error", "-f", demuxer, "-i", str(merged),
            "-f", "null", "-", "-progress", "pipe:1", "-nostats",
        ], tool_timeout_seconds)
        frames = 0
        for line in progress.splitlines():
            if line.startswith("frame="):
                try:
                    frames = int(line.split("=", 1)[1])
                except ValueError:
                    pass
        return frames

    # 前缀帧数是级联计数的起点：之后每个可解码 AU 使解码器输出恰好多一帧。
    last_count = 0
    if has_prefix:
        probe = workdir / "prefix_probe.bin"
        with probe.open("wb") as handle:
            with prefix.open("rb") as source:
                shutil.copyfileobj(source, handle)
        last_count = decode_frame_count(probe)
        probe.unlink()

    cascade = bytearray()
    if has_prefix:
        cascade.extend(prefix.read_bytes())

    missing = 0
    skipped = 0
    decoded = 0
    reference_gap = False
    for row in rows:
        if row.get("key_frame", False):
            # IDR 自含参考，重置断裂标志。
            reference_gap = False
        au_file = row.get("au_file", "")
        if not au_file:
            missing += 1
            # 该序号起的参考链断裂，持续到下一个 key frame。
            reference_gap = True
            continue
        if reference_gap:
            # 参考链经过缺失 AU：解码器只能错误隐藏出花屏帧，
            # 不输出、不计数，避免花屏被误认为编码损失对照结果。
            skipped += 1
            continue
        source = camera_dir / au_file
        if not source.is_file():
            raise TeeExtractError(f"missing access unit file: {source}")
        index = int(row["index"])
        cascade.extend(source.read_bytes())
        merged = workdir / f"merged_{index:05d}.{codec}"
        with merged.open("wb") as handle:
            handle.write(cascade)
        frames = decode_frame_count(merged)
        destination = images_dir / f"encoded_{index:05d}.jpg"
        if frames == last_count + 1:
            # AU 解码输出恰好新增一帧：取该帧（帧号与 index 对齐）。
            run_tool([
                ffmpeg, "-v", "error", "-f", demuxer, "-i", str(merged),
                "-vf", f"trim=start_frame={frames - 1}:end_frame={frames}",
                "-frames:v", "1", "-q:v", "2", str(destination),
            ], tool_timeout_seconds)
            decoded += 1
            last_count = frames
        else:
            # 解码器异常丢弃：不输出、不计数，保持后续帧映射不被错位污染。
            skipped += 1
        merged.unlink()
    shutil.rmtree(workdir, ignore_errors=True)
    return {
        "access_units": len(rows),
        "missing_access_units": missing,
        "decoded_images": decoded,
        "skipped_access_units": skipped,
    }


def extract_capture(
    capture_dir: Path,
    output_dir: Path,
    camera_ids: list[int],
    tool_timeout_seconds: float,
) -> dict[str, Any]:
    ffmpeg = require_tool("ffmpeg")
    session = read_session(capture_dir)
    camera_mask = int(session["camera_mask"])
    for camera_id in camera_ids:
        if not (camera_mask & (1 << camera_id)):
            raise TeeExtractError(f"camera{camera_id} not enabled in capture session")

    rows_by_camera = {
        camera_id: read_frames_index(
            capture_dir / f"cam{camera_id}" / "frames.jsonl", camera_id
        )
        for camera_id in camera_ids
    }

    output_dir.parent.mkdir(parents=True, exist_ok=True)
    staging = output_dir.parent / f".{output_dir.name}.tmp-{os.getpid()}"
    if staging.exists():
        raise TeeExtractError(f"staging directory already exists: {staging}")
    try:
        staging.mkdir(mode=0o755)
        raw_counts: dict[str, int] = {}
        encoded_results: dict[str, dict[str, int]] = {}
        for camera_id in camera_ids:
            camera_staging = staging / f"cam{camera_id}"
            camera_staging.mkdir(parents=True, exist_ok=True)
            raw_counts[f"camera{camera_id}"] = convert_raw_frames(
                ffmpeg, camera_id, rows_by_camera[camera_id],
                capture_dir / f"cam{camera_id}", camera_staging,
                tool_timeout_seconds,
            )
            encoded_results[f"camera{camera_id}"] = convert_encoded_frames(
                ffmpeg, str(session["codec"]), camera_id, rows_by_camera[camera_id],
                capture_dir / f"cam{camera_id}", camera_staging,
                tool_timeout_seconds,
            )
        summary = {
            "schema": SCHEMA,
            "source_capture": str(capture_dir.resolve()),
            "source_schema": SESSION_SCHEMA,
            "source_session": session,
            "output_directory": str(output_dir),
            "cameras": camera_ids,
            "raw_images_by_camera": raw_counts,
            "encoded_images_by_camera": {
                camera: encoded_results[camera]["decoded_images"]
                for camera in encoded_results
            },
            "encoded_access_units_by_camera": {
                camera: encoded_results[camera]["access_units"]
                for camera in encoded_results
            },
            "encoded_missing_access_units_by_camera": {
                camera: encoded_results[camera]["missing_access_units"]
                for camera in encoded_results
            },
            "encoded_skipped_access_units_by_camera": {
                camera: encoded_results[camera]["skipped_access_units"]
                for camera in encoded_results
            },
            "total_images": sum(raw_counts.values())
            + sum(item["decoded_images"] for item in encoded_results.values()),
        }
        (staging / "conversion_summary.json").write_text(
            json.dumps(summary, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        rename_no_replace(staging, output_dir)
        return summary
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Convert a sensor_demo tee capture dataset (compact NV12 planes plus "
            "encoded access units) into per-frame JPEG images."
        )
    )
    parser.add_argument("capture_dir", type=Path, help="tee capture session directory")
    parser.add_argument("output_dir", type=Path, help="new output dataset directory")
    parser.add_argument(
        "--tool-timeout-seconds",
        type=positive_timeout_seconds,
        default=DEFAULT_TOOL_TIMEOUT_SECONDS,
        help="per ffmpeg process timeout, default 1800 seconds",
    )
    parser.add_argument(
        "--cameras",
        type=parse_camera_ids,
        default=[0, 1, 2, 3],
        metavar="IDS",
        help="comma-separated camera IDs, default 0,1,2,3",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        summary = extract_capture(
            args.capture_dir, args.output_dir, args.cameras,
            args.tool_timeout_seconds,
        )
    except TeeExtractError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(
        f"tee extraction complete: {summary['total_images']} images -> "
        f"{summary['output_directory']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
