// xam.xex networking exports (NetDll_*): address parsing, fd_set tests, and the console's sockets on the host's.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>

#include "guest.h"

namespace any360::sys {

// Winsock inet_addr on a NUL-terminated string: the address in host order (a.b.c.d -> 0xAABBCCDD), 0xFFFFFFFF when
// it does not parse, 0 for the empty string.
uint32_t NetDll_inet_addr(Guest g, uint32_t string);
// Writes a dotted string for the address into `out` (`size` bytes including the NUL).
void NetDll_XNetInAddrToString(Guest g, uint32_t in_addr, uint32_t out, uint32_t size);
// 1 when `socket` is one of the first min(fd_count, 64) handles of the fd_set.
uint32_t NetDll___WSAFDIsSet(Guest g, uint32_t socket, uint32_t fd_set);

// The console's sockets (the NetDll_* Winsock calls), on the host's: IPv4, TCP and UDP (VDP, the console's voice and
// data protocol, as UDP), every Xbox constant mapped explicitly so any host runs them. Two modes:
//   loopback (the default): the console is alone at 127.0.0.1. Its XNADDR is that address; a socket bound to any
//     address is bound to 127.0.0.1, and a datagram sent to a broadcast address goes to 127.0.0.1: what a title
//     sends reaches itself (Call of Duty: Black Ops runs a local server over sockets even in solo play), nothing
//     leaves the machine and no firewall asks.
//   off: a console with no connection: no XNADDR, every socket call fails with WSAENETDOWN or WSAENOTSOCK.
// Errors go to the calling thread's WSAGetLastError, as Winsock codes. Overlapped Winsock (WSASend, WSARecv,
// WSAEventSelect) is not implemented: those calls fail as on a console with no connection.
class Network {
 public:
  enum class Mode { kOff, kLoopback };
  void SetMode(Mode mode) { mode_ = mode; }
  Mode mode() const { return mode_; }
  // Closes every socket (a title's end; the tests).
  void Reset();

  // Socket values a title gets: small, never 0 or -1.
  uint32_t Socket(uint32_t af, uint32_t type, uint32_t protocol);
  uint32_t CloseSocket(uint32_t s);
  uint32_t Shutdown(uint32_t s, uint32_t how);
  uint32_t Bind(Guest g, uint32_t s, uint32_t name, uint32_t length);
  uint32_t Connect(Guest g, uint32_t s, uint32_t name, uint32_t length);
  uint32_t Listen(uint32_t s, uint32_t backlog);
  uint32_t Accept(Guest g, uint32_t s, uint32_t name, uint32_t length_ptr);
  uint32_t Send(Guest g, uint32_t s, uint32_t buffer, uint32_t length, uint32_t flags);
  uint32_t SendTo(Guest g, uint32_t s, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t to, uint32_t to_length);
  uint32_t Recv(Guest g, uint32_t s, uint32_t buffer, uint32_t length, uint32_t flags);
  uint32_t RecvFrom(Guest g, uint32_t s, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t from,
                    uint32_t from_length_ptr);
  // getsockname (peer false) and getpeername.
  uint32_t Name(Guest g, uint32_t s, bool peer, uint32_t name, uint32_t length_ptr);
  // FIONBIO (the argument a 32-bit flag) and FIONREAD (the bytes a receive would return, written to the argument).
  uint32_t IoctlSocket(Guest g, uint32_t s, uint32_t command, uint32_t argument);
  // SOL_SOCKET (0xFFFF): SO_REUSEADDR, SO_KEEPALIVE, SO_BROADCAST, SO_LINGER, SO_DONTLINGER, SO_SNDBUF, SO_RCVBUF,
  // SO_SNDTIMEO, SO_RCVTIMEO; SO_EXCLUSIVEADDRUSE and the console's own socket encryption switches (0x5801, 0x5802)
  // accepted with no effect. IPPROTO_TCP: TCP_NODELAY. Anything else: WSAENOPROTOOPT.
  uint32_t SetSockOpt(Guest g, uint32_t s, uint32_t level, uint32_t name, uint32_t value, uint32_t length);
  // As SetSockOpt, plus SO_ERROR and SO_TYPE.
  uint32_t GetSockOpt(Guest g, uint32_t s, uint32_t level, uint32_t name, uint32_t value, uint32_t length_ptr);
  // fd_sets of up to 64 sockets, kept to the ready ones; `timeout` a timeval (null: for ever). The ready count.
  uint32_t Select(Guest g, uint32_t read, uint32_t write, uint32_t except, uint32_t timeout);

  // XNet: the console's XNADDR (XNET_GET_XNADDR_STATIC, 127.0.0.1, in loopback; XNET_GET_XNADDR_NONE when off), and
  // the conversions between it and the IN_ADDR sockets use (the address itself: there is no other console).
  uint32_t XNetGetTitleXnAddr(Guest g, uint32_t xnaddr);
  uint32_t XNetXnAddrToInAddr(Guest g, uint32_t xnaddr, uint32_t in_addr);
  uint32_t XNetInAddrToXnAddr(Guest g, uint32_t in_addr, uint32_t xnaddr, uint32_t xnkid);
  // XNetConnect / XNetGetConnectStatus: the address is the console's own: connected (2). Off: WSAENETDOWN, lost (3).
  uint32_t XNetConnect();
  uint32_t XNetGetConnectStatus();
  // XNetQosLookup: an XNQOS with one complete entry per address asked about (none was contacted: no other console
  // answers), from the kernel pool (XNetQosRelease gives it back), the event set. Off: WSAENETDOWN.
  uint32_t XNetQosLookup(Guest g, uint32_t addresses, uint32_t in_addresses, uint32_t event, uint32_t qos_ptr);

 private:
  struct Entry {
    int64_t host = -1;
    uint32_t type = 0;
  };
  Entry* Find(uint32_t s);
  bool Off();
  Mode mode_ = Mode::kLoopback;
  std::mutex mutex_;
  std::map<uint32_t, Entry> sockets_;
  uint32_t next_ = 0x100;
};

Network& Net();

}  // namespace any360::sys
