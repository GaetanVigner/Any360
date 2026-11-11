// The host's IPv4 sockets as any360::sys's network uses them (host_sockets.cpp): sockets as 64-bit values (-1 for
// none), addresses and ports in network order, errors as Winsock codes on every host.
#pragma once

#include <cstdint>
#include <vector>

namespace any360::sys::host_net {

constexpr int kStream = 1, kDatagram = 2;   // socket types, as the console numbers them
constexpr int kTcp = 6, kUdp = 17;          // protocols (0: the type's own)
constexpr uint32_t kWouldBlock = 10035;     // WSAEWOULDBLOCK

enum class Option { kReuseAddress, kKeepAlive, kBroadcast, kSendBuffer, kReceiveBuffer, kSendTimeout, kReceiveTimeout,
                    kNoDelay, kLinger, kError };

int64_t Open(int type, int protocol, uint32_t& error);
int Close(int64_t s);
int Bind(int64_t s, uint32_t address_be, uint16_t port_be, uint32_t& error);
int Connect(int64_t s, uint32_t address_be, uint16_t port_be, uint32_t& error);
int Listen(int64_t s, int backlog, uint32_t& error);
int64_t Accept(int64_t s, uint32_t& address_be, uint16_t& port_be, uint32_t& error);
int Shutdown(int64_t s, int how, uint32_t& error);
int Send(int64_t s, const uint8_t* data, uint32_t length, int flags, uint32_t& error);
int SendTo(int64_t s, const uint8_t* data, uint32_t length, int flags, uint32_t address_be, uint16_t port_be,
           uint32_t& error);
int Recv(int64_t s, uint8_t* data, uint32_t length, int flags, uint32_t& error);
int RecvFrom(int64_t s, uint8_t* data, uint32_t length, int flags, uint32_t& address_be, uint16_t& port_be,
             uint32_t& error);
// The socket's own address (getsockname) or its peer's (getpeername).
int Name(int64_t s, bool peer, uint32_t& address_be, uint16_t& port_be, uint32_t& error);
int SetNonBlocking(int64_t s, bool on, uint32_t& error);
// Bytes that a receive would return now (FIONREAD).
int Available(int64_t s, uint32_t& bytes, uint32_t& error);
// `value` is an int (a flag, a size, milliseconds); for kLinger, `value` is on/off and `value2` the seconds.
int SetOption(int64_t s, Option option, int value, int value2, uint32_t& error);
int GetOption(int64_t s, Option option, int& value, uint32_t& error);
// Waits until a socket of a list is ready (timeout_us < 0: for ever); each list keeps its ready sockets only. The
// number of ready sockets, or -1.
int Select(std::vector<int64_t>& read, std::vector<int64_t>& write, std::vector<int64_t>& except, int64_t timeout_us,
           uint32_t& error);

}  // namespace any360::sys::host_net
