#!/usr/bin/env python3
"""Ask a Dante device for its clock status, as Dante Controller does, and
print the clock leader it reports.  usage: clock_status_probe.py DEVICE_IP [LOCAL_IP]
Exit 0 when a clock status reply with a leader arrives within 20 s (the device must have locked first)."""
import socket, struct, sys, time

dev = sys.argv[1]
local = sys.argv[2] if len(sys.argv) > 2 else "0.0.0.0"
rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
rx.bind(("224.0.0.231", 8702))
rx.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
              socket.inet_aton("224.0.0.231") + socket.inet_aton(local))
rx.settimeout(0.5)
tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
opcode = bytes([0x07, 0x38, 0x00, 0x21, 0, 0, 0, 0x64])
req = struct.pack(">HHHH", 0xffff, 32, 1, 0) + bytes(8) + b"Probe\0\0\0" + opcode
deadline = time.time() + 20
while time.time() < deadline:
    tx.sendto(req, (dev, 8700))
    try:
        data, src = rx.recvfrom(2048)
    except socket.timeout:
        continue
    if src[0] != dev or len(data) < 32 + 32 or data[24:28] != bytes([0x07, 0x2a, 0x00, 0x20]):
        continue
    body = data[32:]
    leader = body[20:28]
    print("clock status from %s: leader %s, freq offset %d ppb"
          % (dev, ":".join("%02x" % b for b in leader), struct.unpack(">i", body[8:12])[0]))
    if not any(leader):
        sys.exit(1)
    # Sample rate / encoding (Device Config in Dante Controller).
    req2 = req[:24] + bytes([0x07, 0x38, 0x00, 0x81, 0, 0, 0, 0x64])
    end = time.time() + 5
    while time.time() < end:
        tx.sendto(req2, (dev, 8700))
        try:
            data, src = rx.recvfrom(2048)
        except socket.timeout:
            continue
        if src[0] == dev and data[24:28] == bytes([0x07, 0x2a, 0x00, 0x80]):
            print("sample rate info from %s: %d Hz" % (dev, struct.unpack(">I", data[36:40])[0]))
            sys.exit(0)
    print("no sample rate reply from", dev)
    sys.exit(1)
print("no clock status reply from", dev)
sys.exit(1)
