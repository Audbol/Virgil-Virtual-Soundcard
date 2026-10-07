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
#include <vector>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
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
bool posix_virtual_name(const std::string& n) {
  static const char* kPrefixes[] = {"docker", "br-", "veth", "virbr", "vmnet", "vboxnet", "tun",
                                    "tap", "wg", "zt", "tailscale", "utun", "awdl", "llw",
                                    "bridge", "lxc", "cni", "flannel"};
  for (const char* p : kPrefixes)
    if (n.compare(0, std::strlen(p), p) == 0) return true;
  return false;
}
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
    int score = a->IfType == IF_TYPE_ETHERNET_CSMACD ? 4 : a->IfType == IF_TYPE_IEEE80211 ? 2 : 1;
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
      out.push_back(i);
    }
  }
#else
  ifaddrs* ifs = nullptr;
  if (getifaddrs(&ifs) != 0) return out;
  for (ifaddrs* i = ifs; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
    InterfaceInfo info;
    info.name = i->ifa_name;
    info.addr = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
    info.loopback = (i->ifa_flags & IFF_LOOPBACK) != 0;
    info.id = info.name;
    info.virtual_adapter = posix_virtual_name(info.name);
    // Wired names first (eth*, en* on Linux and macOS's en0), Wi-Fi next.
    info.score = (info.name.compare(0, 2, "wl") == 0) ? 2 : 4;
    if (info.virtual_adapter) info.score -= 10;
    info.score += link_local_penalty(info.addr);
    out.push_back(info);
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

bool UdpSocket::enable_rx_timestamps() {
#if defined(SO_TIMESTAMPNS)
  int one = 1;
  kernel_ts_ = setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof one) == 0;
  return kernel_ts_;
#else
  return false;
#endif
}

int UdpSocket::send_to(const void* buf, size_t len, const Endpoint& to) {
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(to.port);
  sa.sin_addr.s_addr = htonl(to.addr);
  return int(sendto(fd_, static_cast<const char*>(buf), int(len), 0,
                    reinterpret_cast<sockaddr*>(&sa), sizeof sa));
}

int UdpSocket::recv_from(void* buf, size_t len, Endpoint* from, int64_t* rx_mono_ns) {
  sockaddr_in sa{};
#if defined(_WIN32)
  int salen = sizeof sa;
  int n = recvfrom(fd_, static_cast<char*>(buf), int(len), 0, reinterpret_cast<sockaddr*>(&sa),
                   &salen);
  if (rx_mono_ns) *rx_mono_ns = mono_ns();
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
  ssize_t n = recvmsg(fd_, &msg, 0);
  const int64_t after_mono = mono_ns();
  if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
  if (rx_mono_ns) {
    *rx_mono_ns = after_mono;
#if defined(SO_TIMESTAMPNS)
    if (kernel_ts_) {
      for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
          timespec ts;
          std::memcpy(&ts, CMSG_DATA(c), sizeof ts);
          // Kernel stamps on CLOCK_REALTIME; move it onto the monotonic clock.
          const int64_t rt = int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
          *rx_mono_ns = after_mono - (realtime_ns() - rt);
          break;
        }
      }
    }
#endif
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
