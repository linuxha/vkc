# vkc

Virtual Kansas City — a Kansas City Standard tape emulator.

A Raspberry Pi **Pico W** sits where the cassette recorder used to be. When
the computer saves to tape, the Pico decodes the KCS audio and stores the
bytes on a Linux server over Wi-Fi. When the computer loads, the Pico fetches
the tape from the server and plays it back as KCS audio. You drive it from
the computer's serial port with `dir`, `play <name>` and `receive <name>`,
or from PLAY / RECORD / MOTOR buttons on the Pico. The computer's REMOTE
motor-control line can start and stop the "tape" like a real recorder.

```
 vintage computer                    Pico W                         Linux
┌──────────────┐  KCS audio   ┌─────────────────┐   VKC/1 TCP   ┌────────────┐
│ cassette OUT ├─────────────►│ GP2  decode     │               │ tape daemon│
│ cassette IN  │◄─────────────┤ GP3  encode     ├──────────────►│ (separate  │
│ REMOTE       ├─────────────►│ GP4  motor in   │   Wi-Fi       │  repo), or │
│ serial port  │◄────────────►│ UART cmd port   │               │ kctool     │
└──────────────┘  play/dir/   │ USB  config     │               │  serve     │
                  receive     │ GP5-7  buttons  │               └────────────┘
                              │ GP8-10 LEDs     │
                              └─────────────────┘
```

| Path | What |
|---|---|
| `firmware/` | Pico W firmware (C, Pico SDK ≥ 2.0, no Arduino) |
| `common/kcs.[ch]` | KCS encoder/decoder, shared by the firmware and kctool |
| `common/vkc_proto.h` | Protocol constants shared by both sides |
| `tools/` | `kctool`, a Linux command to create, decode and test KC tapes |
| `tests/` | Codec torture tests and kctool end-to-end tests |
| `docs/protocol.md` | **VKC/1 wire protocol**, the contract for the daemon repo |
| `docs/hardware.md` | Pinout, audio in/out circuits, bring-up |

## Format

* 1200 Hz = 0 (space), 2400 Hz = 1 (mark)
* 300 baud (classic KCS, the default), 600, or 1200 (CUTS)
* 1 start bit, 8 data bits LSB first, 2 stop bits (or 1)
* A tape on the server is the **decoded byte stream**, not audio.
  `kctool encode`/`decode` convert between tapes and WAV.

## Linux: kctool

```sh
make            # builds build/kctool
make test       # codec tests + WAV/server round trips
sudo make install
```

```sh
kctool encode prog.bin prog.wav              # bytes -> KCS audio (-b 300|600|1200, -q square)
kctool decode -v prog.wav prog.bin           # KCS audio -> bytes (-b auto to detect speed)
kctool encode prog.bin - | aplay             # play straight to the sound card
arecord -f S16_LE -r 44100 -c 1 | kctool decode - prog.bin   # record from a real machine

kctool serve -d ~/tapes                      # reference tape server on port 8300
kctool dir  myserver                         # client: test a server/daemon
kctool put  myserver startrek startrek.bin baud=300
kctool get  myserver startrek out.bin
```

`kctool serve` is a complete, if minimal, implementation of
[docs/protocol.md](docs/protocol.md). Use it to test the Pico before the
real daemon exists. Use the `dir`/`get`/`put` client to test the daemon.

## Pico W firmware

```sh
export PICO_SDK_PATH=~/pico/pico-sdk
make firmware                     # -> build/firmware/vkc.uf2
```

Hold BOOTSEL, plug in the Pico, and copy `vkc.uf2` to the drive (or run
`picotool load -x build/firmware/vkc.uf2`).

### First-time setup (USB console)

Connect with `minicom -D /dev/ttyACM0` or `screen /dev/ttyACM0`:

```
set ssid MyNetwork
set pass correct-horse-battery
set server 192.168.1.10        # or a host name
set port 8300
save
wifi
status
```

| Setting | Default | |
|---|---|---|
| `server` / `port` | — / 8300 | Tape daemon address |
| `baud` / `stop` | 300 / 2 | KCS speed and stop bits |
| `leader` / `trailer` | 5000 / 1000 ms | Mark tone around data on playback |
| `idle` | 3000 ms | `receive` finishes after this long with no bytes |
| `uartbaud` / `echo` | 9600 / off | Command serial port |
| `remote` | off | Motor-control input: `off` (ignore), `on` (closed = run), `inv` |
| `tape` | — | Tape the PLAY button plays after power-up |

Settings live in the last flash sector. `show` lists them and `defaults`
resets them.

### Commands (serial port or USB)

| Command | |
|---|---|
| `dir` | List tapes on the server |
| `play <name>` | Fetch the tape, then play it. Start `LOAD` on the computer during the leader. |
| `receive <name>` | Arm recording. Start `SAVE` on the computer. Upload happens after `idle` ms of no data, or on `stop`. |
| `stop` | Abort playback, or finish a recording now |
| `motor [on\|off\|auto]` | Force the cassette motor on or off, or follow REMOTE |
| `status` | Wi-Fi, server and job state |
| `help` | |

USB-only commands: `set`, `show`, `save`, `wifi`, `defaults`, `selftest`,
`reboot`.

Every command ends with one line that starts with `OK` or `ERR`. Lines
before it are progress messages. A script on the vintage machine only has
to wait for one of those two words.

### Front panel and motor control

Three buttons (PLAY, RECORD, MOTOR) and three LEDs, plus an input for the
computer's REMOTE motor-control jack. PLAY plays the current tape, RECORD
records a new `recNNN` tape, and pressing either again stops. With `set
remote on`, the tape pauses whenever the computer turns the motor off.
See [docs/hardware.md](docs/hardware.md) for wiring and LED meanings.

If the server sends `baud=`/`stop=` for a tape (the Pico stores both on
upload), playback uses those values instead of the current settings.

## Testing without hardware

1. `make test` exercises the decoder with jitter, duty-cycle skew, ±10 %
   tape speed error, comparator chatter and impulse noise at every baud
   rate. It also runs WAV round trips and server put/get/dir.
2. `kctool serve` on the PC, plus `selftest` on the Pico with a GP3–GP2
   jumper, tests the whole firmware path except the analog parts.
   `docs/hardware.md` has the full bring-up checklist.

## Limits / not yet done

* Tapes are held in Pico RAM: 128 KiB max (`VKC_TAPE_MAX` in
  `firmware/board.h`).
* IPv4 only on the Pico. `kctool` does IPv4 and IPv6.
