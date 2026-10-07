#include "virgil/ptp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "virgil/log.h"
#include "virgil/platform.h"

namespace virgil {

FreeRunClock::FreeRunClock() {
  m_.base_local = mono_ns();
  m_.base_ptp = realtime_ns() + kTaiUtcOffsetNs;
  m_.ratio = 1.0;
}

namespace ptp1 {

static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
static void wr16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
static uint32_t rd32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
static void wr32(uint8_t* p, uint32_t v) {
  wr16(p, uint16_t(v >> 16));
  wr16(p + 2, uint16_t(v));
}

bool parse_header(const uint8_t* p, size_t len, Header* h) {
  if (len < kHeaderBytes) return false;
  if (rd16(p) != 1) return false;  // versionPTP 1 (v2 packets start with 0x?2)
  std::memcpy(h->subdomain, p + 4, 16);
  h->message_type = p[20];
  std::memcpy(h->source.uuid.data(), p + 22, 6);
  h->source.port = rd16(p + 28);
  h->sequence = rd16(p + 30);
  h->control = p[32];
  h->flags = p[35];
  return true;
}

void write_header(uint8_t* p, const Header& h) {
  std::memset(p, 0, kHeaderBytes);
  wr16(p, 1);      // versionPTP
  wr16(p + 2, 1);  // versionNetwork
  std::memcpy(p + 4, h.subdomain, 16);
  p[20] = h.message_type;
  p[21] = 1;  // sourceCommunicationTechnology: Ethernet
  std::memcpy(p + 22, h.source.uuid.data(), 6);
  wr16(p + 28, h.source.port);
  wr16(p + 30, h.sequence);
  p[32] = h.control;
  p[35] = h.flags;
}

Timestamp read_timestamp(const uint8_t* p) { return {rd32(p), rd32(p + 4)}; }
void write_timestamp(uint8_t* p, const Timestamp& t) {
  wr32(p, t.seconds);
  wr32(p + 4, t.nanoseconds);
}

bool parse_sync(const uint8_t* p, size_t len, Timestamp* origin, ClockProps* c) {
  if (len < kSyncBytes) return false;
  *origin = read_timestamp(p + 40);
  const uint8_t* g = p + 52;  // grandmaster properties
  std::memcpy(c->gm_uuid.data(), g + 2, 6);
  c->gm_port = rd16(g + 8);
  c->gm_sequence = rd16(g + 10);
  c->stratum = g[15];
  std::memcpy(c->identifier, g + 16, 4);
  c->variance = int16_t(rd16(g + 22));
  c->preferred = g[25] != 0;
  c->boundary_clock = g[27] != 0;
  c->sync_interval = int8_t(p[83]);
  return true;
}

size_t write_sync(uint8_t* p, const Header& h, const Timestamp& origin, const ClockProps& c,
                  const PortId& parent) {
  write_header(p, h);
  std::memset(p + kHeaderBytes, 0, kSyncBytes - kHeaderBytes);
  write_timestamp(p + 40, origin);
  uint8_t* g = p + 52;
  g[1] = 1;  // communication technology
  std::memcpy(g + 2, c.gm_uuid.data(), 6);
  wr16(g + 8, c.gm_port);
  wr16(g + 10, c.gm_sequence);
  g[15] = c.stratum;
  std::memcpy(g + 16, c.identifier, 4);
  wr16(g + 22, uint16_t(c.variance));
  g[25] = c.preferred;
  g[27] = c.boundary_clock;
  p[83] = uint8_t(c.sync_interval);
  wr16(p + 86, uint16_t(c.variance));  // localClockVariance
  p[95] = c.stratum;                   // localClockStratum
  std::memcpy(p + 96, c.identifier, 4);
  p[101] = 1;  // parentCommunicationTechnology
  std::memcpy(p + 102, parent.uuid.data(), 6);
  wr16(p + 110, parent.port);
  return kSyncBytes;
}

bool parse_follow_up(const uint8_t* p, size_t len, uint16_t* seq, Timestamp* t) {
  if (len < kFollowUpBytes) return false;
  *seq = rd16(p + 42);
  *t = read_timestamp(p + 44);
  return true;
}

size_t write_follow_up(uint8_t* p, const Header& h, uint16_t seq, const Timestamp& t) {
  write_header(p, h);
  std::memset(p + kHeaderBytes, 0, kFollowUpBytes - kHeaderBytes);
  wr16(p + 42, seq);
  write_timestamp(p + 44, t);
  return kFollowUpBytes;
}

bool parse_delay_resp(const uint8_t* p, size_t len, Timestamp* receipt, PortId* req,
                      uint16_t* req_seq) {
  if (len < kDelayRespBytes) return false;
  *receipt = read_timestamp(p + 40);
  std::memcpy(req->uuid.data(), p + 50, 6);
  req->port = rd16(p + 56);
  *req_seq = rd16(p + 58);
  return true;
}

size_t write_delay_resp(uint8_t* p, const Header& h, const Timestamp& receipt, const PortId& req,
                        uint16_t req_seq) {
  write_header(p, h);
  std::memset(p + kHeaderBytes, 0, kDelayRespBytes - kHeaderBytes);
  write_timestamp(p + 40, receipt);
  p[49] = 1;
  std::memcpy(p + 50, req.uuid.data(), 6);
  wr16(p + 56, req.port);
  wr16(p + 58, req_seq);
  return kDelayRespBytes;
}

int compare_masters(const ClockProps& a, const PortId& ap, const ClockProps& b, const PortId& bp) {
  if (a.preferred != b.preferred) return a.preferred ? -1 : 1;
  if (a.stratum != b.stratum) return a.stratum < b.stratum ? -1 : 1;
  if (a.variance != b.variance) return a.variance < b.variance ? -1 : 1;
  if (ap.uuid != bp.uuid) return ap.uuid < bp.uuid ? -1 : 1;
  if (ap.port != bp.port) return ap.port < bp.port ? -1 : 1;
  return 0;
}

std::string format_uuid(const Uuid& u) {
  char b[24];
  std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", u[0], u[1], u[2], u[3], u[4], u[5]);
  return b;
}

}  // namespace ptp1

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
  if (first_local_ == 0 && std::llabs(err) <= step_threshold_ns) {
    // First sample after reset_to: start the frequency measurement here.
    last_local_ = first_local_ = local;
    first_master_ = master;
    return kTracking;
  }
  const double dt = double(local - last_local_) * 1e-9;
  if (dt <= 0 && first_local_ != 0) return kOutlier;

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
  const double adj =
      std::clamp(drift_ppb_ - kp * double(err) / dt, -1e6, 1e6);

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

// ---- PtpClock (PTPv1) -------------------------------------------------------

bool PtpClock::start(const Options& o) {
  stop();
  opt_ = o;
  if (!event_.open(ptp1::kEventPort) || !general_.open(ptp1::kGeneralPort)) {
    VIRGIL_LOG_ERROR("ptp: cannot bind UDP 319/320 (needs administrator/root, or another PTP "
                  "program uses them)");
    event_.close();
    general_.close();
    return false;
  }
  for (UdpSocket* s : {&event_, &general_}) {
    s->join_multicast(ptp1::kDefaultGroup, o.interface_addr);
    s->set_multicast_interface(o.interface_addr);
    s->set_multicast_ttl(1);
    s->set_multicast_loop(true);  // several instances may share a host
    s->set_dscp(kDscpPtp);
    s->enable_rx_timestamps();
  }

  // 6-byte UUID: random, locally administered (like a MAC address).
  std::random_device rd;
  std::mt19937_64 rng((uint64_t(rd()) << 32) ^ rd() ^ uint64_t(mono_ns()) ^ o.interface_addr);
  const uint64_t r = rng();
  for (int i = 0; i < 6; ++i) self_.uuid[i] = uint8_t(r >> (i * 8));
  self_.uuid[0] = uint8_t((self_.uuid[0] & 0xFC) | 0x02);
  self_.port = 1;
  proto_ = ptp1::Header{};
  std::memset(proto_.subdomain, 0, sizeof proto_.subdomain);
  std::memcpy(proto_.subdomain, o.subdomain.data(), std::min<size_t>(15, o.subdomain.size()));
  proto_.source = self_;

  ClockModel initial;
  initial.base_local = mono_ns();
  initial.base_ptp = realtime_ns() + kTaiUtcOffsetNs;
  servo_.reset_to(initial);
  publish(initial);
  state_ = kStateFreeRun;
  start_ns_ = mono_ns();
  master_seen_ns_ = start_ns_;
  running_ = true;
  thread_ = std::thread([this] { run(); });
  VIRGIL_LOG_INFO("ptp: PTPv1 on %s subdomain %s, clock id %s%s",
               ipv4_to_string(o.interface_addr).c_str(), o.subdomain.c_str(),
               ptp1::format_uuid(self_.uuid).c_str(),
               o.master_capable ? " (may act as master)" : "");
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

void PtpClock::publish(const ClockModel& m) {
  cell_.store(m);
  if (listener_) listener_(m, listener_ctx_);
}

void PtpClock::run() {
  uint8_t buf[1500];
  while (running_) {
    const int ready = UdpSocket::wait_readable(event_, &general_, 20);
    if (ready & 1) {
      Endpoint from;
      int64_t rx = 0;
      const int n = event_.recv_from(buf, sizeof buf, &from, &rx);
      if (n > 0) handle_event(buf, size_t(n), rx);
    }
    if (ready & 2) {
      Endpoint from;
      const int n = general_.recv_from(buf, sizeof buf, &from);
      if (n > 0) handle_general(buf, size_t(n));
    }
    const int64_t now = mono_ns();

    if (have_master_ && !is_master_ && now - master_seen_ns_ > 4000000000LL) {
      VIRGIL_LOG_WARN("ptp: lost master %s, holding over", ptp1::format_uuid(master_.uuid).c_str());
      have_master_ = false;
      state_ = kStateFreeRun;
      master_seen_ns_ = now;
      std::lock_guard<std::mutex> l(gm_mutex_);
      gm_string_.clear();
    }
    if (!have_master_ && !is_master_ && opt_.master_capable && now - master_seen_ns_ > 4000000000LL) {
      VIRGIL_LOG_INFO("ptp: no master found, acting as PTPv1 master (stratum %u)", opt_.stratum);
      is_master_ = true;
      state_ = kStatePtpMaster;
      next_sync_ns_ = now;
      std::lock_guard<std::mutex> l(gm_mutex_);
      gm_string_ = ptp1::format_uuid(self_.uuid);
    }
    if (is_master_) {
      master_duties(now);
    } else if (have_master_ && have_pair_ && now >= next_delay_req_ns_) {
      send_delay_req();
      next_delay_req_ns_ = now + 750000000LL + (int64_t(std::rand()) % 500) * 1000000LL;
    }
  }
}

static bool same_subdomain(const ptp1::Header& h, const ptp1::Header& mine) {
  return std::strncmp(h.subdomain, mine.subdomain, 16) == 0;
}

void PtpClock::consider_master(const ptp1::PortId& src, const ptp1::ClockProps& props,
                               int64_t now) {
  if (is_master_) {
    ptp1::ClockProps mine;
    mine.gm_uuid = self_.uuid;
    mine.stratum = opt_.stratum;
    if (ptp1::compare_masters(props, src, mine, self_) >= 0) return;
    VIRGIL_LOG_INFO("ptp: better master %s appeared, leaving master role",
                 ptp1::format_uuid(src.uuid).c_str());
    is_master_ = false;
    servo_.reset_to(cell_.load());
  }
  const bool same = have_master_ && src == master_;
  if (!same && have_master_ && now - master_seen_ns_ < 4000000000LL &&
      ptp1::compare_masters(props, src, master_props_, master_) >= 0)
    return;  // keep the current, better master
  if (!same) {
    VIRGIL_LOG_INFO("ptp: following master %s (stratum %u%s)", ptp1::format_uuid(src.uuid).c_str(),
                 props.stratum, props.preferred ? ", preferred" : "");
    master_ = src;
    sync_pending_ = false;
    delay_pending_ = false;
    have_pair_ = false;
    delay_count_ = 0;
    mean_path_delay_ = 0;
    next_delay_req_ns_ = now;
  }
  have_master_ = true;
  master_props_ = props;
  master_seen_ns_ = now;
  std::lock_guard<std::mutex> l(gm_mutex_);
  gm_string_ = ptp1::format_uuid(props.gm_uuid);
}

void PtpClock::handle_event(const uint8_t* p, size_t n, int64_t rx_ns) {
  ptp1::Header h;
  if (!ptp1::parse_header(p, n, &h) || !same_subdomain(h, proto_) || h.source == self_) return;

  if (h.control == ptp1::kSync) {
    ptp1::Timestamp origin;
    ptp1::ClockProps props;
    if (!ptp1::parse_sync(p, n, &origin, &props)) return;
    consider_master(h.source, props, mono_ns());
    if (is_master_ || !have_master_ || h.source != master_) return;
    if (h.flags & ptp1::kFlagAssist) {
      sync_seq_ = h.sequence;
      sync_rx_ns_ = rx_ns;
      sync_pending_ = true;
    } else {
      process_sync_pair(origin.ns(), rx_ns);
    }
  } else if (h.control == ptp1::kDelayReq && is_master_) {
    uint8_t out[ptp1::kDelayRespBytes];
    ptp1::Header r = proto_;
    r.message_type = ptp1::kGeneralMessage;
    r.control = ptp1::kDelayResp;
    r.sequence = h.sequence;
    const size_t len = ptp1::write_delay_resp(
        out, r, ptp1::Timestamp::from_ns(cell_.load().ptp_at(rx_ns)), h.source, h.sequence);
    general_.send_to(out, len, {ptp1::kDefaultGroup, ptp1::kGeneralPort});
  }
}

void PtpClock::handle_general(const uint8_t* p, size_t n) {
  ptp1::Header h;
  if (!ptp1::parse_header(p, n, &h) || !same_subdomain(h, proto_) || h.source == self_) return;
  if (is_master_ || !have_master_ || h.source != master_) return;

  if (h.control == ptp1::kFollowUp) {
    uint16_t seq;
    ptp1::Timestamp t;
    if (!ptp1::parse_follow_up(p, n, &seq, &t)) return;
    if (sync_pending_ && seq == sync_seq_) {
      sync_pending_ = false;
      process_sync_pair(t.ns(), sync_rx_ns_);
    }
  } else if (h.control == ptp1::kDelayResp) {
    ptp1::Timestamp receipt;
    ptp1::PortId req;
    uint16_t req_seq;
    if (!ptp1::parse_delay_resp(p, n, &receipt, &req, &req_seq)) return;
    if (req != self_ || req_seq != delay_seq_ || !delay_pending_) return;
    delay_pending_ = false;
    const int64_t t4 = receipt.ns();
    int64_t d = ((last_t2_ - last_t1_) + (t4 - delay_tx_ns_)) / 2;
    // Software send timestamps are taken just after sendto(), so on fast
    // links the estimate can dip slightly below zero: clamp, don't discard.
    if (d < -1000000 || d > 10000000) return;  // not a LAN path
    if (d < 0) d = 0;
    delays_[delay_count_ % 9] = d;
    ++delay_count_;
    const int k = std::min(delay_count_, 9);
    int64_t sorted[9];
    std::copy(delays_, delays_ + k, sorted);
    std::nth_element(sorted, sorted + k / 2, sorted + k);
    mean_path_delay_ = sorted[k / 2];
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
    VIRGIL_LOG_WARN("ptp: stepped clock by %.3f ms", double(servo_.last_error_ns()) / 1e6);
  publish(servo_.model());
  VIRGIL_LOG_DEBUG("ptp: offset %lld ns, delay %lld ns, ratio %.9f", (long long)servo_.last_error_ns(),
                   (long long)mean_path_delay_, servo_.model().ratio);
  offset_ns_ = servo_.last_error_ns();
  state_ = servo_.locked() ? kStatePtpLocked : kStateFreeRun;
}

void PtpClock::send_delay_req() {
  // Dante sends Delay_Req as a full Sync-sized message echoing the master's
  // grandmaster properties (stratum 255, identifier DFLT, sync interval 0x7f).
  uint8_t out[ptp1::kSyncBytes];
  ptp1::Header h = proto_;
  h.message_type = ptp1::kEventMessage;
  h.control = ptp1::kDelayReq;
  h.sequence = ++delay_seq_;
  ptp1::ClockProps props = master_props_;
  props.stratum = 255;
  std::memcpy(props.identifier, "DFLT", 4);
  props.sync_interval = 0x7f;
  ptp1::write_sync(out, h, ptp1::Timestamp{}, props, master_);
  out[95] = 255;  // local stratum: slave only
  event_.send_to(out, sizeof out, {ptp1::kDefaultGroup, ptp1::kEventPort});
  delay_tx_ns_ = mono_ns();
  delay_pending_ = true;
}

void PtpClock::master_duties(int64_t now) {
  if (now < next_sync_ns_) return;
  const ClockModel m = cell_.load();
  ptp1::ClockProps props;
  props.gm_uuid = self_.uuid;
  props.gm_port = self_.port;
  props.gm_sequence = m_sync_seq_;
  props.stratum = opt_.stratum;
  props.sync_interval = -2;  // 250 ms, as Dante masters
  uint8_t out[ptp1::kSyncBytes];
  ptp1::Header h = proto_;
  h.message_type = ptp1::kEventMessage;
  h.control = ptp1::kSync;
  h.sequence = m_sync_seq_;
  h.flags = ptp1::kFlagAssist;  // two-step
  ptp1::write_sync(out, h, ptp1::Timestamp{}, props, self_);
  event_.send_to(out, sizeof out, {ptp1::kDefaultGroup, ptp1::kEventPort});
  const int64_t t1 = m.ptp_at(mono_ns());

  uint8_t fu[ptp1::kFollowUpBytes];
  ptp1::Header fh = proto_;
  fh.message_type = ptp1::kGeneralMessage;
  fh.control = ptp1::kFollowUp;
  fh.sequence = m_sync_seq_;
  ptp1::write_follow_up(fu, fh, m_sync_seq_, ptp1::Timestamp::from_ns(t1));
  general_.send_to(fu, sizeof fu, {ptp1::kDefaultGroup, ptp1::kGeneralPort});
  ++m_sync_seq_;
  next_sync_ns_ = now + 250000000LL;
}

}  // namespace virgil
