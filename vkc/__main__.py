from __future__ import annotations

import argparse
import binascii

from . import decode, encode


def _read_bytes(value: str) -> bytes:
    try:
        return bytes.fromhex(value)
    except ValueError:
        return value.encode("utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="Virtual Kansas City tape tool")
    subparsers = parser.add_subparsers(dest="command", required=True)

    encode_parser = subparsers.add_parser("encode", help="Encode a payload with Kansas City framing")
    encode_parser.add_argument("payload", help="ASCII payload to encode or a hex string prefixed with 0x")
    encode_parser.add_argument("--hex", action="store_true", help="Print the waveform as hex instead of raw bytes")

    decode_parser = subparsers.add_parser("decode", help="Decode a Kansas City waveform back to bytes")
    decode_parser.add_argument("waveform", help="Hexadecimal waveform bytes to decode")

    args = parser.parse_args()
    if args.command == "encode":
        payload = _read_bytes(args.payload)
        waveform = encode(payload)
        if args.hex:
            print(waveform.hex())
        else:
            print(waveform)
        return

    waveform = bytes.fromhex(args.waveform)
    decoded = decode(waveform)
    try:
        print(decoded.decode("utf-8"))
    except UnicodeDecodeError:
        print(decoded.hex())


if __name__ == "__main__":
    main()
