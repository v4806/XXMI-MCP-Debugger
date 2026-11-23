from __future__ import annotations

import ctypes
from ctypes import wintypes
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import secrets
import struct
import sys
import time
from typing import Any, Iterable


REQUEST_MAGIC = 0x3150434D
PROTOCOL_VERSION = 1
REQUEST_HEADER = struct.Struct("<IHHII")
RESPONSE_HEADER = struct.Struct("<IIII")

COMMAND_STATUS = 1
COMMAND_FRAME_ANALYSIS = 2
COMMAND_RELOAD_CONFIG = 3
COMMAND_RELOAD_SHADERS = 4
COMMAND_PING = 5
COMMAND_LIVE_CONTEXT = 6
COMMAND_RUNTIME_VARIABLES = 7
COMMAND_LIVE_DRAW_SNAPSHOT = 8

SUPPORTED_COMMANDS = {
    COMMAND_STATUS,
    COMMAND_FRAME_ANALYSIS,
    COMMAND_RELOAD_CONFIG,
    COMMAND_RELOAD_SHADERS,
    COMMAND_PING,
    COMMAND_LIVE_CONTEXT,
    COMMAND_RUNTIME_VARIABLES,
    COMMAND_LIVE_DRAW_SNAPSHOT,
}

PIPE_PREFIX = "xxmi-mcp-v1-"
PIPE_PREFIX_PATH = "\\\\.\\pipe\\" + PIPE_PREFIX
PIPE_PATTERN = re.compile(r"^" + re.escape(PIPE_PREFIX) + r"(\d+)-([0-9a-fA-F]{8})$")

DEFAULT_TIMEOUT_S = 2.0
RELOAD_CONFIG_TIMEOUT_S = 10.0
LIVE_DRAW_TIMEOUT_S = 5.0
STATUS_RETRY_TIMEOUT_S = 10.0
STATUS_RETRY_DELAY_S = 0.1
DEFAULT_LIMIT = 100
MAX_LIMIT = 500
MAX_PIPE_PAYLOAD = 1024 * 1024
READ_BLOCK_SIZE = 64 * 1024
LATEST_MCP_VERSION = "2025-11-25"
SUPPORTED_MCP_VERSIONS = {
    "2024-11-05",
    "2025-03-26",
    "2025-06-18",
    "2025-11-25",
}

ERROR_FILE_NOT_FOUND = 2
ERROR_ACCESS_DENIED = 5
ERROR_SEM_TIMEOUT = 121
ERROR_PIPE_BUSY = 231
ERROR_IO_PENDING = 997
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000


class XXMIError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


class NativePipeError(XXMIError):
    def __init__(self, status: int, code: str, message: str):
        super().__init__(code, message)
        self.status = status


class InstanceSelectionError(XXMIError):
    pass


class ToolArgumentError(XXMIError):
    def __init__(self, message: str):
        super().__init__("INVALID_ARGUMENT", message)


class FileReadError(XXMIError):
    pass


@dataclass(frozen=True)
class Instance:
    pid: int
    instance: str

    @property
    def instance_id(self) -> str:
        return f"{self.pid}-{self.instance}"

    @property
    def pipe_name(self) -> str:
        return PIPE_PREFIX_PATH + self.instance_id

    @property
    def pipe_path(self) -> str:
        return self.pipe_name


class OVERLAPPED(ctypes.Structure):
    _fields_ = [
        ("Internal", ctypes.c_size_t),
        ("InternalHigh", ctypes.c_size_t),
        ("Offset", wintypes.DWORD),
        ("OffsetHigh", wintypes.DWORD),
        ("hEvent", wintypes.HANDLE),
    ]


if os.name == "nt":
    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _kernel32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
    _kernel32.WaitNamedPipeW.restype = wintypes.BOOL
    _kernel32.CreateFileW.argtypes = [
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
        ctypes.c_void_p,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.HANDLE,
    ]
    _kernel32.CreateFileW.restype = wintypes.HANDLE
    _kernel32.CreateEventW.argtypes = [
        ctypes.c_void_p,
        wintypes.BOOL,
        wintypes.BOOL,
        wintypes.LPCWSTR,
    ]
    _kernel32.CreateEventW.restype = wintypes.HANDLE
    _kernel32.ReadFile.argtypes = [
        wintypes.HANDLE,
        ctypes.c_void_p,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        ctypes.POINTER(OVERLAPPED),
    ]
    _kernel32.ReadFile.restype = wintypes.BOOL
    _kernel32.WriteFile.argtypes = [
        wintypes.HANDLE,
        ctypes.c_void_p,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        ctypes.POINTER(OVERLAPPED),
    ]
    _kernel32.WriteFile.restype = wintypes.BOOL
    _kernel32.GetOverlappedResult.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(OVERLAPPED),
        ctypes.POINTER(wintypes.DWORD),
        wintypes.BOOL,
    ]
    _kernel32.GetOverlappedResult.restype = wintypes.BOOL
    _kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    _kernel32.WaitForSingleObject.restype = wintypes.DWORD
    _kernel32.CancelIoEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(OVERLAPPED)]
    _kernel32.CancelIoEx.restype = wintypes.BOOL
    _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    _kernel32.CloseHandle.restype = wintypes.BOOL
else:
    _kernel32 = None


def parse_pipe_name(name: str) -> Instance | None:
    base_name = name.rsplit("\\", 1)[-1]
    match = PIPE_PATTERN.fullmatch(base_name)
    if not match:
        return None
    pid = int(match.group(1))
    if pid <= 0:
        return None
    return Instance(pid=pid, instance=match.group(2).lower())


def discover_instances() -> list[Instance]:
    try:
        names = os.listdir("\\\\.\\pipe\\")
    except OSError:
        return []
    instances = {
        instance
        for name in names
        if (instance := parse_pipe_name(name)) is not None
    }
    return sorted(instances, key=lambda item: (item.pid, item.instance))


def select_instance(
    instances: Iterable[Instance],
    selector: str | None,
) -> Instance:
    candidates = list(instances)
    if selector is None:
        if not candidates:
            raise InstanceSelectionError(
                "NO_INSTANCE",
                "No running XXMI/3DMigoto MCP pipe was found.",
            )
        if len(candidates) > 1:
            ids = ", ".join(instance.instance_id for instance in candidates)
            raise InstanceSelectionError(
                "AMBIGUOUS_INSTANCE",
                f"Multiple XXMI instances are available; pass instance_id: {ids}",
            )
        return candidates[0]

    requested = selector.rsplit("\\", 1)[-1]
    if requested.startswith(PIPE_PREFIX):
        requested = requested[len(PIPE_PREFIX) :]
    exact = [
        instance
        for instance in candidates
        if requested in {instance.instance_id, instance.instance}
    ]
    if len(exact) == 1:
        return exact[0]
    if len(exact) > 1:
        raise InstanceSelectionError(
            "AMBIGUOUS_INSTANCE",
            f"instance_id {selector!r} matches more than one XXMI instance.",
        )
    raise InstanceSelectionError(
        "INSTANCE_GONE",
        f"XXMI instance {selector!r} is no longer available.",
    )


def parse_key_value_lines(payload: bytes) -> dict[str, str]:
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        raise XXMIError(
            "PROTOCOL_ERROR",
            f"Native response payload is not valid UTF-8: {error}",
        ) from error

    result: dict[str, str] = {}
    for line in text.splitlines():
        if not line:
            continue
        key, separator, value = line.partition("=")
        if not separator:
            value = ""
        result[key] = value
    return result


def _remaining_ms(deadline: float) -> int:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise XXMIError("PIPE_TIMEOUT", "The XXMI native pipe timed out.")
    return max(1, int(remaining * 1000))


def _raise_pipe_os_error(error_code: int, context: str) -> None:
    if error_code == ERROR_FILE_NOT_FOUND:
        raise XXMIError(
            "INSTANCE_GONE",
            f"{context}: the XXMI named pipe is unavailable (Windows error {error_code}).",
        )
    if error_code in {ERROR_PIPE_BUSY, ERROR_SEM_TIMEOUT}:
        raise XXMIError(
            "PIPE_BUSY",
            f"{context}: the XXMI named pipe is busy (Windows error {error_code}).",
        )
    if error_code == ERROR_ACCESS_DENIED:
        raise XXMIError(
            "ACCESS_DENIED",
            f"{context}: access to the XXMI named pipe was denied.",
        )
    message = ctypes.FormatError(error_code).strip() or "unknown Windows error"
    raise XXMIError(
        "PIPE_ERROR",
        f"{context}: {message} (Windows error {error_code}).",
    )


def _open_pipe(instance: Instance, deadline: float) -> int:
    if os.name != "nt" or _kernel32 is None:
        raise XXMIError("UNSUPPORTED_PLATFORM", "XXMI MCP requires Windows.")

    if not _kernel32.WaitNamedPipeW(instance.pipe_name, _remaining_ms(deadline)):
        error_code = ctypes.get_last_error()
        _raise_pipe_os_error(error_code, "Waiting for native pipe")

    handle = _kernel32.CreateFileW(
        instance.pipe_name,
        GENERIC_READ | GENERIC_WRITE,
        0,
        None,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        None,
    )
    invalid_handle = ctypes.c_void_p(-1).value
    if not handle or handle == invalid_handle:
        _raise_pipe_os_error(ctypes.get_last_error(), "Opening native pipe")
    return handle


def _overlapped_io(
    handle: int,
    data: bytes | None,
    size: int,
    deadline: float,
) -> bytes:
    if _kernel32 is None:
        raise XXMIError("UNSUPPORTED_PLATFORM", "XXMI MCP requires Windows.")

    event = _kernel32.CreateEventW(None, True, False, None)
    if not event:
        error_code = ctypes.get_last_error()
        raise XXMIError(
            "PIPE_ERROR",
            f"Creating a native pipe event failed: {ctypes.FormatError(error_code).strip()}.",
        )

    overlapped = OVERLAPPED()
    overlapped.hEvent = event
    buffer_size = len(data) if data is not None else size
    buffer = ctypes.create_string_buffer(data if data is not None else buffer_size)
    transferred = wintypes.DWORD()
    try:
        if data is None:
            ok = _kernel32.ReadFile(
                handle,
                buffer,
                size,
                ctypes.byref(transferred),
                ctypes.byref(overlapped),
            )
        else:
            ok = _kernel32.WriteFile(
                handle,
                buffer,
                len(data),
                ctypes.byref(transferred),
                ctypes.byref(overlapped),
            )

        if not ok:
            error_code = ctypes.get_last_error()
            if error_code != ERROR_IO_PENDING:
                _raise_pipe_os_error(error_code, "Native pipe I/O")

        wait_result = _kernel32.WaitForSingleObject(event, _remaining_ms(deadline))
        if wait_result == WAIT_TIMEOUT:
            _kernel32.CancelIoEx(handle, ctypes.byref(overlapped))
            raise XXMIError("PIPE_TIMEOUT", "The XXMI native pipe response timed out.")
        if wait_result != WAIT_OBJECT_0:
            raise XXMIError(
                "PIPE_ERROR",
                f"Waiting for native pipe I/O failed with wait status {wait_result}.",
            )

        bytes_transferred = wintypes.DWORD()
        if not _kernel32.GetOverlappedResult(
            handle,
            ctypes.byref(overlapped),
            ctypes.byref(bytes_transferred),
            False,
        ):
            _raise_pipe_os_error(
                ctypes.get_last_error(),
                "Completing native pipe I/O",
            )
        return buffer.raw[: bytes_transferred.value]
    finally:
        _kernel32.CloseHandle(event)


def _read_response_payload(handle: int, deadline: float) -> tuple[bytes, bytes]:
    buffered = bytearray()
    expected_size: int | None = None

    while True:
        remaining_capacity = (
            RESPONSE_HEADER.size + MAX_PIPE_PAYLOAD - len(buffered)
        )
        if remaining_capacity <= 0:
            raise XXMIError(
                "PROTOCOL_ERROR",
                "Native response exceeds the maximum supported payload.",
            )
        chunk = _overlapped_io(handle, None, remaining_capacity, deadline)
        if not chunk:
            raise XXMIError(
                "PROTOCOL_ERROR",
                "The XXMI native pipe closed before the response was complete.",
            )
        buffered.extend(chunk)

        if expected_size is None and len(buffered) >= RESPONSE_HEADER.size:
            _, _, _, payload_size = RESPONSE_HEADER.unpack(
                buffered[: RESPONSE_HEADER.size]
            )
            if payload_size > MAX_PIPE_PAYLOAD:
                raise XXMIError(
                    "PROTOCOL_ERROR",
                    f"Native response payload is too large ({payload_size} bytes).",
                )
            expected_size = RESPONSE_HEADER.size + payload_size

        if expected_size is not None and len(buffered) >= expected_size:
            if len(buffered) != expected_size:
                raise XXMIError(
                    "PROTOCOL_ERROR",
                    "Native response contained trailing bytes.",
                )
            return (
                bytes(buffered[: RESPONSE_HEADER.size]),
                bytes(buffered[RESPONSE_HEADER.size :]),
            )


def _write_all(handle: int, data: bytes, deadline: float) -> None:
    offset = 0
    while offset < len(data):
        written = _overlapped_io(handle, data[offset:], 0, deadline)
        if not written:
            raise XXMIError(
                "PROTOCOL_ERROR",
                "The XXMI native pipe closed before the request was written.",
            )
        offset += len(written)


def send_pipe_command(
    instance: Instance,
    command: int,
    payload: bytes = b"",
    timeout_s: float = DEFAULT_TIMEOUT_S,
    request_id: int | None = None,
) -> dict[str, str]:
    if command not in SUPPORTED_COMMANDS:
        raise ToolArgumentError(f"Unsupported native command {command}.")
    if payload:
        raise ToolArgumentError("Native commands require an empty payload.")
    if request_id is None:
        request_id = secrets.randbits(32)
    if not 0 <= request_id <= 0xFFFFFFFF:
        raise ToolArgumentError("request_id must fit in uint32.")

    deadline = time.monotonic() + timeout_s
    handle = _open_pipe(instance, deadline)
    try:
        request = REQUEST_HEADER.pack(
            REQUEST_MAGIC,
            PROTOCOL_VERSION,
            command,
            request_id,
            len(payload),
        ) + payload
        _write_all(handle, request, deadline)

        response_header, response_payload = _read_response_payload(handle, deadline)
        magic, status, response_request_id, payload_size = RESPONSE_HEADER.unpack(
            response_header
        )
        if magic != REQUEST_MAGIC:
            raise XXMIError(
                "PROTOCOL_ERROR",
                f"Unexpected native response magic 0x{magic:08X}.",
            )
        if response_request_id != request_id:
            raise XXMIError(
                "PROTOCOL_ERROR",
                "Native response request_id does not match the request.",
            )
        if payload_size != len(response_payload):
            raise XXMIError(
                "PROTOCOL_ERROR",
                "Native response payload size does not match its header.",
            )
    finally:
        _kernel32.CloseHandle(handle)

    values = parse_key_value_lines(response_payload)
    if status != 0:
        code = values.get("error") or "NATIVE_ERROR"
        message = values.get("error_message") or f"Native command failed with status {status}."
        raise NativePipeError(status, code, message)
    return values


def _decode_log_line(raw: bytes) -> str:
    line = raw[:-1] if raw.endswith(b"\n") else raw
    line = line[:-1] if line.endswith(b"\r") else line
    return line.decode("utf-8", errors="replace")


def _iter_lines_forward(path: Path) -> Iterable[str]:
    with path.open("rb") as file:
        for raw_line in file:
            yield _decode_log_line(raw_line)


def _iter_lines_reverse(path: Path) -> Iterable[str]:
    with path.open("rb") as file:
        file.seek(0, os.SEEK_END)
        position = file.tell()
        suffix = b""
        at_file_end = True

        while position > 0:
            size = min(READ_BLOCK_SIZE, position)
            position -= size
            file.seek(position)
            parts = (file.read(size) + suffix).split(b"\n")

            if position > 0:
                suffix = parts[0]
                candidates = parts[1:]
            else:
                suffix = b""
                candidates = parts

            end_index = len(candidates) - 1
            if at_file_end and end_index >= 0 and candidates[end_index] == b"":
                end_index -= 1
            for index in range(end_index, -1, -1):
                yield _decode_log_line(candidates[index])
            at_file_end = False


def read_text_lines(
    path: str | Path,
    *,
    tail: bool,
    search: str | None,
    limit: int,
) -> dict[str, Any]:
    log_path = Path(path)
    try:
        if not log_path.is_file():
            raise FileReadError(
                "LOG_UNAVAILABLE",
                f"Log file does not exist: {log_path}",
            )
        iterator = (
            _iter_lines_reverse(log_path)
            if tail
            else _iter_lines_forward(log_path)
        )
        lines: list[str] = []
        matched = 0
        for line in iterator:
            if search is not None and search not in line:
                continue
            matched += 1
            if len(lines) < limit:
                lines.append(line)
            else:
                break
        if tail:
            lines.reverse()
    except FileReadError:
        raise
    except OSError as error:
        raise FileReadError(
            "READ_FAILED",
            f"Could not read log file {log_path}: {error}",
        ) from error

    return {
        "path": str(log_path),
        "tail": tail,
        "search": search,
        "limit": limit,
        "lines": lines,
        "line_count": len(lines),
        "matched": matched,
        "truncated": matched > limit,
    }


def list_frame_analysis_directories(
    analysis_path: str | Path,
    *,
    limit: int,
) -> dict[str, Any]:
    root = Path(analysis_path)
    try:
        if not root.is_dir():
            raise FileReadError(
                "ANALYSIS_UNAVAILABLE",
                f"Frame analysis path does not exist: {root}",
            )
        candidates: list[Path] = []
        if root.name.startswith("FrameAnalysis-"):
            candidates.append(root)
        else:
            candidates.extend(
                child
                for child in root.iterdir()
                if child.is_dir() and child.name.startswith("FrameAnalysis-")
            )
    except FileReadError:
        raise
    except OSError as error:
        raise FileReadError(
            "READ_FAILED",
            f"Could not list frame analyses under {root}: {error}",
        ) from error

    unique = {str(path.resolve()): path for path in candidates}
    ordered = sorted(
        unique.values(),
        key=lambda path: path.stat().st_mtime_ns,
        reverse=True,
    )
    items = [
        {
            "name": path.name,
            "path": str(path),
            "modified_s": path.stat().st_mtime,
        }
        for path in ordered[:limit]
    ]
    return {
        "analysis_path": str(root),
        "items": items,
        "count": len(items),
        "truncated": len(ordered) > limit,
    }


def read_frame_analysis_log(
    analysis_path: str | Path,
    analysis: str,
    *,
    search: str | None,
    tail: bool,
    limit: int,
) -> dict[str, Any]:
    if Path(analysis).name != analysis or analysis in {".", ".."}:
        raise ToolArgumentError("analysis must be a directory name, not a path.")

    root = Path(analysis_path)
    candidate = root if root.name == analysis else root / analysis
    try:
        resolved_root = root.resolve()
        resolved_candidate = candidate.resolve()
        if not resolved_candidate.is_relative_to(resolved_root):
            raise ToolArgumentError("analysis resolves outside analysis_path.")
    except OSError as error:
        raise FileReadError(
            "READ_FAILED",
            f"Could not resolve frame analysis path {candidate}: {error}",
        ) from error

    log_path = resolved_candidate / "log.txt"
    resolved_log_path = log_path.resolve()
    try:
        if not resolved_log_path.is_relative_to(resolved_candidate):
            raise ToolArgumentError("log.txt resolves outside the frame analysis directory.")
    except OSError as error:
        raise FileReadError(
            "READ_FAILED",
            f"Could not resolve frame analysis log {log_path}: {error}",
        ) from error
    page = read_text_lines(
        resolved_log_path,
        tail=tail,
        search=search,
        limit=limit,
    )
    page["analysis"] = analysis
    page["analysis_path"] = str(resolved_candidate)
    return page


TOOL_DEFINITIONS = [
    {
        "name": "xxmi_list_instances",
        "title": "List XXMI Instances",
        "description": "Discover running XXMI/3DMigoto instances and read their current status.",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "additionalProperties": False,
        },
        "annotations": {
            "readOnlyHint": True,
            "openWorldHint": False,
        },
    },
    {
        "name": "xxmi_status",
        "title": "Get XXMI Status",
        "description": "Read the fixed STATUS fields from one running XXMI instance.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "instance_id": {
                    "type": "string",
                    "description": "Returned by xxmi_list_instances. Omit only when exactly one instance exists.",
                }
            },
            "additionalProperties": False,
        },
        "annotations": {
            "readOnlyHint": True,
            "openWorldHint": False,
        },
    },
    {
        "name": "xxmi_read_log",
        "title": "Read XXMI Log",
        "description": "Read d3d11_log.txt with substring search, newest-tail mode, and a bounded line count.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "instance_id": {"type": "string"},
                "search": {
                    "type": "string",
                    "description": "Case-sensitive substring to match.",
                },
                "tail": {
                    "type": "boolean",
                    "default": True,
                    "description": "Read newest matching lines first when true.",
                },
                "limit": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": MAX_LIMIT,
                    "default": DEFAULT_LIMIT,
                },
            },
            "additionalProperties": False,
        },
        "annotations": {
            "readOnlyHint": True,
            "openWorldHint": False,
        },
    },
    {
        "name": "xxmi_list_frame_analyses",
        "title": "List Frame Analyses",
        "description": "List FrameAnalysis-* directories under the instance's analysis_root.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "instance_id": {"type": "string"},
                "limit": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": MAX_LIMIT,
                    "default": 20,
                },
            },
            "additionalProperties": False,
        },
        "annotations": {
            "readOnlyHint": True,
            "openWorldHint": False,
        },
    },
    {
        "name": "xxmi_read_frame_analysis",
        "title": "Read Frame Analysis Log",
        "description": "Search or tail log.txt inside one FrameAnalysis-* directory.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "instance_id": {"type": "string"},
                "analysis": {
                    "type": "string",
                    "description": "Directory name returned by xxmi_list_frame_analyses.",
                },
                "search": {
                    "type": "string",
                    "description": "Case-sensitive substring to match.",
                },
                "tail": {"type": "boolean", "default": True},
                "limit": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": MAX_LIMIT,
                    "default": DEFAULT_LIMIT,
                },
            },
            "required": ["analysis"],
            "additionalProperties": False,
        },
        "annotations": {
            "readOnlyHint": True,
            "openWorldHint": False,
        },
    },
    {
        "name": "xxmi_trigger_frame_capture",
        "title": "Trigger Frame Analysis",
        "description": "Ask the native DLL to temporarily enable hunting and start one configured Frame Analysis capture.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "instance_id": {"type": "string"},
            },
            "additionalProperties": False,
        },
        "annotations": {
            "readOnlyHint": False,
            "destructiveHint": False,
            "idempotentHint": False,
            "openWorldHint": False,
        },
    },
    {
        "name": "xxmi_reload",
        "title": "Reload XXMI",
        "description": "Reload the active Mod configuration or shader overrides through the native DLL.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "instance_id": {"type": "string"},
                "target": {
                    "type": "string",
                    "enum": ["config", "shaders"],
                },
            },
            "required": ["target"],
            "additionalProperties": False,
        },
        "annotations": {
            "readOnlyHint": False,
            "destructiveHint": False,
            "idempotentHint": False,
            "openWorldHint": False,
        },
    },
    {
        "name": "xxmi_live_context",
        "title": "Capture Live D3D11 Context",
        "description": "Capture the current immediate D3D11 pipeline and resource bindings into a bounded JSON snapshot.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "instance_id": {"type": "string"},
            },
            "additionalProperties": False,
        },
        "annotations": {
            "readOnlyHint": False,
            "destructiveHint": False,
            "idempotentHint": False,
            "openWorldHint": False,
        },
    },
    {
        "name": "xxmi_runtime_variables",
        "title": "Read Mod Runtime Variables",
        "description": "Read live Mod state plus bounded d3dx.ini and d3dx_user.ini variables.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "instance_id": {"type": "string"},
            },
            "additionalProperties": False,
        },
        "annotations": {
            "readOnlyHint": True,
            "openWorldHint": False,
        },
    },
    {
        "name": "xxmi_live_draw_snapshot",
        "title": "Capture One Live Draw",
        "description": "Capture metadata for the next immediate-context draw and restore normal drawing immediately.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "instance_id": {"type": "string"},
            },
            "additionalProperties": False,
        },
        "annotations": {
            "readOnlyHint": False,
            "destructiveHint": False,
            "idempotentHint": False,
            "openWorldHint": False,
        },
    },
    {
        "name": "xxmi_watch_draw_delta",
        "title": "Compare Live Draws",
        "description": "Capture a bounded sequence of live draws and return only values that changed.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "instance_id": {"type": "string"},
                "frames": {
                    "type": "integer",
                    "minimum": 2,
                    "maximum": 30,
                    "default": 10,
                },
            },
            "additionalProperties": False,
        },
        "annotations": {
            "readOnlyHint": False,
            "destructiveHint": False,
            "idempotentHint": False,
            "openWorldHint": False,
        },
    },
    {
        "name": "xxmi_probe_resource",
        "title": "Probe Captured Resource",
        "description": "Filter resources in a saved live snapshot by stage, slot, hash, or pointer.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "instance_id": {"type": "string"},
                "snapshot_path": {"type": "string"},
                "stage": {
                    "type": "string",
                    "enum": ["vs", "ps", "cs"],
                },
                "slot": {"type": "integer", "minimum": 0, "maximum": 127},
                "hash": {"type": "string"},
                "pointer": {"type": "string"},
                "include_pointers": {"type": "boolean", "default": False},
            },
            "additionalProperties": False,
        },
        "annotations": {
            "readOnlyHint": True,
            "openWorldHint": False,
        },
    },
    {
        "name": "xxmi_override_trace",
        "title": "Read Live Override Trace",
        "description": "Return observed override, post-command, and skip data from the latest live draw snapshot.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "instance_id": {"type": "string"},
                "snapshot_path": {"type": "string"},
                "limit": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": MAX_LIMIT,
                    "default": DEFAULT_LIMIT,
                },
            },
            "additionalProperties": False,
        },
        "annotations": {
            "readOnlyHint": True,
            "openWorldHint": False,
        },
    },
]
TOOLS_BY_NAME = {tool["name"]: tool for tool in TOOL_DEFINITIONS}


def _validate_argument_keys(
    tool_name: str,
    arguments: dict[str, Any],
) -> None:
    schema = TOOLS_BY_NAME[tool_name]["inputSchema"]
    allowed = set(schema.get("properties", {}))
    unknown = sorted(set(arguments) - allowed)
    if unknown:
        raise ToolArgumentError(
            f"Unknown argument(s) for {tool_name}: {', '.join(unknown)}"
        )
    missing = sorted(set(schema.get("required", [])) - set(arguments))
    if missing:
        raise ToolArgumentError(
            f"Missing required argument(s) for {tool_name}: {', '.join(missing)}"
        )


def _optional_string(arguments: dict[str, Any], name: str) -> str | None:
    value = arguments.get(name)
    if value is None:
        return None
    if not isinstance(value, str) or not value:
        raise ToolArgumentError(f"{name} must be a non-empty string.")
    return value


def _required_string(arguments: dict[str, Any], name: str) -> str:
    value = _optional_string(arguments, name)
    if value is None:
        raise ToolArgumentError(f"{name} is required.")
    return value


def _boolean(arguments: dict[str, Any], name: str, default: bool) -> bool:
    value = arguments.get(name, default)
    if not isinstance(value, bool):
        raise ToolArgumentError(f"{name} must be a boolean.")
    return value


def _limit(arguments: dict[str, Any], default: int = DEFAULT_LIMIT) -> int:
    value = arguments.get("limit", default)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ToolArgumentError("limit must be an integer.")
    if not 1 <= value <= MAX_LIMIT:
        raise ToolArgumentError(f"limit must be between 1 and {MAX_LIMIT}.")
    return value


def _resolve_instance(arguments: dict[str, Any]) -> Instance:
    return select_instance(
        discover_instances(),
        _optional_string(arguments, "instance_id"),
    )


def _required_status_field(status: dict[str, str], field: str) -> str:
    value = status.get(field)
    if not value:
        raise XXMIError(
            "PROTOCOL_ERROR",
            f"Native STATUS response is missing {field}.",
        )
    return value


def _status_for(instance: Instance) -> dict[str, str]:
    deadline = time.monotonic() + STATUS_RETRY_TIMEOUT_S
    transient_seen = False
    while True:
        try:
            return send_pipe_command(instance, COMMAND_STATUS)
        except (NativePipeError, XXMIError) as error:
            retryable = error.code in {
                "render_timeout",
                "PIPE_BUSY",
                "PIPE_TIMEOUT",
            } or (error.code == "INSTANCE_GONE" and transient_seen)
            if not retryable or time.monotonic() >= deadline:
                raise
            transient_seen = True
            time.sleep(STATUS_RETRY_DELAY_S)


def _log_path_from_status(status: dict[str, str]) -> str:
    if status.get("log_enabled") == "0":
        raise XXMIError(
            "LOG_DISABLED",
            "Native logging is disabled; enable log_level in d3dx.ini to get runtime log evidence.",
        )
    return _required_status_field(status, "log_path")


def _analysis_root_from_status(status: dict[str, str]) -> str:
    analysis_root = status.get("analysis_root")
    if analysis_root:
        return analysis_root
    dll_path = status.get("dll_path")
    if dll_path:
        return str(Path(dll_path).parent)
    raise XXMIError(
        "PROTOCOL_ERROR",
        "Native STATUS response is missing analysis_root and dll_path.",
    )


def _with_instance_metadata(
    instance: Instance,
    values: dict[str, str],
) -> dict[str, Any]:
    return {
        **values,
        "instance_id": instance.instance_id,
        "pipe_name": instance.pipe_name,
    }


def _tool_list_instances(arguments: dict[str, Any]) -> tuple[dict[str, Any], str]:
    instances = discover_instances()

    def probe(instance: Instance) -> dict[str, Any]:
        item: dict[str, Any] = {
            "instance_id": instance.instance_id,
            "pid": instance.pid,
            "instance": instance.instance,
            "pipe_name": instance.pipe_name,
        }
        try:
            item.update(_status_for(instance))
        except XXMIError as error:
            item["error"] = error.code
            item["error_message"] = error.message
        return item

    if instances:
        with ThreadPoolExecutor(max_workers=min(8, len(instances))) as executor:
            items = list(executor.map(probe, instances))
    else:
        items = []
    return (
        {"count": len(items), "instances": items},
        f"Found {len(items)} XXMI instance(s).",
    )


def _tool_status(arguments: dict[str, Any]) -> tuple[dict[str, Any], str]:
    instance = _resolve_instance(arguments)
    status = _with_instance_metadata(instance, _status_for(instance))
    summary = (
        f"XXMI instance {instance.instance_id}: "
        f"frame={status.get('frame', '?')}, fps={status.get('fps', '?')}, "
        f"config_initialized={status.get('config_initialized', '?')}."
    )
    return status, summary


def _tool_read_log(arguments: dict[str, Any]) -> tuple[dict[str, Any], str]:
    instance = _resolve_instance(arguments)
    status = _status_for(instance)
    log_path = _log_path_from_status(status)
    page = read_text_lines(
        log_path,
        tail=_boolean(arguments, "tail", True),
        search=_optional_string(arguments, "search"),
        limit=_limit(arguments),
    )
    page["instance_id"] = instance.instance_id
    return (
        page,
        f"Read {page['line_count']} line(s) from {page['path']}"
        + (" (truncated)." if page["truncated"] else "."),
    )


def _tool_list_frame_analyses(
    arguments: dict[str, Any],
) -> tuple[dict[str, Any], str]:
    instance = _resolve_instance(arguments)
    status = _status_for(instance)
    page = list_frame_analysis_directories(
        _analysis_root_from_status(status),
        limit=_limit(arguments, 20),
    )
    page["instance_id"] = instance.instance_id
    return (
        page,
        f"Found {page['count']} frame analysis directorie(s).",
    )


def _tool_read_frame_analysis(
    arguments: dict[str, Any],
) -> tuple[dict[str, Any], str]:
    instance = _resolve_instance(arguments)
    status = _status_for(instance)
    page = read_frame_analysis_log(
        _analysis_root_from_status(status),
        _required_string(arguments, "analysis"),
        search=_optional_string(arguments, "search"),
        tail=_boolean(arguments, "tail", True),
        limit=_limit(arguments),
    )
    page["instance_id"] = instance.instance_id
    return (
        page,
        f"Read {page['line_count']} line(s) from {page['path']}"
        + (" (truncated)." if page["truncated"] else "."),
    )


def _tool_trigger_frame_capture(
    arguments: dict[str, Any],
) -> tuple[dict[str, Any], str]:
    instance = _resolve_instance(arguments)
    response = _with_instance_metadata(
        instance,
        send_pipe_command(instance, COMMAND_FRAME_ANALYSIS),
    )
    result = response.get("result", "accepted")
    path = response.get("analysis_path", "")
    summary = f"Frame analysis {result}"
    if path:
        summary += f": {path}"
    return response, summary + "."


def _tool_reload(arguments: dict[str, Any]) -> tuple[dict[str, Any], str]:
    target = _required_string(arguments, "target")
    if target not in {"config", "shaders"}:
        raise ToolArgumentError("target must be config or shaders.")
    instance = _resolve_instance(arguments)
    command = (
        COMMAND_RELOAD_CONFIG if target == "config" else COMMAND_RELOAD_SHADERS
    )
    timeout_s = (
        RELOAD_CONFIG_TIMEOUT_S
        if command == COMMAND_RELOAD_CONFIG
        else DEFAULT_TIMEOUT_S
    )
    response = _with_instance_metadata(
        instance,
        send_pipe_command(instance, command, timeout_s=timeout_s),
    )
    if response.get("result") == "failed" or response.get("error"):
        code = response.get("error") or "RELOAD_FAILED"
        message = response.get("error_message") or "Native reload failed."
        raise XXMIError(code, message)
    if target == "config":
        summary = f"Config reload {response.get('result', 'queued')}."
    else:
        summary = f"Shaders reload {response.get('result', 'ok')}."
    return response, summary


def _run_live_probe_command(
    arguments: dict[str, Any],
    command: int,
    label: str,
    timeout_s: float = DEFAULT_TIMEOUT_S,
) -> tuple[dict[str, Any], str]:
    instance = _resolve_instance(arguments)
    response = _with_instance_metadata(
        instance,
        send_pipe_command(instance, command, timeout_s=timeout_s),
    )
    if response.get("error"):
        raise XXMIError(
            response.get("error", "LIVE_PROBE_FAILED"),
            response.get("error_message", f"{label} failed."),
        )
    result = response.get("result", "captured")
    snapshot_path = response.get("snapshot_path", "")
    summary = f"{label} {result}"
    if snapshot_path:
        summary += f": {snapshot_path}"
    return response, summary + "."


def _tool_live_context(
    arguments: dict[str, Any],
) -> tuple[dict[str, Any], str]:
    return _run_live_probe_command(
        arguments,
        COMMAND_LIVE_CONTEXT,
        "Live context",
    )


def _tool_runtime_variables(
    arguments: dict[str, Any],
) -> tuple[dict[str, Any], str]:
    return _run_live_probe_command(
        arguments,
        COMMAND_RUNTIME_VARIABLES,
        "Runtime variables",
    )


def _tool_live_draw_snapshot(
    arguments: dict[str, Any],
) -> tuple[dict[str, Any], str]:
    return _run_live_probe_command(
        arguments,
        COMMAND_LIVE_DRAW_SNAPSHOT,
        "Live draw snapshot",
        timeout_s=LIVE_DRAW_TIMEOUT_S,
    )


def _tool_watch_draw_delta(
    arguments: dict[str, Any],
) -> tuple[dict[str, Any], str]:
    raise ToolArgumentError(
        "xxmi_watch_draw_delta is not available in this build."
    )


def _tool_probe_resource(
    arguments: dict[str, Any],
) -> tuple[dict[str, Any], str]:
    raise ToolArgumentError(
        "xxmi_probe_resource is not available in this build."
    )


def _tool_override_trace(
    arguments: dict[str, Any],
) -> tuple[dict[str, Any], str]:
    raise ToolArgumentError(
        "xxmi_override_trace is not available in this build."
    )


TOOL_HANDLERS = {
    "xxmi_list_instances": _tool_list_instances,
    "xxmi_status": _tool_status,
    "xxmi_read_log": _tool_read_log,
    "xxmi_list_frame_analyses": _tool_list_frame_analyses,
    "xxmi_read_frame_analysis": _tool_read_frame_analysis,
    "xxmi_trigger_frame_capture": _tool_trigger_frame_capture,
    "xxmi_reload": _tool_reload,
    "xxmi_live_context": _tool_live_context,
    "xxmi_runtime_variables": _tool_runtime_variables,
    "xxmi_live_draw_snapshot": _tool_live_draw_snapshot,
    "xxmi_watch_draw_delta": _tool_watch_draw_delta,
    "xxmi_probe_resource": _tool_probe_resource,
    "xxmi_override_trace": _tool_override_trace,
}


def _tool_error_result(error: XXMIError) -> dict[str, Any]:
    return {
        "content": [{"type": "text", "text": f"{error.code}: {error.message}"}],
        "structuredContent": {
            "error": error.code,
            "error_message": error.message,
        },
        "isError": True,
    }


def call_tool(name: str, arguments: dict[str, Any]) -> dict[str, Any]:
    handler = TOOL_HANDLERS.get(name)
    if handler is None:
        raise ToolArgumentError(f"Unknown tool: {name}")
    _validate_argument_keys(name, arguments)
    structured, summary = handler(arguments)
    return {
        "content": [{"type": "text", "text": summary}],
        "structuredContent": structured,
        "isError": False,
    }


def _jsonrpc_result(request_id: Any, result: Any) -> dict[str, Any]:
    return {"jsonrpc": "2.0", "id": request_id, "result": result}


def _jsonrpc_error(
    request_id: Any,
    code: int,
    message: str,
) -> dict[str, Any]:
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "error": {"code": code, "message": message},
    }


def handle_rpc(message: dict[str, Any]) -> dict[str, Any] | None:
    method = message.get("method")
    request_id = message.get("id")
    has_id = "id" in message and request_id is not None
    params = message.get("params", {})
    if params is None:
        params = {}
    if not isinstance(params, dict):
        return (
            _jsonrpc_error(request_id, -32602, "params must be an object")
            if has_id
            else None
        )

    if method == "initialize":
        requested_version = params.get("protocolVersion")
        protocol_version = (
            requested_version
            if requested_version in SUPPORTED_MCP_VERSIONS
            else LATEST_MCP_VERSION
        )
        return _jsonrpc_result(
            request_id,
            {
                "protocolVersion": protocol_version,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "xxmi-debug", "version": "0.1.6"},
            },
        )
    if method == "notifications/initialized":
        return None
    if method == "ping":
        return _jsonrpc_result(request_id, {})
    if method == "tools/list":
        return _jsonrpc_result(request_id, {"tools": TOOL_DEFINITIONS})
    if method == "tools/call":
        name = params.get("name")
        arguments = params.get("arguments", {})
        if not isinstance(name, str) or not name:
            return _jsonrpc_error(request_id, -32602, "tools/call name is required")
        if not isinstance(arguments, dict):
            return _jsonrpc_error(request_id, -32602, "arguments must be an object")
        try:
            return _jsonrpc_result(request_id, call_tool(name, arguments))
        except XXMIError as error:
            return _jsonrpc_result(request_id, _tool_error_result(error))
        except Exception as error:
            sys.stderr.write(f"xxmi-debug unexpected tool error: {error}\n")
            return _jsonrpc_result(
                request_id,
                _tool_error_result(
                    XXMIError("INTERNAL_ERROR", "The XXMI MCP tool failed.")
                ),
            )

    if not has_id:
        return None
    return _jsonrpc_error(request_id, -32601, f"Method not found: {method}")


def _write_message(message: dict[str, Any]) -> None:
    sys.stdout.write(
        json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n"
    )
    sys.stdout.flush()


def process_message(message: dict[str, Any]) -> dict[str, Any] | None:
    response = handle_rpc(message)
    return None if "id" not in message else response


def main() -> int:
    for raw_line in sys.stdin.buffer:
        if not raw_line.strip():
            continue
        request_id = None
        try:
            message = json.loads(raw_line)
            if not isinstance(message, dict):
                raise ValueError("JSON-RPC message must be an object")
            request_id = message.get("id")
            response = process_message(message)
        except (json.JSONDecodeError, ValueError) as error:
            response = _jsonrpc_error(None, -32700, f"Parse error: {error}")
        except Exception as error:
            sys.stderr.write(f"xxmi-debug request failed: {error}\n")
            response = _jsonrpc_error(
                request_id,
                -32603,
                "Internal server error",
            )
        if response is not None:
            _write_message(response)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
