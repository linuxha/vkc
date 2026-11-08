from pathlib import Path

from vkc import KansasCityCodec, decode, decode_file, encode, encode_file


def test_kansas_city_round_trip() -> None:
    payload = b"Virtual Kansas City tape emulator\n"
    waveform = encode(payload)
    assert waveform
    assert decode(waveform) == payload


def test_codec_uses_uart_framing() -> None:
    codec = KansasCityCodec()
    payload = b"A"
    waveform = codec.encode(payload)
    assert len(waveform) > 0
    assert codec.decode(waveform) == payload


def test_daemon_handles_payload() -> None:
    from vkc.daemon import TapeDaemon

    daemon = TapeDaemon()
    payload = b"Pico W"
    assert daemon.decode(daemon.encode(payload)) == payload


def test_file_helpers_round_trip(tmp_path: Path) -> None:
    source = tmp_path / "input.txt"
    waveform = tmp_path / "input.kc"
    decoded = tmp_path / "output.txt"

    source.write_bytes(b"play demo.bin\n")
    encode_file(source, waveform)
    assert waveform.exists()
    assert decode_file(waveform, decoded) == source.read_bytes()
    assert decoded.read_bytes() == source.read_bytes()
