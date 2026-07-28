// KnxdAckFilterClient.cs
//
// Reference client for knxd's dynamic ack-filter control socket
// (see ../README.md and src/backend/tpuart.cpp's ack_filter_apply()
// for the authoritative protocol definition).
//
// Design notes, so future maintainers don't "fix" this into something
// heavier:
//
//  - AF_UNIX / SOCK_DGRAM, not System.IO.Pipes. .NET's named pipes on
//    Linux are themselves implemented over Unix sockets at a path .NET
//    picks internally -- they do not interoperate with a real named pipe
//    or FIFO you create from C++, so they're not usable here. Talk to
//    the real Unix domain socket directly via System.Net.Sockets.
//
//  - No persistent connection. Every Send() call is a fire-and-forget
//    SendTo() on a connectionless datagram socket. If knxd isn't running,
//    or the socket path doesn't exist yet, SendTo throws SocketException;
//    we catch it, drop the (disposable) socket, and try again fresh next
//    call. There is deliberately no retry loop, no reconnect timer, and
//    no background thread -- sending to knxd is entirely optional and
//    this class must never make it look otherwise.
//
//  - Non-blocking. If knxd's receive queue is ever full (e.g. knxd is
//    stuck), SendTo must fail fast rather than stall the caller.
//
// Requires .NET Core 3.0+ / .NET 5+ (UnixDomainSocketEndPoint on Linux;
// also works from a Linux container/host talking to knxd over a shared
// bind-mounted socket path).
//
// Typical usage:
//
//   using var ackFilter = new KnxdAckFilterClient("/run/knxd/ackfilter.sock");
//
//   // A virtual device just came "up" -- start acking for it:
//   ackFilter.AddGroup(KnxdAckFilterClient.Group3(1, 2, 3));
//
//   // It went away again:
//   ackFilter.DelGroup(KnxdAckFilterClient.Group3(1, 2, 3));
//
//   // Recommended: push a full resync on startup and on a timer
//   // (e.g. every 15-30s) so knxd and the companion program can never
//   // drift apart for long, with zero coordination protocol required:
//   ackFilter.Resync(currentGroupAddresses, currentIndividualAddresses);

using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;

namespace Knxd.AckFilter
{
    /// <summary>
    /// Fire-and-forget client for knxd's dynamic ack-filter control socket.
    /// Safe to construct and use even when knxd (or the socket) isn't
    /// there yet -- every send method returns false instead of throwing.
    /// </summary>
    public sealed class KnxdAckFilterClient : IDisposable
    {
        // --- opcodes (see contrib/ack-filter/README.md) ---
        private const byte OpNop = 0x00;
        private const byte OpAddGroup = 0x01;
        private const byte OpDelGroup = 0x02;
        private const byte OpClearGroup = 0x03;
        private const byte OpAddIndiv = 0x11;
        private const byte OpDelIndiv = 0x12;
        private const byte OpClearIndiv = 0x13;
        private const byte OpClearAll = 0x20;
        private const byte OpSetEnabled = 0x21;

        // knxd's receive buffer is 8192 bytes = 2048 records; stay comfortably under it.
        private const int MaxRecordsPerDatagram = 2000;

        private readonly UnixDomainSocketEndPoint _endpoint;
        private Socket _socket;

        /// <param name="socketPath">
        /// Filesystem path matching knxd's "ack-filter-socket" config value.
        /// </param>
        public KnxdAckFilterClient(string socketPath)
        {
            if (string.IsNullOrEmpty(socketPath))
                throw new ArgumentException("socketPath must be set", nameof(socketPath));
            _endpoint = new UnixDomainSocketEndPoint(socketPath);
        }

        // --- address packing: mirrors TPUARTwrap's ack-filter-file parser
        //     and ack_filter_apply() in src/backend/tpuart.cpp exactly. ---

        /// <summary>Packs a 3-level group address (main/mid/sub, e.g. 1/2/3).</summary>
        public static ushort Group3(int main, int mid, int sub) =>
            (ushort)(((main & 0x1f) << 11) | ((mid & 0x7) << 8) | (sub & 0xff));

        /// <summary>Packs a 2-level group address (main/sub, e.g. 1/2).</summary>
        public static ushort Group2(int main, int sub) =>
            (ushort)(((main & 0x1f) << 11) | (sub & 0x7ff));

        /// <summary>Packs an individual/physical address (area.line.device, e.g. 1.2.3).</summary>
        public static ushort Individual(int area, int line, int device) =>
            (ushort)(((area & 0xf) << 12) | ((line & 0xf) << 8) | (device & 0xff));

        // --- single operations ---

        /// <summary>Start acking the given group address. No-op if already present.</summary>
        public bool AddGroup(ushort addr) => SendOne(OpAddGroup, addr);

        /// <summary>Stop acking the given group address. No-op if not present.</summary>
        public bool DelGroup(ushort addr) => SendOne(OpDelGroup, addr);

        /// <summary>Clear all group addresses (individual addresses unaffected).</summary>
        public bool ClearGroups() => SendOne(OpClearGroup, 0);

        /// <summary>Start acking the given individual/physical address. No-op if already present.</summary>
        public bool AddIndividual(ushort addr) => SendOne(OpAddIndiv, addr);

        /// <summary>Stop acking the given individual/physical address. No-op if not present.</summary>
        public bool DelIndividual(ushort addr) => SendOne(OpDelIndiv, addr);

        /// <summary>Clear all individual addresses (group addresses unaffected).</summary>
        public bool ClearIndividuals() => SendOne(OpClearIndiv, 0);

        /// <summary>
        /// Clear both lists. knxd stays in filter mode afterwards (acks
        /// nothing) rather than reverting to ack-group/ack-individual --
        /// use <see cref="SetEnabled"/> if you actually want that.
        /// </summary>
        public bool ClearAll() => SendOne(OpClearAll, 0);

        /// <summary>
        /// Explicitly enable (true) or disable (false) filter mode. Disabling
        /// reverts knxd to its legacy ack-group/ack-individual/checkSysAddress
        /// behaviour; re-enabling resumes filtering with whatever the lists
        /// currently contain. Rarely needed -- mainly a maintenance-window escape hatch.
        /// </summary>
        public bool SetEnabled(bool enabled)
        {
            var rec = new byte[4];
            rec[0] = OpSetEnabled;
            rec[1] = (byte)(enabled ? 1 : 0);
            return Send(rec);
        }

        /// <summary>Send a no-op record (e.g. as a liveness/connectivity probe).</summary>
        public bool Ping() => SendOne(OpNop, 0);

        // --- batch resync: the recommended steady-state usage pattern ---

        /// <summary>
        /// Replaces knxd's entire ack-filter state in one datagram:
        /// clears both lists, then adds every address given here. Because
        /// every operation is idempotent and this is a single datagram,
        /// calling this on a timer (e.g. every 15-30s) alongside individual
        /// Add/Del calls makes the whole system self-healing -- no sequence
        /// numbers, heartbeats, or acknowledgements required on either side.
        ///
        /// Automatically split into multiple datagrams if the combined
        /// address count would exceed knxd's receive buffer.
        /// </summary>
        public bool Resync(IReadOnlyCollection<ushort> groupAddresses, IReadOnlyCollection<ushort> individualAddresses)
        {
            groupAddresses ??= Array.Empty<ushort>();
            individualAddresses ??= Array.Empty<ushort>();

            var records = new List<byte[]> { MakeRecord(OpClearAll, 0) };
            foreach (var a in groupAddresses)
                records.Add(MakeRecord(OpAddGroup, a));
            foreach (var a in individualAddresses)
                records.Add(MakeRecord(OpAddIndiv, a));

            bool ok = true;
            for (int i = 0; i < records.Count; i += MaxRecordsPerDatagram)
            {
                int count = Math.Min(MaxRecordsPerDatagram, records.Count - i);
                var batch = new byte[count * 4];
                for (int j = 0; j < count; j++)
                    Buffer.BlockCopy(records[i + j], 0, batch, j * 4, 4);
                ok &= Send(batch);
            }
            return ok;
        }

        // --- plumbing ---

        private bool SendOne(byte opcode, ushort addr) => Send(MakeRecord(opcode, addr));

        private static byte[] MakeRecord(byte opcode, ushort addr)
        {
            return new byte[] { opcode, 0, (byte)(addr >> 8), (byte)(addr & 0xff) };
        }

        /// <summary>
        /// Sends a pre-built datagram (one or more concatenated 4-byte
        /// records). Never throws -- returns false on any failure
        /// (knxd not running, socket path missing, queue full, etc.).
        /// </summary>
        public bool Send(byte[] datagram)
        {
            try
            {
                if (_socket == null)
                {
                    _socket = new Socket(AddressFamily.Unix, SocketType.Dgram, ProtocolType.Unspecified)
                    {
                        Blocking = false
                    };
                }
                _socket.SendTo(datagram, _endpoint);
                return true;
            }
            catch (SocketException)
            {
                // knxd not running, path missing, EAGAIN on a full queue, ...
                ResetSocket();
                return false;
            }
            catch (ObjectDisposedException)
            {
                ResetSocket();
                return false;
            }
            catch (IOException)
            {
                ResetSocket();
                return false;
            }
        }

        private void ResetSocket()
        {
            try { _socket?.Dispose(); } catch { /* best effort */ }
            _socket = null;
        }

        public void Dispose() => ResetSocket();
    }
}
