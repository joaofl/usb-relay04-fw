# usb-relay04-fw

Open-source replacement firmware for the cheap **USB-RELAY04** 4-channel USB
relay card (sold by Seeit, and as ICStation ICSE012A): a plain text protocol
with state readback, timed pulses, a watchdog, and a bootloader for firmware
updates over the card's own USB port.

## Why

The factory firmware makes the card hard to rely on:

- **No readback.** Nothing reports which relays are on.
- **Arming byte.** The card answers `0x50` with an ID and then needs `0x51`
  to arm. After that, every byte is a relay state, including `0x50` and
  `0x51` themselves: `0x50` sent to an armed card switches all four relays on.
- **Inverted bits.** A `0` bit switches a relay on.
- **No recovery.** It can stop responding without any USB event, and only
  cutting its USB power brings it back. DTR and RTS do nothing.

This firmware replaces it with readable commands that always reply, and it
resets itself if it hangs.

The factory firmware is read-protected. Installing this firmware removes the
protection, which erases the chip: the factory firmware cannot be restored.

## Hardware

| Part | Role |
|---|---|
| STM8S003F3P6 | microcontroller, 8 KB flash, 16 MHz internal clock |
| PL2303 | USB-serial bridge, on the STM8's UART1 (PD5 TX, PD6 RX) |
| ULN2803 | relay driver; an STM8 pin driven high switches its relay on |

| Relay | STM8 pin |
|---|---|
| 1 | PA1 |
| 2 | PA2 |
| 3 | PA3 |
| 4 | PC3 |

The relays need the 5 V from the card's USB port; the card has no other power
input.

### Other boards

The same family has a 2-relay (ICSE013A) and an 8-relay (ICSE014A) card.
The firmware supports 1 to 8 relays: each board is a pin table in
[firmware/boards/](firmware/boards/), and only `icse012a` is mapped and tested.

To add a board, find which pin drives each relay (for example with the
wiring test below, or by driving pins one by one over SWIM), then create
`firmware/boards/<name>.h`:

```c
#define BOARD_NAME "icse014a"
#define RELAY_PINS { PIN(A, 1), PIN(A, 2), /* ... relay 1 first */ }
```

and build with `make BOARD=<name>`. Pins must leave PD1 (SWIM) and PD5/PD6
(UART) free. A pin driven high must switch its relay on.

## Protocol

Serial, 115200 8N1, one command per line (`\r`, `\n` or both). Every command
gets one reply line starting with `OK` or `ERR`. A state is one character per
relay, relay 1 first, `1` = on. The examples are for a 4-relay board.

| Command | Reply |
|---|---|
| `on 1 3`, `on 13`, `on all` | `OK 1010` |
| `off 1`, `off all` | `OK 0010` |
| `set 0101` | `OK 0101`, sets all relays at once |
| `pulse 1 10s`, `pulse 2 4 1500ms` | `OK 1000`; when each relay goes off again: `DONE 1 0000` |
| `status` | `OK 1000 t1=9862 up=14 reset=power` |
| `version` | `OK relay04 1.1.0 icse012a 4`: firmware version, board, number of relays |
| `boot` | `OK boot`, switches all relays off and starts the bootloader |

- `t<n>=` is the time left on a running pulse, in ms. `up=` is seconds since
  start. `reset=` is why the chip last started: `power` (power-on or the reset
  pin), `watchdog`, `illegal-opcode`, `emc` or `swim`.
- Pulses run up to 24 h. `on`, `off` and `set` cancel a pulse on the relays
  they touch.
- At start all relays are off, and the card prints
  `READY relay04 1.1.0 icse012a 4 reset=<cause>`.
- An independent watchdog (about 0.5 s) restarts the firmware if it hangs;
  the relays then come back off.

### Host tool

[tools/relayctl.py](tools/relayctl.py) needs `pyserial`.

```sh
tools/relayctl.py -d /dev/ttyUSB0 status
tools/relayctl.py -d /dev/ttyUSB0 on 1 3
tools/relayctl.py -d /dev/ttyUSB0 pulse 1 10s --wait   # returns after DONE
```

The default device is `/dev/ttyUSB0`.

## Memory map

| Range | Content |
|---|---|
| `0x8000`-`0x87FF` | bootloader ([firmware/boot.c](firmware/boot.c)) |
| `0x8800`-`0x9FFF` | relay firmware ([firmware/relay04.c](firmware/relay04.c)) |

The bootloader starts the relay firmware unless the firmware asked for an
update (`boot`) or there is no firmware yet. Interrupts go through the
bootloader's vector table, which forwards each one to the firmware's table at
`0x8800`. In the bootloader the card answers `version`, `read <aaaa>`,
`write <aaaa> <64 bytes hex>` and `run`, at the same baud rate.

## Build

Needs `make`, Python 3 and [SDCC](https://sdcc.sourceforge.net/) 4.2 or newer
with the stm8 port (Debian/Ubuntu: `apt install sdcc`).

```sh
make                 # BOARD=icse012a by default
make boards          # list the boards
```

Outputs in `build/<board>/`:

| File | Use |
|---|---|
| `relay04-full.ihx` | bootloader + firmware, first install over SWIM |
| `relay04.ihx` | firmware only, updates over USB |
| `boot.ihx` | bootloader only |

`make swim-flash-relaytest` builds and flashes
[firmware/relaytest.c](firmware/relaytest.c), which switches the board's
relays on one after the other and prints `R1`, `R2`, ... at 115200, for
checking a card's wiring. It has no bootloader; `make swim-flash` puts the
relay firmware back.

## First install over SWIM

Needs a SWIM programmer once per card. This repository uses a Raspberry Pi
Debug Probe running [rp2040swim](https://github.com/ARDev1161/rp2040swim)
v0.1.0, included as the submodule `tools/rp2040swim`:

```sh
git submodule update --init
```

### Probe firmware

Build it from the submodule (needs the
[Pico SDK](https://github.com/raspberrypi/pico-sdk) and an arm-none-eabi GCC):

```sh
cd tools/rp2040swim
cmake -B build -S firmware -DPICO_BOARD=pico -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build -j
```

Hold the probe's BOOTSEL button while plugging it in, and copy
`build/rp2040_swim.uf2` to the `RPI-RP2` drive. It then shows up as
`/dev/ttyACM0` (USB `2e8a:000a`). Your user needs access to it (`dialout` group).

No patch to rp2040swim is needed. Its `flash` command does not work on this
chip, so [tools/swim-flash.py](tools/swim-flash.py) uses only its basic memory
read/write commands and programs flash byte by byte.

### Wiring

The card has a 4-pin programming header: 3V3, SW, RE, GND.

| Debug Probe | RP2040 | Card |
|---|---|---|
| SD | GPIO14 | SW |
| SC | GPIO12 | RE |
| GND | GND | GND |
| - | - | 3V3: not connected |

- Fit a **1 kΩ pull-up from SW to 3V3** on the card. SWIM is open-drain, and
  without it the chip does not answer.
- Power the card from its own USB port. The RP2040 is not 5 V tolerant; the
  header carries 3.3 V.

### Flash

```sh
make
tools/swim-flash.py --port /dev/ttyACM0 --unprotect build/icse012a/relay04-full.ihx   # card with factory firmware
make swim-flash PORT=/dev/ttyACM0                                           # card already running this firmware
```

`swim-flash.py` resets and halts the chip (retrying, since the first SWIM
write occasionally NACKs), removes read-out protection with `--unprotect`
(mass erase), restores the option bytes to their defaults when they are
invalid, programs only the bytes that differ, verifies, and starts the
firmware.

Disconnect the probe afterwards: while it is attached, every reset of the chip
leaves it halted.

## Updates over USB

```sh
make update RELAY=/dev/ttyUSB0
```

`relayctl.py update` sends `boot`, writes the firmware block by block, writes
the block holding the reset vector last, and starts the new firmware. An
interrupted update leaves the card in the bootloader, where the next `update`
can finish it. The bootloader itself is only written over SWIM.

## License

MIT, see [LICENSE](LICENSE). rp2040swim is MIT licensed by its authors.
