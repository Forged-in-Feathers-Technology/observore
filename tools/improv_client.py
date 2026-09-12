#!/usr/bin/env python3
"""Speak Improv Serial to an Observore device over USB.

Two uses. It provisions a device from a terminal, for anyone who would rather
not use a browser -- and it is how the firmware's Improv support is tested,
because driving the browser dialog by hand is not a test anybody repeats.

  tools/improv_client.py --port /dev/ttyACM0 info
  tools/improv_client.py --port /dev/ttyACM0 scan
  tools/improv_client.py --port /dev/ttyACM0 provision --ssid MyAP --password secret

Needs pyserial.
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    raise SystemExit("pyserial is required: pip install pyserial")

MAGIC = b"IMPROV"
VERSION = 1

TYPE_CURRENT_STATE, TYPE_ERROR_STATE, TYPE_RPC, TYPE_RPC_RESULT = 1, 2, 3, 4

STATES = {0: "stopped", 2: "ready", 3: "provisioning", 4: "provisioned"}
ERRORS = {
    0: "none", 1: "invalid RPC packet", 2: "unknown RPC command",
    3: "unable to connect", 5: "bad hostname", 0xFF: "unknown error",
}
CMD_WIFI_SETTINGS, CMD_GET_STATE, CMD_GET_DEVICE_INFO, CMD_GET_NETWORKS = 1, 2, 3, 4


def frame(ptype, payload):
    """IMPROV | version | type | length | payload | checksum(sum of all prior)."""
    pkt = bytearray(MAGIC + bytes([VERSION, ptype, len(payload)]) + payload)
    pkt.append(sum(pkt) & 0xFF)
    return bytes(pkt)


def rpc(command, data=b""):
    return frame(TYPE_RPC, bytes([command, len(data)]) + data)


def strings(payload):
    """RPC result payload: command, length, then length-prefixed strings."""
    out, i = [], 2
    while i < len(payload):
        n = payload[i]
        out.append(payload[i + 1:i + 1 + n].decode("utf-8", "replace"))
        i += 1 + n
    return out


class Reader:
    """Finds packets in a stream that also carries ordinary log output."""

    def __init__(self, port):
        self.port = port
        self.buf = bytearray()

    def poll(self, timeout):
        """Yield (type, payload) until timeout elapses with nothing new."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self.port.read(256)
            if chunk:
                self.buf += chunk
            for pkt in self._drain():
                yield pkt
            if not chunk:
                time.sleep(0.02)

    def _drain(self):
        while True:
            start = self.buf.find(MAGIC)
            if start < 0:
                # Keep a tail in case a header straddles two reads.
                if len(self.buf) > len(MAGIC):
                    del self.buf[:-len(MAGIC)]
                return
            if len(self.buf) < start + 9:
                return
            length = self.buf[start + 8]
            end = start + 9 + length + 1
            if len(self.buf) < end:
                return
            pkt = bytes(self.buf[start:end])
            del self.buf[:end]
            if (sum(pkt[:-1]) & 0xFF) != pkt[-1]:
                print("  (checksum mismatch, skipped)", file=sys.stderr)
                continue
            yield pkt[7], pkt[9:-1]


def collect(reader, timeout, want_types=None):
    got = []
    for ptype, payload in reader.poll(timeout):
        if want_types and ptype not in want_types:
            continue
        got.append((ptype, payload))
    return got


def describe(ptype, payload):
    if ptype == TYPE_CURRENT_STATE:
        return "state: %s" % STATES.get(payload[0], "0x%02x" % payload[0])
    if ptype == TYPE_ERROR_STATE:
        return "error: %s" % ERRORS.get(payload[0], "0x%02x" % payload[0])
    if ptype == TYPE_RPC_RESULT:
        return "result(0x%02x): %s" % (payload[0], strings(payload))
    return "type 0x%02x: %r" % (ptype, payload)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=3.0)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    sub.add_parser("state")
    sub.add_parser("scan")
    p = sub.add_parser("provision")
    p.add_argument("--ssid", required=True)
    p.add_argument("--password", default="")
    p.add_argument("--wait", type=float, default=30.0,
                   help="seconds to allow for the join")
    args = ap.parse_args()

    # DTR and RTS are wired to EN and BOOT on every ESP devkit, and pyserial
    # asserts both when it opens a port. Opening the port would therefore
    # reset the board, and the first seconds of the conversation would be shouted
    # at a chip that is still booting. Set them down before opening, not after.
    port = serial.Serial()
    port.port = args.port
    port.baudrate = args.baud
    port.timeout = 0.1
    port.dtr = False
    port.rts = False
    port.open()

    with port:
        reader = Reader(port)
        time.sleep(0.3)
        port.reset_input_buffer()

        if args.cmd == "info":
            port.write(rpc(CMD_GET_DEVICE_INFO))
            expect = args.timeout
        elif args.cmd == "state":
            port.write(rpc(CMD_GET_STATE))
            expect = args.timeout
        elif args.cmd == "scan":
            port.write(rpc(CMD_GET_NETWORKS))
            expect = args.timeout
        else:
            data = (bytes([len(args.ssid)]) + args.ssid.encode() +
                    bytes([len(args.password)]) + args.password.encode())
            port.write(rpc(CMD_WIFI_SETTINGS, data))
            expect = args.wait

        seen = collect(reader, expect)
        if not seen:
            print("no response -- is the firmware running, and is this the port "
                  "the console reads from?", file=sys.stderr)
            return 1
        for ptype, payload in seen:
            print(" ", describe(ptype, payload))
        if any(t == TYPE_ERROR_STATE and p[0] != 0 for t, p in seen):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
