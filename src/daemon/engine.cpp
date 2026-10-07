#include "dsv/engine.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <random>

#include "dsv/client.h"
#include "dsv/log.h"
#include "dsv/platform.h"
#include "dsv/ptp.h"
#include "dsv/sample_convert.h"

namespace dsv {

Engine::Engine(Config cfg) : cfg_(std::move(cfg)) {}
Engine::~Engine() { stop(); }

bool Engine::start(std::unique_ptr<ClockSource> clock) {
  std::string err;
  if (!validate_config(&cfg_, &err)) {
    DSV_LOG_ERROR("config: %s", err.c_str());
    return false;
  }
  if (!resolve_interface(cfg_.interface, &iface_)) {
    DSV_LOG_ERROR("cannot find network interface '%s'", cfg_.interface.c_str());
    return false;
  }
  if (cfg_.lock_memory && !lock_memory())
    DSV_LOG_WARN("mlockall failed; page faults may cause dropouts (raise RLIMIT_MEMLOCK)");

  // --- shared memory soundcard ---
  const std::string shm_name = cfg_.shm_name.empty() ? default_shm_name() : cfg_.shm_name;
  const size_t bytes = shm_total_bytes(cfg_.ring_frames, cfg_.tx_channels, cfg_.rx_channels);
  if (!shm_.create(shm_name, bytes)) {
    DSV_LOG_ERROR("cannot create shared memory '%s' (%zu bytes); is another dsvd running?",
                  shm_name.c_str(), bytes);
    return false;
  }
  hdr_ = static_cast<ShmHeader*>(shm_.data());
  hdr_->magic = kShmMagic;
  hdr_->version = kShmVersion;
  hdr_->total_bytes = bytes;
  hdr_->sample_rate = cfg_.sample_rate;
  hdr_->tx_channels = cfg_.tx_channels;
  hdr_->rx_channels = cfg_.rx_channels;
  hdr_->ring_frames = cfg_.ring_frames;
  hdr_->period_frames = cfg_.period_frames();
  hdr_->rx_latency_frames = cfg_.rx_latency_frames();
  hdr_->tx_lead_frames = 2 * cfg_.period_frames();
  std::strncpy(hdr_->device_name, cfg_.device_name.c_str(), sizeof hdr_->device_name - 1);
  mix_.assign(size_t(hdr_->period_frames) * std::max(1u, cfg_.tx_channels), 0.f);

  // --- clock ---
  if (clock) {
    clock_ = std::move(clock);
  } else if (cfg_.clock == "ptp") {
    auto p = std::make_unique<PtpClock>();
    PtpClock::Options o;
    o.interface_addr = iface_;
    o.domain = uint8_t(cfg_.ptp_domain);
    o.master_capable = cfg_.ptp_master_capable;
    o.priority1 = uint8_t(cfg_.ptp_priority1);
    if (!p->start(o)) {
      DSV_LOG_WARN("falling back to free-running clock");
      clock_ = std::make_unique<FreeRunClock>();
    } else {
      clock_ = std::move(p);
    }
  } else {
    clock_ = std::make_unique<FreeRunClock>();
  }

  // --- transmit streams ---
  std::random_device rd;
  for (const auto& sc : cfg_.tx) {
    auto t = std::make_unique<TxStream>();
    t->cfg = sc;
    if (!parse_ipv4(sc.address, &t->dst.addr)) {
      DSV_LOG_ERROR("tx '%s': bad address %s", sc.name.c_str(), sc.address.c_str());
      return false;
    }
    t->dst.port = sc.port;
    if (!t->sock.open(0, false)) {
      DSV_LOG_ERROR("tx '%s': socket failed", sc.name.c_str());
      return false;
    }
    t->sock.set_multicast_interface(iface_);
    t->sock.set_multicast_ttl(int(sc.ttl));
    t->sock.set_multicast_loop(true);
    t->sock.set_dscp(kDscpMedia);
    t->rtp.payload_type = sc.payload_type;
    t->rtp.ssrc = rd();
    t->rtp.sequence = uint16_t(rd());
    const uint32_t bps = sc.encoding == "L16" ? 2 : 3;
    t->packet.resize(kRtpHeaderBytes + size_t(hdr_->period_frames) * sc.channels * bps);
    t->sdp = make_sdp(*t);
    DSV_LOG_INFO("tx \"%s\": ch %u-%u -> %s:%u %s/%u ptime %uus", sc.name.c_str(),
                 sc.first_channel, sc.first_channel + sc.channels - 1, sc.address.c_str(), sc.port,
                 sc.encoding.c_str(), cfg_.sample_rate, cfg_.packet_time_us);
    tx_.push_back(std::move(t));
  }

  // --- discovery ---
  if (cfg_.sap) {
    sap_ = std::make_unique<SapService>();
    if (!sap_->start(iface_)) {
      sap_.reset();
    } else {
      std::vector<SdpInfo> ann;
      for (auto& t : tx_)
        if (is_multicast(t->dst.addr)) ann.push_back(t->sdp);
      sap_->set_announcements(ann);
    }
  }

  // --- receive streams ---
  for (const auto& sc : cfg_.rx) {
    auto r = std::make_unique<RxStream>();
    r->cfg = sc;
    if (!sc.source.empty()) parse_ipv4(sc.source, &r->source_addr);
    if (!sc.address.empty() || sc.sap_name.empty()) {
      SdpInfo s;
      s.connection_address = sc.address;
      s.port = sc.port;
      s.payload_type = sc.payload_type;
      s.encoding = sc.encoding;
      s.sample_rate = cfg_.sample_rate;
      s.channels = sc.channels;
      s.ts_offset = sc.ts_offset;
      r->target = s;
      r->have_target = true;
      r->generation = 1;
    }
    rx_.push_back(std::move(r));
  }

  running_ = true;
  hdr_->state.store(clock_->state(), std::memory_order_release);
  tick_thread_ = std::thread([this] { tick_loop(); });
  for (auto& r : rx_) r->thread = std::thread([this, p = r.get()] { rx_loop(p); });
  hk_thread_ = std::thread([this] { housekeeping_loop(); });

  DSV_LOG_INFO("\"%s\" up: %u Hz, %u playback / %u capture ch, packet %u frames, rx latency %u "
               "frames (%.2f ms), ring %u",
               cfg_.device_name.c_str(), cfg_.sample_rate, cfg_.tx_channels, cfg_.rx_channels,
               hdr_->period_frames, hdr_->rx_latency_frames,
               1000.0 * hdr_->rx_latency_frames / cfg_.sample_rate, cfg_.ring_frames);
  return true;
}

void Engine::stop() {
  if (!running_.exchange(false)) {
    shm_.close();
    return;
  }
  if (tick_thread_.joinable()) tick_thread_.join();
  for (auto& r : rx_)
    if (r->thread.joinable()) r->thread.join();
  if (hk_thread_.joinable()) hk_thread_.join();
  if (sap_) sap_->stop();
  if (hdr_) hdr_->state.store(kStateStopped, std::memory_order_release);
  clock_.reset();
  tx_.clear();
  rx_.clear();
  hdr_ = nullptr;
  shm_.close();
}

SdpInfo Engine::make_sdp(const TxStream& t) const {
  SdpInfo s;
  s.session_name = t.cfg.name;
  s.session_info = "Channels " + std::to_string(t.cfg.first_channel) + "-" +
                   std::to_string(t.cfg.first_channel + t.cfg.channels - 1);
  s.session_id = t.rtp.ssrc;
  s.session_version = 1;
  s.origin_address = ipv4_to_string(iface_);
  s.connection_address = t.cfg.address;
  s.ttl = t.cfg.ttl;
  s.port = t.cfg.port;
  s.payload_type = t.cfg.payload_type;
  s.encoding = t.cfg.encoding;
  s.sample_rate = cfg_.sample_rate;
  s.channels = t.cfg.channels;
  s.ptime_us = cfg_.packet_time_us;
  s.ts_offset = 0;
  s.ptp_domain = int(cfg_.ptp_domain);
  s.ptp_grandmaster = clock_ ? clock_->grandmaster() : std::string();
  return s;
}

void Engine::clear_rings() {
  for (uint32_t i = 0; i < kMaxTxClients; ++i)
    std::memset(tx_ring(hdr_, i), 0, tx_ring_floats(hdr_) * sizeof(float));
  std::memset(rx_ring(hdr_), 0, rx_ring_floats(hdr_) * sizeof(float));
}

// ---------------------------------------------------------------------------
// The real-time tick. Runs once per packet time on media-clock boundaries:
//   1. publish the clock anchor drivers use to time their own callbacks
//   2. mix [T-P, T) from every client slot, packetize, send
//   3. zero the receive ring far ahead so lost packets play as silence
void Engine::tick_loop() {
  const uint32_t P = hdr_->period_frames;
  const uint32_t rate = cfg_.sample_rate;
  const int64_t period_ns = int64_t(P) * 1000000000LL / rate;
  if (!set_realtime_priority(cfg_.rt_priority, period_ns))
    DSV_LOG_WARN("could not get real-time scheduling; expect higher jitter (see README)");

  const uint32_t ring = hdr_->ring_frames;
  const uint32_t rxch = hdr_->rx_channels;
  float* rx = rx_ring(hdr_);
  const int64_t spin = int64_t(cfg_.spin_us) * 1000;
  const int64_t resync_ns = std::max<int64_t>(50000000, 4 * period_ns);

  ClockModel m = clock_->model();
  uint64_t next = (ptp_ns_to_frames(m.ptp_at(mono_ns()), rate) / P + 1) * P;
  int64_t last_reclaim = 0;

  while (running_) {
    m = clock_->model();
    const int64_t deadline = m.local_at(frames_to_ptp_ns(next, rate));
    const int64_t now = mono_ns();
    if (deadline - now > resync_ns || now - deadline > resync_ns) {
      // Clock stepped (PTP lock, master change) or we were descheduled for a
      // long time: realign instead of bursting or stalling.
      hdr_->clock_steps.fetch_add(1, std::memory_order_relaxed);
      next = (ptp_ns_to_frames(m.ptp_at(now), rate) / P + 1) * P;
      clear_rings();
      continue;
    }
    sleep_until_ns(deadline, spin);
    if (mono_ns() - deadline > period_ns / 2)
      hdr_->late_ticks.fetch_add(1, std::memory_order_relaxed);

    ClockAnchor a;
    a.host_ns = deadline;
    a.frame = next;
    a.frames_per_ns = double(rate) * 1e-9 * m.ratio;
    publish_anchor(hdr_, a);
    hdr_->now_frames.store(next, std::memory_order_release);
    hdr_->heartbeat_ns.store(mono_ns(), std::memory_order_release);
    hdr_->state.store(clock_->state(), std::memory_order_relaxed);
    hdr_->ptp_offset_ns.store(clock_->last_offset_ns(), std::memory_order_relaxed);

    mix_and_send(next - P);

    // Silence the slot half a ring ahead (== half a ring behind).
    const uint64_t z = next + ring / 2 - P;
    for (uint32_t f = 0; f < P; ++f)
      std::memset(rx + size_t((z + f) & (ring - 1)) * rxch, 0, sizeof(float) * rxch);

    if (now - last_reclaim > 1000000000LL) {
      reclaim_clients(now);
      last_reclaim = now;
    }
    next += P;
  }
}

void Engine::mix_and_send(uint64_t start) {
  const uint32_t P = hdr_->period_frames;
  const uint32_t ch = hdr_->tx_channels;
  const uint32_t mask = hdr_->ring_frames - 1;
  if (ch == 0) return;
  std::fill(mix_.begin(), mix_.end(), 0.f);

  for (uint32_t i = 0; i < kMaxTxClients; ++i) {
    ClientSlot& s = hdr_->clients[i];
    if (s.pid.load(std::memory_order_acquire) == 0) continue;
    float* ring = tx_ring(hdr_, i);
    const bool active = s.active.load(std::memory_order_acquire) != 0;
    for (uint32_t f = 0; f < P; ++f) {
      float* src = ring + size_t((start + f) & mask) * ch;
      if (active) {
        float* dst = mix_.data() + size_t(f) * ch;
        for (uint32_t c = 0; c < ch; ++c) dst[c] += src[c];
      }
      std::memset(src, 0, sizeof(float) * ch);  // consumed
    }
  }

  for (auto& tp : tx_) {
    TxStream& t = *tp;
    const uint32_t first = t.cfg.first_channel - 1;
    const uint32_t n = t.cfg.channels;
    const bool l16 = t.cfg.encoding == "L16";
    t.rtp.timestamp = uint32_t(start);
    uint8_t* p = t.packet.data() + write_rtp_header(t.packet.data(), t.rtp);
    for (uint32_t f = 0; f < P; ++f) {
      const float* src = mix_.data() + size_t(f) * ch + first;
      if (l16) {
        for (uint32_t c = 0; c < n; ++c, p += 2) float_to_l16(src[c], p);
      } else {
        for (uint32_t c = 0; c < n; ++c, p += 3) float_to_l24(src[c], p);
      }
    }
    t.sock.send_to(t.packet.data(), t.packet.size(), t.dst);
    ++t.rtp.sequence;
    hdr_->tx_packets.fetch_add(1, std::memory_order_relaxed);
  }
}

void Engine::reclaim_clients(int64_t now) {
  for (uint32_t i = 0; i < kMaxTxClients; ++i) {
    ClientSlot& s = hdr_->clients[i];
    const uint32_t pid = s.pid.load(std::memory_order_acquire);
    if (pid == 0) continue;
    if (!process_alive(pid)) {
      DSV_LOG_INFO("client slot %u (%s, pid %u) died; releasing", i, s.name, pid);
      s.active.store(0, std::memory_order_release);
      s.pid.store(0, std::memory_order_release);
    } else if (now - s.heartbeat_ns.load(std::memory_order_relaxed) > 5000000000LL) {
      s.active.store(0, std::memory_order_release);
    }
  }
}

// ---------------------------------------------------------------------------
// One thread per receive stream. Packets are written straight into the
// capture ring at the slot of their own RTP timestamp, so the ring *is* the
// jitter buffer and reordering costs nothing.
void Engine::rx_loop(RxStream* s) {
  set_realtime_priority(std::max(1, cfg_.rt_priority - 5), 1000000);
  UdpSocket sock;
  uint32_t applied_gen = 0;
  SdpInfo t;
  uint32_t group = 0;
  bool have_seq = false;
  uint16_t expect_seq = 0;
  uint8_t buf[2048];

  const uint32_t ring = hdr_->ring_frames;
  const uint64_t mask = ring - 1;
  const uint32_t rxch = hdr_->rx_channels;
  float* rx = rx_ring(hdr_);
  const uint32_t first = s->cfg.first_channel - 1;

  while (running_) {
    const uint32_t gen = s->generation.load(std::memory_order_acquire);
    if (gen != applied_gen) {
      {
        std::lock_guard<std::mutex> l(s->mutex);
        t = s->target;
      }
      applied_gen = gen;
      if (group) sock.leave_multicast(group, iface_);
      sock.close();
      group = 0;
      have_seq = false;
      if (!sock.open(t.port)) {
        DSV_LOG_ERROR("rx: cannot bind port %u", t.port);
        continue;
      }
      sock.set_recv_timeout_ms(100);
      sock.set_buffer_sizes(1 << 20);
      uint32_t addr = 0;
      if (parse_ipv4(t.connection_address, &addr) && is_multicast(addr)) {
        if (!sock.join_multicast(addr, iface_))
          DSV_LOG_ERROR("rx: IGMP join %s failed", t.connection_address.c_str());
        group = addr;
      }
      DSV_LOG_INFO("rx -> ch %u-%u: %s:%u %s/%u/%u offset %u", first + 1,
                   first + std::min(t.channels, rxch - first), t.connection_address.c_str(),
                   t.port, t.encoding.c_str(), t.sample_rate, t.channels, t.ts_offset);
      if (t.sample_rate != cfg_.sample_rate)
        DSV_LOG_WARN("rx: stream is %u Hz but device runs %u Hz; stream ignored", t.sample_rate,
                     cfg_.sample_rate);
    }
    if (!sock.is_open()) {
      sleep_until_ns(mono_ns() + 100000000LL);
      continue;
    }

    Endpoint from;
    const int n = sock.recv_from(buf, sizeof buf, &from);
    if (n <= 0) continue;
    if (s->source_addr && from.addr != s->source_addr) continue;
    if (t.sample_rate != cfg_.sample_rate) continue;

    RtpHeader h;
    const uint8_t* payload;
    size_t plen;
    if (!parse_rtp(buf, size_t(n), &h, &payload, &plen) || h.payload_type != t.payload_type)
      continue;
    const uint32_t bps = t.bytes_per_sample();
    const uint32_t frame_bytes = bps * t.channels;
    const uint32_t frames = uint32_t(plen / frame_bytes);
    if (frames == 0) continue;

    hdr_->rx_packets.fetch_add(1, std::memory_order_relaxed);
    if (have_seq && h.sequence != expect_seq) {
      const uint16_t gap = uint16_t(h.sequence - expect_seq);
      if (gap < 0x8000) hdr_->rx_lost.fetch_add(gap, std::memory_order_relaxed);
    }
    have_seq = true;
    expect_seq = uint16_t(h.sequence + 1);

    const uint64_t now = hdr_->now_frames.load(std::memory_order_acquire);
    const uint64_t start = unwrap_timestamp(h.timestamp - t.ts_offset, now);
    const int64_t rel = int64_t(start - now);
    // Older than the playout point, or implausibly far ahead (sender clock
    // not locked to ours): drop instead of scribbling over live audio.
    if (rel + int64_t(frames) < -int64_t(hdr_->rx_latency_frames) || rel > int64_t(ring / 4)) {
      hdr_->rx_late.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    const uint32_t nch = first < rxch ? std::min(t.channels, rxch - first) : 0;
    for (uint32_t f = 0; f < frames; ++f) {
      float* dst = rx + size_t((start + f) & mask) * rxch + first;
      const uint8_t* src = payload + size_t(f) * frame_bytes;
      if (bps == 3) {
        for (uint32_t c = 0; c < nch; ++c) dst[c] = l24_to_float(src + c * 3);
      } else {
        for (uint32_t c = 0; c < nch; ++c) dst[c] = l16_to_float(src + c * 2);
      }
    }
  }
  if (group) sock.leave_multicast(group, iface_);
}

// ---------------------------------------------------------------------------
void Engine::housekeeping_loop() {
  int64_t next_status = mono_ns() + 10000000000LL;
  while (running_) {
    sleep_until_ns(mono_ns() + 250000000LL);
    // Follow SAP-announced sessions (e.g. a Dante device's AES67 flow).
    if (sap_) {
      for (auto& r : rx_) {
        if (r->cfg.sap_name.empty()) continue;
        SdpInfo s;
        if (!sap_->find(r->cfg.sap_name, &s)) continue;
        std::lock_guard<std::mutex> l(r->mutex);
        const bool changed = !r->have_target ||
                             s.connection_address != r->target.connection_address ||
                             s.port != r->target.port || s.ts_offset != r->target.ts_offset ||
                             s.channels != r->target.channels ||
                             s.payload_type != r->target.payload_type;
        if (changed) {
          r->target = s;
          r->have_target = true;
          r->generation.fetch_add(1, std::memory_order_release);
        }
      }
      // Refresh ts-refclk in our announcements when the grandmaster changes.
      const std::string gm = clock_->grandmaster();
      if (gm != last_gm_) {
        last_gm_ = gm;
        std::vector<SdpInfo> ann;
        for (auto& t : tx_) {
          t->sdp = make_sdp(*t);
          if (is_multicast(t->dst.addr)) ann.push_back(t->sdp);
        }
        sap_->set_announcements(ann);
      }
    }
    if (g_log_level >= kLogInfo && mono_ns() >= next_status) {
      DSV_LOG_INFO("%s", status_line().c_str());
      next_status = mono_ns() + 10000000000LL;
    }
  }
}

std::string Engine::status_line() const {
  if (!hdr_) return "stopped";
  static const char* states[] = {"stopped", "free-run", "ptp-locked", "ptp-master"};
  const uint32_t st = hdr_->state.load();
  uint32_t clients = 0;
  for (const auto& c : hdr_->clients)
    if (c.pid.load() && c.active.load()) ++clients;
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "clock %s offset %+.1f us | tx %" PRIu64 " pkts | rx %" PRIu64 " pkts, %" PRIu64
                " lost, %" PRIu64 " late | late ticks %" PRIu64 " | clients %u",
                st < 4 ? states[st] : "?", double(hdr_->ptp_offset_ns.load()) / 1000.0,
                hdr_->tx_packets.load(), hdr_->rx_packets.load(), hdr_->rx_lost.load(),
                hdr_->rx_late.load(), hdr_->late_ticks.load(), clients);
  return buf;
}

}  // namespace dsv
