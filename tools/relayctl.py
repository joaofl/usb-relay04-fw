#!/usr/bin/env python3
"""Talk to a relay card running relay04 firmware, and update that firmware over USB.

    relayctl.py [-d DEV] status | version | on 1 3 | off all | set 1010
    relayctl.py [-d DEV] pulse 1 10s [--wait]
    relayctl.py [-d DEV] update build/icse012a/relay04.ihx
    relayctl.py [-d DEV] update-boot build/icse012a/boot.ihx
"""

import argparse
import sys
import time

BOOT_START, APP_START, FLASH_END, BLOCK = 0x8000, 0x8800, 0xA000, 64
BAUD = 115200


def load_ihex(path):
    mem, base = {}, 0
    for raw in open(path):
        rec = raw.strip()
        if not rec.startswith(":"):
            continue
        b = bytes.fromhex(rec[1:])
        if sum(b) & 0xFF:
            sys.exit(f"{path}: bad checksum in {rec}")
        n, addr, kind, data = b[0], b[1] << 8 | b[2], b[3], b[4:4 + b[0]]
        if kind == 0:
            for i in range(n):
                mem[base + addr + i] = data[i]
        elif kind == 4:
            base = (data[0] << 8 | data[1]) << 16
        elif kind == 1:
            break
    return mem


def save_ihex(path, mem):
    with open(path, "w") as f:
        addrs = sorted(mem)
        i = 0
        while i < len(addrs):
            start, chunk = addrs[i], []
            while i < len(addrs) and addrs[i] == start + len(chunk) and len(chunk) < 32:
                chunk.append(mem[addrs[i]])
                i += 1
            rec = bytes([len(chunk), start >> 8, start & 0xFF, 0]) + bytes(chunk)
            f.write(f":{rec.hex().upper()}{(-sum(rec)) & 0xFF:02X}\n")
        f.write(":00000001FF\n")


class Relay:
    def __init__(self, dev, timeout=2.0):
        import serial
        self.port = serial.Serial(dev, BAUD, timeout=timeout)
        self.port.reset_input_buffer()

    def line(self, timeout=None):
        old = self.port.timeout
        if timeout is not None:
            self.port.timeout = timeout
        try:
            raw = self.port.readline()
        finally:
            self.port.timeout = old
        return raw.decode(errors="replace").rstrip()

    def cmd(self, text, timeout=None, body=None):
        """Send one command; return its OK/ERR reply. The firmware echoes the command line,
        and READY/BOOT/DONE notices can arrive in between; lines before the reply go to body."""
        self.port.write(text.encode() + b"\r\n")
        deadline = time.monotonic() + (timeout or self.port.timeout)
        echo = text.lower()
        while time.monotonic() < deadline:
            reply = self.line()
            if reply.startswith(("OK", "ERR")):
                return reply
            if body is not None and reply and reply.lower() != echo:
                body.append(reply)
        raise TimeoutError(f"no reply to {text!r}")


def check(reply):
    print(reply)
    return 0 if reply.startswith("OK") else 1


def cmd_update(relay, path):
    mem = load_ihex(path)
    bad = [a for a in mem if not APP_START <= a < FLASH_END]
    if bad:
        sys.exit(f"{path}: data outside 0x{APP_START:04x}-0x{FLASH_END - 1:04x} (first 0x{min(bad):04x})")
    if mem.get(APP_START) != 0x82:
        sys.exit(f"{path}: no reset vector at 0x{APP_START:04x}; build it with --code-loc 0x{APP_START:04x}")

    reply = relay.cmd("version")
    if not reply.startswith("OK relay04-boot"):
        relay.cmd("boot")
        deadline = time.monotonic() + 3
        while not relay.line(timeout=1.0).startswith("BOOT"):
            if time.monotonic() > deadline:
                sys.exit("no BOOT banner after the boot command; if an update-boot was "
                         "interrupted, run update-boot again first")
        reply = relay.cmd("version")
    print(reply)

    def write(addr, data):
        reply = relay.cmd(f"write {addr:04x} {data.hex()}")
        if reply != "OK":
            sys.exit(f"0x{addr:04x}: {reply}")

    # Clear the reset vector first and write it last: an interrupted update stays in the bootloader.
    blocks = sorted({a - a % BLOCK for a in mem} - {APP_START})
    write(APP_START, bytes(BLOCK))
    for n, addr in enumerate(blocks, 1):
        write(addr, bytes(mem.get(addr + i, 0) for i in range(BLOCK)))
        print(f"\r{n}/{len(blocks) + 1} blocks", end="", flush=True)
    write(APP_START, bytes(mem.get(APP_START + i, 0) for i in range(BLOCK)))
    print(f"\r{len(blocks) + 1}/{len(blocks) + 1} blocks")
    print(relay.cmd("run"))
    ready = relay.line(timeout=2.0)
    print(ready)
    return 0 if ready.startswith("READY") else 1


def cmd_update_boot(relay, path):
    """Rewrite the bootloader from the running firmware. Order: firmware vectors over the
    bootloader's (bbridge), then the bootloader code, then its vector table. Every word is
    written whole, so after an interruption the card still starts, at worst straight into
    the firmware, and update-boot can be run again."""
    mem = load_ihex(path)
    bad = [a for a in mem if not BOOT_START <= a < APP_START]
    if bad:
        sys.exit(f"{path}: data outside 0x{BOOT_START:04x}-0x{APP_START - 1:04x} (first 0x{min(bad):04x})")
    if mem.get(BOOT_START) != 0x82:
        sys.exit(f"{path}: no reset vector at 0x{BOOT_START:04x}; is it a bootloader image?")

    reply = relay.cmd("version")
    if reply.startswith("OK relay04-boot"):
        relay.cmd("run")
        relay.line(timeout=2.0)
        reply = relay.cmd("version")
    print(reply)

    def run(text, what):
        reply = relay.cmd(text, timeout=3)
        if reply != "OK":
            hint = "; the firmware needs 1.3.0 or newer, run update first" if "unknown" in reply else ""
            sys.exit(f"{what}: {reply}{hint}")

    def block(addr):
        return bytes(mem.get(addr + i, 0) for i in range(BLOCK))

    run("bbridge", "bridge")
    code = range(BOOT_START + 2 * BLOCK, APP_START, BLOCK)
    total = len(code) + 2
    for n, addr in enumerate(code, 1):
        run(f"bwrite {addr:04x} {block(addr).hex()}", f"0x{addr:04x}")
        print(f"\r{n}/{total} blocks", end="", flush=True)
    run(f"bwrite {BOOT_START + BLOCK:04x} {block(BOOT_START + BLOCK).hex()}", "vectors 16-31")
    run(f"bwrite {BOOT_START:04x} {block(BOOT_START).hex()}", "vectors 0-15")
    print(f"\r{total}/{total} blocks")

    relay.port.write(b"boot\r\n")
    deadline, banner = time.monotonic() + 3, ""
    while not banner.startswith("BOOT") and time.monotonic() < deadline:
        banner = relay.line(timeout=1.0)
    print(banner or "no BOOT banner from the new bootloader")
    if not banner.startswith("BOOT"):
        return 1
    print(relay.cmd("run"))
    ready = relay.line(timeout=2.0)
    print(ready)
    return 0 if ready.startswith("READY") else 1


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-d", "--device", default="/dev/ttyUSB0")
    p.add_argument("--wait", action="store_true", help="pulse: wait for the DONE notice")
    p.add_argument("command")
    p.add_argument("args", nargs="*")
    a = p.parse_args()

    if a.command == "size":
        mem = load_ihex(a.args[0])
        lo, hi = int(a.args[1], 0), int(a.args[2], 0)
        used = max(mem) + 1 - lo
        print(f"{a.args[0]}: {used} of {hi - lo} bytes")
        return 0 if min(mem) >= lo and max(mem) < hi else 1
    if a.command == "merge":
        mem = {}
        for path in a.args[1:]:
            mem.update(load_ihex(path))
        save_ihex(a.args[0], mem)
        return 0

    relay = Relay(a.device)
    if a.command == "update":
        return cmd_update(relay, a.args[0])
    if a.command == "update-boot":
        return cmd_update_boot(relay, a.args[0])
    text = " ".join([a.command] + a.args)
    if a.command in ("h", "help", "?"):
        body = []
        reply = relay.cmd(text, body=body)
        print("\n".join(body))
        return check(reply)
    reply = relay.cmd(text)
    if a.command == "pulse" and a.wait and reply.startswith("OK"):
        print(reply)
        if a.args[0] == "all":
            relays = {str(n) for n in range(1, len(reply.split()[1]) + 1)}
        else:
            relays = {c for r in a.args[:-1] for c in r if c.isdigit()}
        while relays:
            notice = relay.line(timeout=None)
            if notice.startswith("DONE"):
                print(notice)
                relays.discard(notice.split()[1])
        return 0
    return check(reply)


if __name__ == "__main__":
    sys.exit(main())
