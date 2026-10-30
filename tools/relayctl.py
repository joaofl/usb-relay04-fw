#!/usr/bin/env python3
"""Talk to a relay card running relay04 firmware, and update that firmware over USB.

    relayctl.py [-d DEV] status | version | on 1 3 | off all | set 1010
    relayctl.py [-d DEV] pulse 1 10s [--wait]
    relayctl.py [-d DEV] update build/icse012a/relay04.ihx
"""

import argparse
import sys
import time

APP_START, FLASH_END, BLOCK = 0x8800, 0xA000, 64
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
        return raw.decode(errors="replace").strip()

    def cmd(self, text, timeout=None):
        """Send one command; return its OK/ERR reply, skipping READY/BOOT/DONE notices."""
        self.port.write(text.encode() + b"\r\n")
        deadline = time.monotonic() + (timeout or self.port.timeout)
        while time.monotonic() < deadline:
            reply = self.line()
            if reply.startswith(("OK", "ERR")):
                return reply
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
                sys.exit("no BOOT banner after the boot command")
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
    text = " ".join([a.command] + a.args)
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
