# vkc
Virtual Kansas City - A Kansas City tape emulator.

This project provides a minimal Python implementation of the Kansas City Standard
used by old tape-based RS-232 links. It can encode arbitrary bytes into a tone
sequence, decode the same sequence back to the original data, and expose a tiny
TCP daemon for network-based emulation.

Usage:

    python -m vkc encode "hello world"
    python -m vkc decode <hex waveform>

The daemon can be used by sending raw bytes over a socket to the listening
service and receiving the encoded Kansas City waveform back in response.
