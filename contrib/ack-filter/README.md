# Dynamic ack-filter control socket

`tpuarts`/`ncn5120` drivers can send a bus-level ("virtual") KNX ack for
addresses that have no real device behind them, so that a sender doesn't
think its telegram was lost. Which addresses get this treatment is the
`ack-filter-group` / `ack-filter-indiv` list inside knxd, normally seeded
once at startup from a text file (`ack-filter-file`, see `doc/inifile.rst`).

This directory adds a way to change that list **while knxd keeps running**,
driven by a separate companion program (e.g. a building-automation
controller that knows, at any given moment, which virtual devices are
currently "up" and should therefore be acked). knxd itself never parses
JSON or any other structured text for this -- the wire protocol is a
handful of fixed-size binary records over a Unix domain socket, deliberately
small enough that both the C++ and the C# side of it are almost too simple
to get wrong.

**Using this is entirely optional.** If `ack-filter-socket` isn't set in
your knxd config, none of this exists at runtime. If it is set but the
companion program never runs, or never sends anything, knxd just keeps
using whatever `ack-filter-file` (if any) gave it at startup -- nothing
blocks, times out, or degrades.

## Configuration (knxd side)

```ini
[tpuarts-eth1]
driver = tpuarts
device = /dev/ttyKNX1
ack-filter-file = /etc/knxd/ack-filter.txt      ; optional initial seed
ack-filter-socket = /run/knxd/ackfilter.sock    ; optional, enables dynamic control
ack-filter-socket-mode = 0660                   ; optional, default 0660
```

Make sure the directory (e.g. `/run/knxd`) exists before knxd starts --
under systemd, add `RuntimeDirectory=knxd` to the unit, or point the path
somewhere that already exists and is writable by knxd.

Once `ack-filter-socket` is configured (and binds successfully), knxd acks
**only** the addresses in the list -- starting from whatever
`ack-filter-file` seeded, or nothing at all if that's unset. This is
deliberately fail-safe: a companion process that hasn't started yet (or
never starts) means knxd acks nothing on its behalf, rather than silently
falling back to acking everything.

## Wire protocol

Transport: `AF_UNIX` / `SOCK_DGRAM` (a connectionless Unix datagram
socket). No connection to establish, no reconnect logic needed, message
boundaries are preserved by the kernel, and there is nothing to hang or
time out on either end if the other side isn't there.

Each datagram holds one or more fixed 4-byte records, applied in order:

```
byte 0: opcode
byte 1: flag        (only meaningful for SET_ENABLED; must be 0 otherwise)
byte 2: address hi  (big-endian, packed 16-bit KNX address)
byte 3: address lo
```

| Opcode | Name         | Effect                                              |
|-------:|--------------|------------------------------------------------------|
| `0x00` | NOP          | No-op (harmless liveness/padding record)              |
| `0x01` | ADD_GROUP    | Add a group address to the ack list                   |
| `0x02` | DEL_GROUP    | Remove a group address                                |
| `0x03` | CLEAR_GROUP  | Clear all group addresses                             |
| `0x11` | ADD_INDIV    | Add an individual/physical address                    |
| `0x12` | DEL_INDIV    | Remove an individual/physical address                 |
| `0x13` | CLEAR_INDIV  | Clear all individual addresses                         |
| `0x20` | CLEAR_ALL    | Clear both lists (stays in filter mode -- acks nothing afterwards, does **not** fall back to `ack-group`/`ack-individual`) |
| `0x21` | SET_ENABLED  | `flag=0` reverts to legacy `ack-group`/`ack-individual` behaviour; `flag=1` re-enables filter mode |

Anything else in byte 0 is logged and that one record is skipped; the rest
of the batch still applies. A datagram whose length isn't a multiple of 4,
or that's larger than knxd's 8 KiB receive buffer, is dropped **in its
entirety** and logged -- it is never applied partially.

Address packing (identical to what `ack-filter-file` already uses, and to
what's on the KNX wire):

```
3-level group address x/y/z:  (x & 0x1f) << 11 | (y & 0x7) << 8 | (z & 0xff)
2-level group address x/y:    (x & 0x1f) << 11 | (y & 0x7ff)
individual address x.y.z:     (x & 0xf)  << 12 | (y & 0xf) << 8 | (z & 0xff)
```

There is no reply, no handshake, and no version field -- the socket is a
private, permission-controlled control channel (`chmod` via
`ack-filter-socket-mode`), and the fixed record size plus the
unknown-opcode/malformed-length rules above already make bad input
harmless.

### Recommended pattern: periodic full resync

Rather than trying to keep knxd's list and the companion program's state
in lockstep with individual ADD/DEL messages, send a full resync
(`CLEAR_ALL` followed by one `ADD_*` per currently-active address) as a
single datagram, on startup and again every 15-30 seconds. Every operation
here is idempotent, so this makes the whole system self-healing with no
sequence numbers or acknowledgements needed: if knxd restarts, the next
resync repopulates it; if a datagram is dropped, the next resync corrects
the drift.

## Files in this directory

- `KnxdAckFilterClient.cs` -- reference C# client (`System.Net.Sockets`,
  no `System.IO.Pipes` -- see the design notes in the file header for why).
  Drop it into your companion program, point it at the same path as
  `ack-filter-socket`, and call `AddGroup`/`DelGroup`/`ClearAll`/`Resync`.
- `ackfilter_test.py` -- a small standalone script (stdlib only) for
  manually poking a running knxd instance from the command line, useful
  for verifying the socket works before wiring up the real C# side.
