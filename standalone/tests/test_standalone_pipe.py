from __future__ import annotations

import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import re
import struct
import time
import unittest


REQUEST_MAGIC = 0x3150434D
PROTOCOL_VERSION = 1
REQUEST_HEADER = struct.Struct("<IHHII")
RESPONSE_HEADER = struct.Struct("<IIII")
PIPE_PREFIX = "xxmi-mcp-v1-"
PIPE_PATTERN = re.compile(r"^" + re.escape(PIPE_PREFIX) + r"(\d+)-([0-9a-fA-F]{8})$")

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
ERROR_FILE_NOT_FOUND = 2
ERROR_PIPE_BUSY = 231

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
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
_kernel32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
_kernel32.WaitNamedPipeW.restype = wintypes.BOOL
_kernel32.ReadFile.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.c_void_p,
]
_kernel32.ReadFile.restype = wintypes.BOOL
_kernel32.WriteFile.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.c_void_p,
]
_kernel32.WriteFile.restype = wintypes.BOOL
_kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
_kernel32.CloseHandle.restype = wintypes.BOOL


def find_pipe(pid: int, timeout_s: float = 5.0) -> str:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for name in os.listdir("\\\\.\\pipe\\"):
            match = PIPE_PATTERN.fullmatch(name)
            if match and int(match.group(1)) == pid:
                return "\\\\.\\pipe\\" + name
        time.sleep(0.05)
    raise TimeoutError(f"mcp pipe for pid {pid} was not created")


def read_exact(handle: int, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        buffer = ctypes.create_string_buffer(size - len(result))
        transferred = wintypes.DWORD()
        if not _kernel32.ReadFile(
            handle,
            buffer,
            len(buffer),
            ctypes.byref(transferred),
            None,
        ):
            raise ctypes.WinError(ctypes.get_last_error())
        if transferred.value == 0:
            raise EOFError("pipe closed")
        result.extend(buffer.raw[: transferred.value])
    return bytes(result)


def write_all(handle: int, data: bytes) -> None:
    offset = 0
    while offset < len(data):
        buffer = ctypes.create_string_buffer(data[offset:])
        transferred = wintypes.DWORD()
        if not _kernel32.WriteFile(
            handle,
            buffer,
            len(data) - offset,
            ctypes.byref(transferred),
            None,
        ):
            raise ctypes.WinError(ctypes.get_last_error())
        if transferred.value == 0:
            raise EOFError("pipe closed")
        offset += transferred.value


def send(
    pipe: str,
    command: int,
    request_id: int,
    magic: int = REQUEST_MAGIC,
    payload_size: int = 0,
):
    deadline = time.monotonic() + 5.0
    while True:
        handle = _kernel32.CreateFileW(
            pipe,
            GENERIC_READ | GENERIC_WRITE,
            0,
            None,
            OPEN_EXISTING,
            0,
            None,
        )
        if handle and handle != INVALID_HANDLE_VALUE:
            break
        error = ctypes.get_last_error()
        if error not in {ERROR_FILE_NOT_FOUND, ERROR_PIPE_BUSY}:
            raise ctypes.WinError(error)
        if time.monotonic() >= deadline:
            raise ctypes.WinError(error)
        if error == ERROR_PIPE_BUSY:
            remaining_ms = max(
                1, int((deadline - time.monotonic()) * 1000)
            )
            _kernel32.WaitNamedPipeW(pipe, remaining_ms)
        else:
            time.sleep(0.01)
    try:
        write_all(
            handle,
            REQUEST_HEADER.pack(
                magic,
                PROTOCOL_VERSION,
                command,
                request_id,
                payload_size,
            ),
        )
        header = read_exact(handle, RESPONSE_HEADER.size)
        response_magic, status, response_id, payload_size = RESPONSE_HEADER.unpack(
            header
        )
        payload = read_exact(handle, payload_size)
        return response_magic, status, response_id, payload.decode("utf-8")
    finally:
        _kernel32.CloseHandle(handle)


class StandalonePipeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dll_path = Path(
            os.environ.get(
                "XXMI_MCP_DLL",
                str(
                    Path(__file__).parents[1]
                    / "MCPStandalone"
                    / "x64"
                    / "Debug"
                    / "mcp.dll"
                ),
            )
        )
        if not cls.dll_path.is_file():
            raise unittest.SkipTest(f"missing test DLL: {cls.dll_path}")
        cls.dll = ctypes.WinDLL(str(cls.dll_path))
        cls.pipe = find_pipe(os.getpid())

    def test_ping(self):
        magic, status, request_id, payload = send(self.pipe, 5, 123)
        self.assertEqual(magic, REQUEST_MAGIC)
        self.assertEqual(status, 0)
        self.assertEqual(request_id, 123)
        self.assertIn("pong=1", payload)

    def test_status_degrades_to_read_only(self):
        magic, status, request_id, payload = send(self.pipe, 1, 124)
        self.assertEqual(magic, REQUEST_MAGIC)
        self.assertEqual(status, 0)
        self.assertEqual(request_id, 124)
        values = dict(
            line.split("=", 1) if "=" in line else (line, "")
            for line in payload.splitlines()
            if line
        )
        self.assertEqual(values.get("adapter_mode"), "read_only")
        self.assertEqual(values.get("capabilities"), "")
        self.assertEqual(values.get("adapter_capabilities"), "")
        self.assertEqual(values.get("frame"), "0")
        self.assertEqual(values.get("adapter_ready"), "0")
        self.assertIn(
            values.get("capture_state"),
            {
                "waiting_for_module",
                "waiting_for_device",
                "waiting_for_present",
                "timed_out",
                "present_mismatch",
                "hook_failed",
            },
        )
        self.assertEqual(values.get("device_captured"), "0")
        self.assertEqual(values.get("present_hooked"), "0")
        self.assertEqual(values.get("capture_restore_hunting"), "-1")
        self.assertNotIn("reload_shaders_pending", values)
        self.assertNotIn("last_reload_shaders_result", values)
        self.assertIn("adapter_module_sha256", values)

    def test_bad_magic_returns_protocol_error(self):
        magic, status, request_id, payload = send(
            self.pipe,
            5,
            125,
            magic=0xDEADBEEF,
        )
        self.assertEqual(magic, REQUEST_MAGIC)
        self.assertNotEqual(status, 0)
        self.assertEqual(request_id, 125)
        self.assertIn("bad_magic", payload)

    def test_oversized_payload_returns_protocol_error(self):
        magic, status, request_id, payload = send(
            self.pipe,
            5,
            126,
            payload_size=16 * 1024 + 1,
        )
        self.assertEqual(magic, REQUEST_MAGIC)
        self.assertNotEqual(status, 0)
        self.assertEqual(request_id, 126)
        self.assertIn("payload_too_large", payload)

    def test_control_command_degrades_without_ready_adapter(self):
        magic, status, request_id, payload = send(self.pipe, 3, 127)
        self.assertEqual(magic, REQUEST_MAGIC)
        self.assertNotEqual(status, 0)
        self.assertEqual(request_id, 127)
        self.assertIn("adapter_not_ready", payload)


if __name__ == "__main__":
    unittest.main()
