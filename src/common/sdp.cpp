#include "dsv/sdp.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dsv {

static std::string format_ptime(uint32_t us) {
  char buf[32];
  if (us % 1000 == 0)
    std::snprintf(buf, sizeof buf, "%u", us / 1000);
  else
    std::snprintf(buf, sizeof buf, "%.3f", us / 1000.0);
  std::string s = buf;
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
  }
  return s;
}

std::string build_sdp(const SdpInfo& s) {
  const bool multicast = [&] {
    int a = std::atoi(s.connection_address.c_str());
    return a >= 224 && a <= 239;
  }();
  std::ostringstream o;
  o << "v=0\r\n";
  o << "o=- " << s.session_id << ' ' << s.session_version << " IN IP4 " << s.origin_address
    << "\r\n";
  o << "s=" << s.session_name << "\r\n";
  if (!s.session_info.empty()) o << "i=" << s.session_info << "\r\n";
  o << "c=IN IP4 " << s.connection_address;
  if (multicast) o << '/' << s.ttl;
  o << "\r\n";
  o << "t=0 0\r\n";
  o << "a=clock-domain:PTPv2 " << s.ptp_domain << "\r\n";
  o << "m=audio " << s.port << " RTP/AVP " << unsigned(s.payload_type) << "\r\n";
  if (multicast)
    o << "a=source-filter: incl IN IP4 " << s.connection_address << ' ' << s.origin_address
      << "\r\n";
  o << "a=rtpmap:" << unsigned(s.payload_type) << ' ' << s.encoding << '/' << s.sample_rate << '/'
    << s.channels << "\r\n";
  o << "a=sync-time:0\r\n";
  o << "a=framecount:" << s.frames_per_packet() << "\r\n";
  o << "a=ptime:" << format_ptime(s.ptime_us) << "\r\n";
  o << "a=maxptime:" << format_ptime(s.ptime_us) << "\r\n";
  o << "a=mediaclk:direct=" << s.ts_offset << "\r\n";
  if (!s.ptp_grandmaster.empty())
    o << "a=ts-refclk:ptp=IEEE1588-2008:" << s.ptp_grandmaster << ':' << s.ptp_domain << "\r\n";
  else
    o << "a=ts-refclk:ptp=IEEE1588-2008:traceable\r\n";
  o << "a=recvonly\r\n";
  return o.str();
}

static bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

bool parse_sdp(const std::string& text, SdpInfo* out) {
  SdpInfo s;
  s.channels = 0;
  bool have_media = false, have_rtpmap = false, have_conn = false;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() < 2 || line[1] != '=') continue;
    const std::string v = line.substr(2);
    switch (line[0]) {
      case 's': s.session_name = v; break;
      case 'i': if (s.session_info.empty()) s.session_info = v; break;
      case 'o': {
        std::istringstream o(v);
        std::string user, nettype, addrtype;
        o >> user >> s.session_id >> s.session_version >> nettype >> addrtype >> s.origin_address;
        break;
      }
      case 'c': {
        // c=IN IP4 239.69.1.2/32
        std::istringstream c(v);
        std::string nettype, addrtype, addr;
        c >> nettype >> addrtype >> addr;
        if (addrtype != "IP4") return false;
        size_t slash = addr.find('/');
        if (slash != std::string::npos) {
          s.ttl = uint32_t(std::strtoul(addr.c_str() + slash + 1, nullptr, 10));
          addr.resize(slash);
        }
        s.connection_address = addr;
        have_conn = true;
        break;
      }
      case 'm': {
        if (have_media) break;  // first audio stream only
        std::istringstream m(v);
        std::string media, proto;
        unsigned port = 0, pt = 0;
        m >> media >> port >> proto >> pt;
        if (media != "audio") break;
        s.port = uint16_t(port);
        s.payload_type = uint8_t(pt);
        have_media = true;
        break;
      }
      case 'a': {
        if (starts_with(v, "rtpmap:")) {
          // rtpmap:96 L24/48000/8
          unsigned pt = 0;
          char enc[16] = {0};
          unsigned rate = 0, ch = 1;
          int n = std::sscanf(v.c_str(), "rtpmap:%u %15[^/]/%u/%u", &pt, enc, &rate, &ch);
          if (n >= 3 && pt == s.payload_type) {
            s.encoding = enc;
            s.sample_rate = rate;
            s.channels = n >= 4 ? ch : 1;
            have_rtpmap = true;
          }
        } else if (starts_with(v, "ptime:")) {
          s.ptime_us = uint32_t(std::strtod(v.c_str() + 6, nullptr) * 1000.0 + 0.5);
        } else if (starts_with(v, "mediaclk:direct=")) {
          s.ts_offset = uint32_t(std::strtoul(v.c_str() + 16, nullptr, 10));
        } else if (starts_with(v, "ts-refclk:ptp=IEEE1588-2008:")) {
          std::string r = v.substr(28);
          size_t colon = r.find(':');
          s.ptp_grandmaster = r.substr(0, colon);
          if (colon != std::string::npos) s.ptp_domain = std::atoi(r.c_str() + colon + 1);
        } else if (starts_with(v, "clock-domain:PTPv2 ")) {
          s.ptp_domain = std::atoi(v.c_str() + 19);
        } else if (starts_with(v, "source-filter:")) {
          s.source_filter = v.substr(14);
        }
        break;
      }
      default: break;
    }
  }
  if (!have_media || !have_rtpmap || !have_conn) return false;
  if (s.encoding != "L16" && s.encoding != "L24") return false;
  if (s.channels == 0 || s.sample_rate == 0) return false;
  *out = s;
  return true;
}

}  // namespace dsv
