"""Public package interface for the Virtual Kansas City emulator."""

from .codec import KansasCityCodec

__all__ = ["KansasCityCodec", "encode", "decode"]


def _as_bytes(payload: bytes | bytearray | memoryview | str) -> bytes:
    if isinstance(payload, str):
        return payload.encode("utf-8")
    return bytes(payload)


def encode(payload: bytes | bytearray | memoryview | str, **kwargs) -> bytes:
    """Encode raw bytes as a Kansas City tape waveform."""
    return KansasCityCodec(**kwargs).encode(_as_bytes(payload))


def decode(payload: bytes | bytearray | memoryview | str, **kwargs) -> bytes:
    """Decode a Kansas City tape waveform back to raw bytes."""
    return KansasCityCodec(**kwargs).decode(_as_bytes(payload))
