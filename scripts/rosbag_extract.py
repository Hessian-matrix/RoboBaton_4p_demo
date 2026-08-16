#!/usr/bin/env python3
"""Extract RoboBaton ROS1 bag images, IMU samples, and camera parameters."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
from typing import Any

from rosbag_info import BagInfoError, UnindexedBagError, iter_bag_messages

NSEC_PER_SEC = 1_000_000_000
IMAGE_TOPIC = re.compile(r"^/camera(?P<camera>[0-3])/image/compressed$")
CAMERA_INFO_TOPIC = re.compile(r"^/camera(?P<camera>[0-3])/camera_info$")
IMU_TOPIC = "/imu/data"
IMAGE_TYPE = "sensor_msgs/CompressedImage"
CAMERA_INFO_TYPE = "sensor_msgs/CameraInfo"
IMU_TYPE = "sensor_msgs/Imu"


class ConversionError(Exception):
    """The requested dataset cannot be produced safely."""


@dataclass(frozen=True)
class Header:
    sequence: int
    timestamp_ns: int
    frame_id: str


class PayloadReader:
    """Bounds-checked little-endian ROS1 message payload reader."""

    def __init__(self, payload: bytes, context: str) -> None:
        self.payload = payload
        self.context = context
        self.offset = 0

    def read(self, size: int, label: str) -> bytes:
        if size < 0 or self.offset + size > len(self.payload):
            raise ConversionError(
                f"malformed {self.context}: {label} overruns payload at offset {self.offset}"
            )
        data = self.payload[self.offset : self.offset + size]
        self.offset += size
        return data

    def u8(self, label: str) -> int:
        return self.read(1, label)[0]

    def u32(self, label: str) -> int:
        return struct.unpack("<I", self.read(4, label))[0]

    def f64(self, label: str) -> float:
        return struct.unpack("<d", self.read(8, label))[0]

    def string(self, label: str) -> str:
        size = self.u32(label + " length")
        try:
            return self.read(size, label).decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ConversionError(f"malformed {self.context}: {label} is not UTF-8") from exc

    def f64_array(self, count: int, label: str) -> list[float]:
        return [self.f64(f"{label}[{index}]") for index in range(count)]

    def variable_f64_array(self, label: str) -> list[float]:
        return self.f64_array(self.u32(label + " length"), label)

    def header(self) -> Header:
        sequence = self.u32("header sequence")
        sec = self.u32("header stamp seconds")
        nsec = self.u32("header stamp nanoseconds")
        if nsec >= NSEC_PER_SEC:
            raise ConversionError(
                f"malformed {self.context}: header stamp nanoseconds {nsec} is invalid"
            )
        return Header(
            sequence=sequence,
            timestamp_ns=sec * NSEC_PER_SEC + nsec,
            frame_id=self.string("header frame_id"),
        )

    def finish(self) -> None:
        if self.offset != len(self.payload):
            raise ConversionError(
                f"malformed {self.context}: {len(self.payload) - self.offset} trailing bytes"
            )


def decode_compressed_image(payload: bytes, topic: str) -> tuple[Header, str, bytes]:
    reader = PayloadReader(payload, topic)
    header = reader.header()
    image_format = reader.string("format")
    image = reader.read(reader.u32("image data length"), "image data")
    reader.finish()
    if image_format.lower() not in {"jpeg", "jpg"}:
        raise ConversionError(f"unsupported image format on {topic}: {image_format}")
    if len(image) < 4 or not image.startswith(b"\xff\xd8") or not image.endswith(b"\xff\xd9"):
        raise ConversionError(f"malformed JPEG payload on {topic} at {header.timestamp_ns}")
    return header, image_format, image


def decode_camera_info(payload: bytes, topic: str) -> dict[str, Any]:
    reader = PayloadReader(payload, topic)
    header = reader.header()
    height = reader.u32("height")
    width = reader.u32("width")
    distortion_model = reader.string("distortion_model")
    distortion = reader.variable_f64_array("D")
    intrinsic = reader.f64_array(9, "K")
    rectification = reader.f64_array(9, "R")
    projection = reader.f64_array(12, "P")
    binning_x = reader.u32("binning_x")
    binning_y = reader.u32("binning_y")
    roi = {
        "x_offset": reader.u32("roi.x_offset"),
        "y_offset": reader.u32("roi.y_offset"),
        "height": reader.u32("roi.height"),
        "width": reader.u32("roi.width"),
        "do_rectify": bool(reader.u8("roi.do_rectify")),
    }
    reader.finish()
    return {
        "timestamp_ns": header.timestamp_ns,
        "sequence": header.sequence,
        "frame_id": header.frame_id,
        "width": width,
        "height": height,
        "distortion_model": distortion_model,
        "D": distortion,
        "K": intrinsic,
        "R": rectification,
        "P": projection,
        "binning_x": binning_x,
        "binning_y": binning_y,
        "roi": roi,
    }


def decode_imu(payload: bytes, topic: str) -> dict[str, Any]:
    reader = PayloadReader(payload, topic)
    header = reader.header()
    orientation = reader.f64_array(4, "orientation")
    orientation_covariance = reader.f64_array(9, "orientation_covariance")
    angular_velocity = reader.f64_array(3, "angular_velocity")
    angular_velocity_covariance = reader.f64_array(9, "angular_velocity_covariance")
    linear_acceleration = reader.f64_array(3, "linear_acceleration")
    linear_acceleration_covariance = reader.f64_array(9, "linear_acceleration_covariance")
    reader.finish()
    return {
        "timestamp_ns": header.timestamp_ns,
        "sequence": header.sequence,
        "frame_id": header.frame_id,
        "orientation": orientation,
        "orientation_covariance": orientation_covariance,
        "angular_velocity": angular_velocity,
        "angular_velocity_covariance": angular_velocity_covariance,
        "linear_acceleration": linear_acceleration,
        "linear_acceleration_covariance": linear_acceleration_covariance,
    }


def imu_csv_fields() -> list[str]:
    fields = ["timestamp_ns", "sequence", "frame_id"]
    fields.extend(f"orientation_{axis}" for axis in "xyzw")
    fields.extend(f"orientation_covariance_{index}" for index in range(9))
    fields.extend(f"angular_velocity_{axis}" for axis in "xyz")
    fields.extend(f"angular_velocity_covariance_{index}" for index in range(9))
    fields.extend(f"linear_acceleration_{axis}" for axis in "xyz")
    fields.extend(f"linear_acceleration_covariance_{index}" for index in range(9))
    return fields


def format_float(value: float) -> str:
    return f"{value:.15f}"


def imu_csv_row(sample: dict[str, Any]) -> list[str | int]:
    row: list[str | int] = [sample["timestamp_ns"], sample["sequence"], sample["frame_id"]]
    for field in (
        "orientation",
        "orientation_covariance",
        "angular_velocity",
        "angular_velocity_covariance",
        "linear_acceleration",
        "linear_acceleration_covariance",
    ):
        row.extend(format_float(value) for value in sample[field])
    return row


def yaml_string(value: str) -> str:
    if re.fullmatch(r"[A-Za-z0-9_./-]+", value):
        return value
    return json.dumps(value, ensure_ascii=False)


def yaml_float_list(values: list[float]) -> str:
    return "[" + ", ".join(format_float(value) for value in values) + "]"


def render_camera_params(camera_params: dict[int, dict[str, Any]]) -> str:
    lines = ["schema: robobaton_camera_parameters_v1", "cameras:"]
    for camera_id in sorted(camera_params):
        params = camera_params[camera_id]
        lines.extend(
            [
                f"  camera{camera_id}:",
                f"    topic: {yaml_string(f'/camera{camera_id}/camera_info')}",
                f"    timestamp_ns: {params['timestamp_ns']}",
                f"    sequence: {params['sequence']}",
                f"    frame_id: {yaml_string(params['frame_id'])}",
                f"    width: {params['width']}",
                f"    height: {params['height']}",
                f"    distortion_model: {yaml_string(params['distortion_model'])}",
                f"    D: {yaml_float_list(params['D'])}",
                f"    K: {yaml_float_list(params['K'])}",
                f"    R: {yaml_float_list(params['R'])}",
                f"    P: {yaml_float_list(params['P'])}",
                f"    binning_x: {params['binning_x']}",
                f"    binning_y: {params['binning_y']}",
                "    roi:",
                f"      x_offset: {params['roi']['x_offset']}",
                f"      y_offset: {params['roi']['y_offset']}",
                f"      height: {params['roi']['height']}",
                f"      width: {params['roi']['width']}",
                f"      do_rectify: {'true' if params['roi']['do_rectify'] else 'false'}",
            ]
        )
    return "\n".join(lines) + "\n"


def parse_expected_cameras(value: str) -> list[int]:
    try:
        cameras = [int(item) for item in value.split(",") if item != ""]
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected cameras must be comma-separated integers") from exc
    if not cameras or len(cameras) != len(set(cameras)) or any(camera not in range(4) for camera in cameras):
        raise argparse.ArgumentTypeError("expected cameras must be a unique subset of 0,1,2,3")
    return sorted(cameras)


def write_image(
    root: Path, camera_id: int, header: Header, image: bytes,
    used_names: dict[int, set[str]],
) -> tuple[str, bool]:
    camera_dir = root / f"camera{camera_id}"
    camera_dir.mkdir(exist_ok=True)
    name = f"{header.timestamp_ns}.jpg"
    duplicate_timestamp = name in used_names[camera_id]
    if duplicate_timestamp:
        name = f"{header.timestamp_ns}_{header.sequence}.jpg"
        if name in used_names[camera_id]:
            raise ConversionError(
                f"duplicate image timestamp and sequence for camera{camera_id}: "
                f"{header.timestamp_ns}/{header.sequence}"
            )
    used_names[camera_id].add(name)
    (camera_dir / name).write_bytes(image)
    return name, duplicate_timestamp


def extract_bag(input_bag: Path, output_dir: Path, expected_cameras: list[int]) -> dict[str, Any]:
    input_bag = input_bag.resolve()
    output_dir = output_dir.resolve()
    if output_dir.exists():
        raise ConversionError(f"output directory already exists: {output_dir}")
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    staging = output_dir.parent / f".{output_dir.name}.tmp-{os.getpid()}"
    if staging.exists():
        raise ConversionError(f"staging directory already exists: {staging}")
    staging.mkdir()

    image_counts = {camera_id: 0 for camera_id in expected_cameras}
    used_names = {camera_id: set() for camera_id in expected_cameras}
    camera_params: dict[int, dict[str, Any]] = {}
    imu_count = 0
    ignored_count = 0
    duplicate_timestamps = 0
    try:
        for camera_id in expected_cameras:
            (staging / f"camera{camera_id}").mkdir()
        with (staging / "imu.csv").open("w", newline="", encoding="utf-8") as imu_stream:
            imu_writer = csv.writer(imu_stream)
            imu_writer.writerow(imu_csv_fields())
            for message in iter_bag_messages(input_bag):
                topic = message.connection.topic
                image_match = IMAGE_TOPIC.fullmatch(topic)
                camera_info_match = CAMERA_INFO_TOPIC.fullmatch(topic)
                if image_match:
                    camera_id = int(image_match.group("camera"))
                    if camera_id not in image_counts:
                        ignored_count += 1
                        continue
                    if message.connection.datatype != IMAGE_TYPE:
                        raise ConversionError(
                            f"topic {topic} has type {message.connection.datatype}, expected {IMAGE_TYPE}"
                        )
                    header, _image_format, image = decode_compressed_image(message.payload, topic)
                    _name, duplicate = write_image(staging, camera_id, header, image, used_names)
                    image_counts[camera_id] += 1
                    duplicate_timestamps += int(duplicate)
                elif camera_info_match:
                    camera_id = int(camera_info_match.group("camera"))
                    if camera_id not in image_counts:
                        ignored_count += 1
                        continue
                    if message.connection.datatype != CAMERA_INFO_TYPE:
                        raise ConversionError(
                            f"topic {topic} has type {message.connection.datatype}, expected {CAMERA_INFO_TYPE}"
                        )
                    params = decode_camera_info(message.payload, topic)
                    if camera_id in camera_params and camera_params[camera_id] != params:
                        raise ConversionError(f"camera{camera_id} has conflicting CameraInfo messages")
                    camera_params[camera_id] = params
                elif topic == IMU_TOPIC:
                    if message.connection.datatype != IMU_TYPE:
                        raise ConversionError(
                            f"topic {topic} has type {message.connection.datatype}, expected {IMU_TYPE}"
                        )
                    imu_writer.writerow(imu_csv_row(decode_imu(message.payload, topic)))
                    imu_count += 1
                else:
                    ignored_count += 1

        missing_info = [camera for camera in expected_cameras if camera not in camera_params]
        missing_images = [camera for camera in expected_cameras if image_counts[camera] == 0]
        if missing_info:
            raise ConversionError(f"missing CameraInfo for cameras: {missing_info}")
        if missing_images:
            raise ConversionError(f"missing compressed images for cameras: {missing_images}")
        (staging / "camera_params.yaml").write_text(
            render_camera_params(camera_params), encoding="utf-8"
        )
        summary: dict[str, Any] = {
            "schema": "robobaton_rosbag_extract_v1",
            "input_bag": str(input_bag),
            "output_dir": str(output_dir),
            "timestamp_unit": "nanoseconds",
            "cameras": expected_cameras,
            "image_messages": sum(image_counts.values()),
            "image_messages_by_camera": {
                f"camera{camera_id}": image_counts[camera_id] for camera_id in expected_cameras
            },
            "camera_info_messages": len(camera_params),
            "imu_messages": imu_count,
            "ignored_messages": ignored_count,
            "duplicate_image_timestamps": duplicate_timestamps,
            "files": {
                "imu": "imu.csv",
                "camera_params": "camera_params.yaml",
            },
        }
        (staging / "conversion_summary.json").write_text(
            json.dumps(summary, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        os.replace(staging, output_dir)
        return summary
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="rosbag_extract.py",
        description=(
            "Extract four camera JPEG frames, sensor_msgs/Imu CSV rows, and "
            "sensor_msgs/CameraInfo parameters from a finalized RoboBaton ROS1 bag."
        ),
    )
    parser.add_argument("bagfile", type=Path, help="finalized .bag or .partial.bag input")
    parser.add_argument("output_dir", type=Path, help="new output directory")
    parser.add_argument(
        "--expected-cameras",
        type=parse_expected_cameras,
        default=[0, 1, 2, 3],
        metavar="IDS",
        help="comma-separated camera IDs to extract, default 0,1,2,3",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        summary = extract_bag(args.bagfile, args.output_dir, args.expected_cameras)
    except UnindexedBagError:
        print(f"ERROR: bag is unindexed: {args.bagfile}", file=sys.stderr)
        return 1
    except (BagInfoError, ConversionError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(
        "converted "
        f"images={summary['image_messages']} "
        f"imu={summary['imu_messages']} "
        f"output={summary['output_dir']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
