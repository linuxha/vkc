from vkc import KansasCityCodec, decode, encode


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
