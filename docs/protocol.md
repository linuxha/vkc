# VKC/1 network protocol

This is the contract between the Pico W (client) and the Linux tape daemon
(server, developed in a separate repository). `kctool serve` is a reference
server, and `kctool dir|get|put` is a reference client. You can use them to
test either side on its own.

## Transport

* TCP. Default port **8300**.
* The client opens one connection, does a handshake, sends one or more
  commands, then sends `QUIT` or simply closes. The Pico sends one command per
  connection.
* Control lines are ASCII and end with `\n`. A server should also accept
  `\r\n`. The longest line is 255 bytes plus the newline.
* Payloads are raw bytes. Each one directly follows the line that announces
  its length. There is no escaping and no trailing newline.
* Every command gets exactly one status line back, either `OK ...` or
  `ERR <human readable text>`. An `ERR` never closes the connection unless
  the error is fatal.

## What a "tape" is

A tape is the **decoded byte stream**: the bytes the vintage machine wrote
between its start and stop bits. It does not include leader tone or framing.
The Pico decodes the audio before uploading and re-encodes it on playback.
`kctool encode` and `kctool decode` convert between tapes and WAV audio.

The server stores the bytes as they are and may keep metadata next to them.

### Tape names

1–63 characters from `[A-Za-z0-9._-]`. A name does not start with `.`.
Servers reject other names with `ERR bad name`. This keeps path traversal
out of the daemon.

## Handshake

```
C: HELLO VKC/1 <client-id>
S: OK VKC/1 [free text]
```

`<client-id>` is free-form with no spaces. The Pico sends `vkc-pico-<board id>`.
A server that does not speak `VKC/1` replies `ERR unsupported version` and
closes the connection.

## Commands

### DIR — list tapes

```
C: DIR
S: OK <count>
S: <name> <size> [key=value ...]      (repeated <count> times)
```

Servers may append `key=value` attributes such as `baud=300` or
`mtime=1731024000`. Clients ignore attributes they don't know.

### GET — fetch a tape

```
C: GET <name>
S: OK <size> [key=value ...]
S: <size raw bytes>
```

or `ERR not found`. If the server returns `baud=` and the client has no
override, the client should use that rate to play the tape.

### PUT — store a tape

```
C: PUT <name> <size> [key=value ...]
C: <size raw bytes>
S: OK <size>
```

The Pico sends `baud=<n>` and `stop=<n>`. An existing tape with the same name
is replaced. The server reads all `<size>` bytes before it replies. If it
must refuse a PUT after the client has started sending (for example, the
disk is full), it still reads and discards the payload, then replies
`ERR ...`.

### QUIT

```
C: QUIT
S: OK bye
```

The server then closes the connection.

## Limits

* The Pico holds a whole tape in RAM. It refuses tapes larger than its buffer
  (128 KiB by default, about 70 minutes at 300 baud).
* Servers should pick a sensible maximum size and reject anything larger
  with `ERR too large`.
* Clients time out after about 10 seconds of inactivity.

## Example session

```
C: HELLO VKC/1 vkc-pico-E6614103E7452D2F
S: OK VKC/1 vkcd 0.1
C: PUT basic-startrek 4096 baud=300 stop=2
C: <4096 bytes>
S: OK 4096
```
