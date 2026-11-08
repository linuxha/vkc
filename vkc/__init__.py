"""Public package interface for the Virtual Kansas City emulator."""

from __future__ import annotations

from pathlib import Path

from .codec import KansasCityCodec

__all__ = ["KansasCityCodec", "encode", "decode", "encode_file", "decode_file"]


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


def encode_file(source: str | Path, destination: str | Path, **kwargs) -> bytes:
    """Read a binary file, encode it as Kansas City tape data, and write it to disk."""
    source_path = Path(source)
    destination_path = Path(destination)
    waveform = encode(source_path.read_bytes(), **kwargs)
    destination_path.write_bytes(waveform)
    return waveform


def decode_file(source: str | Path, destination: str | Path, **kwargs) -> bytes:
    """Read a Kansas City waveform file, decode it, and write the original bytes back."""
    source_path = Path(source)
    destination_path = Path(destination)
    payload = decode(source_path.read_bytes(), **kwargs)
    destination_path.write_bytes(payload)
    return payload
