/*
    EIBD eib bus access and management daemon
    Copyright (C) 2005-2011 Martin Koegler <mkoegler@auto.tuwien.ac.at>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <unistd.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "tpuart.h"
#include "router.h"

#define NO_MAP
#include "nat.h"
#include "llserial.h"
#include "lltcp.h"
#include "log.h"
#include "cm_tp1.h"
#include <fstream>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/stat.h>

/* add formatter for fmt >= 10.0.0 */
int format_as(LPDU_Type t) { return t; }

/* --- dynamic ack-filter control socket: wire format -----------------------
 *
 * One UDP-like datagram (AF_UNIX, SOCK_DGRAM) holds 1..N fixed 4-byte
 * records, applied in order:
 *
 *   byte 0: opcode (see ACK_OP_* below)
 *   byte 1: flag (only used by ACK_OP_SET_ENABLED; must be 0 otherwise)
 *   byte 2: address, high byte (big-endian, packed 16 bit KNX address)
 *   byte 3: address, low byte
 *
 * A datagram whose length isn't a multiple of 4, or that was truncated
 * (MSG_TRUNC), is dropped in its entirety -- never applied partially, so
 * the filter state can never desync from a malformed message. Unknown
 * opcodes are skipped (that one record only) so a batch keeps applying.
 * There is no reply and no handshake: this is a fire-and-forget control
 * channel, safe to leave permanently unused.
 */
static const uint8_t ACK_OP_NOP          = 0x00;
static const uint8_t ACK_OP_ADD_GROUP    = 0x01;
static const uint8_t ACK_OP_DEL_GROUP    = 0x02;
static const uint8_t ACK_OP_CLEAR_GROUP  = 0x03;
static const uint8_t ACK_OP_ADD_INDIV    = 0x11;
static const uint8_t ACK_OP_DEL_INDIV    = 0x12;
static const uint8_t ACK_OP_CLEAR_INDIV  = 0x13;
static const uint8_t ACK_OP_CLEAR_ALL    = 0x20;
static const uint8_t ACK_OP_SET_ENABLED  = 0x21;

static const size_t ACK_SOCK_BUFSZ = 8192;             // 2048 records/datagram
static const int ACK_SOCK_MAX_DGRAMS_PER_CB = 32;       // event-loop starvation guard

class TPUARTserial : public LLserial
{
public:
  TPUARTserial(LowLevelIface* a, IniSectionPtr& b) : LLserial(a,b)
  {
    t->setAuxName("TPU_ser");
  }
  virtual ~TPUARTserial() = default;
protected:
  void termios_settings (struct termios &t1)
  {
    t1.c_cflag = CS8 | CLOCAL | CREAD | PARENB;
    t1.c_iflag = IGNBRK | ISIG;
    t1.c_oflag = 0;
    t1.c_lflag = 0;
    t1.c_cc[VTIME] = 1;
    t1.c_cc[VMIN] = 0;
  }
  unsigned int default_baudrate()
  {
    return 19200;
  }
};

LowLevelFilter *
TPUART::create_wrapper(LowLevelIface* parent, IniSectionPtr& s, LowLevelDriver* i)
{
  return new TPUARTwrap(parent,s, i);
}

LLserial *
TPUARTwrap::create_serial(LowLevelIface* parent, IniSectionPtr& s)
{
  return new TPUARTserial(parent, s);
}


static const char* SN(enum TSTATE s)
{
  static int x = 0;
  static char buf[2][10];
  switch(s)
    {
    case T_new:
      return "new";
    case T_error:
      return "error";
    case T_start:
      return "start";
    case T_in_reset:
      return "in_reset";
    case T_in_setaddr:
      return "in_setaddr";
    case T_in_getstate:
      return "in_getstate";
    case T_is_online:
      return "is_online";
    case T_wait:
      return "wait";
    case T_wait_more:
      return "wait_more";
    case T_wait_keepalive:
      return "wait_keepalive";
    case T_busmonitor:
      return "busmonitor";
    default:
      x = 1-x;
      sprintf(buf[x],"? %d",s);
      return buf[x];
    }
}

bool
TPUART::setup()
{
  iface = create_wrapper(this, cfg);

  if (t->ShowPrint(0))
    iface = new LLlog (this,cfg, iface);

  if (!LowLevelAdapter::setup())
    return false;

  return true;
}

bool
TPUARTwrap::setup()
{
  ackallgroup = cfg->value("ack-group",false);
  ackallindividual = cfg->value("ack-individual",false);
  {
    std::string aff = cfg->value("ack-filter-file", "");
    if (!aff.empty())
      {
        std::ifstream fin(aff);
        if (!fin)
          ERRORPRINTF(t, E_ERROR | 44, "ack-filter-file: impossibile aprire %s", aff.c_str());
        else
          {
            std::string line; int n = 0;
            while (std::getline(fin, line))
              {
                size_t h = line.find('#'); if (h != std::string::npos) line = line.substr(0, h);
                size_t a = line.find_first_not_of(" \t\r\n"); if (a == std::string::npos) continue;
                size_t b = line.find_last_not_of(" \t\r\n");
                std::string tok = line.substr(a, b - a + 1);
                int x, y, z;
                if (sscanf(tok.c_str(), "%d/%d/%d", &x, &y, &z) == 3)
                  ack_filter_group.insert(((x & 0x1f) << 11) | ((y & 0x7) << 8) | (z & 0xff));
                else if (sscanf(tok.c_str(), "%d/%d", &x, &y) == 2)
                  ack_filter_group.insert(((x & 0x1f) << 11) | (y & 0x7ff));
                else if (sscanf(tok.c_str(), "%d.%d.%d", &x, &y, &z) == 3)
                  ack_filter_indiv.insert(((x & 0xf) << 12) | ((y & 0xf) << 8) | (z & 0xff));
                else
                  { ERRORPRINTF(t, E_ERROR | 44, "ack-filter-file: indirizzo non valido '%s'", tok.c_str()); continue; }
                n++;
              }
            ack_filter_active = true;
            ERRORPRINTF(t, E_INFO | 45, "ack-filter: %d indirizzi (%d gruppo, %d individuali) da %s",
                        n, (int)ack_filter_group.size(), (int)ack_filter_indiv.size(), aff.c_str());
          }
      }
  }
  {
    std::string asp = cfg->value("ack-filter-socket", "");
    if (!asp.empty())
      {
        std::string modestr = cfg->value("ack-filter-socket-mode", "0660");
        char *end = nullptr;
        unsigned long mode = strtoul(modestr.c_str(), &end, 8);
        if (end == modestr.c_str() || mode > 0777)
          {
            ERRORPRINTF(t, E_WARNING | 160, "ack-filter-socket-mode '%s' non valido, uso 0660", modestr.c_str());
            mode = 0660;
          }
        // A live control socket means an external process is expected to
        // decide who gets acked; fail safe by acking nobody until told
        // otherwise, rather than silently falling back to ack-group /
        // ack-individual / checkSysAddress while the companion process
        // is still starting up (or never starts at all).
        if (ack_filter_socket_setup(asp, (unsigned int)mode))
          ack_filter_active = true;
        // on failure, ack_filter_socket_setup() already logged why;
        // the feature just stays disabled and knxd continues normally.
      }
  }
  monitor = cfg->value("monitor",false);

  if (cfg->value("device","").length() > 0)
    {
      if (cfg->value("ip-address","").length() > 0 ||
          cfg->value("dest-port",-1) != -1)
        {
          ERRORPRINTF (t, E_ERROR | 25, "Don't specify both device and IP options!");
          return false;
        }
      ll_serial = create_serial(this, cfg);
      iface = ll_serial;
    }
  else
    {
      if (cfg->value("baudrate",-1) != -1)
        {
          ERRORPRINTF (t, E_ERROR | 33, "Don't specify both device and IP options!");
          return false;
        }
      iface = new LLtcp(this, cfg);
    }

  if (t->ShowPrint(0))
    iface = new LLlog (this,cfg, iface);

  FilterPtr single = findFilter("single");
  if (single != nullptr)
    {
      std::shared_ptr<NatL2Filter> f = std::dynamic_pointer_cast<NatL2Filter>(single);
      if (f)
        my_addr = f->addr;
    }

  if (!LowLevelFilter::setup())
    return false;

  return true;
}

TPUARTwrap::~TPUARTwrap ()
{
  TRACEPRINTF (t, 2, "Close C");

  timer.stop();
  sendtimer.stop();
  ack_filter_socket_stop();
}

bool
TPUARTwrap::ack_filter_socket_setup(const std::string &path, unsigned int mode)
{
  bool abstract = (!path.empty() && path[0] == '@');
  const std::string &name = abstract ? path.substr(1) : path;

  struct sockaddr_un sa;
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;

  // +1 for the trailing NUL (filesystem path) or the leading NUL (abstract)
  if (name.size() + 1 > sizeof(sa.sun_path))
    {
      ERRORPRINTF(t, E_WARNING | 161, "ack-filter-socket: path troppo lungo: %s", path.c_str());
      return false;
    }

  socklen_t alen;
  if (abstract)
    {
      // Linux abstract namespace: sun_path[0] == '\0', no filesystem entry,
      // no stale-socket cleanup needed, name vanishes when we close the fd.
      memcpy(sa.sun_path + 1, name.data(), name.size());
      alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name.size());
    }
  else
    {
      memcpy(sa.sun_path, name.data(), name.size());
      alen = sizeof(sa);
    }

  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd == -1)
    {
      ERRORPRINTF(t, E_WARNING | 162, "ack-filter-socket: socket: %s", strerror(errno));
      return false;
    }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  set_non_blocking(fd);

  if (bind(fd, (struct sockaddr *)&sa, alen) == -1)
    {
      bool bound = false;
      if (errno == EADDRINUSE && !abstract)
        {
          // Possibly a leftover socket file from a crashed knxd; probe it
          // the same way LocalServer does before stealing/removing it.
          int probe = socket(AF_UNIX, SOCK_DGRAM, 0);
          if (probe != -1)
            {
              if (connect(probe, (struct sockaddr *)&sa, alen) == -1 && errno == ECONNREFUSED)
                {
                  ::unlink(name.c_str());
                  if (bind(fd, (struct sockaddr *)&sa, alen) == 0)
                    bound = true;
                }
              close(probe);
            }
        }
      if (!bound)
        {
          ERRORPRINTF(t, E_WARNING | 162, "ack-filter-socket: bind %s: %s", path.c_str(), strerror(errno));
          close(fd);
          return false;
        }
    }

  if (!abstract && chmod(name.c_str(), (mode_t)mode) == -1)
    ERRORPRINTF(t, E_WARNING | 163, "ack-filter-socket: chmod %s: %s", path.c_str(), strerror(errno));

  ack_sock_fd = fd;
  ack_sock_path = abstract ? std::string() : name;
  ack_sock_io.start(ack_sock_fd, ev::READ);
  ERRORPRINTF(t, E_INFO | 167, "ack-filter-socket: in ascolto su %s", path.c_str());
  return true;
}

void
TPUARTwrap::ack_filter_socket_stop()
{
  if (ack_sock_fd == -1)
    return;
  ack_sock_io.stop();
  close(ack_sock_fd);
  ack_sock_fd = -1;
  if (!ack_sock_path.empty())
    {
      ::unlink(ack_sock_path.c_str());
      ack_sock_path.clear();
    }
}

void
TPUARTwrap::ack_filter_apply(const uint8_t *rec)
{
  uint16_t addr = ((uint16_t)rec[2] << 8) | rec[3];

  switch (rec[0])
    {
    case ACK_OP_NOP:
      return;
    case ACK_OP_ADD_GROUP:
      ack_filter_group.insert(addr);
      break;
    case ACK_OP_DEL_GROUP:
      ack_filter_group.erase(addr);
      break;
    case ACK_OP_CLEAR_GROUP:
      ack_filter_group.clear();
      break;
    case ACK_OP_ADD_INDIV:
      ack_filter_indiv.insert(addr);
      break;
    case ACK_OP_DEL_INDIV:
      ack_filter_indiv.erase(addr);
      break;
    case ACK_OP_CLEAR_INDIV:
      ack_filter_indiv.clear();
      break;
    case ACK_OP_CLEAR_ALL:
      // Empties the lists but stays in filter mode (i.e. acks nothing
      // afterwards) -- it must never widen acking back to "everything",
      // which is what would happen if this fell back to ack-group /
      // ack-individual. Use ACK_OP_SET_ENABLED to explicitly leave
      // filter mode.
      ack_filter_group.clear();
      ack_filter_indiv.clear();
      break;
    case ACK_OP_SET_ENABLED:
      ack_filter_active = (rec[1] != 0);
      TRACEPRINTF(t, 3, "ack-filter %s via socket", ack_filter_active ? "enabled" : "disabled");
      return;
    default:
      ERRORPRINTF(t, E_WARNING | 166, "ack-filter-socket: opcode sconosciuto 0x%02X, record ignorato", rec[0]);
      return;
    }

  TRACEPRINTF(t, 8, "ack-filter-socket: op 0x%02X addr %04X -> group=%zu indiv=%zu",
              rec[0], addr, ack_filter_group.size(), ack_filter_indiv.size());
}

void
TPUARTwrap::ack_sock_read_cb(ev::io &, int)
{
  uint8_t buf[ACK_SOCK_BUFSZ];

  for (int guard = 0; guard < ACK_SOCK_MAX_DGRAMS_PER_CB; guard++)
    {
      struct iovec iov;
      iov.iov_base = buf;
      iov.iov_len = sizeof(buf);
      struct msghdr mh;
      memset(&mh, 0, sizeof(mh));
      mh.msg_iov = &iov;
      mh.msg_iovlen = 1;

      ssize_t n = recvmsg(ack_sock_fd, &mh, 0);
      if (n < 0)
        {
          if (errno == EINTR)
            {
              guard--;
              continue;
            }
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            return; // queue drained, normal case
          ERRORPRINTF(t, E_WARNING | 164, "ack-filter-socket: recv: %s", strerror(errno));
          return; // never close the fd, never spin -- just try again next time
        }
      if (n == 0)
        continue; // legal no-op datagram

      if (mh.msg_flags & MSG_TRUNC)
        {
          ERRORPRINTF(t, E_WARNING | 165, "ack-filter-socket: datagram troppo grande (> %zu byte), scartato", ACK_SOCK_BUFSZ);
          continue;
        }
      if ((n % 4) != 0)
        {
          ERRORPRINTF(t, E_WARNING | 165, "ack-filter-socket: lunghezza %zd non multipla di 4, scartato", n);
          continue;
        }

      for (ssize_t i = 0; i < n; i += 4)
        ack_filter_apply(buf + i);
    }
  // guard hit: remaining datagrams, if any, are handled on the next
  // loop iteration (the watcher is level-triggered).
}

void
TPUARTwrap::send_L_Data (LDataPtr l)
{
  assert(out.size() == 0);
  out = L_Data_to_CM_TP1 (l);

  send_again();
}

/* ignore low level send_Next -- just assume that this works */
void
TPUARTwrap::do_send_Next()
{
  next_free = true;
  if (send_wait)
    {
      send_wait = false;
      send_again();
    }
}

void
TPUARTwrap::do__send_Next()
{
  out.clear();
  send_retry = 0;
  sendtimer.stop();
  LowLevelFilter::do_send_Next();
}

void
TPUARTwrap::send_again()
{
  if (out.size() > 0 && state > T_is_online && state < T_busmonitor)
    {
      if (!next_free)
        {
          send_wait = true;
          return;
        }

      CArray w;
      unsigned i;
      unsigned z = out.size();

      w.resize (z * 2);
      for (i = 0; i < z; i++)
        {
          w[2 * i] = 0x80 | (i & 0x3f);
          w[2 * i + 1] = out[i];
        }
      z = (z - 1) * 2;
      w[z] = (w[z] & 0x3f) | 0x40;
      LowLevelFilter::send_Data(w);
      sendtimer.start(2,0);

      if (out[0] & 0x20)
        {
          // clear retry flag. for later comparison
          out[0] ^= 0x20;
          out[out.size()-1] ^= 0x20; // fix the checksum
        }
    }
}

void
TPUARTwrap::started()
{
  setstate(T_new);
  setstate(T_start);
}

void
TPUARTwrap::stopped(bool err)
{
  setstate(T_new);

  LowLevelFilter::stopped(err);
}

void
TPUARTwrap::RecvLPDU (const uint8_t * data, int len)
{
  t->TracePacket (1, "RecvLP", len, data);
  if (state == T_busmonitor)
    {
      LBusmonPtr l = LBusmonPtr(new L_Busmon_PDU ());
      l->lpdu.set (data, len);
      recv_L_Busmonitor (std::move(l));
    }
  else if (state > T_start)
    {
      if (LPDUPtr l = CM_TP1_to_L_Data (CArray (data, len), t))
        {
          if (l->getType () != L_Data)
            TRACEPRINTF (t, 1, "dropping packet: type %d", l->getType ());
          else
            {
              if (((L_Data_PDU *)(&*l))->valid_checksum)
                recv_L_Data (dynamic_unique_cast<L_Data_PDU>(std::move(l)));
              else
                TRACEPRINTF (t, 1, "dropping packet: checksum invalid");
            }
        }
      else
        {
          TRACEPRINTF (t, 1, "dropping packet: invalid");
        }
    }
}

TPUARTwrap::TPUARTwrap(LowLevelIface* parent, IniSectionPtr& s, LowLevelDriver* i) : LowLevelFilter(parent,s,i)
{
  timer.set <TPUARTwrap,&TPUARTwrap::timer_cb> (this);
  sendtimer.set <TPUARTwrap,&TPUARTwrap::sendtimer_cb> (this);
  ack_sock_io.set <TPUARTwrap,&TPUARTwrap::ack_sock_read_cb> (this);
}

void
TPUARTwrap::sendtimer_cb(ev::timer &, int)
{
  if (send_retry++ > 3)
    {
      ERRORPRINTF (t, E_ERROR | 43, "send timeout: too many retries");
      setstate(T_error);
      return;
    } // TODO error
  TRACEPRINTF (t, 8, "send timeout: retry");
  send_again();
}

void
TPUARTwrap::timer_cb(ev::timer &, int)
{
  switch(state)
    {
    case T_error:
      stop(true);
      break;
    case T_new:
      break;
    case T_in_reset:
      if (retry < 3)
        {
          setstate(T_in_reset);
          return;
        }
      setstate(T_error);
      break;

    case T_in_getstate:
      if (retry > 5)
        {
          stop(true);
          return;
        }
      setstate(state);
      break;

    case T_in_setaddr:
    {
      uint8_t addrbuf[2] = { (uint8_t)((my_addr>>8)&0xFF), (uint8_t)(my_addr&0xFF) };
      TRACEPRINTF (t, 0, "SendAddr %02X%02X", addrbuf[0],addrbuf[1]);
      LowLevelIface::send_Data(CArray(addrbuf, sizeof(addrbuf)));
      setstate(T_in_getstate);
    }
    break;

    case T_wait:
      setstate(T_wait_keepalive);
      break;
    case T_wait_more:
      t->TracePacket (8, "Incomplete packet", in);
      in.clear();
      setstate(T_wait);
      break;
    case T_wait_keepalive:
      if (retry > 2)
        {
          setstate(T_in_reset);
          return;
        }
      setstate(T_wait_keepalive);
      break;
    default:
      TRACEPRINTF (t, 8, "Timeout in state %s",SN(state));
      break;
    }
}

int
TPUARTwrap::enable_input_parity_check()
{
  if (ll_serial == nullptr)
  {
    // Not possible and not necessary to enable on TCP connections, so just continue.
    return 0;
  }

  return ll_serial->enable_input_parity_check();
}

void
TPUARTwrap::in_check()
{
  bool ext = !(in[0] & 0x80);

  if (in.size () >= 6u+ext)
    {

      if (!acked && !recvecho && my_addr == 0 && state >= T_is_online && state < T_busmonitor)
        {
          if (out.size() >= 6u+ext && !((in[0]^out[0])&~0x20) && !memcmp(in.data()+1,out.data()+1,5+ext))
            recvecho = true;
          else
            {
              uint8_t c = 0x10;
              uint16_t ackdst = (in[3+ext] << 8) | in[4+ext];
              bool ackgrp = (in[ext ? 1 : 5] & 0x80) != 0;
              bool do_ack;
              if (ack_filter_active)
                do_ack = ackgrp ? (ack_filter_group.find(ackdst) != ack_filter_group.end())
                                : (ack_filter_indiv.find(ackdst) != ack_filter_indiv.end());
              else if (!ackgrp)
                do_ack = ackallindividual || checkSysAddress (ackdst);
              else
                do_ack = ackallgroup || checkSysGroupAddress (ackdst);
              if (do_ack)
                c |= 0x1;
              TRACEPRINTF (t, 0, "SendAck %02X", c);
              LowLevelIface::send_Data(c);
              acked = true;
            }
        }

      unsigned len = ext ? in[6] : (in[5] & 0x0f);
      len += 6 + ext + 2;

      if (in.size() > len)
        TRACEPRINTF (t, 8, "Datalen %d has len %d?", len, in.size());

      if (in.size() >= len)
        {
          if (!recvecho)
            RecvLPDU (in.data(), in.size());
          in.clear();
        }
    }

  if (state > T_is_online && state < T_busmonitor)
    {
      if (in.size() == 0)
        setstate(T_wait);
      else
        setstate(T_wait_more);
    }
}

void
TPUARTwrap::recv_Data(CArray &c)
{
  uint8_t *buf = c.data();
  size_t len = c.size();

  if (state < T_start)
    {
      t->TracePacket (0, "ReadDrop", len, buf);
      return; // discard
    }

  while(len--)
    {
      uint8_t c = *buf++;
      if (in.size() > 0)
        {
          in.setpart (&c, in.size(), 1);
          in_check();
          continue;
        }
      if (skip_char)
        {
          skip_char = false;
          continue;
        }

      if (c == 0x03) // RESET
        {
          if (state == T_in_reset)
            {
              TRACEPRINTF (t, 8, "RESET_ACK");
              if (enable_input_parity_check() >= 0)
                setstate(T_in_setaddr);
              // else time out
            }
          else
            TRACEPRINTF (t, 8, "spurious RESET_ACK");
        }
      else if (c == 0x8B) // L_DataConfirm positive
        {
          if (out.size() == 0 || state < T_is_online)
            {
              TRACEPRINTF (t, 8, "ACK: but not sending");
              continue;
            }
          do__send_Next();
          continue;
        }
      else if (c == 0xCB) // frame end, NCN5120
        { }
      else if (c == 0x0B) // L_DataConfirm negative
        {
          if (out.size() == 0 || state < T_is_online)
            {
              TRACEPRINTF (t, 8, "NACK: but not sending");
              continue;
            }
          do__send_Next();
          continue;
        }
      else if ((c & 0x17) == 0x13) // frame state indication, NCN5120
        { }
      else if ((c & 0x07) == 0x07) // state indication
        {
          TRACEPRINTF (t, 8, "State: %02X", c);
          if (c != 0x07)
            ERRORPRINTF (t, E_WARNING | 116, "TPUART error state x%02X", c);

          switch(state)
            {
            case T_wait_keepalive:
              setstate(T_wait);
              break;
            case T_in_reset:
              // setstate(T_in_reset); // do not immediately retry
              break;
            case T_in_setaddr:
              // if (c == 0x47)
              //   {
              //     ERRORPRINTF (t, E_ERROR | 62, "TPUART detected. Hardware ACK not supported.");
              //     my_addr = 0;
              //   }
              setstate(T_in_getstate);
              break;
            case T_in_getstate:
              setstate(T_is_online);
              break;

            default:
              ERRORPRINTF (t, E_WARNING | 117, "TPUART state %s should not happen", SN(state));
              break;
            }
        }
      /*
        * 0xCC acknowledge frame
        * 0x0C NotAcknowledge frame
        * 0xC0 Busy Frame
        */
      else if (c == 0xCC || c == 0xC0 || c == 0x0C)
        {
          RecvLPDU (&c, 1);
        }
      else if ((c & 0x50) == 0x10) // Matches KNX control byte L_Data_Standard/Extended Frame
        {
          assert(!in.size());
          in.setpart (&c, in.size(), 1);
        }
      else
        {
          acked = false;
          TRACEPRINTF (t, 0, "unknown %02X", c);
        }
    }
  return;
}

void
TPUARTwrap::setstate(enum TSTATE new_state)
{
  if (state != new_state)
    TRACEPRINTF (t, 8, "state: %s > %s", SN(state),SN(new_state));

  if (state < T_is_online && new_state >= T_is_online)
    {
      LowLevelFilter::started();
      if (monitor)
        new_state = T_busmonitor;
      else if (new_state < T_busmonitor)
        send_again();
    }

  switch(new_state)
    {
    case T_start:
      new_state = T_in_reset;
    /* fall thru */
    case T_in_reset:
      if (state == T_in_reset)
        retry++;
      else
        retry = 1;
      {
        uint8_t c = 0x01;
        TRACEPRINTF (t, 0, "SendReset %02X", c);
        LowLevelIface::send_Data(c);
      }
      timer.start(0.5,0);
      break;

    case T_in_setaddr:
      if (my_addr)
        {
          if(1)
            {
              uint8_t addrbuf[3] = { 0x28, (uint8_t)((my_addr>>8)&0xFF), (uint8_t)(my_addr&0xFF) };
              TRACEPRINTF (t, 0, "SendAddr %02X%02X", addrbuf[1],addrbuf[2]);
              LowLevelIface::send_Data(CArray(addrbuf, sizeof(addrbuf)));
            }
          else
            {
              uint8_t c = 0x28;
              TRACEPRINTF (t, 0, "SendAddr %02X", c);
              LowLevelIface::send_Data(c);
              timer.start(0.2,0);
              break;
            }
        }
      new_state = T_in_getstate;
      TRACEPRINTF (t, 8, "addr zero: %s > %s", SN(state),SN(new_state));
    // FALL THRU
    case T_in_getstate:
    {
      uint8_t c = 0x02;
      TRACEPRINTF (t, 0, "Send GetState %02X", c);
      LowLevelIface::send_Data(c);
      timer.start(0.5,0);
    }
    break;

    case T_busmonitor:
    {
      uint8_t c = 0x05;
      TRACEPRINTF (t, 0, "Send openBusmonitor %02X", c);
      LowLevelIface::send_Data(c);
    }
    break;

    case T_is_online:
      new_state = T_wait;
      do__send_Next();
    // fall thru
    case T_wait:
      timer.start(10,0);
      acked = false;
      recvecho = false;
      break;

    case T_wait_more:
      timer.start(1,0);
      break;

    case T_wait_keepalive:
    {
      if (state == T_wait_keepalive)
        retry++;
      else
        retry = 1;

      uint8_t c = 0x02;
      TRACEPRINTF (t, 0, "Send keepalive GetState %02X", c);
      LowLevelIface::send_Data(c);
      timer.start(0.5,0);
      break;
    }

    case T_error:
      timer.start(1,0);
      break;

    default:
      break;
    }
  state = new_state;
}
