# Hardware

## Pins (Pico W)

| Signal | GPIO | Pico pin | Notes |
|---|---|---|---|
| Command UART TX | GP0 | 1 | 3.3 V logic, to the computer's serial RX |
| Command UART RX | GP1 | 2 | 3.3 V logic, from the computer's serial TX |
| Tape in | GP2 | 4 | From the comparator below (the computer's cassette **output**) |
| Tape out | GP3 | 5 | To the filter below (the computer's cassette **input**) |
| Motor (REMOTE) in | GP4 | 6 | From the computer's motor-control output, see below |
| PLAY button | GP5 | 7 | Push button to GND |
| RECORD button | GP6 | 9 | Push button to GND |
| MOTOR button | GP7 | 10 | Push button to GND |
| PLAY LED | GP8 | 11 | 330 Ω to LED anode, cathode to GND |
| RECORD LED | GP9 | 12 | 330 Ω to LED anode, cathode to GND |
| MOTOR LED | GP10 | 14 | 330 Ω to LED anode, cathode to GND |
| GND | — | 3, 8, … | Common ground with the computer |
| USB | — | — | Power and the configuration console |

To change the pins, edit `firmware/board.h` or pass `-DVKC_PIN_TAPE_IN=…`
etc. to CMake.

## Command serial port

The GPIOs are **3.3 V only**. Most vintage computers use RS-232 levels
(±12 V), and a direct connection will destroy the Pico. Put a MAX3232
(or similar 3.3 V RS-232 transceiver) between them. A 5 V TTL serial port
needs at least a divider on the Pico's RX pin.

The default is 9600 8N1. Change it with `set uartbaud <n>`.

## Tape out (Pico → computer cassette input)

GP3 outputs a 3.3 V square wave at 1200/2400 Hz. A single RC low-pass
rounds off the edges, a capacitor removes the DC offset, and a pot sets
the level:

```
GP3 ──[ 4.7k ]──┬──[ 1µF ]──┬────────── computer TAPE IN / EAR
                │           │
              [10nF]      [10k pot] (wiper to TAPE IN if level needs trimming)
                │           │
GND ────────────┴───────────┴────────── computer GND
```

The corner frequency is about 3.4 kHz. Most KCS interfaces accept
anything from 0.3 to 3 Vpp. Start with the pot at about half and adjust
until the computer loads reliably.

## Tape in (computer cassette output → Pico)

The decoder times zero crossings, so it needs a clean digital signal on
GP2. Use a comparator with a little hysteresis. An LM393 (open collector)
or an MCP6541 (push-pull, no pull-up needed) on 3.3 V works:

```
                     Vref = 1.65 V (two 10k from 3V3 to GND, 10µF across the lower one)
                        │
TAPE OUT ──[ 1µF ]──┬──[ 10k ]──┬──── (+)
                    │           │           LM393 ──┬──── GP2
                 [100k]       [1M] ◄─────── out ────┤
                    │                               └─[ 10k ]── 3V3
                  Vref ───────────────────── (−)
```

* The 1 µF and 100 k AC-couple the signal and centre it on Vref.
* The 1 M from output to (+) gives about 30 mV of hysteresis, which keeps
  the comparator from chattering at the crossings.
* The decoder also rejects short glitches, works with ±10 % tape speed
  error, and decodes at 300 and 600 baud even with impulse noise. 1200 baud
  (CUTS) is more sensitive to noise. A 0 bit there is only one cycle long.

**Quick and dirty alternative:** the RP2040 input has a Schmitt trigger. For
a strong signal (2 Vpp or more), AC-couple it straight into GP2 and bias the
pin to mid-supply with two 100 k resistors. It is less reliable than a
comparator.

## Motor control (REMOTE) input

Many machines switch the recorder's motor through a 2.5 mm REMOTE jack, so
`LOAD` and `SAVE` can start and stop the tape. VKC treats it the same way:
with `set remote on`, playback pauses (output silent, position kept) and
recording ignores the input while the motor is off. Playback of a tape
started with `play` waits for the computer to start the motor.

**Relay contacts** (TRS-80 and most others), or an open-collector
transistor to ground:

```
3V3 ──[ 1k ]──┬──[ 1k ]──── GP4
              │
REMOTE ───────┘
REMOTE ──────────────────── GND
```

The internal pull-up alone is only ~50 kΩ, which is too little current to
keep old relay contacts clean, so the external 1 k pull-up gives about
3 mA. The series 1 k protects the pin. Contacts closed = motor on
(`set remote on`).

**Anything else** (an output that drives a voltage, or you aren't sure):
use an opto-isolator such as a PC817. Put its LED, with a resistor for
about 5 mA, across the REMOTE output, and connect the transistor's
collector to GP4 and its emitter to GND. Never connect a REMOTE jack that
carries more than 3.3 V straight to the Pico.

If the motor runs when it should be stopped, use `set remote inv`.
`status` shows whether the input is asserted. `set remote off` (the
default) ignores the input and the motor always runs.

## Buttons and LEDs

Each button connects its GPIO to GND (internal pull-ups, 20 ms debounce).
Each LED is driven high through a 330 Ω resistor (about 4 mA).

| Button | Idle | During its own job |
|---|---|---|
| PLAY | Play the current tape (the last one played or recorded, or `set tape <name>`) | Stop |
| RECORD | Record a new tape named `recNNN` (next free number on the server) | Finish and upload |
| MOTOR | Force the motor on (or off, if it is running), overriding REMOTE | Same. Press again to go back to automatic |

| LED | Steady | Slow blink | Fast flash (1 s) |
|---|---|---|---|
| PLAY | Playing | Fetching the tape, or waiting for the motor | Error |
| RECORD | Recording | Uploading, or waiting for the motor | Error, or nothing was received |
| MOTOR | Motor running under REMOTE or override control | — | — |

The MOTOR LED stays dark when REMOTE is off and there is no override,
because the motor is then simply always on. Messages for button actions
go to the USB console.

## Bring-up checklist

1. Flash `vkc.uf2`, open the USB console (`minicom -D /dev/ttyACM0` or
   `screen /dev/ttyACM0`), and type `help`.
2. Fit a jumper from GP3 to GP2 and run `selftest`. This exercises the
   output timer, the edge interrupt and the decoder without any analog
   parts. Try each baud rate with `set baud 300|600|1200`.
3. Remove the jumper and build the two circuits above.
4. Check the output with a Linux sound card. Run `play <name>` on the Pico
   while recording with
   `arecord -f S16_LE -r 44100 -c 1 out.wav`, then
   `kctool decode -v out.wav out.bin`.
5. Check the input with `kctool encode prog.bin - | aplay` into the
   comparator while the Pico runs `receive test`.
6. Press MOTOR and check the MOTOR LED. Wire the REMOTE input, `set remote
   on`, then type `MOTOR` on the computer (or run `LOAD`) and watch
   `status` report the input as asserted.
