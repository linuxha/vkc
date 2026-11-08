"""Public package interface for the Virtual Kansas City emulator."""

from .codec import KansasCityCodec

__all__ = ["KansasCityCodec"]


def encode(payload: bytes, **kwargs) -> bytes:
    """Encode raw bytes as a Kansas City tape waveform."""
    return KansasCityCodec(**kwargs).encode(payload)


def decode(payload: bytes, **kwargs) -> bytes:
    """Decode a Kansas City tape waveform back to raw bytes."""
    return KansasCityCodec(**kwargs).decode(payload)
