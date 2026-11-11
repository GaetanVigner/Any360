// The host's sockets, for any360::sys's network (net.cpp): IPv4 only, addresses and ports in network order as the
// guest holds them, errors as Winsock codes on every host (the codes an Xbox 360 title reads from WSAGetLastError).
// Kept apart so the platform headers (winsock2.h, windows.h) stay out of the rest of the layer.
#include "host_sockets.h"

#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace any360::sys::host_net {

namespace {

#ifdef _WIN32
using Native = SOCKET;
Native ToNative(int64_t s) { return Native(s); }
bool Started() {
  static const bool started = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return started;
}
#else
using Native = int;
Native ToNative(int64_t s) { return Native(s); }
bool Started() { return true; }
#endif

// The calling thread's last error as a Winsock code.
uint32_t Error() {
#ifdef _WIN32
  return uint32_t(WSAGetLastError());
#else
  switch (errno) {
    case EWOULDBLOCK: return kWouldBlock;
    case EINPROGRESS: return 10036;
    case EALREADY: return 10037;
    case ENOTSOCK: return 10038;
    case EDESTADDRREQ: return 10039;
    case EMSGSIZE: return 10040;
    case ENOPROTOOPT: return 10042;
    case EPROTONOSUPPORT: return 10043;
    case EOPNOTSUPP: return 10045;
    case EAFNOSUPPORT: return 10047;
    case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049;
    case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051;
    case ECONNABORTED: return 10053;
    case ECONNRESET: return 10054;
    case ENOBUFS: return 10055;
    case EISCONN: return 10056;
    case ENOTCONN: return 10057;
    case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061;
    case EHOSTUNREACH: return 10065;
    case EACCES: return 10013;
    case EFAULT: return 10014;
    case EBADF: return 10009;
    case EMFILE: return 10024;
    default: return 10022;  // WSAEINVAL
  }
#endif
}

int Result(int value, uint32_t& error) {
  error = value < 0 ? Error() : 0;
  return value < 0 ? -1 : value;
}

sockaddr_in Address(uint32_t address_be, uint16_t port_be) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = port_be;
  a.sin_addr.s_addr = address_be;
  return a;
}

}  // namespace

int64_t Open(int type, int protocol, uint32_t& error) {
  error = 0;
  if (!Started()) {
    error = 10050;  // WSAENETDOWN
    return -1;
  }
  const int host_type = type == kStream ? SOCK_STREAM : SOCK_DGRAM;
  const int host_protocol = protocol == kTcp ? IPPROTO_TCP : protocol == kUdp ? IPPROTO_UDP : 0;
  const Native s = socket(AF_INET, host_type, host_protocol);
#ifdef _WIN32
  if (s == INVALID_SOCKET) {
    error = Error();
    return -1;
  }
  if (host_type == SOCK_DGRAM) {
    // A datagram socket on Windows reports an earlier send's "port unreachable" as WSAECONNRESET on its next
    // receive; the console's sockets do not, and a title would take it for a broken socket.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, 0x9800000C /* SIO_UDP_CONNRESET */, &report, sizeof report, nullptr, 0, &returned, nullptr, nullptr);
  }
#else
  if (s < 0) {
    error = Error();
    return -1;
  }
#endif
  return int64_t(s);
}

int Close(int64_t s) {
#ifdef _WIN32
  return closesocket(ToNative(s));
#else
  return close(ToNative(s));
#endif
}

int Bind(int64_t s, uint32_t address_be, uint16_t port_be, uint32_t& error) {
  const sockaddr_in a = Address(address_be, port_be);
  return Result(bind(ToNative(s), reinterpret_cast<const sockaddr*>(&a), sizeof a), error);
}

int Connect(int64_t s, uint32_t address_be, uint16_t port_be, uint32_t& error) {
  const sockaddr_in a = Address(address_be, port_be);
  return Result(connect(ToNative(s), reinterpret_cast<const sockaddr*>(&a), sizeof a), error);
}

int Listen(int64_t s, int backlog, uint32_t& error) { return Result(listen(ToNative(s), backlog), error); }

int64_t Accept(int64_t s, uint32_t& address_be, uint16_t& port_be, uint32_t& error) {
  sockaddr_in a{};
  socklen_t length = sizeof a;
  const Native accepted = accept(ToNative(s), reinterpret_cast<sockaddr*>(&a), &length);
#ifdef _WIN32
  const bool failed = accepted == INVALID_SOCKET;
#else
  const bool failed = accepted < 0;
#endif
  if (failed) {
    error = Error();
    return -1;
  }
  error = 0;
  address_be = a.sin_addr.s_addr;
  port_be = a.sin_port;
  return int64_t(accepted);
}

int Shutdown(int64_t s, int how, uint32_t& error) { return Result(shutdown(ToNative(s), how), error); }

int Send(int64_t s, const uint8_t* data, uint32_t length, int flags, uint32_t& error) {
  return Result(int(send(ToNative(s), reinterpret_cast<const char*>(data), int(length), flags)), error);
}

int SendTo(int64_t s, const uint8_t* data, uint32_t length, int flags, uint32_t address_be, uint16_t port_be,
           uint32_t& error) {
  const sockaddr_in a = Address(address_be, port_be);
  return Result(int(sendto(ToNative(s), reinterpret_cast<const char*>(data), int(length), flags,
                           reinterpret_cast<const sockaddr*>(&a), sizeof a)),
                error);
}

int Recv(int64_t s, uint8_t* data, uint32_t length, int flags, uint32_t& error) {
  return Result(int(recv(ToNative(s), reinterpret_cast<char*>(data), int(length), flags)), error);
}

int RecvFrom(int64_t s, uint8_t* data, uint32_t length, int flags, uint32_t& address_be, uint16_t& port_be,
             uint32_t& error) {
  sockaddr_in a{};
  socklen_t from_length = sizeof a;
  const int n = Result(int(recvfrom(ToNative(s), reinterpret_cast<char*>(data), int(length), flags,
                                    reinterpret_cast<sockaddr*>(&a), &from_length)),
                       error);
  address_be = a.sin_addr.s_addr;
  port_be = a.sin_port;
  return n;
}

int Name(int64_t s, bool peer, uint32_t& address_be, uint16_t& port_be, uint32_t& error) {
  sockaddr_in a{};
  socklen_t length = sizeof a;
  const int r = peer ? getpeername(ToNative(s), reinterpret_cast<sockaddr*>(&a), &length)
                     : getsockname(ToNative(s), reinterpret_cast<sockaddr*>(&a), &length);
  address_be = a.sin_addr.s_addr;
  port_be = a.sin_port;
  return Result(r, error);
}

int SetNonBlocking(int64_t s, bool on, uint32_t& error) {
#ifdef _WIN32
  u_long value = on ? 1 : 0;
  return Result(ioctlsocket(ToNative(s), FIONBIO, &value), error);
#else
  const int flags = fcntl(ToNative(s), F_GETFL, 0);
  return Result(fcntl(ToNative(s), F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK), error);
#endif
}

int Available(int64_t s, uint32_t& bytes, uint32_t& error) {
#ifdef _WIN32
  u_long value = 0;
  const int r = ioctlsocket(ToNative(s), FIONREAD, &value);
#else
  int value = 0;
  const int r = ioctl(ToNative(s), FIONREAD, &value);
#endif
  bytes = uint32_t(value);
  return Result(r, error);
}

int SetOption(int64_t s, Option option, int value, int value2, uint32_t& error) {
  int level = SOL_SOCKET, name = 0;
  switch (option) {
    case Option::kReuseAddress: name = SO_REUSEADDR; break;
    case Option::kKeepAlive: name = SO_KEEPALIVE; break;
    case Option::kBroadcast: name = SO_BROADCAST; break;
    case Option::kSendBuffer: name = SO_SNDBUF; break;
    case Option::kReceiveBuffer: name = SO_RCVBUF; break;
    case Option::kNoDelay: level = IPPROTO_TCP; name = TCP_NODELAY; break;
    case Option::kSendTimeout:
    case Option::kReceiveTimeout: {
      name = option == Option::kSendTimeout ? SO_SNDTIMEO : SO_RCVTIMEO;
#ifdef _WIN32
      const DWORD ms = DWORD(value);
      return Result(setsockopt(ToNative(s), level, name, reinterpret_cast<const char*>(&ms), sizeof ms), error);
#else
      timeval t{value / 1000, (value % 1000) * 1000};
      return Result(setsockopt(ToNative(s), level, name, &t, sizeof t), error);
#endif
    }
    case Option::kLinger: {
      linger l{};
      l.l_onoff = decltype(l.l_onoff)(value);
      l.l_linger = decltype(l.l_linger)(value2);
      return Result(setsockopt(ToNative(s), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l), error);
    }
    default:
      error = 10042;  // WSAENOPROTOOPT
      return -1;
  }
  return Result(setsockopt(ToNative(s), level, name, reinterpret_cast<const char*>(&value), sizeof value), error);
}

int GetOption(int64_t s, Option option, int& value, uint32_t& error) {
  int level = SOL_SOCKET, name = 0;
  switch (option) {
    case Option::kReuseAddress: name = SO_REUSEADDR; break;
    case Option::kKeepAlive: name = SO_KEEPALIVE; break;
    case Option::kBroadcast: name = SO_BROADCAST; break;
    case Option::kSendBuffer: name = SO_SNDBUF; break;
    case Option::kReceiveBuffer: name = SO_RCVBUF; break;
    case Option::kError: name = SO_ERROR; break;
    case Option::kNoDelay: level = IPPROTO_TCP; name = TCP_NODELAY; break;
    default:
      error = 10042;
      return -1;
  }
  value = 0;
  socklen_t length = sizeof value;
  const int r = Result(getsockopt(ToNative(s), level, name, reinterpret_cast<char*>(&value), &length), error);
  if (option == Option::kError && r == 0 && value) {
#ifndef _WIN32
    // SO_ERROR holds an errno on POSIX hosts.
    const int saved = errno;
    errno = value;
    value = int(Error());
    errno = saved;
#endif
  }
  return r;
}

int Select(std::vector<int64_t>& read, std::vector<int64_t>& write, std::vector<int64_t>& except, int64_t timeout_us,
           uint32_t& error) {
  fd_set sets[3];
  std::vector<int64_t>* lists[3] = {&read, &write, &except};
  Native highest = 0;
  for (int i = 0; i < 3; ++i) {
    FD_ZERO(&sets[i]);
    for (const int64_t s : *lists[i]) {
      FD_SET(ToNative(s), &sets[i]);
      highest = std::max(highest, ToNative(s));
    }
  }
  timeval timeout{};
  if (timeout_us >= 0) {
    timeout.tv_sec = decltype(timeout.tv_sec)(timeout_us / 1000000);
    timeout.tv_usec = decltype(timeout.tv_usec)(timeout_us % 1000000);
  }
  const int r = select(int(highest) + 1, read.empty() ? nullptr : &sets[0], write.empty() ? nullptr : &sets[1],
                       except.empty() ? nullptr : &sets[2], timeout_us >= 0 ? &timeout : nullptr);
  if (r < 0) {
    error = Error();
    return -1;
  }
  error = 0;
  for (int i = 0; i < 3; ++i) {
    std::erase_if(*lists[i], [&](int64_t s) { return !FD_ISSET(ToNative(s), &sets[i]); });
  }
  return r;
}

}  // namespace any360::sys::host_net
