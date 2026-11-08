from __future__ import annotations

import argparse
import socket
import threading
from typing import Optional

from .codec import KansasCityCodec


class TapeDaemon:
    """A small TCP daemon that encodes and decodes Kansas City audio streams."""

    def __init__(self, host: str = "127.0.0.1", port: int = 9000, codec: Optional[KansasCityCodec] = None) -> None:
        self.host = host
        self.port = port
        self.codec = codec or KansasCityCodec()
        self._server: Optional[socket.socket] = None
        self._lock = threading.Lock()

    def encode(self, payload: bytes) -> bytes:
        return self.codec.encode(payload)

    def decode(self, payload: bytes) -> bytes:
        return self.codec.decode(payload)

    def handle_client(self, data: bytes) -> bytes:
        """Process a payload and return the emulated tape waveform."""
        return self.encode(data)

    def serve_forever(self, host: Optional[str] = None, port: Optional[int] = None) -> None:
        host = host or self.host
        port = port or self.port
        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server.bind((host, port))
        self._server.listen(5)
        while True:
            connection, _ = self._server.accept()
            with connection:
                request = connection.recv(65536)
                if not request:
                    continue
                connection.sendall(self.handle_client(request))

    def close(self) -> None:
        if self._server is not None:
            self._server.close()
            self._server = None


def main() -> None:
    parser = argparse.ArgumentParser(description="Virtual Kansas City tape daemon")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--payload", default="hello")
    args = parser.parse_args()

    daemon = TapeDaemon(host=args.host, port=args.port)
    encoded = daemon.encode(args.payload.encode())
    print(encoded.hex())


if __name__ == "__main__":
    main()
