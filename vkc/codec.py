from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Iterable, Sequence


@dataclass(frozen=True)
class TapeConfig:
    baud: int = 1200
    low_tone_hz: int = 1200
    high_tone_hz: int = 2400
    sample_rate_hz: int = 24000


class KansasCityCodec:
    """Encode and decode bytes using the Kansas City Standard framing format.

    The data stream is framed as a UART-style byte with a 0 start bit, eight data
    bits (LSB first), and a 1 stop bit. Each bit is encoded as one tone period:
    1200 Hz for 0 and 2400 Hz for 1.
    """

    def __init__(
        self,
        baud: int = 1200,
        low_tone_hz: int = 1200,
        high_tone_hz: int = 2400,
        sample_rate_hz: int = 24000,
    ) -> None:
        self.config = TapeConfig(
            baud=baud,
            low_tone_hz=low_tone_hz,
            high_tone_hz=high_tone_hz,
            sample_rate_hz=sample_rate_hz,
        )

    @property
    def bit_samples(self) -> int:
        return max(1, round(self.config.sample_rate_hz / self.config.baud))

    def _tone_samples(self, frequency_hz: int, samples: int) -> list[int]:
        if samples <= 0:
            return []
        waveform: list[int] = []
        for index in range(samples):
            phase = 2.0 * math.pi * frequency_hz * index / self.config.sample_rate_hz
            value = 128 + int(127 * math.sin(phase))
            waveform.append(max(0, min(255, value)))
        return waveform

    def _decode_bit(self, segment: Sequence[int]) -> int:
        if not segment:
            return 0
        centered = [byte - 128 for byte in segment]
        low_score = 0.0
        high_score = 0.0
        for index, sample in enumerate(centered):
            low_phase = 2.0 * math.pi * self.config.low_tone_hz * index / self.config.sample_rate_hz
            high_phase = 2.0 * math.pi * self.config.high_tone_hz * index / self.config.sample_rate_hz
            low_score += sample * math.sin(low_phase)
            high_score += sample * math.sin(high_phase)
        return 1 if abs(high_score) > abs(low_score) else 0

    def encode_bits(self, bits: Iterable[int]) -> bytes:
        """Convert a UART bit stream into a PCM waveform."""
        stream = bytearray()
        for bit in bits:
            if bit not in (0, 1):
                raise ValueError(f"Expected bit values 0 or 1, got {bit!r}")
            tone_hz = self.config.high_tone_hz if bit else self.config.low_tone_hz
            stream.extend(self._tone_samples(tone_hz, self.bit_samples))
        return bytes(stream)

    def encode(self, payload: bytes) -> bytes:
        bits: list[int] = []
        for byte in payload:
            bits.append(0)
            for shift in range(8):
                bits.append((byte >> shift) & 1)
            bits.append(1)
        return self.encode_bits(bits)

    def decode_bits(self, bitstream: Sequence[int]) -> bytes:
        if not bitstream:
            return b""
        bytes_out = bytearray()
        for index in range(0, len(bitstream) - 9, 10):
            start = bitstream[index]
            stop = bitstream[index + 9]
            if start != 0 or stop != 1:
                continue
            value = 0
            for bit_index in range(8):
                value |= bitstream[index + 1 + bit_index] << bit_index
            bytes_out.append(value)
        return bytes(bytes_out)

    def decode(self, payload: bytes) -> bytes:
        """Decode a PCM waveform produced by :meth:`encode` and return the original bytes."""
        if not payload:
            return b""
        if all(byte in (0, 1) for byte in payload):
            return self.decode_bits([int(bit) for bit in payload])

        bitstream: list[int] = []
        for index in range(0, len(payload), self.bit_samples):
            segment = payload[index : index + self.bit_samples]
            if not segment:
                continue
            bitstream.append(self._decode_bit(segment))
        return self.decode_bits(bitstream)


__all__ = ["KansasCityCodec", "TapeConfig"]
