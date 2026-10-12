#include "virgil/net.h"

#include <cctype>
#include <cstring>

#include <cerrno>
#include <cstring>

#include "virgil/platform.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mswsock.h>
#include <vector>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#if defined(__APPLE__)
#include <net/if_dl.h>
#else
#include <netpacket/packet.h>
#endif
#include <net/if.h>
#if defined(__APPLE__)
#include <net/if_media.h>  // after net/if.h
#include <sys/ioctl.h>
#include <sys/sockio.h>
#endif
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif
#if defined(__linux__)
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <poll.h>
// (SOF_TIMESTAMPING_* are enumerators, not macros; OPT_TSONLY needs the
// headers of Linux 4.0 or later, which every supported distribution has.)
#if defined(SO_TIMESTAMPING) && defined(SCM_TIMESTAMPING)
#define VIRGIL_LINUX_TX_TS 1
#endif
#endif
#endif
#if defined(__APPLE__) && defined(SO_TIMESTAMP_MONOTONIC) && defined(SCM_TIMESTAMP_MONOTONIC)
#define VIRGIL_APPLE_RX_TS 1
#endif

namespace virgil {

#if defined(_WIN32)
namespace {
struct WsaInit {
  WsaInit() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
  }
  ~WsaInit() { WSACleanup(); }
};
void ensure_wsa() { static WsaInit init; }
}  // namespace
#define VIRGIL_CLOSESOCK closesocket
#else
static void ensure_wsa() {}
#define VIRGIL_CLOSESOCK ::close
#endif

bool parse_ipv4(const std::string& s, uint32_t* out) {
  in_addr a;
  if (inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
  *out = ntohl(a.s_addr);
  return true;
}

std::string ipv4_to_string(uint32_t addr) {
  char buf[INET_ADDRSTRLEN];
  in_addr a;
  a.s_addr = htonl(addr);
  inet_ntop(AF_INET, &a, buf, sizeof buf);
  return buf;
}

bool is_multicast(uint32_t addr) { return (addr >> 28) == 0xE; }

bool resolve_interface(const std::string& name, uint32_t* out) {
  ensure_wsa();
  if (!name.empty() && parse_ipv4(name, out)) return true;
  // By name: exact match. Automatic: the best-scored interface, so that a
  // Hyper-V/WSL, VPN or container adapter is not picked over the real NIC.
  const InterfaceInfo* best = nullptr;
  const auto all = list_interfaces();
  for (const auto& i : all) {
    if (!name.empty()) {
      if (i.name == name || i.id == name) {
        *out = i.addr;
        return true;
      }
      continue;
    }
    if (i.loopback) continue;
    if (!best || i.score > best->score) best = &i;
  }
  if (!best) return false;
  *out = best->addr;
  return true;
}

namespace {
#if defined(_WIN32)
bool contains_ci(const std::string& hay, const char* needle) {
  std::string h = hay, n = needle;
  for (auto& c : h) c = char(std::tolower(static_cast<unsigned char>(c)));
  for (auto& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
  return h.find(n) != std::string::npos;
}
bool looks_virtual(const std::string& s) {
  static const char* kWords[] = {"hyper-v", "vethernet", "virtual", "vmware", "virtualbox", "wsl",
                                 "tap-", "tap ", "vpn", "wireguard", "tailscale", "zerotier",
                                 "npcap", "bluetooth", "miniport", "docker"};
  for (const char* w : kWords)
    if (contains_ci(s, w)) return true;
  return false;
}
#else
bool posix_virtual_name(const std::string& n) {
  static const char* kPrefixes[] = {"docker", "br-", "veth", "virbr", "vmnet", "vboxnet", "tun",
                                    "tap", "wg", "zt", "tailscale", "utun", "awdl", "llw",
                                    "bridge", "lxc", "cni", "flannel"};
  for (const char* p : kPrefixes)
    if (n.compare(0, std::strlen(p), p) == 0) return true;
  return false;
}
// Wi-Fi by media type where the OS tells us, else by Linux naming (wl*).
// macOS names Wi-Fi "en0"/"en1" like Ethernet, so the name says nothing there.
bool posix_is_wifi(const std::string& n, int probe_fd) {
#if defined(__APPLE__)
  if (probe_fd >= 0) {
    ifmediareq ifmr;
    std::memset(&ifmr, 0, sizeof ifmr);
    std::strncpy(ifmr.ifm_name, n.c_str(), sizeof ifmr.ifm_name - 1);
    if (ioctl(probe_fd, SIOCGIFMEDIA, &ifmr) == 0)
      return IFM_TYPE(ifmr.ifm_current ? ifmr.ifm_current : ifmr.ifm_active) == IFM_IEEE80211;
  }
#else
  (void)probe_fd;
#endif
  return n.compare(0, 2, "wl") == 0;
}
#endif
int link_local_penalty(uint32_t addr) { return (addr >> 16) == 0xA9FE ? -1 : 0; }  // 169.254/16
}  // namespace

std::vector<InterfaceInfo> list_interfaces() {
  ensure_wsa();
  std::vector<InterfaceInfo> out;
#if defined(_WIN32)
  ULONG size = 16 * 1024;
  std::vector<unsigned char> buf;
  ULONG rc = ERROR_BUFFER_OVERFLOW;
  IP_ADAPTER_ADDRESSES* aa = nullptr;
  for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
    buf.assign(size, 0);
    aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    rc = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
        aa, &size);
  }
  if (rc != NO_ERROR) return out;
  for (auto* a = aa; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp) continue;
    char fname[256], desc[256];
    WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, fname, sizeof fname, nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, desc, sizeof desc, nullptr, nullptr);
    const bool virt = looks_virtual(fname) || looks_virtual(desc);
    // Wired always outranks Wi-Fi, even on a link-local (169.254) Dante network
    // against a Wi-Fi network with a gateway.
    int score = a->IfType == IF_TYPE_ETHERNET_CSMACD ? 6 : a->IfType == IF_TYPE_IEEE80211 ? 2 : 1;
    if (a->FirstGatewayAddress) score += 1;
    if (virt) score -= 10;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      InterfaceInfo i;
      i.name = fname;
      i.id = a->AdapterName;
      i.addr = ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
      i.loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
      i.virtual_adapter = virt;
      i.score = score + link_local_penalty(i.addr);
      if (a->PhysicalAddressLength == 6) std::memcpy(i.mac, a->PhysicalAddress, 6);
      out.push_back(i);
    }
  }
#else
  ifaddrs* ifs = nullptr;
  if (getifaddrs(&ifs) != 0) return out;
#if defined(__APPLE__)
  const int probe_fd = socket(AF_INET, SOCK_DGRAM, 0);  // for SIOCGIFMEDIA
#else
  const int probe_fd = -1;
#endif
  for (ifaddrs* i = ifs; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
    InterfaceInfo info;
    info.name = i->ifa_name;
    info.addr = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
    info.loopback = (i->ifa_flags & IFF_LOOPBACK) != 0;
    info.id = info.name;
    info.virtual_adapter = posix_virtual_name(info.name);
    // Wired first, Wi-Fi next (by media type on macOS, by name elsewhere).
    info.score = posix_is_wifi(info.name, probe_fd) ? 2 : 4;
    if (info.virtual_adapter) info.score -= 10;
    // Up but no carrier (cable unplugged): last resort only.
    if (!info.loopback && !(i->ifa_flags & IFF_RUNNING)) info.score -= 5;
    info.score += link_local_penalty(info.addr);
    out.push_back(info);
  }
#if defined(__APPLE__)
  if (probe_fd >= 0) ::close(probe_fd);
#endif
  // Hardware addresses come as separate link-layer entries.
  for (ifaddrs* i = ifs; i; i = i->ifa_next) {
    if (!i->ifa_addr) continue;
    const uint8_t* hw = nullptr;
#if defined(__APPLE__)
    if (i->ifa_addr->sa_family == AF_LINK) {
      const auto* dl = reinterpret_cast<const sockaddr_dl*>(i->ifa_addr);
      if (dl->sdl_alen == 6) hw = reinterpret_cast<const uint8_t*>(LLADDR(dl));
    }
#else
    if (i->ifa_addr->sa_family == AF_PACKET) {
      const auto* ll = reinterpret_cast<const sockaddr_ll*>(i->ifa_addr);
      if (ll->sll_halen == 6) hw = ll->sll_addr;
    }
#endif
    if (!hw) continue;
    for (auto& info : out)
      if (info.name == i->ifa_name) std::memcpy(info.mac, hw, 6);
  }
  freeifaddrs(ifs);
#endif
  return out;
}

bool UdpSocket::open(uint16_t port, bool reuse) {
  ensure_wsa();
  close();
  auto fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == decltype(fd)(kInvalid)) return false;
  fd_ = Fd(fd);
  if (reuse) {
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&one), sizeof one);
#endif
  }
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
    close();
    return false;
  }
  return true;
}

void UdpSocket::close() {
  if (fd_ != kInvalid) VIRGIL_CLOSESOCK(fd_);
  fd_ = kInvalid;
  kernel_ts_ = false;
  tx_ts_ = false;
  tx_key_ = 0;
  win_recvmsg_ = nullptr;
}

static bool mreq_op(uintptr_t fd, int op, uint32_t group, uint32_t iface) {
  ip_mreq m{};
  m.imr_multiaddr.s_addr = htonl(group);
  m.imr_interface.s_addr = htonl(iface);
  return setsockopt(decltype(socket(0, 0, 0))(fd), IPPROTO_IP, op,
                    reinterpret_cast<const char*>(&m), sizeof m) == 0;
}

bool UdpSocket::join_multicast(uint32_t group, uint32_t iface) {
#if defined(IP_MULTICAST_ALL)
  // Linux otherwise delivers every group joined by *any* socket on this port.
  int zero = 0;
  setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof zero);
#endif
  return mreq_op(uintptr_t(fd_), IP_ADD_MEMBERSHIP, group, iface);
}
bool UdpSocket::leave_multicast(uint32_t group, uint32_t iface) {
  return mreq_op(uintptr_t(fd_), IP_DROP_MEMBERSHIP, group, iface);
}

bool UdpSocket::set_multicast_interface(uint32_t iface) {
  in_addr a;
  a.s_addr = htonl(iface);
  return setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&a),
                    sizeof a) == 0;
}

bool UdpSocket::set_multicast_ttl(int ttl) {
#if defined(_WIN32)
  DWORD v = DWORD(ttl);
#else
  unsigned char v = static_cast<unsigned char>(ttl);
#endif
  return setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&v),
                    sizeof v) == 0;
}

bool UdpSocket::set_multicast_loop(bool on) {
#if defined(_WIN32)
  DWORD v = on;
#else
  unsigned char v = on;
#endif
  return setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&v),
                    sizeof v) == 0;
}

bool UdpSocket::set_dscp(int dscp) {
  int tos = dscp << 2;
  return setsockopt(fd_, IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&tos), sizeof tos) == 0;
}

bool UdpSocket::set_recv_timeout_ms(int ms) {
#if defined(_WIN32)
  DWORD v = DWORD(ms);
  return setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof v) == 0;
#else
  timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0;
#endif
}

bool UdpSocket::set_buffer_sizes(int bytes) {
  bool ok = setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes),
                       sizeof bytes) == 0;
  ok &= setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes),
                   sizeof bytes) == 0;
  return ok;
}

namespace {
#if !defined(_WIN32)
// Kernel stamps on CLOCK_REALTIME; move one onto the monotonic clock. The
// realtime-to-monotonic offset is read with both clocks back to back, retried
// if we were preempted in between (an offset taken across a preemption makes
// the timestamp early by that long).
[[maybe_unused]] int64_t realtime_to_mono(int64_t rt) {
  int64_t offset = 0;
  for (int tries = 0; tries < 4; ++tries) {
    const int64_t r1 = realtime_ns(), m = mono_ns(), r2 = realtime_ns();
    offset = (r1 + (r2 - r1) / 2) - m;
    if (r2 - r1 < 5000) break;
  }
  return rt - offset;
}
#endif
#if defined(VIRGIL_APPLE_RX_TS)
// mach_absolute_time units -> ns, exactly as platform.cpp's mono_ns().
int64_t mach_to_mono(uint64_t t) {
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t i;
    mach_timebase_info(&i);
    return i;
  }();
  return int64_t((__uint128_t)t * tb.numer / tb.denom);
}
#endif
#if defined(_WIN32)
// Winsock timestamping (Windows 10 2004+/11): SIO_TIMESTAMPING and the
// SO_TIMESTAMP control message, declared here from the documented values
// because MinGW only exposes them for NTDDI_WIN10_FE and later targets.
constexpr DWORD kSioTimestamping = IOC_IN | IOC_VENDOR | 235;  // _WSAIOW(IOC_VENDOR, 235)
constexpr INT kWinSoTimestamp = 0x300A;
constexpr ULONG kTimestampingFlagRx = 0x1;
struct WinTimestampingConfig {
  ULONG Flags;
  USHORT TxTimestampsBuffered;
};
// QPC ticks -> ns, exactly as platform.cpp's mono_ns().
int64_t qpc_to_mono(int64_t t) {
  static const int64_t f = [] {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return int64_t(li.QuadPart);
  }();
  return (t / f) * 1000000000LL + (t % f) * 1000000000LL / f;
}
#endif
// A kernel receive stamp is used only if plausible: not after the moment we
// read the packet, and not absurdly long before it (wrong clock base).
[[maybe_unused]] bool plausible_rx(int64_t k, int64_t after) { return k <= after + 1000000 && k > after - 2000000000LL; }
}  // namespace

bool UdpSocket::enable_rx_timestamps() {
#if defined(_WIN32)
  // Runtime capability check: older Windows rejects the ioctl. Where it
  // succeeds but the adapter provides no stamps, the control message is
  // simply absent and recv_from stamps in user space as before.
  WinTimestampingConfig cfg{kTimestampingFlagRx, 0};
  DWORD ret = 0;
  if (WSAIoctl(fd_, kSioTimestamping, &cfg, sizeof cfg, nullptr, 0, &ret, nullptr, nullptr) != 0)
    return false;
  GUID g = WSAID_WSARECVMSG;
  LPFN_WSARECVMSG fn = nullptr;
  if (WSAIoctl(fd_, SIO_GET_EXTENSION_FUNCTION_POINTER, &g, sizeof g, &fn, sizeof fn, &ret, nullptr,
               nullptr) != 0 || !fn)
    return false;
  win_recvmsg_ = reinterpret_cast<void*>(fn);
  kernel_ts_ = true;
  return true;
#elif defined(VIRGIL_APPLE_RX_TS)
  // mach_absolute_time() at UDP input, the clock mono_ns() reads.
  int one = 1;
  kernel_ts_ = setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMP_MONOTONIC, &one, sizeof one) == 0;
  return kernel_ts_;
#elif defined(SO_TIMESTAMPNS)
  int one = 1;
  kernel_ts_ = setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof one) == 0;
  return kernel_ts_;
#else
  return false;
#endif
}

bool UdpSocket::enable_tx_timestamps() {
#if defined(VIRGIL_LINUX_TX_TS)
  // Software stamp in the driver's transmit path, reported on the error
  // queue without the packet (OPT_TSONLY needs no privilege), keyed per send.
  int flags = SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE |
              SOF_TIMESTAMPING_OPT_TSONLY | SOF_TIMESTAMPING_OPT_ID;
  tx_ts_ = setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof flags) == 0;
  tx_key_ = 0;
  return tx_ts_;
#else
  return false;
#endif
}

void UdpSocket::drain_tx_timestamps() {
#if defined(VIRGIL_LINUX_TX_TS)
  for (int i = 0; i < 64; ++i) {
    char data[64];
    alignas(cmsghdr) char ctrl[256];
    iovec iov{data, sizeof data};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof ctrl;
    if (recvmsg(fd_, &msg, MSG_ERRQUEUE | MSG_DONTWAIT) < 0) break;
  }
#endif
}

bool UdpSocket::read_tx_timestamp(int64_t* tx_mono_ns, int timeout_ms) {
#if defined(VIRGIL_LINUX_TX_TS)
  if (!tx_ts_ || fd_ == kInvalid) return false;
  const uint32_t want = tx_key_ - 1;
  const int64_t deadline = mono_ns() + int64_t(timeout_ms) * 1000000LL;
  for (;;) {
    char data[64];
    alignas(cmsghdr) char ctrl[256];
    iovec iov{data, sizeof data};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof ctrl;
    if (recvmsg(fd_, &msg, MSG_ERRQUEUE | MSG_DONTWAIT) >= 0) {
      int64_t rt = 0;
      bool have_key = false;
      uint32_t key = 0;
      for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPING) {
          scm_timestamping st;
          std::memcpy(&st, CMSG_DATA(c), sizeof st);
          rt = int64_t(st.ts[0].tv_sec) * 1000000000LL + st.ts[0].tv_nsec;  // [0]: software
        } else if (c->cmsg_level == SOL_IP && c->cmsg_type == IP_RECVERR) {
          sock_extended_err ee;
          std::memcpy(&ee, CMSG_DATA(c), sizeof ee);
          if (ee.ee_errno == ENOMSG && ee.ee_origin == SO_EE_ORIGIN_TIMESTAMPING) {
            have_key = true;
            key = ee.ee_data;
          }
        }
      }
      if (rt != 0 && (!have_key || int32_t(key - want) >= 0)) {
        if (have_key) tx_key_ = key + 1;  // resynchronise if we counted wrong
        *tx_mono_ns = realtime_to_mono(rt);
        return true;
      }
      continue;  // an older send's stamp: look further
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
    const int64_t left = deadline - mono_ns();
    if (left <= 0) return false;
    pollfd p{fd_, 0, 0};  // POLLERR: something on the error queue
    poll(&p, 1, int((left + 999999) / 1000000));
  }
#else
  (void)tx_mono_ns;
  (void)timeout_ms;
  return false;
#endif
}

int UdpSocket::send_to(const void* buf, size_t len, const Endpoint& to) {
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(to.port);
  sa.sin_addr.s_addr = htonl(to.addr);
  if (tx_ts_) drain_tx_timestamps();  // stale stamps (too late for their send)
  const int n = int(sendto(fd_, static_cast<const char*>(buf), int(len), 0,
                           reinterpret_cast<sockaddr*>(&sa), sizeof sa));
  if (n >= 0 && tx_ts_) ++tx_key_;
  return n;
}

int UdpSocket::recv_from(void* buf, size_t len, Endpoint* from, int64_t* rx_mono_ns) {
  sockaddr_in sa{};
#if defined(_WIN32)
  int salen = sizeof sa;
  int n = 0;
  if (win_recvmsg_) {
    WSABUF data;
    data.len = ULONG(len);
    data.buf = static_cast<char*>(buf);
    alignas(WSACMSGHDR) char ctrl[128];
    WSAMSG msg{};
    msg.name = reinterpret_cast<LPSOCKADDR>(&sa);
    msg.namelen = salen;
    msg.lpBuffers = &data;
    msg.dwBufferCount = 1;
    msg.Control.len = sizeof ctrl;
    msg.Control.buf = ctrl;
    DWORD got = 0;
    const int rc = reinterpret_cast<LPFN_WSARECVMSG>(win_recvmsg_)(fd_, &msg, &got, nullptr, nullptr);
    const int64_t after_mono = mono_ns();
    if (rx_mono_ns) *rx_mono_ns = after_mono;
    n = rc == 0 ? int(got) : -1;
    if (n >= 0 && rx_mono_ns) {
      for (WSACMSGHDR* c = WSA_CMSG_FIRSTHDR(&msg); c; c = WSA_CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == kWinSoTimestamp) {
          UINT64 qpc;
          std::memcpy(&qpc, WSA_CMSG_DATA(c), sizeof qpc);
          const int64_t k = qpc_to_mono(int64_t(qpc));
          if (plausible_rx(k, after_mono)) *rx_mono_ns = k;
          break;
        }
      }
    }
  } else {
    n = recvfrom(fd_, static_cast<char*>(buf), int(len), 0, reinterpret_cast<sockaddr*>(&sa),
                 &salen);
    if (rx_mono_ns) *rx_mono_ns = mono_ns();
  }
  if (n < 0) {
    int e = WSAGetLastError();
    return (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) ? 0 : -1;
  }
#else
  iovec iov{buf, len};
  alignas(cmsghdr) char ctrl[256];
  msghdr msg{};
  msg.msg_name = &sa;
  msg.msg_namelen = sizeof sa;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ctrl;
  msg.msg_controllen = sizeof ctrl;
  // With TX timestamps on, select() also reports the socket readable for
  // a (late) stamp on the error queue: drain it and do not block.
  if (tx_ts_) drain_tx_timestamps();
  ssize_t n = recvmsg(fd_, &msg, tx_ts_ ? MSG_DONTWAIT : 0);
  const int64_t after_mono = mono_ns();
  if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
  if (rx_mono_ns) {
    *rx_mono_ns = after_mono;
    if (kernel_ts_) {
      for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
#if defined(VIRGIL_APPLE_RX_TS)
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP_MONOTONIC) {
          uint64_t t;
          std::memcpy(&t, CMSG_DATA(c), sizeof t);
          const int64_t k = mach_to_mono(t);
          if (plausible_rx(k, after_mono)) *rx_mono_ns = k;
          break;
        }
#elif defined(SO_TIMESTAMPNS)
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
          timespec ts;
          std::memcpy(&ts, CMSG_DATA(c), sizeof ts);
          const int64_t k = realtime_to_mono(int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec);
          if (plausible_rx(k, after_mono)) *rx_mono_ns = k;
          break;
        }
#endif
      }
    }
  }
#endif
  if (from) {
    from->addr = ntohl(sa.sin_addr.s_addr);
    from->port = ntohs(sa.sin_port);
  }
  return int(n);
}

int UdpSocket::wait_readable(UdpSocket& a, UdpSocket* b, int timeout_ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(a.fd_, &set);
  auto maxfd = a.fd_;
  if (b && b->is_open()) {
    FD_SET(b->fd_, &set);
    if (b->fd_ > maxfd) maxfd = b->fd_;
  }
  timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  int r = select(int(maxfd) + 1, &set, nullptr, nullptr, &tv);
  if (r <= 0) return 0;
  int mask = 0;
  if (FD_ISSET(a.fd_, &set)) mask |= 1;
  if (b && b->is_open() && FD_ISSET(b->fd_, &set)) mask |= 2;
  return mask;
}

}  // namespace virgil
