import ctypes
from ctypes import wintypes
import struct
import threading


INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
PIPE_ACCESS_DUPLEX = 0x00000003
PIPE_TYPE_BYTE = 0x00000000
PIPE_READMODE_BYTE = 0x00000000
PIPE_TYPE_MESSAGE = 0x00000004
PIPE_READMODE_MESSAGE = 0x00000002
PIPE_WAIT = 0x00000000
PIPE_REJECT_REMOTE_CLIENTS = 0x00000008
ERROR_PIPE_CONNECTED = 535
REQUEST_HEADER = struct.Struct("<IHHII")
RESPONSE_HEADER = struct.Struct("<IIII")


_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_kernel32.CreateNamedPipeW.argtypes = [
    wintypes.LPCWSTR,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.DWORD,
    ctypes.c_void_p,
]
_kernel32.CreateNamedPipeW.restype = wintypes.HANDLE
_kernel32.ConnectNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
_kernel32.ConnectNamedPipe.restype = wintypes.BOOL
_kernel32.DisconnectNamedPipe.argtypes = [wintypes.HANDLE]
_kernel32.DisconnectNamedPipe.restype = wintypes.BOOL
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


def _read_exact(handle, size):
    buffer = ctypes.create_string_buffer(size)
    offset = 0
    while offset < size:
        transferred = wintypes.DWORD()
        ok = _kernel32.ReadFile(
            handle,
            ctypes.byref(buffer, offset),
            size - offset,
            ctypes.byref(transferred),
            None,
        )
        if not ok:
            raise ctypes.WinError(ctypes.get_last_error())
        if transferred.value == 0:
            raise EOFError("named pipe closed before the full message arrived")
        offset += transferred.value
    return buffer.raw[:size]


def _write_all(handle, data):
    buffer = ctypes.create_string_buffer(data)
    offset = 0
    while offset < len(data):
        transferred = wintypes.DWORD()
        ok = _kernel32.WriteFile(
            handle,
            ctypes.byref(buffer, offset),
            len(data) - offset,
            ctypes.byref(transferred),
            None,
        )
        if not ok:
            raise ctypes.WinError(ctypes.get_last_error())
        if transferred.value == 0:
            raise EOFError("named pipe closed while writing the response")
        offset += transferred.value


class MockPipeServer:
    def __init__(
        self,
        pipe_name,
        payload=b"",
        status=0,
        response_request_id=None,
        message_mode=False,
    ):
        self.pipe_name = pipe_name
        self.payload = payload
        self.status = status
        self.response_request_id = response_request_id
        self.message_mode = message_mode
        self.request = None
        self.error = None
        self._handle = None
        self._thread = None

    def __enter__(self):
        self._handle = _kernel32.CreateNamedPipeW(
            self.pipe_name,
            PIPE_ACCESS_DUPLEX,
            (
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE
                if self.message_mode
                else PIPE_TYPE_BYTE | PIPE_READMODE_BYTE
            )
            | PIPE_WAIT
            | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            4096,
            4096,
            0,
            None,
        )
        if not self._handle or self._handle == INVALID_HANDLE_VALUE:
            raise ctypes.WinError(ctypes.get_last_error())
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, exc_type, exc, traceback):
        if self._thread is not None:
            self._thread.join(timeout=3)
        if self._handle:
            _kernel32.DisconnectNamedPipe(self._handle)
            _kernel32.CloseHandle(self._handle)
            self._handle = None
        if exc_type is None and self.error is not None:
            raise self.error
        if exc_type is None and self._thread is not None and self._thread.is_alive():
            raise TimeoutError("mock pipe server did not finish")

    def _serve(self):
        try:
            connected = _kernel32.ConnectNamedPipe(self._handle, None)
            if not connected and ctypes.get_last_error() != ERROR_PIPE_CONNECTED:
                raise ctypes.WinError(ctypes.get_last_error())

            header = _read_exact(self._handle, REQUEST_HEADER.size)
            magic, version, command, request_id, payload_size = REQUEST_HEADER.unpack(header)
            payload = _read_exact(self._handle, payload_size)
            self.request = {
                "magic": magic,
                "version": version,
                "command": command,
                "request_id": request_id,
                "payload": payload,
            }

            response_request_id = (
                request_id
                if self.response_request_id is None
                else self.response_request_id
            )
            response = RESPONSE_HEADER.pack(
                magic,
                self.status,
                response_request_id,
                len(self.payload),
            ) + self.payload
            _write_all(self._handle, response)
            _kernel32.FlushFileBuffers(self._handle)
        except Exception as error:
            self.error = error
