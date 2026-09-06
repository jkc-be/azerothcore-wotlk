"""One controller connection, serialized requests, no automatic action replay."""

import json
import socket
import threading
import time


class ProtocolError(RuntimeError):
    """The peer violated the protocol; this connection has been discarded."""


class OutcomeUnknown(ConnectionError):
    """Transport failed; the action may already have executed. Never blindly retry."""


class ActionRejected(RuntimeError):
    """The server rejected the request without a successful observation."""


class Client:
    MAX_RESPONSE = 65536

    def __init__(self, host="127.0.0.1", port=5001, timeout=10.0):
        if timeout <= 0:
            raise ValueError("timeout must be positive")
        self._socket = socket.create_connection((host, port), timeout=timeout)
        self._socket.settimeout(timeout)
        self._timeout = timeout
        self._buffer = bytearray()
        self._next_id = 0
        self._lock = threading.Lock()

    def _read_line(self, deadline):
        while True:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                if newline >= self.MAX_RESPONSE:
                    raise ProtocolError("response too large")
                line = bytes(self._buffer[:newline])
                del self._buffer[:newline + 1]
                return line
            if len(self._buffer) >= self.MAX_RESPONSE:
                raise ProtocolError("response too large")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("response deadline expired")
            self._socket.settimeout(remaining)
            chunk = self._socket.recv(min(8192, self.MAX_RESPONSE - len(self._buffer)))
            if not chunk:
                raise EOFError("connection closed before acknowledgment")
            self._buffer.extend(chunk)

    def request(self, operation, **fields):
        if {"version", "id", "op"} & fields.keys():
            raise ValueError("reserved request field")
        with self._lock:
            if self._socket is None:
                raise ConnectionError("client is closed")
            self._next_id += 1
            request_id = str(self._next_id)
            # Version 1 requests have flat scalar strings; responses use real JSON types.
            request = {"version": "1", "id": request_id, "op": operation}
            request.update({key: str(value) for key, value in fields.items()})
            wire = json.dumps(request, separators=(",", ":"), allow_nan=False).encode() + b"\n"
            if len(wire) > 4096:
                raise ValueError("request too large")
            try:
                deadline = time.monotonic() + self._timeout
                self._socket.settimeout(self._timeout)
                self._socket.sendall(wire)
                response = json.loads(self._read_line(deadline))
                if not isinstance(response, dict) or type(response.get("version")) is not int:
                    raise ProtocolError("invalid response envelope")
                if response["version"] != 1 or response.get("id") != request_id:
                    raise ProtocolError("version or request ID mismatch")
                if type(response.get("ok")) is not bool:
                    raise ProtocolError("ok must be a JSON boolean")
            except (ProtocolError, ValueError, UnicodeError) as error:
                self.close()
                raise ProtocolError(str(error)) from error
            except (OSError, EOFError) as error:
                self.close()
                raise OutcomeUnknown("request was not acknowledged; its outcome is unknown") from error
            if not response["ok"]:
                raise ActionRejected(response.get("error", "request_rejected"))
            return response

    def list_bots(self):
        return self.request("list")["bots"]

    def close(self):
        connection, self._socket = self._socket, None
        if connection is not None:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            connection.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
