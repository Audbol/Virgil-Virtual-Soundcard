// RFC 2974 Session Announcement Protocol: how AES67 devices (including Dante
// devices in AES67 mode) advertise and discover multicast streams.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsv/net.h"
#include "dsv/sdp.h"

namespace dsv {

namespace sap {
constexpr uint16_t kPort = 9875;
constexpr uint32_t kAdminScopeGroup = 0xEFFFFFFF;  // 239.255.255.255 (Dante, most AES67)
constexpr uint32_t kGlobalGroup = 0xE0027FFE;      // 224.2.127.254

std::vector<uint8_t> build_packet(const std::string& sdp, uint32_t origin, uint16_t msg_hash,
                                  bool deletion);
bool parse_packet(const uint8_t* p, size_t len, std::string* sdp, bool* deletion,
                  uint16_t* msg_hash, uint32_t* origin);
}  // namespace sap

class SapService {
 public:
  ~SapService() { stop(); }
  bool start(uint32_t interface_addr);
  void stop();

  // Replace the set of sessions this host announces.
  void set_announcements(const std::vector<SdpInfo>& sessions);

  // Discovered remote sessions.
  bool find(const std::string& session_name, SdpInfo* out) const;
  std::vector<SdpInfo> sessions() const;

 private:
  struct Remote {
    SdpInfo sdp;
    int64_t seen_ns = 0;
  };
  void run();
  void announce_all(bool deletion);

  uint32_t iface_ = 0;
  UdpSocket sock_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  mutable std::mutex mutex_;
  std::vector<SdpInfo> local_;
  bool local_dirty_ = false;
  std::map<std::string, Remote> remote_;  // keyed by origin + session id
};

}  // namespace dsv
