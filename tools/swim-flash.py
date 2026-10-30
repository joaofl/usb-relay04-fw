#!/usr/bin/env python3
"""Flash an Intel HEX image into the card's STM8S003F3 over SWIM, with an rp2040swim probe.

Byte-programs only the bytes that differ, restores invalid option bytes, then starts the
firmware. --unprotect first removes read-out protection, which mass-erases the chip.
"""

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "rp2040swim" / "host"))
from ihex import HexError, image_for_range, load_ihex  # noqa: E402
from protocol import ProtocolError  # noqa: E402
from rp2040_swim import (  # noqa: E402
    add_common, open_probe, read_u8, reconnect_after_rop_change, unlock_data_eeprom,
    unlock_program_flash, wait_flash_eop, write_u8,
)

FLASH_START, FLASH_SIZE = 0x8000, 8 * 1024
OPT_START = 0x4800
# Factory defaults: OPT0 (ROP off), then OPT1..OPT5 each followed by its complement.
OPT_DEFAULTS = bytes([0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF])
DM_CSR2, DM_CSR2_STALL = 0x7F99, 0x08


def option_write(probe, addr, value):
    """Option programming briefly drops the SWIM link; reconnect and judge by reading back."""
    unlock_data_eeprom(probe)
    try:
        probe.option_write_byte(addr, value)
    except ProtocolError:
        reconnect_after_rop_change(probe)


def unprotect(probe):
    print("removing read-out protection: this mass-erases flash, EEPROM and option bytes")
    option_write(probe, OPT_START, 0x00)
    reconnect_after_rop_change(probe)


def restore_options(probe):
    current = probe.read_memory(OPT_START, len(OPT_DEFAULTS))
    if current == OPT_DEFAULTS:
        return
    print(f"option bytes {current.hex(' ')} -> {OPT_DEFAULTS.hex(' ')}")
    for i, value in enumerate(OPT_DEFAULTS):
        if current[i] != value:
            option_write(probe, OPT_START + i, value)
    reconnect_after_rop_change(probe)
    current = probe.read_memory(OPT_START, len(OPT_DEFAULTS))
    if current != OPT_DEFAULTS:
        raise ProtocolError(f"option bytes read back {current.hex(' ')}")


def program(probe, image):
    """Byte programming: the probe firmware's block erase/write NACKs on this chip."""
    unlock_program_flash(probe)
    current = probe.read_memory(FLASH_START, len(image))
    changed = [i for i in range(len(image)) if image[i] != current[i]]
    for n, i in enumerate(changed, 1):
        write_u8(probe, FLASH_START + i, image[i])
        wait_flash_eop(probe)
        if n % 256 == 0 or n == len(changed):
            print(f"\r{n}/{len(changed)} bytes", end="", flush=True)
    print(f"\rwrote {len(changed)} bytes")
    if probe.read_memory(FLASH_START, len(image)) != image:
        raise ProtocolError("verify failed")


def attach(probe, attempts=10):
    """Reset, enter SWIM and stall the CPU. The first write after entry NACKs now and then; retry."""
    for attempt in range(1, attempts + 1):
        try:
            probe.reset_target()
            time.sleep(0.05)
            probe.enter_swim()
            write_u8(probe, DM_CSR2, read_u8(probe, DM_CSR2) | DM_CSR2_STALL)
            return
        except ProtocolError as exc:
            if attempt == attempts:
                raise
            print(f"attach attempt {attempt} failed ({exc}), retrying", file=sys.stderr)
            time.sleep(0.5)


def reset_and_run(probe):
    """A reset under SWIM leaves the CPU stalled in the debug module; release it."""
    probe.reset_target()
    time.sleep(0.05)
    probe.enter_swim()
    write_u8(probe, DM_CSR2, read_u8(probe, DM_CSR2) & ~DM_CSR2_STALL)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    add_common(p)
    p.set_defaults(swim_pin=14, nrst_pin=12)
    p.add_argument("--unprotect", action="store_true",
                   help="remove read-out protection first; destroys the factory firmware")
    p.add_argument("image", help="Intel HEX file for 0x8000-0x9FFF")
    a = p.parse_args()
    try:
        image = image_for_range(load_ihex(a.image), FLASH_START, FLASH_SIZE, fill=0x00)
        with open_probe(a) as probe:
            attach(probe)
            if a.unprotect:
                unprotect(probe)
            restore_options(probe)
            program(probe, image)
            reset_and_run(probe)
    except (OSError, ProtocolError, HexError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"flashed {a.image}, firmware started")
    return 0


if __name__ == "__main__":
    sys.exit(main())
