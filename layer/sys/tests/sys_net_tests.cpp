// The console's sockets on the host's (any360::sys's network, net.h), over real loopback sockets: what a title sends
// to itself arrives, the Winsock errors it reads are the console's, and the network switched off answers as a
// console with no connection.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "any360/sys/net.h"
#include "any360/sys/objects.h"
#include "any360/sys/xam.h"

namespace {

using namespace any360::sys;

std::vector<uint8_t> g_memory(0x40000);
const Guest g{g_memory.data()};
int g_failures = 0;

#define CHECK(condition)                                          \
  do {                                                            \
    if (!(condition)) {                                           \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

constexpr uint32_t kError = 0xFFFFFFFF;
constexpr uint32_t kAfInet = 2, kStream = 1, kDgram = 2, kTcp = 6, kUdp = 17, kVdp = 254;
constexpr uint32_t kSolSocket = 0xFFFF, kSoBroadcast = 0x20, kSoType = 0x1008, kSoRcvBuf = 0x1002, kTcpNoDelay = 1;
constexpr uint32_t kFionbio = 0x8004667E, kFionread = 0x4004667F;

// A guest sockaddr_in at `at`: family 2, the port and address given in host order, stored in network order.
void Address(uint32_t at, uint32_t address, uint16_t port) {
  std::memset(g.host(at), 0, 16);
  g.Store16(at, kAfInet);
  g.Store16(at + 2, port);
  g.Store32(at + 4, address);
}

uint32_t LastError() { return XamState().WsaGetLastError(); }

// A guest fd_set at `at` holding `s`; a timeval of `ms` milliseconds at `timeout`.
void Set(uint32_t at, uint32_t s) {
  g.Store32(at, 1);
  g.Store32(at + 4, s);
}
void Timeout(uint32_t at, uint32_t ms) {
  g.Store32(at, ms / 1000);
  g.Store32(at + 4, (ms % 1000) * 1000);
}

void Off() {
  Net().SetMode(Network::Mode::kOff);
  CHECK(Net().Socket(kAfInet, kDgram, kUdp) == kError && LastError() == 10050);  // WSAENETDOWN
  CHECK(Net().XNetGetTitleXnAddr(g, 0x100) == 1);                               // XNET_GET_XNADDR_NONE
  CHECK(Net().XNetXnAddrToInAddr(g, 0x100, 0x140) == 1);
  CHECK(Net().XNetGetConnectStatus() == 3);
  NetDll_XNetInAddrToString(g, 0x7F000001, 0x200, 32);
  CHECK(std::strcmp(reinterpret_cast<const char*>(g.host(0x200)), "666.666.666.666") == 0);
  Net().SetMode(Network::Mode::kLoopback);
}

void XNet() {
  // The console's address: 127.0.0.1, static, with an Ethernet address that is not 0.
  CHECK(Net().XNetGetTitleXnAddr(g, 0x100) == 4);
  const uint8_t loopback[4] = {127, 0, 0, 1};
  CHECK(std::memcmp(g.host(0x100), loopback, 4) == 0 && g.Load32(0x104) == 0);
  bool ethernet = false;
  for (uint32_t i = 0; i < 6; ++i) ethernet = ethernet || g.Load8(0x10A + i) != 0;
  CHECK(ethernet);
  CHECK(Net().XNetXnAddrToInAddr(g, 0x100, 0x140) == 0 && g.Load32(0x140) == 0x7F000001);
  CHECK(Net().XNetInAddrToXnAddr(g, 0x7F000001, 0x180, 0x1C0) == 0 && std::memcmp(g.host(0x180), loopback, 4) == 0);
  CHECK(Net().XNetConnect() == 0 && Net().XNetGetConnectStatus() == 2);
  NetDll_XNetInAddrToString(g, 0x7F000001, 0x200, 32);
  CHECK(std::strcmp(reinterpret_cast<const char*>(g.host(0x200)), "127.0.0.1") == 0);
  // QoS: one complete entry per address, the event set.
  CHECK(Net().XNetQosLookup(g, 1, 0, 0, 0x240) == 0);
  const uint32_t qos = g.Load32(0x240);
  CHECK(qos != 0 && g.Load32(qos) == 1 && g.Load32(qos + 4) == 0 && g.Load8(qos + 8) == 0x01);
  CHECK(XamState().XNetQosRelease(qos) == 0);
}

void Datagrams() {
  const uint32_t a = Net().Socket(kAfInet, kDgram, kUdp), b = Net().Socket(kAfInet, kDgram, kVdp);
  CHECK(a != kError && b != kError && a != b && a != 0 && b != 0);
  // Bound to any address: the console's, 127.0.0.1; the port the host chose comes back.
  Address(0x1000, 0, 0);
  CHECK(Net().Bind(g, a, 0x1000, 16) == 0);
  g.Store32(0x1020, 16);
  CHECK(Net().Name(g, a, false, 0x1010, 0x1020) == 0 && g.Load16(0x1010) == kAfInet && g.Load32(0x1014) == 0x7F000001);
  const uint16_t port = g.Load16(0x1012);
  CHECK(port != 0 && g.Load32(0x1020) == 16);
  // Sent to 127.0.0.1 from a VDP socket (UDP underneath): select sees it, FIONREAD counts it, recvfrom reads it.
  std::memcpy(g.host(0x2000), "hello", 5);
  Address(0x1030, 0x7F000001, port);
  CHECK(Net().SendTo(g, b, 0x2000, 5, 0, 0x1030, 16) == 5);
  Set(0x3000, a);
  Timeout(0x3200, 2000);
  CHECK(Net().Select(g, 0x3000, 0, 0, 0x3200) == 1 && g.Load32(0x3000) == 1 && g.Load32(0x3004) == a);
  CHECK(NetDll___WSAFDIsSet(g, a, 0x3000) == 1);
  CHECK(Net().IoctlSocket(g, a, kFionread, 0x1040) == 0 && g.Load32(0x1040) == 5);
  g.Store32(0x1060, 16);
  CHECK(Net().RecvFrom(g, a, 0x2100, 64, 0, 0x1050, 0x1060) == 5 && std::memcmp(g.host(0x2100), "hello", 5) == 0);
  CHECK(g.Load16(0x1050) == kAfInet && g.Load32(0x1054) == 0x7F000001 && g.Load32(0x1060) == 16);
  // Non-blocking, nothing waiting: WSAEWOULDBLOCK.
  g.Store32(0x1070, 1);
  CHECK(Net().IoctlSocket(g, a, kFionbio, 0x1070) == 0);
  CHECK(Net().RecvFrom(g, a, 0x2100, 64, 0, 0, 0) == kError && LastError() == 10035);
  // Nothing ready: select times out with an empty set.
  Set(0x3000, a);
  Timeout(0x3200, 10);
  CHECK(Net().Select(g, 0x3000, 0, 0, 0x3200) == 0 && g.Load32(0x3000) == 0);
  // A broadcast reaches this console.
  g.Store32(0x1080, 1);
  CHECK(Net().SetSockOpt(g, b, kSolSocket, kSoBroadcast, 0x1080, 4) == 0);
  Address(0x1030, 0xFFFFFFFF, port);
  CHECK(Net().SendTo(g, b, 0x2000, 5, 0, 0x1030, 16) == 5);
  Set(0x3000, a);
  Timeout(0x3200, 2000);
  CHECK(Net().Select(g, 0x3000, 0, 0, 0x3200) == 1 && Net().Recv(g, a, 0x2100, 64, 0) == 5);
  // Options read back; one the console does not have is refused.
  g.Store32(0x1090, 4);
  CHECK(Net().GetSockOpt(g, a, kSolSocket, kSoType, 0x1088, 0x1090) == 0 && g.Load32(0x1088) == kDgram);
  CHECK(Net().GetSockOpt(g, b, kSolSocket, kSoBroadcast, 0x1088, 0x1090) == 0 && g.Load32(0x1088) == 1);
  CHECK(Net().GetSockOpt(g, a, kSolSocket, kSoRcvBuf, 0x1088, 0x1090) == 0 && g.Load32(0x1088) > 0);
  CHECK(Net().SetSockOpt(g, a, kSolSocket, 0x1234, 0x1080, 4) == kError && LastError() == 10042);
  g.Store32(0x1080, 1);
  CHECK(Net().SetSockOpt(g, a, kSolSocket, 0x5801, 0x1080, 4) == 0);  // the console's encryption switch: accepted
  CHECK(Net().CloseSocket(a) == 0 && Net().CloseSocket(b) == 0);
  // Closed or never opened: WSAENOTSOCK, in select too.
  CHECK(Net().CloseSocket(a) == kError && LastError() == 10038);
  Set(0x3000, a);
  CHECK(Net().Select(g, 0x3000, 0, 0, 0x3200) == kError && LastError() == 10038);
  CHECK(Net().Socket(23, kDgram, kUdp) == kError && LastError() == 10047);  // WSAEAFNOSUPPORT
}

void Streams() {
  const uint32_t listening = Net().Socket(kAfInet, kStream, kTcp), client = Net().Socket(kAfInet, kStream, 0);
  CHECK(listening != kError && client != kError);
  Address(0x1000, 0, 0);
  CHECK(Net().Bind(g, listening, 0x1000, 16) == 0 && Net().Listen(listening, 4) == 0);
  g.Store32(0x1020, 16);
  CHECK(Net().Name(g, listening, false, 0x1010, 0x1020) == 0);
  const uint16_t port = g.Load16(0x1012);
  // Connected to "any address": the console itself.
  Address(0x1030, 0, port);
  CHECK(Net().Connect(g, client, 0x1030, 16) == 0);
  g.Store32(0x1060, 16);
  const uint32_t accepted = Net().Accept(g, listening, 0x1050, 0x1060);
  CHECK(accepted != kError && g.Load32(0x1054) == 0x7F000001 && g.Load32(0x1060) == 16);
  g.Store32(0x1080, 1);
  CHECK(Net().SetSockOpt(g, client, kTcp, kTcpNoDelay, 0x1080, 4) == 0);
  std::memcpy(g.host(0x2000), "ping", 4);
  CHECK(Net().Send(g, client, 0x2000, 4, 0) == 4);
  CHECK(Net().Recv(g, accepted, 0x2100, 64, 0) == 4 && std::memcmp(g.host(0x2100), "ping", 4) == 0);
  // Writable at once; the peer's address is the console's.
  Set(0x3100, client);
  Timeout(0x3200, 1000);
  CHECK(Net().Select(g, 0, 0x3100, 0, 0x3200) == 1);
  g.Store32(0x1060, 16);
  CHECK(Net().Name(g, client, true, 0x1050, 0x1060) == 0 && g.Load16(0x1052) == port);
  CHECK(Net().Shutdown(client, 2) == 0);
  CHECK(Net().CloseSocket(client) == 0 && Net().CloseSocket(accepted) == 0 && Net().CloseSocket(listening) == 0);
}

}  // namespace

int main() {
  Objects().SetKernelHeap(0x20000, 0x10000);
  Objects().SetGuest(g);
  Objects().Reset();
  Off();
  XNet();
  Datagrams();
  Streams();
  Net().Reset();
  std::printf("sys_net_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
