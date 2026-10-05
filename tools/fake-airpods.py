#!/usr/bin/env python3
"""Fake AirPods for the sim: speaks AAP (src/audio/aap.h) on a Unix SEQPACKET
socket the way AirPods Pro 2 do -- answers the setup with battery, in-ear
state and every setting, echoes setting changes -- and turns commands typed
on stdin into the events real ones send.

    python3 tools/fake-airpods.py /tmp/rpod-airpods.sock
    RPOD_AIRPODS_SOCK=/tmp/rpod-airpods.sock make sim

Commands:
    out [2]                 take a bud out (or both)
    in                      put both back in
    case                    both buds in the case
    talk N                  Conversation Awareness level N (1-2 talking .. 8-9 normal)
    press single|double|triple|long
    mode off|anc|transparency|adaptive    as if switched from the stem
    battery L R CASE        levels in percent
    charge on|off           the case charging
    drop                    close the connection (rPod reconnects)
    quit
"""

import os
import select
import socket
import sys

HEADER = bytes([0x04, 0x00, 0x04, 0x00])
MODES = {"off": 1, "anc": 2, "transparency": 3, "adaptive": 4}
PRESSES = {"single": 0x05, "double": 0x06, "triple": 0x07, "long": 0x08}
LEFT, RIGHT, CASE = 0x04, 0x02, 0x08


def packet(opcode, payload):
    return HEADER + bytes([opcode & 0xFF, opcode >> 8]) + bytes(payload)


class AirPods:
    def __init__(self):
        self.ear = [0, 0]  # primary, secondary: 0 in ear, 1 out, 2 in case
        self.level = {LEFT: 80, RIGHT: 78, CASE: 45}
        self.case_charging = False
        # Setting id -> value, as AirPods Pro 2 report them.
        self.controls = {
            0x0A: 1,     # ear detection on
            0x0D: 2,     # noise cancellation
            0x17: 0,     # press speed: default
            0x18: 0,     # hold duration: default
            0x1A: 0x0E,  # stem cycles ANC, transparency, adaptive
            0x1B: 1,     # one-bud ANC on
            0x23: 1,     # swipe speed: default
            0x25: 1,     # volume swipe on
            0x26: 1,     # personalized volume on
            0x28: 1,     # conversation awareness on
            0x2E: 50,    # adaptive strength
            0x34: 2,     # off listening mode not offered
        }
        self.gestures = 0

    def battery(self):
        items = [3]
        for comp in (LEFT, RIGHT, CASE):
            charging = comp == CASE and self.case_charging
            items += [comp, 0x01, self.level[comp], 0x01 if charging else 0x02, 0x01]
        return packet(0x0004, items)

    def ear_state(self):
        return packet(0x0006, self.ear)

    def control(self, ident):
        return packet(0x0009, [ident, self.controls[ident], 0, 0, 0])

    def info(self):
        fields = ["Fake AirPods Pro", "A3048", "Apple Inc.", "FAKESERIAL1",
                  "61.1868040002000000.2713", "61.1868040002000000.2713", "1.0.0",
                  "com.apple.accessory.updater.app.71", "FAKELEFT", "FAKERIGHT", "6357536"]
        body = bytes([0x02, 0xD5, 0x00, 0x04, 0x00]) + b"".join(f.encode() + b"\0" for f in fields)
        return packet(0x001D, body)


def log(*args):
    print(*args, flush=True)


def handle(pods, data, send):
    if data[:6] == bytes([0x00, 0x00, 0x04, 0x00, 0x01, 0x00]):
        log("<- handshake")
        send(bytes([0x01, 0x00, 0x04, 0x00, 0x00, 0x00]))
        return
    if data[:4] != HEADER or len(data) < 6:
        log("<- ?", data.hex(" "))
        return
    opcode = data[4] | data[5] << 8
    if opcode == 0x004D:
        log("<- feature flags", data[6:].hex(" "))
        send(packet(0x002B, [0x00]))
    elif opcode == 0x000F:
        log("<- notification request: sending state")
        send(pods.info())
        send(pods.battery())
        send(pods.ear_state())
        for ident in sorted(pods.controls):
            send(pods.control(ident))
    elif opcode == 0x0009 and len(data) >= 11:
        ident, value = data[6], data[7]
        if ident == 0x39:
            pods.gestures = value
            log(f"<- raw gestures {value:#04x}")
            return
        log(f"<- set {ident:#04x} = {value}")
        if ident in pods.controls:
            pods.controls[ident] = value
            send(pods.control(ident))
    else:
        log(f"<- opcode {opcode:#06x}", data[6:].hex(" "))


def command(pods, line, send):
    words = line.split()
    if not words:
        return True
    cmd, args = words[0], words[1:]
    if cmd == "out":
        if args[:1] == ["2"]:
            pods.ear = [1, 1]
        else:
            pods.ear = [0, 1]
        send(pods.ear_state())
    elif cmd == "in":
        pods.ear = [0, 0]
        send(pods.ear_state())
    elif cmd == "case":
        pods.ear = [2, 2]
        send(pods.ear_state())
    elif cmd == "talk" and args:
        send(packet(0x004B, [0x02, 0x00, 0x01, int(args[0])]))
    elif cmd == "press" and args and args[0] in PRESSES:
        send(packet(0x0019, [PRESSES[args[0]], 0x01]))
    elif cmd == "mode" and args and args[0] in MODES:
        pods.controls[0x0D] = MODES[args[0]]
        send(pods.control(0x0D))
    elif cmd == "battery" and len(args) == 3:
        pods.level = {LEFT: int(args[0]), RIGHT: int(args[1]), CASE: int(args[2])}
        send(pods.battery())
    elif cmd == "charge" and args:
        pods.case_charging = args[0] == "on"
        send(pods.battery())
    elif cmd == "drop":
        return False
    elif cmd == "quit":
        sys.exit(0)
    else:
        log("?", line.strip(), "-- see the top of this file")
    return True


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    if os.path.exists(path):
        os.unlink(path)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    server.bind(path)
    server.listen(1)
    log(f"fake AirPods on {path}; waiting for rPod")

    pods = AirPods()
    client = None
    stdin_open = True

    def send(data):
        if client is not None:
            client.send(data)

    while True:
        watch = [server] + ([client] if client else []) + ([sys.stdin] if stdin_open else [])
        ready, _, _ = select.select(watch, [], [])
        if server in ready:
            conn, _ = server.accept()
            if client is not None:
                client.close()
            client = conn
            log("rPod connected")
        if client is not None and client in ready:
            data = client.recv(1024)
            if not data:
                log("rPod disconnected")
                client.close()
                client = None
            else:
                handle(pods, data, send)
        if sys.stdin in ready:
            line = sys.stdin.readline()
            if not line:
                stdin_open = False
            elif not command(pods, line, send) and client is not None:
                client.close()
                client = None
                log("dropped the connection")


if __name__ == "__main__":
    main()
