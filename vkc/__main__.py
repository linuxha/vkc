from __future__ import annotations

import argparse
from pathlib import Path

from . import decode, decode_file, encode, encode_file


def _read_payload(value: str) -> bytes:
    try:
        return bytes.fromhex(value)
    except ValueError:
        return value.encode("utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="Virtual Kansas City tape tool")
    subparsers = parser.add_subparsers(dest="command", required=True)

    encode_parser = subparsers.add_parser("encode", help="Encode a payload or file into Kansas City waveform bytes")
    encode_parser.add_argument("payload", help="Payload text or a hexadecimal string to encode")
    encode_parser.add_argument("--file", action="store_true", help="Treat payload as a path to a file to encode to disk")
    encode_parser.add_argument("--output", help="Optional output file for waveform data")
    encode_parser.add_argument("--hex", action="store_true", help="Print waveform as hex instead of raw bytes")

    decode_parser = subparsers.add_parser("decode", help="Decode a Kansas City waveform from hex or a file")
    decode_parser.add_argument("waveform", help="Hexadecimal waveform to decode or a path to a waveform file")
    decode_parser.add_argument("--output", help="Optional output file for decoded payload bytes")

    args = parser.parse_args()

    if args.command == "encode":
        if args.file:
            source = Path(args.payload)
            output = Path(args.output) if args.output else source.with_suffix(source.suffix + ".kct")
            waveform = encode_file(source, output)
            if args.hex:
                print(waveform.hex())
            else:
                print(output)
            return

        payload = _read_payload(args.payload)
        waveform = encode(payload)
        if args.output:
            Path(args.output).write_bytes(waveform)
        if args.hex:
            print(waveform.hex())
        else:
            print(waveform)
        return

    waveform = Path(args.waveform).read_bytes() if Path(args.waveform).exists() else bytes.fromhex(args.waveform)
    decoded = decode(waveform)
    if args.output:
        Path(args.output).write_bytes(decoded)
    try:
        print(decoded.decode("utf-8"))
    except UnicodeDecodeError:
        print(decoded.hex())


if __name__ == "__main__":
    main()
