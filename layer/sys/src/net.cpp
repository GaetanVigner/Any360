#include "any360/sys/net.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "any360/sys/objects.h"
#include "any360/sys/xam.h"
#include "host_sockets.h"

namespace any360::sys {

namespace {

// One component of a dotted address: decimal, 0x hexadecimal or 0 octal, as the BSD / Winsock parser reads it.
bool Component(const char*& p, uint32_t& value) {
  uint32_t base = 10;
  if (*p == '0') {
    ++p;
    if (*p == 'x' || *p == 'X') {
      base = 16;
      ++p;
    } else {
      base = 8;
    }
  }
  value = 0;
  bool digits = base != 10;  // "0" alone is a valid octal component
  for (;; ++p) {
    uint32_t d;
    if (*p >= '0' && *p <= '9') d = uint32_t(*p - '0');
    else if (base == 16 && *p >= 'a' && *p <= 'f') d = uint32_t(*p - 'a' + 10);
    else if (base == 16 && *p >= 'A' && *p <= 'F') d = uint32_t(*p - 'A' + 10);
    else break;
    if (d >= base) return false;
    value = value * base + d;
    digits = true;
  }
  return digits;
}

}  // namespace

uint32_t NetDll_inet_addr(Guest g, uint32_t string) {
  if (!string) return 0xFFFFFFFF;
  const char* p = reinterpret_cast<const char*>(g.host(string));
  if (!*p) return 0;
  uint32_t parts[4];
  int n = 0;
  for (;;) {
    if (n == 4 || !Component(p, parts[n])) return 0xFFFFFFFF;
    ++n;
    if (*p != '.') break;
    ++p;
  }
  // Trailing blanks are allowed; anything else is not.
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') ++p;
  if (*p) return 0xFFFFFFFF;
  uint32_t address = parts[n - 1];
  const uint32_t limit = n == 1 ? 0xFFFFFFFF : n == 2 ? 0xFFFFFF : n == 3 ? 0xFFFF : 0xFF;
  if (address > limit) return 0xFFFFFFFF;
  for (int i = 0; i < n - 1; ++i) {
    if (parts[i] > 0xFF) return 0xFFFFFFFF;
    address |= parts[i] << (24 - 8 * i);
  }
  return address;
}

void NetDll_XNetInAddrToString(Guest g, uint32_t in_addr, uint32_t out, uint32_t size) {
  // Offline, the reference's placeholder; with the network, the dotted address (the register holds it big-endian).
  char text[16] = "666.666.666.666";
  if (Net().mode() != Network::Mode::kOff) {
    std::snprintf(text, sizeof text, "%u.%u.%u.%u", in_addr >> 24, (in_addr >> 16) & 0xFF, (in_addr >> 8) & 0xFF,
                  in_addr & 0xFF);
  }
  if (!size) return;
  const uint32_t n = std::min<uint32_t>(size - 1, uint32_t(std::strlen(text)));
  std::memcpy(g.host(out), text, n);
  g.Store8(out + n, 0);
}

uint32_t NetDll___WSAFDIsSet(Guest g, uint32_t socket, uint32_t fd_set) {
  const uint32_t count = std::min<uint32_t>(g.Load32(fd_set), 64);
  for (uint32_t i = 0; i < count; ++i) {
    if (g.Load32(fd_set + 4 + 4 * i) == socket) return 1;
  }
  return 0;
}

// --- sockets ------------------------------------------------------------------------------------------------------

namespace {

constexpr uint32_t kSocketError = 0xFFFFFFFF;  // SOCKET_ERROR, INVALID_SOCKET
constexpr uint32_t kWsaEInval = 10022, kWsaENotSock = 10038, kWsaEFault = 10014, kWsaENoProtoOpt = 10042,
                   kWsaESockTNoSupport = 10044, kWsaEAfNoSupport = 10047, kWsaENetDown = 10050;
constexpr uint32_t kAfInet = 2, kSockStream = 1, kSockDgram = 2, kIpprotoTcp = 6, kIpprotoUdp = 17, kIpprotoVdp = 254;
constexpr uint32_t kSolSocket = 0xFFFF, kSoReuseAddr = 0x4, kSoKeepAlive = 0x8, kSoBroadcast = 0x20, kSoLinger = 0x80,
                   kSoDontLinger = 0xFFFFFF7F, kSoExclusiveAddrUse = 0xFFFFFFFB, kSoSndBuf = 0x1001, kSoRcvBuf = 0x1002,
                   kSoSndTimeo = 0x1005, kSoRcvTimeo = 0x1006, kSoError = 0x1007, kSoType = 0x1008,
                   kSoConsoleSecure1 = 0x5801, kSoConsoleSecure2 = 0x5802, kTcpNoDelay = 1;
constexpr uint32_t kFionbio = 0x8004667E, kFionread = 0x4004667F;
// 127.0.0.1 and 255.255.255.255 as the 32 bits of an address in network order, read as the host reads memory.
const uint32_t kLoopback = [] {
  const uint8_t bytes[4] = {127, 0, 0, 1};
  uint32_t v;
  std::memcpy(&v, bytes, 4);
  return v;
}();
constexpr uint32_t kBroadcast = 0xFFFFFFFF;

void SetError(uint32_t error) { XamState().WsaSetLastError(error); }

uint32_t Fail(uint32_t error) {
  SetError(error);
  return kSocketError;
}

// A port in network order (as the host's sockets take it) from a value, and back.
uint16_t NetworkOrder(uint16_t value) {
  const uint8_t bytes[2] = {uint8_t(value >> 8), uint8_t(value)};
  uint16_t port;
  std::memcpy(&port, bytes, 2);
  return port;
}
uint16_t PortValue(uint16_t network_order) {
  uint8_t bytes[2];
  std::memcpy(bytes, &network_order, 2);
  return uint16_t(bytes[0] << 8 | bytes[1]);
}

// A guest sockaddr_in: family (16 bits, big-endian), port and address in network order. The port comes out as the
// host's (SwapPrivilegedPort, net.h), in network order.
bool ReadAddress(Guest g, uint32_t name, uint32_t length, uint32_t& address, uint16_t& port) {
  if (!name || length < 8) return false;
  port = NetworkOrder(SwapPrivilegedPort(g.Load16(name + 2)));
  std::memcpy(&address, g.host(name + 4), 4);
  return g.Load16(name) == kAfInet;
}

// A host address (port in network order) as the guest's sockaddr_in: the port back to the guest's.
void WriteAddress(Guest g, uint32_t name, uint32_t length_ptr, uint32_t address, uint16_t port) {
  if (!name) return;
  if (length_ptr && g.Load32(length_ptr) < 16) return;
  g.Store16(name, kAfInet);
  g.Store16(name + 2, SwapPrivilegedPort(PortValue(port)));
  std::memcpy(g.host(name + 4), &address, 4);
  std::memset(g.host(name + 8), 0, 8);
  if (length_ptr) g.Store32(length_ptr, 16);
}

uint32_t ReadValue(Guest g, uint32_t value, uint32_t length) {
  if (!value || !length) return 0;
  return length == 1 ? g.Load8(value) : length == 2 ? g.Load16(value) : g.Load32(value);
}

// The 6 bytes of the console's Ethernet address in its XNADDR: a locally administered one, never 0 (titles built
// on RakNet make their ids from it and refuse 0).
constexpr uint8_t kEthernet[6] = {0x02, 0x00, 0x00, 0xA3, 0x60, 0x01};

}  // namespace

Network& Net() {
  static Network network;
  return network;
}

bool Network::Off() {
  if (mode_ != Mode::kOff) return false;
  SetError(kWsaENetDown);
  return true;
}

Network::Entry* Network::Find(uint32_t s) {
  std::lock_guard lock(mutex_);
  auto it = sockets_.find(s);
  if (it == sockets_.end()) {
    SetError(kWsaENotSock);
    return nullptr;
  }
  return &it->second;
}

void Network::Reset() {
  std::lock_guard lock(mutex_);
  for (auto& [s, entry] : sockets_) host_net::Close(entry.host);
  sockets_.clear();
}

uint32_t Network::Socket(uint32_t af, uint32_t type, uint32_t protocol) {
  if (Off()) return kSocketError;
  if (af != kAfInet) return Fail(kWsaEAfNoSupport);
  if (type != kSockStream && type != kSockDgram) return Fail(kWsaESockTNoSupport);
  const int host_protocol = protocol == kIpprotoTcp ? host_net::kTcp
                            : protocol == kIpprotoUdp || protocol == kIpprotoVdp ? host_net::kUdp  // VDP rides on UDP
                                                                                 : 0;
  uint32_t error = 0;
  const int64_t host = host_net::Open(type == kSockStream ? host_net::kStream : host_net::kDatagram, host_protocol, error);
  if (host < 0) return Fail(error);
  std::lock_guard lock(mutex_);
  const uint32_t s = next_;
  next_ += 4;
  sockets_[s] = {host, type};
  return s;
}

uint32_t Network::CloseSocket(uint32_t s) {
  int64_t host = -1;
  {
    std::lock_guard lock(mutex_);
    auto it = sockets_.find(s);
    if (it == sockets_.end()) return Fail(kWsaENotSock);
    host = it->second.host;
    sockets_.erase(it);
  }
  host_net::Close(host);
  return 0;
}

uint32_t Network::Shutdown(uint32_t s, uint32_t how) {
  Entry* e = Find(s);
  if (!e) return kSocketError;
  uint32_t error = 0;
  return host_net::Shutdown(e->host, int(how), error) < 0 ? Fail(error) : 0;
}

uint32_t Network::Bind(Guest g, uint32_t s, uint32_t name, uint32_t length) {
  Entry* e = Find(s);
  if (!e) return kSocketError;
  uint32_t address = 0, error = 0;
  uint16_t port = 0;
  if (!ReadAddress(g, name, length, address, port)) return Fail(name ? kWsaEAfNoSupport : kWsaEFault);
  if (address == 0) address = kLoopback;  // any address: the console's only one
  return host_net::Bind(e->host, address, port, error) < 0 ? Fail(error) : 0;
}

uint32_t Network::Connect(Guest g, uint32_t s, uint32_t name, uint32_t length) {
  Entry* e = Find(s);
  if (!e) return kSocketError;
  uint32_t address = 0, error = 0;
  uint16_t port = 0;
  if (!ReadAddress(g, name, length, address, port)) return Fail(name ? kWsaEAfNoSupport : kWsaEFault);
  if (address == 0 || address == kBroadcast) address = kLoopback;
  return host_net::Connect(e->host, address, port, error) < 0 ? Fail(error) : 0;
}

uint32_t Network::Listen(uint32_t s, uint32_t backlog) {
  Entry* e = Find(s);
  if (!e) return kSocketError;
  uint32_t error = 0;
  return host_net::Listen(e->host, int(backlog), error) < 0 ? Fail(error) : 0;
}

uint32_t Network::Accept(Guest g, uint32_t s, uint32_t name, uint32_t length_ptr) {
  Entry* e = Find(s);
  if (!e) return kSocketError;
  const int64_t listening = e->host;
  const uint32_t type = e->type;
  uint32_t address = 0, error = 0;
  uint16_t port = 0;
  const int64_t host = host_net::Accept(listening, address, port, error);
  if (host < 0) return Fail(error);
  WriteAddress(g, name, length_ptr, address, port);
  std::lock_guard lock(mutex_);
  const uint32_t accepted = next_;
  next_ += 4;
  sockets_[accepted] = {host, type};
  return accepted;
}

uint32_t Network::Send(Guest g, uint32_t s, uint32_t buffer, uint32_t length, uint32_t flags) {
  Entry* e = Find(s);
  if (!e) return kSocketError;
  uint32_t error = 0;
  const int n = host_net::Send(e->host, g.host(buffer), length, int(flags & 3), error);
  return n < 0 ? Fail(error) : uint32_t(n);
}

uint32_t Network::SendTo(Guest g, uint32_t s, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t to,
                         uint32_t to_length) {
  Entry* e = Find(s);
  if (!e) return kSocketError;
  uint32_t error = 0;
  if (!to) {
    const int n = host_net::Send(e->host, g.host(buffer), length, int(flags & 3), error);
    return n < 0 ? Fail(error) : uint32_t(n);
  }
  uint32_t address = 0;
  uint16_t port = 0;
  if (!ReadAddress(g, to, to_length, address, port)) return Fail(kWsaEAfNoSupport);
  // A broadcast reaches every console on the network: this one alone.
  if (address == kBroadcast || address == 0) address = kLoopback;
  const int n = host_net::SendTo(e->host, g.host(buffer), length, int(flags & 3), address, port, error);
  return n < 0 ? Fail(error) : uint32_t(n);
}

uint32_t Network::Recv(Guest g, uint32_t s, uint32_t buffer, uint32_t length, uint32_t flags) {
  Entry* e = Find(s);
  if (!e) return kSocketError;
  uint32_t error = 0;
  const int n = host_net::Recv(e->host, g.host(buffer), length, int(flags & 3), error);
  return n < 0 ? Fail(error) : uint32_t(n);
}

uint32_t Network::RecvFrom(Guest g, uint32_t s, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t from,
                           uint32_t from_length_ptr) {
  Entry* e = Find(s);
  if (!e) return kSocketError;
  uint32_t address = 0, error = 0;
  uint16_t port = 0;
  const int n = host_net::RecvFrom(e->host, g.host(buffer), length, int(flags & 3), address, port, error);
  if (n < 0) return Fail(error);
  WriteAddress(g, from, from_length_ptr, address, port);
  return uint32_t(n);
}

uint32_t Network::Name(Guest g, uint32_t s, bool peer, uint32_t name, uint32_t length_ptr) {
  Entry* e = Find(s);
  if (!e) return kSocketError;
  if (!name || !length_ptr || g.Load32(length_ptr) < 16) return Fail(kWsaEFault);
  uint32_t address = 0, error = 0;
  uint16_t port = 0;
  if (host_net::Name(e->host, peer, address, port, error) < 0) return Fail(error);
  WriteAddress(g, name, length_ptr, address, port);
  return 0;
}

uint32_t Network::IoctlSocket(Guest g, uint32_t s, uint32_t command, uint32_t argument) {
  Entry* e = Find(s);
  if (!e) return kSocketError;
  if (!argument) return Fail(kWsaEFault);
  uint32_t error = 0;
  if (command == kFionbio) {
    return host_net::SetNonBlocking(e->host, g.Load32(argument) != 0, error) < 0 ? Fail(error) : 0;
  }
  if (command == kFionread) {
    uint32_t bytes = 0;
    if (host_net::Available(e->host, bytes, error) < 0) return Fail(error);
    g.Store32(argument, bytes);
    return 0;
  }
  return Fail(kWsaEInval);
}

uint32_t Network::SetSockOpt(Guest g, uint32_t s, uint32_t level, uint32_t name, uint32_t value, uint32_t length) {
  Entry* e = Find(s);
  if (!e) return kSocketError;
  if (!value && name != kSoDontLinger) return Fail(kWsaEFault);
  using host_net::Option;
  const int v = int(ReadValue(g, value, length));
  int v2 = 0;
  Option option;
  if (level == kSolSocket) {
    switch (name) {
      case kSoReuseAddr: option = Option::kReuseAddress; break;
      case kSoKeepAlive: option = Option::kKeepAlive; break;
      case kSoBroadcast: option = Option::kBroadcast; break;
      case kSoSndBuf: option = Option::kSendBuffer; break;
      case kSoRcvBuf: option = Option::kReceiveBuffer; break;
      case kSoSndTimeo: option = Option::kSendTimeout; break;
      case kSoRcvTimeo: option = Option::kReceiveTimeout; break;
      case kSoLinger: {  // struct linger: two 16-bit fields, on/off and seconds
        uint32_t error = 0;
        return host_net::SetOption(e->host, Option::kLinger, int(g.Load16(value)), int(g.Load16(value + 2)), error) < 0
                   ? Fail(error)
                   : 0;
      }
      case kSoDontLinger: {
        uint32_t error = 0;
        return host_net::SetOption(e->host, Option::kLinger, v ? 0 : 1, 0, error) < 0 ? Fail(error) : 0;
      }
      case kSoExclusiveAddrUse:
      case kSoConsoleSecure1:
      case kSoConsoleSecure2:
        return 0;  // the console's own; nothing on the host to set
      default: return Fail(kWsaENoProtoOpt);
    }
  } else if (level == kIpprotoTcp && name == kTcpNoDelay) {
    option = Option::kNoDelay;
  } else {
    return Fail(kWsaENoProtoOpt);
  }
  uint32_t error = 0;
  return host_net::SetOption(e->host, option, v, v2, error) < 0 ? Fail(error) : 0;
}

uint32_t Network::GetSockOpt(Guest g, uint32_t s, uint32_t level, uint32_t name, uint32_t value, uint32_t length_ptr) {
  Entry* e = Find(s);
  if (!e) return kSocketError;
  if (!value || !length_ptr || g.Load32(length_ptr) < 4) return Fail(kWsaEFault);
  using host_net::Option;
  int v = 0;
  uint32_t error = 0;
  if (level == kSolSocket && name == kSoType) {
    v = int(e->type);
  } else {
    Option option;
    if (level == kSolSocket) {
      switch (name) {
        case kSoReuseAddr: option = Option::kReuseAddress; break;
        case kSoKeepAlive: option = Option::kKeepAlive; break;
        case kSoBroadcast: option = Option::kBroadcast; break;
        case kSoSndBuf: option = Option::kSendBuffer; break;
        case kSoRcvBuf: option = Option::kReceiveBuffer; break;
        case kSoError: option = Option::kError; break;
        default: return Fail(kWsaENoProtoOpt);
      }
    } else if (level == kIpprotoTcp && name == kTcpNoDelay) {
      option = Option::kNoDelay;
    } else {
      return Fail(kWsaENoProtoOpt);
    }
    if (host_net::GetOption(e->host, option, v, error) < 0) return Fail(error);
    if (option != Option::kSendBuffer && option != Option::kReceiveBuffer && option != Option::kError) v = v ? 1 : 0;
  }
  g.Store32(value, uint32_t(v));
  g.Store32(length_ptr, 4);
  return 0;
}

uint32_t Network::Select(Guest g, uint32_t read, uint32_t write, uint32_t except, uint32_t timeout) {
  if (Off()) return kSocketError;
  // Each set: the guest's sockets and the host's, in step.
  struct Set {
    uint32_t address = 0;
    std::vector<uint32_t> guest;
    std::vector<int64_t> host;
  } sets[3];
  const uint32_t addresses[3] = {read, write, except};
  for (int i = 0; i < 3; ++i) {
    sets[i].address = addresses[i];
    if (!addresses[i]) continue;
    const uint32_t count = std::min<uint32_t>(g.Load32(addresses[i]), 64);
    for (uint32_t k = 0; k < count; ++k) {
      const uint32_t s = g.Load32(addresses[i] + 4 + 4 * k);
      Entry* e = Find(s);
      if (!e) return kSocketError;
      sets[i].guest.push_back(s);
      sets[i].host.push_back(e->host);
    }
  }
  int64_t timeout_us = -1;
  if (timeout) timeout_us = int64_t(int32_t(g.Load32(timeout))) * 1000000 + int32_t(g.Load32(timeout + 4));
  std::vector<int64_t> ready[3] = {sets[0].host, sets[1].host, sets[2].host};
  uint32_t error = 0;
  const int n = host_net::Select(ready[0], ready[1], ready[2], timeout_us, error);
  if (n < 0) return Fail(error);
  for (int i = 0; i < 3; ++i) {
    if (!sets[i].address) continue;
    uint32_t kept = 0;
    for (size_t k = 0; k < sets[i].host.size(); ++k) {
      if (std::find(ready[i].begin(), ready[i].end(), sets[i].host[k]) != ready[i].end()) {
        g.Store32(sets[i].address + 4 + 4 * kept++, sets[i].guest[k]);
      }
    }
    g.Store32(sets[i].address, kept);
  }
  return uint32_t(n);
}

uint32_t Network::XNetGetTitleXnAddr(Guest g, uint32_t xnaddr) {
  if (xnaddr) std::memset(g.host(xnaddr), 0, 36);
  if (mode_ == Mode::kOff) return 1;  // XNET_GET_XNADDR_NONE
  if (xnaddr) {
    // XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20].
    std::memcpy(g.host(xnaddr), &kLoopback, 4);
    std::memcpy(g.host(xnaddr + 10), kEthernet, 6);
  }
  return 4;  // XNET_GET_XNADDR_STATIC, as the reference
}

uint32_t Network::XNetXnAddrToInAddr(Guest g, uint32_t xnaddr, uint32_t in_addr) {
  if (mode_ == Mode::kOff) return 1;  // as the reference: no address
  if (!xnaddr || !in_addr) return kWsaEInval;
  std::memcpy(g.host(in_addr), g.host(xnaddr), 4);
  return 0;
}

uint32_t Network::XNetInAddrToXnAddr(Guest g, uint32_t in_addr, uint32_t xnaddr, uint32_t xnkid) {
  if (mode_ == Mode::kOff) return 1;
  if (!xnaddr) return kWsaEInval;
  std::memset(g.host(xnaddr), 0, 36);
  const uint32_t address = __builtin_bswap32(in_addr);  // the register holds it big-endian; memory wants its bytes
  std::memcpy(g.host(xnaddr), &address, 4);
  std::memcpy(g.host(xnaddr + 10), kEthernet, 6);
  if (xnkid) std::memset(g.host(xnkid), 0, 8);
  return 0;
}

uint32_t Network::XNetConnect() { return mode_ == Mode::kOff ? kWsaENetDown : 0; }

uint32_t Network::XNetGetConnectStatus() { return mode_ == Mode::kOff ? 3 : 2; }

uint32_t Network::XNetQosLookup(Guest g, uint32_t addresses, uint32_t in_addresses, uint32_t event, uint32_t qos_ptr) {
  if (mode_ == Mode::kOff) return kWsaENetDown;
  // XNQOS: count, pending count, then one 24-byte XNQOSINFO an address (flags first: XNET_XNQOSINFO_COMPLETE).
  const uint32_t count = addresses + in_addresses;
  if (qos_ptr) {
    const uint32_t qos = XamState().AllocatePool(g, 8 + 24 * std::max<uint32_t>(count, 1));
    if (qos) {
      std::memset(g.host(qos), 0, 8 + 24 * std::max<uint32_t>(count, 1));
      g.Store32(qos, count);
      for (uint32_t i = 0; i < count; ++i) g.Store8(qos + 8 + 24 * i, 0x01);
    }
    g.Store32(qos_ptr, qos);
  }
  if (event) Objects().NtSetEvent(g, event, 0);
  return 0;
}

}  // namespace any360::sys
