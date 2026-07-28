#!/usr/bin/env python3
"""
Manual test / debugging tool for knxd's dynamic ack-filter control socket.

This talks the same tiny binary protocol the C# client
(KnxdAckFilterClient.cs) uses -- see ../README.md for the full spec. It's
meant for poking a running knxd instance from the command line while
watching its trace log (run knxd with a high enough trace level, e.g.
`-t 1023` or the section's `trace-mask`, to see "ack-filter-socket: op ..."
lines at level 8), to verify the socket works before wiring up the real
companion program.

Requires only the Python standard library.

Examples
--------

    # Add group address 1/2/3 to the ack list
    ./ackfilter_test.py /run/knxd/ackfilter.sock add-group 1/2/3

    # Remove it again
    ./ackfilter_test.py /run/knxd/ackfilter.sock del-group 1/2/3

    # Add an individual address
    ./ackfilter_test.py /run/knxd/ackfilter.sock add-indiv 1.1.5

    # Clear everything
    ./ackfilter_test.py /run/knxd/ackfilter.sock clear-all

    # Full resync in one datagram: clear, then add a batch
    ./ackfilter_test.py /run/knxd/ackfilter.sock resync 1/2/3 1/2/4 1.1.5

    # Explicitly leave/re-enter filter mode
    ./ackfilter_test.py /run/knxd/ackfilter.sock set-enabled off
    ./ackfilter_test.py /run/knxd/ackfilter.sock set-enabled on
"""
import socket
import struct
import sys

OP_NOP = 0x00
OP_ADD_GROUP = 0x01
OP_DEL_GROUP = 0x02
OP_CLEAR_GROUP = 0x03
OP_ADD_INDIV = 0x11
OP_DEL_INDIV = 0x12
OP_CLEAR_INDIV = 0x13
OP_CLEAR_ALL = 0x20
OP_SET_ENABLED = 0x21


def group_addr(s):
    """Parse '1/2/3' or '1/2' into knxd's packed 16-bit form."""
    parts = [int(p) for p in s.split('/')]
    if len(parts) == 3:
        main, mid, sub = parts
        return ((main & 0x1f) << 11) | ((mid & 0x7) << 8) | (sub & 0xff)
    if len(parts) == 2:
        main, sub = parts
        return ((main & 0x1f) << 11) | (sub & 0x7ff)
    raise ValueError(f"not a group address: {s!r}")


def indiv_addr(s):
    """Parse '1.2.3' into knxd's packed 16-bit form."""
    area, line, dev = (int(p) for p in s.split('.'))
    return ((area & 0xf) << 12) | ((line & 0xf) << 8) | (dev & 0xff)


def is_group(s):
    return '/' in s


def record(opcode, addr=0, flag=0):
    return struct.pack("!BBH", opcode, flag, addr)


def send(path, payload):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.sendto(payload, path)
    finally:
        s.close()


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1

    path, cmd, *rest = argv[1:]

    if cmd == "add-group":
        send(path, record(OP_ADD_GROUP, group_addr(rest[0])))
    elif cmd == "del-group":
        send(path, record(OP_DEL_GROUP, group_addr(rest[0])))
    elif cmd == "clear-group":
        send(path, record(OP_CLEAR_GROUP))
    elif cmd == "add-indiv":
        send(path, record(OP_ADD_INDIV, indiv_addr(rest[0])))
    elif cmd == "del-indiv":
        send(path, record(OP_DEL_INDIV, indiv_addr(rest[0])))
    elif cmd == "clear-indiv":
        send(path, record(OP_CLEAR_INDIV))
    elif cmd == "clear-all":
        send(path, record(OP_CLEAR_ALL))
    elif cmd == "ping":
        send(path, record(OP_NOP))
    elif cmd == "set-enabled":
        flag = 1 if rest[0].lower() in ("on", "true", "1", "yes") else 0
        send(path, record(OP_SET_ENABLED, 0, flag))
    elif cmd == "resync":
        batch = record(OP_CLEAR_ALL)
        for a in rest:
            if is_group(a):
                batch += record(OP_ADD_GROUP, group_addr(a))
            else:
                batch += record(OP_ADD_INDIV, indiv_addr(a))
        send(path, batch)
    elif cmd == "malformed":
        # Sends a 3-byte (non-multiple-of-4) datagram -- should be logged
        # and dropped by knxd without affecting anything else.
        send(path, b"\x01\x00\x01")
    else:
        print(f"unknown command: {cmd}", file=sys.stderr)
        print(__doc__)
        return 1

    print(f"sent '{cmd}' to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
