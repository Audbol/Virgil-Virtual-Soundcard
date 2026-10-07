#include "dsv/sap.h"

#include <cstring>

#include "dsv/log.h"
#include "dsv/platform.h"

namespace dsv {
namespace sap {

static const char kMime[] = "application/sdp";

std::vector<uint8_t> build_packet(const std::string& sdp, uint32_t origin, uint16_t hash,
                                  bool deletion) {
  std::vector<uint8_t> p;
  p.reserve(8 + sizeof kMime + sdp.size());
  p.push_back(uint8_t(0x20 | (deletion ? 0x04 : 0)));  // V=1, IPv4, announce/delete
  p.push_back(0);                                        // no auth
  p.push_back(uint8_t(hash >> 8));
  p.push_back(uint8_t(hash));
  for (int s = 24; s >= 0; s -= 8) p.push_back(uint8_t(origin >> s));
  p.insert(p.end(), kMime, kMime + sizeof kMime);  // includes NUL
  p.insert(p.end(), sdp.begin(), sdp.end());
  return p;
}

bool parse_packet(const uint8_t* p, size_t len, std::string* sdp, bool* deletion, uint16_t* hash,
                  uint32_t* origin) {
  if (len < 8 || (p[0] >> 5) != 1) return false;
  if (p[0] & 0x02) return false;  // encrypted
  if (p[0] & 0x01) return false;  // compressed (zlib) - not used by AES67 gear
  const bool ipv6 = (p[0] & 0x10) != 0;
  *deletion = (p[0] & 0x04) != 0;
  *hash = uint16_t(p[2] << 8 | p[3]);
  size_t off = 4;
  if (ipv6) {
    *origin = 0;
    off += 16;
  } else {
    *origin = uint32_t(p[4]) << 24 | uint32_t(p[5]) << 16 | uint32_t(p[6]) << 8 | p[7];
    off += 4;
  }
  off += size_t(p[1]) * 4;  // authentication data
  if (off >= len) return false;
  // Optional payload type; absent if the payload starts directly with "v=0".
  if (!(len - off >= 3 && std::memcmp(p + off, "v=0", 3) == 0)) {
    const void* nul = std::memchr(p + off, 0, len - off);
    if (!nul) return false;
    const size_t type_len = size_t(static_cast<const uint8_t*>(nul) - (p + off));
    if (std::string(reinterpret_cast<const char*>(p + off), type_len) != kMime) return false;
    off += type_len + 1;
  }
  sdp->assign(reinterpret_cast<const char*>(p + off), len - off);
  return true;
}

}  // namespace sap

static uint16_t session_hash(const SdpInfo& s) {
  uint32_t h = 2166136261u;
  for (char c : s.session_name) h = (h ^ uint8_t(c)) * 16777619u;
  h ^= uint32_t(s.session_id);
  return uint16_t(h ^ (h >> 16));
}

bool SapService::start(uint32_t iface) {
  stop();
  iface_ = iface;
  if (!sock_.open(sap::kPort)) {
    DSV_LOG_ERROR("sap: cannot bind UDP %u", sap::kPort);
    return false;
  }
  sock_.join_multicast(sap::kAdminScopeGroup, iface);
  sock_.join_multicast(sap::kGlobalGroup, iface);
  sock_.set_multicast_interface(iface);
  sock_.set_multicast_ttl(32);
  sock_.set_multicast_loop(true);
  running_ = true;
  thread_ = std::thread([this] { run(); });
  return true;
}

void SapService::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  announce_all(true);
  sock_.close();
}

void SapService::set_announcements(const std::vector<SdpInfo>& sessions) {
  std::lock_guard<std::mutex> l(mutex_);
  local_ = sessions;
  local_dirty_ = true;
}

bool SapService::find(const std::string& name, SdpInfo* out) const {
  std::lock_guard<std::mutex> l(mutex_);
  for (const auto& [key, r] : remote_) {
    if (r.sdp.session_name == name) {
      *out = r.sdp;
      return true;
    }
  }
  return false;
}

std::vector<SdpInfo> SapService::sessions() const {
  std::lock_guard<std::mutex> l(mutex_);
  std::vector<SdpInfo> v;
  for (const auto& [key, r] : remote_) v.push_back(r.sdp);
  return v;
}

void SapService::announce_all(bool deletion) {
  std::vector<SdpInfo> local;
  {
    std::lock_guard<std::mutex> l(mutex_);
    local = local_;
  }
  for (const auto& s : local) {
    auto pkt = sap::build_packet(build_sdp(s), iface_, session_hash(s), deletion);
    sock_.send_to(pkt.data(), pkt.size(), {sap::kAdminScopeGroup, sap::kPort});
  }
}

void SapService::run() {
  uint8_t buf[4096];
  int64_t next_announce = 0;
  while (running_) {
    const int64_t now = mono_ns();
    bool dirty;
    {
      std::lock_guard<std::mutex> l(mutex_);
      dirty = local_dirty_;
      local_dirty_ = false;
    }
    if (dirty || now >= next_announce) {
      announce_all(false);
      next_announce = now + 30000000000LL;  // RFC 2974 interval for few sessions
    }
    if (!(UdpSocket::wait_readable(sock_, nullptr, 200) & 1)) continue;
    Endpoint from;
    int n = sock_.recv_from(buf, sizeof buf, &from);
    if (n <= 0) continue;
    std::string text;
    bool deletion;
    uint16_t hash;
    uint32_t origin;
    if (!sap::parse_packet(buf, size_t(n), &text, &deletion, &hash, &origin)) continue;
    const std::string key = ipv4_to_string(origin) + "/" + std::to_string(hash);
    std::lock_guard<std::mutex> l(mutex_);
    if (deletion) {
      remote_.erase(key);
      continue;
    }
    SdpInfo s;
    if (!parse_sdp(text, &s)) continue;
    auto it = remote_.find(key);
    if (it == remote_.end())
      DSV_LOG_INFO("sap: discovered \"%s\" %s:%u %s/%u/%u", s.session_name.c_str(),
                   s.connection_address.c_str(), s.port, s.encoding.c_str(), s.sample_rate,
                   s.channels);
    remote_[key] = Remote{s, now};
    // Expire sessions not refreshed for 10 announcement intervals (RFC 2974).
    for (auto i = remote_.begin(); i != remote_.end();)
      i = now - i->second.seen_ns > 300000000000LL ? remote_.erase(i) : std::next(i);
  }
}

}  // namespace dsv
