#include "dsv/ptp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "dsv/log.h"
#include "dsv/platform.h"

namespace dsv {

FreeRunClock::FreeRunClock() {
  m_.base_local = mono_ns();
  m_.base_ptp = realtime_ns() + kTaiUtcOffsetNs;
  m_.ratio = 1.0;
}

namespace ptp {

static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
static void wr16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}

bool parse_header(const uint8_t* p, size_t len, Header* h) {
  if (len < kHeaderBytes) return false;
  h->type = p[0] & 0x0f;
  h->version = p[1] & 0x0f;
  if (h->version != 2) return false;
  h->length = rd16(p + 2);
  if (h->length > len) return false;
  h->domain = p[4];
  h->flags = rd16(p + 6);
  uint64_t c = 0;
  for (int i = 0; i < 8; ++i) c = c << 8 | p[8 + i];
  h->correction = int64_t(c);
  std::memcpy(h->source.clock.data(), p + 20, 8);
  h->source.port = rd16(p + 28);
  h->sequence = rd16(p + 30);
  h->control = p[32];
  h->log_interval = int8_t(p[33]);
  return true;
}

size_t write_header(uint8_t* p, const Header& h) {
  std::memset(p, 0, kHeaderBytes);
  p[0] = h.type & 0x0f;
  p[1] = 2;
  wr16(p + 2, h.length);
  p[4] = h.domain;
  wr16(p + 6, h.flags);
  uint64_t c = uint64_t(h.correction);
  for (int i = 7; i >= 0; --i) {
    p[8 + i] = uint8_t(c);
    c >>= 8;
  }
  std::memcpy(p + 20, h.source.clock.data(), 8);
  wr16(p + 28, h.source.port);
  wr16(p + 30, h.sequence);
  p[32] = h.control;
  p[33] = uint8_t(h.log_interval);
  return kHeaderBytes;
}

Timestamp read_timestamp(const uint8_t* p) {
  Timestamp t;
  for (int i = 0; i < 6; ++i) t.seconds = t.seconds << 8 | p[i];
  t.nanoseconds = uint32_t(p[6]) << 24 | uint32_t(p[7]) << 16 | uint32_t(p[8]) << 8 | p[9];
  return t;
}

void write_timestamp(uint8_t* p, const Timestamp& t) {
  uint64_t s = t.seconds;
  for (int i = 5; i >= 0; --i) {
    p[i] = uint8_t(s);
    s >>= 8;
  }
  p[6] = uint8_t(t.nanoseconds >> 24);
  p[7] = uint8_t(t.nanoseconds >> 16);
  p[8] = uint8_t(t.nanoseconds >> 8);
  p[9] = uint8_t(t.nanoseconds);
}

bool parse_announce(const uint8_t* p, size_t len, Announce* a) {
  if (len < 64) return false;
  a->utc_offset = int16_t(rd16(p + 44));
  a->priority1 = p[47];
  a->clock_class = p[48];
  a->clock_accuracy = p[49];
  a->variance = rd16(p + 50);
  a->priority2 = p[52];
  std::memcpy(a->grandmaster.data(), p + 53, 8);
  a->steps_removed = rd16(p + 61);
  a->time_source = p[63];
  return true;
}

size_t write_announce(uint8_t* p, const Header& h, const Announce& a) {
  Header hh = h;
  hh.type = kAnnounce;
  hh.length = 64;
  write_header(p, hh);
  std::memset(p + 34, 0, 30);
  wr16(p + 44, uint16_t(a.utc_offset));
  p[47] = a.priority1;
  p[48] = a.clock_class;
  p[49] = a.clock_accuracy;
  wr16(p + 50, a.variance);
  p[52] = a.priority2;
  std::memcpy(p + 53, a.grandmaster.data(), 8);
  wr16(p + 61, a.steps_removed);
  p[63] = a.time_source;
  return 64;
}

int compare_announce(const Announce& a, const PortId& ap, const Announce& b, const PortId& bp) {
  if (a.grandmaster != b.grandmaster) {
    if (a.priority1 != b.priority1) return a.priority1 < b.priority1 ? -1 : 1;
    if (a.clock_class != b.clock_class) return a.clock_class < b.clock_class ? -1 : 1;
    if (a.clock_accuracy != b.clock_accuracy) return a.clock_accuracy < b.clock_accuracy ? -1 : 1;
    if (a.variance != b.variance) return a.variance < b.variance ? -1 : 1;
    if (a.priority2 != b.priority2) return a.priority2 < b.priority2 ? -1 : 1;
    return a.grandmaster < b.grandmaster ? -1 : 1;
  }
  if (a.steps_removed != b.steps_removed) return a.steps_removed < b.steps_removed ? -1 : 1;
  if (ap.clock != bp.clock) return ap.clock < bp.clock ? -1 : 1;
  if (ap.port != bp.port) return ap.port < bp.port ? -1 : 1;
  return 0;
}

std::string format_clock_id(const ClockId& id) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X", id[0], id[1], id[2],
                id[3], id[4], id[5], id[6], id[7]);
  return buf;
}

}  // namespace ptp

// ---- Servo ----------------------------------------------------------------

PiServo::Result PiServo::sample(int64_t local, int64_t master) {
  if (!init_) {
    m_ = ClockModel{local, master, m_.ratio > 0 ? m_.ratio : 1.0};
    init_ = true;
    count_ = 0;
    last_local_ = first_local_ = local;
    first_master_ = master;
    return kInit;
  }
  const int64_t est = m_.ptp_at(local);
  const int64_t err = est - master;
  last_err_ = err;
  const double dt = double(local - last_local_) * 1e-9;
  if (dt <= 0) return kOutlier;

  if (std::llabs(err) > step_threshold_ns) {
    m_.base_local = local;
    m_.base_ptp = master;
    count_ = 0;
    good_ = 0;
    locked_ = false;
    rms_ = 0;
    last_local_ = first_local_ = local;
    first_master_ = master;
    return kStep;
  }
  // Software timestamps occasionally catch a scheduling hiccup; once tracking,
  // ignore samples far outside the recent error envelope.
  if (locked_ && std::fabs(double(err)) > 6.0 * rms_ + 20000.0) return kOutlier;

  last_local_ = local;
  ++count_;
  if (count_ == 16 && local - first_local_ > 1000000000LL) {
    // Seed the integrator with the raw frequency offset so lock does not
    // have to wait for the slow integral term.
    const double raw = double(master - first_master_) / double(local - first_local_);
    drift_ppb_ = (raw - 1.0) * 1e9;
  } else {
    drift_ppb_ -= ki * double(err) / dt;
  }
  drift_ppb_ = std::clamp(drift_ppb_, -1e6, 1e6);
  const double adj = std::clamp(drift_ppb_ - kp * double(err) / dt, -1e6, 1e6);

  m_.base_local = local;
  m_.base_ptp = est;  // keep phase continuous; frequency does the steering
  m_.ratio = 1.0 + adj * 1e-9;

  rms_ = rms_ == 0 ? std::fabs(double(err)) : 0.95 * rms_ + 0.05 * std::fabs(double(err));
  if (std::llabs(err) < 100000) {
    if (++good_ >= 16) locked_ = true;
  } else {
    good_ = 0;
  }
  return kTracking;
}

// ---- PtpClock ---------------------------------------------------------------

bool PtpClock::start(const Options& o) {
  stop();
  opt_ = o;
  if (!event_.open(ptp::kEventPort) || !general_.open(ptp::kGeneralPort)) {
    DSV_LOG_ERROR("ptp: cannot bind UDP 319/320 (need root/CAP_NET_BIND_SERVICE, or another PTP "
                  "daemon owns them)");
    event_.close();
    general_.close();
    return false;
  }
  for (UdpSocket* s : {&event_, &general_}) {
    s->join_multicast(ptp::kPrimaryGroup, o.interface_addr);
    s->set_multicast_interface(o.interface_addr);
    s->set_multicast_ttl(1);
    s->set_multicast_loop(true);  // lets several dsv instances share a host
    s->set_dscp(kDscpPtp);
    s->enable_rx_timestamps();
  }

  // Clock identity: random EUI-64 with the locally-administered bit set.
  std::random_device rd;
  std::mt19937_64 rng((uint64_t(rd()) << 32) ^ rd() ^ uint64_t(mono_ns()) ^ o.interface_addr);
  uint64_t r = rng();
  for (int i = 0; i < 8; ++i) self_.clock[i] = uint8_t(r >> (i * 8));
  self_.clock[0] = uint8_t((self_.clock[0] & 0xFC) | 0x02);
  self_.port = 1;

  ClockModel initial;
  initial.base_local = mono_ns();
  initial.base_ptp = realtime_ns() + kTaiUtcOffsetNs;
  cell_.store(initial);
  servo_.reset_to(initial);
  state_ = kStateFreeRun;
  start_ns_ = mono_ns();
  running_ = true;
  thread_ = std::thread([this] { run(); });
  DSV_LOG_INFO("ptp: started on %s domain %u, clock id %s",
               ipv4_to_string(o.interface_addr).c_str(), o.domain,
               ptp::format_clock_id(self_.clock).c_str());
  return true;
}

void PtpClock::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  event_.close();
  general_.close();
}

std::string PtpClock::grandmaster() const {
  std::lock_guard<std::mutex> l(gm_mutex_);
  return gm_string_;
}

void PtpClock::run() {
  uint8_t buf[1500];
  while (running_) {
    int ready = UdpSocket::wait_readable(event_, &general_, 20);
    if (ready & 1) {
      Endpoint from;
      int64_t rx = 0;
      int n = event_.recv_from(buf, sizeof buf, &from, &rx);
      if (n > 0) handle_event(buf, size_t(n), rx, from);
    }
    if (ready & 2) {
      Endpoint from;
      int64_t rx = 0;
      int n = general_.recv_from(buf, sizeof buf, &from, &rx);
      if (n > 0) handle_general(buf, size_t(n), rx);
    }
    const int64_t now = mono_ns();
    select_master(now);
    if (is_master_) {
      master_duties(now);
    } else if (have_master_ && have_pair_ && now >= next_delay_req_ns_) {
      send_delay_req();
      // Randomise around 1 s so many slaves do not burst in lockstep.
      next_delay_req_ns_ = now + 750000000LL + (int64_t(std::rand()) % 500) * 1000000LL;
    }
  }
}

void PtpClock::handle_event(const uint8_t* p, size_t n, int64_t rx_ns, const Endpoint&) {
  ptp::Header h;
  if (!ptp::parse_header(p, n, &h) || h.domain != opt_.domain || h.source == self_) return;

  if (h.type == ptp::kSync && !is_master_ && have_master_ && h.source == master_ && n >= 44) {
    if (h.two_step()) {
      sync_seq_ = h.sequence;
      sync_rx_ns_ = rx_ns;
      sync_correction_ns_ = h.correction_ns();
      sync_pending_ = true;
    } else {
      const int64_t t1 = ptp::read_timestamp(p + 34).ns() + h.correction_ns();
      process_sync_pair(t1, rx_ns);
    }
  } else if (h.type == ptp::kDelayReq && is_master_) {
    uint8_t out[54];
    ptp::Header r;
    r.type = ptp::kDelayResp;
    r.length = 54;
    r.domain = opt_.domain;
    r.source = self_;
    r.sequence = h.sequence;
    r.control = 3;
    r.log_interval = 0;
    r.correction = h.correction;  // echo residence/transparent corrections
    ptp::write_header(out, r);
    ptp::write_timestamp(out + 34, ptp::Timestamp::from_ns(cell_.load().ptp_at(rx_ns)));
    std::memcpy(out + 44, h.source.clock.data(), 8);
    out[52] = uint8_t(h.source.port >> 8);
    out[53] = uint8_t(h.source.port);
    general_.send_to(out, sizeof out, {ptp::kPrimaryGroup, ptp::kGeneralPort});
  }
}

void PtpClock::handle_general(const uint8_t* p, size_t n, int64_t rx_ns) {
  ptp::Header h;
  if (!ptp::parse_header(p, n, &h) || h.domain != opt_.domain || h.source == self_) return;
  const int64_t now = mono_ns();
  (void)rx_ns;

  switch (h.type) {
    case ptp::kFollowUp:
      if (!is_master_ && have_master_ && h.source == master_ && sync_pending_ &&
          h.sequence == sync_seq_ && n >= 44) {
        sync_pending_ = false;
        const int64_t t1 =
            ptp::read_timestamp(p + 34).ns() + sync_correction_ns_ + h.correction_ns();
        process_sync_pair(t1, sync_rx_ns_);
      }
      break;

    case ptp::kDelayResp: {
      if (is_master_ || !have_master_ || h.source != master_ || !delay_pending_ || n < 54) break;
      ptp::PortId req;
      std::memcpy(req.clock.data(), p + 44, 8);
      req.port = uint16_t(p[52] << 8 | p[53]);
      if (req != self_ || h.sequence != delay_seq_) break;
      delay_pending_ = false;
      const int64_t t4 = ptp::read_timestamp(p + 34).ns() - h.correction_ns();
      const int64_t d = ((last_t2_ - last_t1_) + (t4 - delay_tx_ns_)) / 2;
      if (d < 0 || d > 10000000) break;  // > 10 ms: not a LAN path, discard
      delays_[delay_count_ % 9] = d;
      ++delay_count_;
      const int k = std::min(delay_count_, 9);
      int64_t sorted[9];
      std::copy(delays_, delays_ + k, sorted);
      std::nth_element(sorted, sorted + k / 2, sorted + k);
      mean_path_delay_ = sorted[k / 2];
      break;
    }

    case ptp::kAnnounce: {
      ptp::Announce a;
      if (!ptp::parse_announce(p, n, &a)) break;
      if (a.steps_removed >= 255) break;
      const bool same = have_master_ && h.source == master_;
      bool better = !have_master_ || same ||
                    ptp::compare_announce(a, h.source, master_announce_, master_) < 0;
      if (is_master_) {
        ptp::Announce mine;
        mine.priority1 = opt_.priority1;
        mine.priority2 = opt_.priority2;
        mine.grandmaster = self_.clock;
        better = ptp::compare_announce(a, h.source, mine, self_) < 0;
        if (better) {
          DSV_LOG_INFO("ptp: better grandmaster %s appeared, leaving master role",
                       ptp::format_clock_id(a.grandmaster).c_str());
          is_master_ = false;
          servo_.reset_to(cell_.load());
        }
      }
      if (!better) break;
      if (!same) {
        DSV_LOG_INFO("ptp: following master %s (grandmaster %s, prio1 %u class %u)",
                     ptp::format_clock_id(h.source.clock).c_str(),
                     ptp::format_clock_id(a.grandmaster).c_str(), a.priority1, a.clock_class);
        master_ = h.source;
        sync_pending_ = false;
        delay_pending_ = false;
        have_pair_ = false;
        delay_count_ = 0;
        mean_path_delay_ = 0;
        next_delay_req_ns_ = now;
      }
      have_master_ = true;
      master_announce_ = a;
      master_seen_ns_ = now;
      std::lock_guard<std::mutex> l(gm_mutex_);
      gm_string_ = ptp::format_clock_id(a.grandmaster);
      break;
    }
    default: break;
  }
}

void PtpClock::process_sync_pair(int64_t t1, int64_t t2) {
  last_t1_ = t1;
  last_t2_ = t2;
  have_pair_ = true;
  if (delay_count_ == 0) return;  // need a path delay before steering
  const auto r = servo_.sample(t2, t1 + mean_path_delay_);
  if (r == PiServo::kOutlier) return;
  if (r == PiServo::kStep)
    DSV_LOG_WARN("ptp: stepped media clock by %.3f ms", double(servo_.last_error_ns()) / 1e6);
  cell_.store(servo_.model());
  offset_ns_ = servo_.last_error_ns();
  state_ = servo_.locked() ? kStatePtpLocked : kStateFreeRun;
}

void PtpClock::send_delay_req() {
  uint8_t out[44];
  ptp::Header h;
  h.type = ptp::kDelayReq;
  h.length = 44;
  h.domain = opt_.domain;
  h.source = self_;
  h.sequence = ++delay_seq_;
  h.control = 1;
  h.log_interval = 0x7F;
  ptp::write_header(out, h);
  std::memset(out + 34, 0, 10);
  event_.send_to(out, sizeof out, {ptp::kPrimaryGroup, ptp::kEventPort});
  delay_tx_ns_ = mono_ns();
  delay_pending_ = true;
}

void PtpClock::select_master(int64_t now) {
  if (have_master_ && now - master_seen_ns_ > 3000000000LL) {
    DSV_LOG_WARN("ptp: lost master %s, holding over", ptp::format_clock_id(master_.clock).c_str());
    have_master_ = false;
    state_ = kStateFreeRun;
    master_seen_ns_ = now;
    std::lock_guard<std::mutex> l(gm_mutex_);
    gm_string_.clear();
  }
  if (!have_master_ && !is_master_ && opt_.master_capable && now - start_ns_ > 4000000000LL &&
      now - master_seen_ns_ > 4000000000LL)
    become_master();
}

void PtpClock::become_master() {
  DSV_LOG_INFO("ptp: no grandmaster found, acting as grandmaster (priority1 %u)", opt_.priority1);
  is_master_ = true;
  state_ = kStatePtpMaster;
  next_sync_ns_ = next_announce_ns_ = mono_ns();
  std::lock_guard<std::mutex> l(gm_mutex_);
  gm_string_ = ptp::format_clock_id(self_.clock);
}

void PtpClock::master_duties(int64_t now) {
  const ClockModel m = cell_.load();
  if (now >= next_announce_ns_) {
    uint8_t out[64];
    ptp::Header h;
    h.domain = opt_.domain;
    h.flags = 0x0008;  // PTP timescale
    h.source = self_;
    h.sequence = m_announce_seq_++;
    h.control = 5;
    h.log_interval = 0;
    ptp::Announce a;
    a.priority1 = opt_.priority1;
    a.priority2 = opt_.priority2;
    a.grandmaster = self_.clock;
    ptp::write_announce(out, h, a);
    general_.send_to(out, sizeof out, {ptp::kPrimaryGroup, ptp::kGeneralPort});
    next_announce_ns_ = now + 1000000000LL;
  }
  if (now >= next_sync_ns_) {
    uint8_t out[44];
    ptp::Header h;
    h.type = ptp::kSync;
    h.length = 44;
    h.domain = opt_.domain;
    h.flags = 0x0200;  // two-step
    h.source = self_;
    h.sequence = m_sync_seq_;
    h.control = 0;
    h.log_interval = -3;  // 125 ms, AES67 default
    ptp::write_header(out, h);
    std::memset(out + 34, 0, 10);
    event_.send_to(out, sizeof out, {ptp::kPrimaryGroup, ptp::kEventPort});
    const int64_t t1 = m.ptp_at(mono_ns());

    h.type = ptp::kFollowUp;
    h.flags = 0;
    h.control = 2;
    ptp::write_header(out, h);
    ptp::write_timestamp(out + 34, ptp::Timestamp::from_ns(t1));
    general_.send_to(out, sizeof out, {ptp::kPrimaryGroup, ptp::kGeneralPort});
    ++m_sync_seq_;
    next_sync_ns_ = now + 125000000LL;
  }
}

}  // namespace dsv
