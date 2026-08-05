#!/usr/bin/env python3
"""Python-stdlib-only ROS1 bag v2 info reader for finalized RoboBaton bags."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from pathlib import Path
import re
import struct
import sys
import time
from typing import Any


BAG_MAGIC = b"#ROSBAG V2.0\n"
OP_MSG_DATA = 0x02
OP_FILE_HEADER = 0x03
OP_INDEX_DATA = 0x04
OP_CHUNK = 0x05
OP_CHUNK_INFO = 0x06
OP_CONNECTION = 0x07
MAX_HEADER_BYTES = 4 * 1024 * 1024
NSEC_PER_SEC = 1_000_000_000.0


class BagInfoError(Exception):
    """Bag metadata cannot be read safely."""


class MissingYamlKeyError(BagInfoError):
    """Requested YAML key is missing or malformed."""


class UnindexedBagError(BagInfoError):
    """Bag lacks a finalized index position."""


@dataclass
class ConnectionInfo:
    conn_id: int
    topic: str
    datatype: str
    md5sum: str
    message_definition: str


@dataclass
class ChunkHeaderInfo:
    pos: int
    compression: str
    compressed_size: int
    uncompressed_size: int


@dataclass
class ChunkInfo:
    pos: int
    start_time: tuple[int, int]
    end_time: tuple[int, int]
    connection_counts: dict[int, int]


@dataclass
class TopicInfo:
    msg_type: str
    message_count: int
    connections: int
    frequency: float | None = None


@dataclass
class BagInfo:
    path: str
    version: str
    size: int
    connections: dict[int, ConnectionInfo]
    chunks: list[ChunkInfo]
    chunk_headers: dict[int, ChunkHeaderInfo]
    indexes: dict[int, list[tuple[int, int]]] = field(default_factory=dict)

    @property
    def message_count(self) -> int:
        return sum(count for chunk in self.chunks for count in chunk.connection_counts.values())

    @property
    def start_stamp(self) -> float:
        if not self.chunks:
            return 0.0
        return time_to_float(self.chunks[0].start_time)

    @property
    def end_stamp(self) -> float:
        if not self.chunks:
            return 0.0
        return time_to_float(self.chunks[-1].end_time)

    @property
    def duration(self) -> float:
        return self.end_stamp - self.start_stamp


class BagReader:
    """Seek-based little-endian ROS bag record reader."""

    def __init__(self, path: Path) -> None:
        self.path = path
        try:
            self.size = path.stat().st_size
            self.file = path.open("rb")
        except OSError as exc:
            raise BagInfoError(str(exc)) from exc

    def close(self) -> None:
        self.file.close()

    def tell(self) -> int:
        return self.file.tell()

    def seek(self, offset: int) -> None:
        if offset < 0 or offset > self.size:
            raise BagInfoError(f"malformed bag: seek offset {offset} outside file size {self.size}")
        self.file.seek(offset)

    def read_exact(self, size: int, context: str) -> bytes:
        if size < 0:
            raise BagInfoError(f"malformed bag: negative {context} length")
        start = self.tell()
        if start + size > self.size:
            raise BagInfoError(
                f"truncated bag: {context} needs {size} bytes at offset {start}, file size {self.size}"
            )
        data = self.file.read(size)
        if len(data) != size:
            raise BagInfoError(f"truncated bag: short read while reading {context}")
        return data

    def skip(self, size: int, context: str) -> None:
        if size < 0:
            raise BagInfoError(f"malformed bag: negative {context} length")
        start = self.tell()
        end = start + size
        if end > self.size:
            raise BagInfoError(f"truncated bag: {context} skips to {end}, file size {self.size}")
        self.file.seek(end)

    def read_u32(self, context: str) -> int:
        return struct.unpack("<I", self.read_exact(4, context))[0]

    def parse_header(self, context: str) -> dict[str, bytes]:
        header_len = self.read_u32(context + " header length")
        if header_len > MAX_HEADER_BYTES:
            raise BagInfoError(f"malformed bag: {context} header length {header_len} is too large")
        header = self.read_exact(header_len, context + " header")
        fields: dict[str, bytes] = {}
        cursor = 0
        while cursor < header_len:
            if cursor + 4 > header_len:
                raise BagInfoError(f"malformed bag: {context} header field length is truncated")
            field_len = struct.unpack_from("<I", header, cursor)[0]
            cursor += 4
            field_end = cursor + field_len
            if field_end > header_len:
                raise BagInfoError(f"malformed bag: {context} header field overruns header")
            raw_field = header[cursor:field_end]
            cursor = field_end
            separator = raw_field.find(b"=")
            if separator <= 0:
                raise BagInfoError(f"malformed bag: {context} header field has no name")
            try:
                name = raw_field[:separator].decode("ascii")
            except UnicodeDecodeError as exc:
                raise BagInfoError(
                    f"malformed bag: {context} header field name is not ASCII"
                ) from exc
            fields[name] = raw_field[separator + 1 :]
        return fields


def field_u8(fields: dict[str, bytes], name: str, context: str) -> int:
    value = require_field(fields, name, context)
    if len(value) != 1:
        raise BagInfoError(f"malformed bag: {context} field {name} length {len(value)} != 1")
    return value[0]


def field_u32(fields: dict[str, bytes], name: str, context: str) -> int:
    value = require_field(fields, name, context)
    if len(value) != 4:
        raise BagInfoError(f"malformed bag: {context} field {name} length {len(value)} != 4")
    return struct.unpack("<I", value)[0]


def field_u64(fields: dict[str, bytes], name: str, context: str) -> int:
    value = require_field(fields, name, context)
    if len(value) != 8:
        raise BagInfoError(f"malformed bag: {context} field {name} length {len(value)} != 8")
    return struct.unpack("<Q", value)[0]


def field_time(fields: dict[str, bytes], name: str, context: str) -> tuple[int, int]:
    value = require_field(fields, name, context)
    if len(value) != 8:
        raise BagInfoError(f"malformed bag: {context} field {name} length {len(value)} != 8")
    sec, nsec = struct.unpack("<II", value)
    if nsec >= 1_000_000_000:
        raise BagInfoError(f"malformed bag: {context} field {name} has invalid nanoseconds")
    return sec, nsec


def field_string(fields: dict[str, bytes], name: str, context: str) -> str:
    value = require_field(fields, name, context)
    try:
        return value.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise BagInfoError(f"malformed bag: {context} field {name} is not UTF-8") from exc


def require_field(fields: dict[str, bytes], name: str, context: str) -> bytes:
    if name not in fields:
        raise BagInfoError(f"malformed bag: {context} missing field {name}")
    return fields[name]


def time_to_float(stamp: tuple[int, int]) -> float:
    return float(stamp[0]) + float(stamp[1]) / NSEC_PER_SEC


def read_bag_info(path_arg: str, *, read_indexes: bool) -> BagInfo:
    path = Path(path_arg)
    if not path.exists():
        raise BagInfoError("does not exist")
    reader = BagReader(path)
    try:
        magic = reader.read_exact(len(BAG_MAGIC), "version line")
        if magic != BAG_MAGIC:
            raise BagInfoError("unsupported bag version: expected #ROSBAG V2.0")

        file_header = reader.parse_header("file header")
        if field_u8(file_header, "op", "file header") != OP_FILE_HEADER:
            raise BagInfoError("malformed bag: first record is not a file header")
        index_pos = field_u64(file_header, "index_pos", "file header")
        conn_count = field_u32(file_header, "conn_count", "file header")
        chunk_count = field_u32(file_header, "chunk_count", "file header")
        padding_len = reader.read_u32("file header data length")
        reader.skip(padding_len, "file header data")

        if index_pos == 0:
            raise UnindexedBagError("bag has no finalized index_pos")
        if index_pos < reader.tell() or index_pos > reader.size:
            raise BagInfoError(f"malformed bag: index_pos {index_pos} outside indexed region")

        chunk_headers, indexes = scan_chunk_region(reader, index_pos, read_indexes=read_indexes)
        connections, chunks = scan_index_region(reader, index_pos)

        if conn_count != len(connections):
            raise BagInfoError(
                f"malformed bag: file header conn_count {conn_count} != parsed {len(connections)}"
            )
        if chunk_count != len(chunks):
            raise BagInfoError(
                f"malformed bag: file header chunk_count {chunk_count} != parsed {len(chunks)}"
            )
        for chunk in chunks:
            if chunk.pos not in chunk_headers:
                raise BagInfoError(f"malformed bag: chunk_info references missing chunk at {chunk.pos}")
            for conn_id in chunk.connection_counts:
                if conn_id not in connections:
                    raise BagInfoError(
                        f"malformed bag: chunk_info references missing connection {conn_id}"
                    )
        for conn_id in indexes:
            if conn_id not in connections:
                raise BagInfoError(f"malformed bag: index references missing connection {conn_id}")

        return BagInfo(
            path=path_arg,
            version="2.0",
            size=reader.size,
            connections=connections,
            chunks=chunks,
            chunk_headers=chunk_headers,
            indexes=indexes,
        )
    finally:
        reader.close()


def scan_chunk_region(
    reader: BagReader, index_pos: int, *, read_indexes: bool
) -> tuple[dict[int, ChunkHeaderInfo], dict[int, list[tuple[int, int]]]]:
    chunk_headers: dict[int, ChunkHeaderInfo] = {}
    indexes: dict[int, list[tuple[int, int]]] = {}
    while reader.tell() < index_pos:
        record_start = reader.tell()
        fields = reader.parse_header(f"record at {record_start}")
        op = field_u8(fields, "op", f"record at {record_start}")
        data_len = reader.read_u32(f"record at {record_start} data length")
        data_start = reader.tell()
        if data_start + data_len > index_pos:
            raise BagInfoError(f"truncated bag: record at {record_start} overruns index_pos {index_pos}")

        if op == OP_CHUNK:
            compression = field_string(fields, "compression", f"chunk at {record_start}")
            uncompressed_size = field_u32(fields, "size", f"chunk at {record_start}")
            if compression != "none":
                raise BagInfoError(f"unsupported compression: {compression}")
            if uncompressed_size != data_len:
                raise BagInfoError(
                    f"malformed bag: uncompressed chunk at {record_start} size mismatch"
                )
            chunk_headers[record_start] = ChunkHeaderInfo(
                pos=record_start,
                compression=compression,
                compressed_size=data_len,
                uncompressed_size=uncompressed_size,
            )
            reader.skip(data_len, f"chunk payload at {record_start}")
        elif op == OP_INDEX_DATA:
            conn_id = field_u32(fields, "conn", f"index at {record_start}")
            count = field_u32(fields, "count", f"index at {record_start}")
            if field_u32(fields, "ver", f"index at {record_start}") != 1:
                raise BagInfoError(f"malformed bag: unsupported index version at {record_start}")
            if data_len != count * 12:
                raise BagInfoError(f"malformed bag: index at {record_start} count/data length mismatch")
            if read_indexes:
                entries = indexes.setdefault(conn_id, [])
                for _ in range(count):
                    sec, nsec, _offset = struct.unpack("<III", reader.read_exact(12, "index entry"))
                    if nsec >= 1_000_000_000:
                        raise BagInfoError(
                            f"malformed bag: index at {record_start} has invalid nanoseconds"
                        )
                    entries.append((sec, nsec))
            else:
                reader.skip(data_len, f"index entries at {record_start}")
        else:
            raise BagInfoError(f"malformed bag: unexpected op {op} before index_pos")

    if reader.tell() != index_pos:
        raise BagInfoError(f"malformed bag: reader stopped at {reader.tell()}, expected index_pos {index_pos}")
    return chunk_headers, indexes


def scan_index_region(reader: BagReader, index_pos: int) -> tuple[dict[int, ConnectionInfo], list[ChunkInfo]]:
    reader.seek(index_pos)
    connections: dict[int, ConnectionInfo] = {}
    chunks: list[ChunkInfo] = []
    while reader.tell() < reader.size:
        record_start = reader.tell()
        fields = reader.parse_header(f"index record at {record_start}")
        op = field_u8(fields, "op", f"index record at {record_start}")
        if op == OP_CONNECTION:
            conn_id = field_u32(fields, "conn", f"connection at {record_start}")
            topic = field_string(fields, "topic", f"connection at {record_start}")
            metadata = reader.parse_header(f"connection metadata at {reader.tell()}")
            connections[conn_id] = ConnectionInfo(
                conn_id=conn_id,
                topic=topic,
                datatype=field_string(metadata, "type", f"connection {conn_id}"),
                md5sum=field_string(metadata, "md5sum", f"connection {conn_id}"),
                message_definition=field_string(
                    metadata, "message_definition", f"connection {conn_id}"
                ),
            )
        elif op == OP_CHUNK_INFO:
            version = field_u32(fields, "ver", f"chunk_info at {record_start}")
            if version != 1:
                raise BagInfoError(f"malformed bag: unsupported chunk_info version at {record_start}")
            count = field_u32(fields, "count", f"chunk_info at {record_start}")
            data_len = reader.read_u32(f"chunk_info at {record_start} data length")
            if data_len != count * 8:
                raise BagInfoError(
                    f"malformed bag: chunk_info at {record_start} count/data length mismatch"
                )
            connection_counts: dict[int, int] = {}
            for _ in range(count):
                conn_id, message_count = struct.unpack("<II", reader.read_exact(8, "chunk_info entry"))
                connection_counts[conn_id] = message_count
            chunks.append(
                ChunkInfo(
                    pos=field_u64(fields, "chunk_pos", f"chunk_info at {record_start}"),
                    start_time=field_time(fields, "start_time", f"chunk_info at {record_start}"),
                    end_time=field_time(fields, "end_time", f"chunk_info at {record_start}"),
                    connection_counts=connection_counts,
                )
            )
        else:
            raise BagInfoError(f"malformed bag: unexpected indexed record op {op} at {record_start}")
    return connections, chunks


def topic_infos(info: BagInfo, *, include_frequency: bool) -> dict[str, TopicInfo]:
    by_topic: dict[str, list[ConnectionInfo]] = {}
    for connection in info.connections.values():
        by_topic.setdefault(connection.topic, []).append(connection)

    topics: dict[str, TopicInfo] = {}
    for topic in sorted(by_topic):
        connections = sorted(by_topic[topic], key=lambda item: item.conn_id)
        message_count = 0
        for connection in connections:
            for chunk in info.chunks:
                message_count += chunk.connection_counts.get(connection.conn_id, 0)
        frequency = topic_frequency(connections, info.indexes) if include_frequency else None
        topics[topic] = TopicInfo(
            msg_type=connections[0].datatype,
            message_count=message_count,
            connections=len(connections),
            frequency=frequency,
        )
    return topics


def topic_frequency(connections: list[ConnectionInfo], indexes: dict[int, list[tuple[int, int]]]) -> float | None:
    stamps: list[float] = []
    for connection in connections:
        stamps.extend(time_to_float(stamp) for stamp in indexes.get(connection.conn_id, []))
    stamps.sort()
    if len(stamps) <= 1:
        return None
    periods = [newer - older for newer, older in zip(stamps[1:], stamps[:-1])]
    median_period = median(periods)
    if median_period > 0.0:
        return 1.0 / median_period
    return None


def datatype_infos(info: BagInfo) -> list[ConnectionInfo]:
    seen: set[str] = set()
    datatypes: list[ConnectionInfo] = []
    for connection in sorted(info.connections.values(), key=lambda item: item.datatype):
        if connection.datatype in seen:
            continue
        seen.add(connection.datatype)
        datatypes.append(connection)
    return datatypes


def format_plain(info: BagInfo, *, include_frequency: bool) -> str:
    rows: list[tuple[str, str]] = [("path", info.path), ("version", info.version)]
    if info.chunks:
        rows.append(("duration", human_readable_duration(info.duration)))
        rows.append(("start", f"{time_to_str(info.start_stamp)} ({info.start_stamp:.2f})"))
        rows.append(("end", f"{time_to_str(info.end_stamp)} ({info.end_stamp:.2f})"))
    rows.append(("size", human_readable_size(info.size)))
    if info.chunks:
        rows.append(("messages", str(info.message_count)))
        rows.append(("compression", plain_compression(info)))
        add_type_rows(rows, info)
        add_topic_rows(rows, info, include_frequency=include_frequency)

    first_column_width = max(len(field) for field, _ in rows) + 1
    return "\n".join(
        f"{(field + ':') if field else '':<{first_column_width}} {value}" for field, value in rows
    ).rstrip()


def add_type_rows(rows: list[tuple[str, str]], info: BagInfo) -> None:
    datatypes = datatype_infos(info)
    if not datatypes:
        return
    max_datatype_len = max(len(connection.datatype) for connection in datatypes)
    for index, connection in enumerate(datatypes):
        value = f"{connection.datatype:<{max_datatype_len}} [{connection.md5sum}]"
        rows.append(("types" if index == 0 else "", value))


def add_topic_rows(rows: list[tuple[str, str]], info: BagInfo, *, include_frequency: bool) -> None:
    topics = topic_infos(info, include_frequency=include_frequency)
    if not topics:
        return
    max_topic_len = max(len(topic) for topic in topics)
    max_datatype_len = max(len(topic_info.msg_type) for topic_info in topics.values())
    max_msg_count_len = max(len(str(topic_info.message_count)) for topic_info in topics.values())
    frequency_values = [topic_info.frequency for topic_info in topics.values() if topic_info.frequency is not None]
    max_frequency_len = max((len(human_readable_frequency(freq)) for freq in frequency_values), default=0)

    for index, (topic, topic_info) in enumerate(topics.items()):
        msg_word = "msgs" if topic_info.message_count > 1 else "msg "
        value = f"{topic:<{max_topic_len}}   {topic_info.message_count:>{max_msg_count_len}} {msg_word}"
        if topic_info.frequency is not None:
            value += f" @ {human_readable_frequency(topic_info.frequency):>{max_frequency_len}}"
        else:
            value += f"   {'':>{max_frequency_len}}"
        value += f" : {topic_info.msg_type:<{max_datatype_len}}"
        if topic_info.connections > 1:
            value += f" ({topic_info.connections} connections)"
        rows.append(("topics" if index == 0 else "", value))


def format_yaml(info: BagInfo, *, include_frequency: bool, key: str | None) -> str:
    data = yaml_data(info, include_frequency=include_frequency)
    if key:
        try:
            selected = select_key(data, key)
        except (KeyError, IndexError, TypeError, ValueError) as exc:
            raise MissingYamlKeyError(f'Missing YAML key "{key}"') from exc
        return print_yaml_value(selected)
    return render_full_yaml(data).rstrip("\n")


def yaml_data(info: BagInfo, *, include_frequency: bool) -> dict[str, Any]:
    data: dict[str, Any] = {
        "path": info.path,
        "version": info.version,
    }
    if info.chunks:
        data.update(
            {
                "duration": info.duration,
                "start": info.start_stamp,
                "end": info.end_stamp,
            }
        )
    data["size"] = info.size
    if info.chunks:
        data.update(
            {
                "messages": info.message_count,
                "indexed": True,
                "compression": "none",
                "types": [
                    {"type": connection.datatype, "md5": connection.md5sum}
                    for connection in datatype_infos(info)
                ],
                "topics": [],
            }
        )
        for topic, topic_info in topic_infos(info, include_frequency=include_frequency).items():
            topic_data: dict[str, Any] = {
                "topic": topic,
                "type": topic_info.msg_type,
                "messages": topic_info.message_count,
            }
            if topic_info.connections > 1:
                topic_data["connections"] = topic_info.connections
            if topic_info.frequency is not None:
                topic_data["frequency"] = topic_info.frequency
            data["topics"].append(topic_data)
    else:
        data["indexed"] = False
    return data


def render_full_yaml(data: dict[str, Any]) -> str:
    lines: list[str] = []
    for key, value in data.items():
        if key in {"types", "topics"}:
            lines.append(f"{key}:")
            for item in value:
                first = True
                for item_key, item_value in item.items():
                    if item_key == "frequency" and isinstance(item_value, float):
                        rendered = f"{item_value:.4f}"
                    else:
                        rendered = render_scalar(item_value)
                    if first:
                        lines.append(f"    - {item_key}: {rendered}")
                        first = False
                    else:
                        lines.append(f"      {item_key}: {rendered}")
        else:
            lines.append(f"{key}: {render_scalar(value)}")
    return "\n".join(lines)


def render_scalar(value: Any) -> str:
    if isinstance(value, bool):
        return "True" if value else "False"
    if isinstance(value, float):
        return f"{value:.6f}"
    return str(value)


def select_key(data: Any, key: str) -> Any:
    current = data
    for name, index in parse_key(key):
        if name:
            if not isinstance(current, dict):
                raise TypeError(name)
            current = current[name]
        if index is not None:
            if not isinstance(current, list):
                raise TypeError(index)
            current = current[index]
    return current


def parse_key(key: str) -> list[tuple[str, int | None]]:
    if not key:
        raise ValueError("empty key")
    parts: list[tuple[str, int | None]] = []
    for raw_part in key.split("."):
        match = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_]*)(?:\[(\d+)\])?", raw_part)
        if not match:
            raise ValueError(key)
        name = match.group(1)
        index = int(match.group(2)) if match.group(2) is not None else None
        parts.append((name, index))
    return parts


def print_yaml_value(value: Any, indent: int = 0) -> str:
    indent_text = "  " * indent
    if isinstance(value, list):
        output = ""
        for item in value:
            output += f"{indent_text}- {print_yaml_value(item, indent + 1)}\n"
        return output
    if isinstance(value, dict):
        output = ""
        for index, (key, item_value) in enumerate(value.items()):
            if index != 0:
                output += indent_text
            output += f"{key}: {item_value}"
            if index < len(value) - 1:
                output += "\n"
        return output
    return indent_text + str(value)


def plain_compression(info: BagInfo) -> str:
    chunk_count = len(info.chunk_headers)
    return f"none [{chunk_count}/{chunk_count} chunks]"


def human_readable_duration(duration: float) -> str:
    duration_secs = duration % 60
    duration_mins = int(duration / 60)
    duration_hrs = int(duration_mins / 60)
    if duration_hrs > 0:
        duration_mins %= 60
        return "%dhr %d:%02ds (%ds)" % (duration_hrs, duration_mins, int(duration_secs), int(duration))
    if duration_mins > 0:
        return "%d:%02ds (%ds)" % (duration_mins, int(duration_secs), int(duration))
    return "%.1fs" % duration


def time_to_str(secs: float) -> str:
    secs_frac = secs - int(secs)
    secs_frac_str = ("%.2f" % secs_frac)[1:]
    return time.strftime("%b %d %Y %H:%M:%S", time.localtime(secs)) + secs_frac_str


def human_readable_size(size: int | float) -> str:
    value = float(size)
    for suffix in ["KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"]:
        value /= 1024.0
        if value < 1024.0:
            return "%.1f %s" % (value, suffix)
    return "-"


def human_readable_frequency(freq: float) -> str:
    value = freq
    for suffix in ["Hz", "kHz", "MHz", "GHz", "THz", "PHz", "EHz", "ZHz", "YHz"]:
        if value < 1000.0:
            return "%.1f %s" % (value, suffix)
        value /= 1000.0
    return "-"


def median(values: list[float]) -> float:
    if not values:
        return float("nan")
    ordered = sorted(values)
    midpoint = len(ordered) // 2
    if len(ordered) % 2 == 1:
        return ordered[midpoint]
    return float(ordered[midpoint - 1] + ordered[midpoint]) / 2.0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="rosbag_info.py",
        usage="%(prog)s [options] BAGFILE1 [BAGFILE2 BAGFILE3 ...]",
        description="Summarize the contents of one or more ROS1 bag files.",
    )
    parser.add_argument("-y", "--yaml", action="store_true", help="print information in YAML format")
    parser.add_argument("-k", "--key", default=None, help="print information on the given key")
    parser.add_argument("--freq", action="store_true", help="display topic message frequency statistics")
    parser.add_argument("bagfiles", nargs="+")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.key and not args.yaml:
        parser.error("You can only specify key when printing in YAML format.")

    for index, bagfile in enumerate(args.bagfiles):
        try:
            info = read_bag_info(bagfile, read_indexes=args.freq)
            if args.yaml:
                output = format_yaml(info, include_frequency=args.freq, key=args.key)
                print(output)
                if args.key is None:
                    print()
            else:
                print(format_plain(info, include_frequency=args.freq))
            if index < len(args.bagfiles) - 1:
                print("---")
        except MissingYamlKeyError as exc:
            print(str(exc), file=sys.stderr)
            return 1
        except UnindexedBagError:
            print(f"ERROR bag unindexed: {bagfile}.  Run rosbag reindex.", file=sys.stderr)
            return 1
        except BagInfoError as exc:
            print(f"ERROR reading {bagfile}: {exc}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
