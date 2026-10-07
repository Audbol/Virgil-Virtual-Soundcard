// Minimal IPv4 UDP socket wrapper (BSD sockets / Winsock).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace virgil {

struct Endpoint {
  uint32_t addr = 0;  // host byte order
  uint16_t port = 0;
};

bool parse_ipv4(const std::string& s, uint32_t* out);  // host byte order
std::string ipv4_to_string(uint32_t addr);
bool is_multicast(uint32_t addr);

// Pick the IPv4 address of a named interface ("eth0"), or validate a literal
// address. Empty input picks the most likely physical network interface:
// wired over Wi-Fi, never a virtual adapter unless nothing else is up.
bool resolve_interface(const std::string& name_or_ip, uint32_t* out);

struct InterfaceInfo {
  std::string name;  // OS name ("eth0", "en0", "Ethernet 2")
  uint32_t addr = 0;
  bool loopback = false;
  std::string id;                // adapter GUID on Windows, else the name
  bool virtual_adapter = false;  // Hyper-V/WSL, VPN, container bridge, ...
  int score = 0;                 // automatic choice prefers the highest
};
// Active IPv4 interfaces.
std::vector<InterfaceInfo> list_interfaces();

// DSCP values used for media and clock traffic (as Dante and AES67 do).
constexpr int kDscpPtp = 46;    // EF
constexpr int kDscpMedia = 34;  // AF41

class UdpSocket {
 public:
  UdpSocket() = default;
  ~UdpSocket() { close(); }
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  UdpSocket(UdpSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = kInvalid; }

  // Bind to INADDR_ANY:port (port 0 = ephemeral) with SO_REUSEADDR/PORT so
  // several receivers and other PTP software can share the port.
  bool open(uint16_t port, bool reuse = true);
  void close();
  bool is_open() const { return fd_ != kInvalid; }

  bool join_multicast(uint32_t group, uint32_t iface);
  bool leave_multicast(uint32_t group, uint32_t iface);
  bool set_multicast_interface(uint32_t iface);
  bool set_multicast_ttl(int ttl);
  bool set_multicast_loop(bool on);
  bool set_dscp(int dscp);
  bool set_recv_timeout_ms(int ms);
  bool set_buffer_sizes(int bytes);
  // Ask the kernel for receive timestamps (SO_TIMESTAMPNS on Linux).
  bool enable_rx_timestamps();

  int send_to(const void* buf, size_t len, const Endpoint& to);
  // Returns bytes received, 0 on timeout, <0 on error. `rx_mono_ns` is the
  // best available receive time on the virgil monotonic clock.
  int recv_from(void* buf, size_t len, Endpoint* from, int64_t* rx_mono_ns = nullptr);

  // Wait up to timeout_ms for either socket (b may be null) to be readable.
  // Returns a bitmask: 1 = a readable, 2 = b readable.
  static int wait_readable(UdpSocket& a, UdpSocket* b, int timeout_ms);

 private:
#if defined(_WIN32)
  using Fd = uintptr_t;
  static constexpr Fd kInvalid = ~uintptr_t(0);
#else
  using Fd = int;
  static constexpr Fd kInvalid = -1;
#endif
  Fd fd_ = kInvalid;
  bool kernel_ts_ = false;
};

}  // namespace virgil
