#include "virgil/engine.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "virgil/client.h"
#include "virgil/dante_bridge.h"
#include "virgil/log.h"
#include "virgil/net.h"
#include "virgil/platform.h"
#include "virgil/ptp.h"
#include "virgil/sample_convert.h"

namespace virgil {

namespace {
// Lock-free running maximum of a non-negative float stored as its bits
// (IEEE-754 ordering of non-negative floats matches unsigned ordering).
inline void peak_max(std::atomic<uint32_t>& a, float v) {
  uint32_t b;
  std::memcpy(&b, &v, sizeof b);
  uint32_t cur = a.load(std::memory_order_relaxed);
  while (b > cur && !a.compare_exchange_weak(cur, b, std::memory_order_relaxed)) {
  }
}

extern "C" int64_t bridge_mono_ns() { return mono_ns(); }

extern "C" void bridge_log(int level, const char* msg) {
  log_message(level < 0 ? 0 : level > 3 ? 3 : level, "%s", msg);
}
}  // namespace

Engine::Engine(Config cfg) : cfg_(std::move(cfg)) {}
Engine::~Engine() { stop(); }

void Engine::on_clock(const ClockModel& m, void*) {
  // Inferno's overlay: ptp = t + shift + (t - last_sync) * freq_scale.
  vg_dante_set_clock(m.base_local, m.base_ptp - m.base_local, m.ratio - 1.0);
}

bool Engine::start(std::unique_ptr<ClockSource> clock) {
  std::string err;
  if (!validate_config(&cfg_, &err)) {
    VIRGIL_LOG_ERROR("config: %s", err.c_str());
    return false;
  }
  if (!resolve_interface(cfg_.interface, &iface_)) {
    VIRGIL_LOG_ERROR("cannot find network interface '%s'", cfg_.interface.c_str());
    return false;
  }
  if (cfg_.lock_memory && !lock_memory())
    VIRGIL_LOG_WARN("mlockall failed; page faults may cause dropouts (raise RLIMIT_MEMLOCK)");

  // --- shared memory soundcard ---
  std::string shm_name = cfg_.shm_name.empty() ? default_shm_name() : cfg_.shm_name;
  const size_t bytes = shm_total_bytes(cfg_.ring_frames, cfg_.tx_channels, cfg_.rx_channels);
  SharedMemory& shm_ = shm();
  bool reused = false;
  if (shm_ext_ && shm_.is_open() && shm_.size() >= bytes) {
    const auto* old = static_cast<const ShmHeader*>(shm_.data());
    bool gen_name = false;
    for (int gen = 0; gen < kShmGenerations; ++gen) gen_name |= shm_.name() == shm_generation_name(shm_name, gen);
    const bool same_name = gen_name
#if defined(_WIN32)
                           || (cfg_.shm_name.empty() && shm_.name() == kFallbackShmName)
#endif
        ;
    reused = same_name && old->magic == kShmMagic && old->version == kShmVersion &&
             old->total_bytes == bytes && old->sample_rate == cfg_.sample_rate &&
             old->tx_channels == cfg_.tx_channels && old->rx_channels == cfg_.rx_channels &&
             old->ring_frames == cfg_.ring_frames && old->period_frames == cfg_.period_frames() &&
             old->tx_lead_frames == cfg_.tx_lead_frames() && old->rx_latency_frames == cfg_.rx_latency_frames();
  }
  bool created = reused;
  if (reused) {
    VIRGIL_LOG_INFO("reusing soundcard '%s'; connected apps stay attached", shm_.name().c_str());
  } else {
    if (shm_.is_open()) {
      // Layout changed: drivers holding the old segment see it stopped and
      // reconnect to the new one.
      static_cast<ShmHeader*>(shm_.data())->state.store(kStateStopped);
      shm_.close();
    }
    // Another live virgild already serves one of our names: do not touch it
    // (on POSIX, re-creating would unlink the soundcard under its apps).
    for (int gen = 0; gen < kShmGenerations; ++gen) {
      SharedMemory probe;
      if (!probe.open(shm_generation_name(shm_name, gen)) || probe.size() < kShmHeaderBytes) continue;
      const auto* h = static_cast<const ShmHeader*>(probe.data());
      if (h->magic == kShmMagic && h->state.load() != kStateStopped &&
          mono_ns() - h->heartbeat_ns.load() < 1000000000LL) {
        VIRGIL_LOG_ERROR("another virgild is running (soundcard '%s' is live); not starting a second one",
                         shm_generation_name(shm_name, gen).c_str());
        return false;
      }
    }
    for (int gen = 0; gen < kShmGenerations && !created; ++gen) {
      created = shm_.create(shm_generation_name(shm_name, gen), bytes);
      if (created && gen > 0)
        VIRGIL_LOG_INFO("previous soundcard still open in apps; using '%s' until they reconnect",
                        shm_.name().c_str());
    }
    if (created) shm_name = shm_.name();
  }
#if defined(_WIN32)
  if (!created && cfg_.shm_name.empty() && !std::getenv("VIRGIL_SHM_NAME")) {
    // Global\ needs SeCreateGlobalPrivilege (services, elevated consoles).
    VIRGIL_LOG_WARN("cannot create %s (not elevated?); using %s, visible to this session only",
                 shm_name.c_str(), kFallbackShmName);
    shm_name = kFallbackShmName;
    created = shm_.create(shm_name, bytes);
  }
#endif
  if (!created) {
    VIRGIL_LOG_ERROR("cannot create shared memory '%s' (%llu bytes); is another virgild running?",
                  shm_name.c_str(), (unsigned long long)bytes);
    return false;
  }
  hdr_ = static_cast<ShmHeader*>(shm_.data());
  if (reused) clear_rings();  // keep client slots, drop stale audio
  hdr_->magic = kShmMagic;
  hdr_->version = kShmVersion;
  hdr_->total_bytes = bytes;
  hdr_->sample_rate = cfg_.sample_rate;
  hdr_->tx_channels = cfg_.tx_channels;
  hdr_->rx_channels = cfg_.rx_channels;
  hdr_->ring_frames = cfg_.ring_frames;
  hdr_->period_frames = cfg_.period_frames();
  hdr_->rx_latency_frames = cfg_.rx_latency_frames();
  hdr_->tx_lead_frames = cfg_.tx_lead_frames();
  std::memset(hdr_->device_name, 0, sizeof hdr_->device_name);
  std::strncpy(hdr_->device_name, cfg_.device_name.c_str(), sizeof hdr_->device_name - 1);
  mix_.assign(size_t(hdr_->period_frames) * std::max(1u, cfg_.tx_channels), 0.f);

  // --- Dante-side rings ---
  dring_ = cfg_.ring_frames;
  dtx_.assign(size_t(dring_) * std::max(1u, cfg_.tx_channels), 0);
  drx_.assign(size_t(dring_) * std::max(1u, cfg_.rx_channels), 0);

  // --- clock ---
  if (clock) {
    clock_ = std::move(clock);
  } else if (cfg_.clock == "ptp") {
    auto p = std::make_unique<PtpClock>();
    PtpClock::Options o;
    o.interface_addr = iface_;
    o.subdomain = cfg_.ptp_subdomain;
    o.master_capable = cfg_.ptp_master_capable;
    p->set_listener(&Engine::on_clock, this);
    if (!p->start(o)) {
      VIRGIL_LOG_WARN("falling back to a free-running clock; Dante devices will not lock to it");
      clock_ = std::make_unique<FreeRunClock>();
    } else {
      clock_ = std::move(p);
    }
  } else {
    clock_ = std::make_unique<FreeRunClock>();
  }
  on_clock(clock_->model(), this);

  // --- Dante device ---
  if (dante_enabled_) {
    const std::string ip = ipv4_to_string(iface_);
    VgDanteConfig dc{};
    dc.name = cfg_.device_name.c_str();
    dc.bind_ip = ip.c_str();
    dc.sample_rate = cfg_.sample_rate;
    dc.tx_channels = cfg_.tx_channels;
    dc.rx_channels = cfg_.rx_channels;
    dc.tx_latency_ns = cfg_.tx_latency_us * 1000;
    dc.tx_send_delay_ns = cfg_.tx_send_delay_us() * 1000;
    dc.rx_latency_ns = cfg_.latency_us * 1000;
    dc.tx_ring = dtx_.data();
    dc.tx_ring_frames = dring_;
    dc.rx_ring = drx_.data();
    dc.rx_ring_frames = dring_;
    dc.mono_ns = &bridge_mono_ns;
    dc.log = &bridge_log;
    dante_ = vg_dante_start(&dc);
    if (!dante_) {
      const char* why = vg_dante_last_error();
      const std::string reason = why ? why : "";
      if (reason.empty() || reason.find("in use") != std::string::npos ||
          reason.find("AddrInUse") != std::string::npos)
        VIRGIL_LOG_ERROR("Dante device failed to start%s%s (is another Virgil or Inferno instance "
                         "running? Dante ports 4400/4455/8700/8800 must be free)",
                         reason.empty() ? "" : ": ", reason.c_str());
      else
        VIRGIL_LOG_ERROR("Dante device failed to start: %s", reason.c_str());
      clock_.reset();
      return false;
    }
  }

  running_ = true;
  hdr_->state.store(clock_->state(), std::memory_order_release);
  tick_thread_ = std::thread([this] { tick_loop(); });

  // Which adapter that is, and what else is up: two adapters on the same
  // network (e.g. Wi-Fi and Ethernet) are a common cause of missing clock
  // packets, which are multicast.
  {
    std::string mine, others;
    for (const auto& i : list_interfaces()) {
      if (i.loopback) continue;
      const std::string d = "'" + i.name + "' " + ipv4_to_string(i.addr) + (i.virtual_adapter ? " (virtual)" : "");
      if (i.addr == iface_) mine = d;
      else others += (others.empty() ? "" : ", ") + d;
    }
    VIRGIL_LOG_INFO("network adapter: %s%s%s", mine.empty() ? ipv4_to_string(iface_).c_str() : mine.c_str(),
                    others.empty() ? "" : "; other adapters: ", others.c_str());
  }
  VIRGIL_LOG_INFO("\"%s\" up on %s: %u Hz, %u transmit / %u receive channels, latency rx %.1f ms "
               "tx %.1f ms",
               cfg_.device_name.c_str(), ipv4_to_string(iface_).c_str(), cfg_.sample_rate,
               cfg_.tx_channels, cfg_.rx_channels, cfg_.latency_us / 1000.0,
               cfg_.tx_latency_us / 1000.0);
  return true;
}

void Engine::stop() {
  if (!running_.exchange(false)) {
    if (!shm_ext_) shm_own_.close();
    return;
  }
  if (tick_thread_.joinable()) tick_thread_.join();
  if (dante_) {
    vg_dante_set_clock_master(nullptr);
    reported_master_ = false;
    vg_dante_stop(dante_);
  }
  dante_ = nullptr;
  if (hdr_) hdr_->state.store(kStateStopped, std::memory_order_release);
  clock_.reset();
  hdr_ = nullptr;
  if (!shm_ext_) shm_own_.close();  // a shared segment stays for the next engine
}

bool Engine::dante_healthy() const {
  return !dante_ || vg_dante_healthy(dante_) != 0;
}

void Engine::clear_rings() {
  const size_t txf = size_t(cfg_.ring_frames) * cfg_.tx_channels, rxf = size_t(cfg_.ring_frames) * cfg_.rx_channels;
  for (uint32_t i = 0; i < kMaxTxClients; ++i)
    std::memset(tx_ring_at(hdr_, cfg_.ring_frames, cfg_.tx_channels, i), 0, txf * sizeof(float));
  std::memset(rx_ring_at(hdr_, cfg_.ring_frames, cfg_.tx_channels), 0, rxf * sizeof(float));
}

// ---------------------------------------------------------------------------
// The real-time tick, once per tick_us on media-clock boundaries T:
//   1. publish the clock anchor drivers use to time their callbacks
//   2. mix [T-P, T) from every client slot into the Dante transmit ring
//      (Inferno sends frame f at media time f + tx_latency)
//   3. move received frames [T-2P, T-P) from the Dante ring to the capture
//      ring (Inferno has finished them: it writes at arrival + rx_latency and
//      fills gaps with silence up to "now")
void Engine::tick_loop() {
  // Geometry from our own configuration, never from the shared header.
  const uint32_t P = cfg_.period_frames();
  const uint32_t rate = cfg_.sample_rate;
  const int64_t period_ns = int64_t(P) * 1000000000LL / rate;
  if (!set_realtime_priority(cfg_.rt_priority, period_ns))
    VIRGIL_LOG_WARN("could not get real-time scheduling; expect higher jitter (see README)");

  const uint32_t ring = cfg_.ring_frames;
  const uint64_t mask = ring - 1;
  const uint64_t dmask = dring_ - 1;
  const uint32_t txch = cfg_.tx_channels;
  const uint32_t rxch = cfg_.rx_channels;
  float* rx = rx_ring_at(hdr_, ring, txch);
  const int64_t spin = int64_t(cfg_.spin_us) * 1000;
  const int64_t resync_ns = std::max<int64_t>(50000000, 4 * period_ns);

  // A new engine (reload) may run on a different timeline than the one
  // connected drivers were following: tell them to realign.
  hdr_->clock_steps.fetch_add(1, std::memory_order_relaxed);

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
      std::fill(drx_.begin(), drx_.end(), 0);
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

    // --- transmit: mix clients into the Dante ring ---
    if (txch) {
      const uint64_t start = next - P;
      std::fill(mix_.begin(), mix_.end(), 0.f);
      for (uint32_t i = 0; i < kMaxTxClients; ++i) {
        ClientSlot& s = hdr_->clients[i];
        if (s.pid.load(std::memory_order_acquire) == 0) continue;
        float* cring = tx_ring_at(hdr_, ring, txch, i);
        const bool active = s.active.load(std::memory_order_acquire) != 0;
        for (uint32_t f = 0; f < P; ++f) {
          float* src = cring + size_t((start + f) & mask) * txch;
          if (active) {
            float* dst = mix_.data() + size_t(f) * txch;
            for (uint32_t c = 0; c < txch; ++c) dst[c] += src[c];
          }
          std::memset(src, 0, sizeof(float) * txch);  // consumed
        }
      }
      float peaks[kMaxChannels] = {};
      for (uint32_t f = 0; f < P; ++f) {
        const float* src = mix_.data() + size_t(f) * txch;
        int32_t* dst = dtx_.data() + size_t((start + f) & dmask) * txch;
        for (uint32_t c = 0; c < txch; ++c) {
          dst[c] = float_to_s32(src[c]);
          peaks[c] = std::max(peaks[c], std::fabs(src[c]));
        }
      }
      for (uint32_t c = 0; c < txch; ++c)
        if (peaks[c] > 0) peak_max(tx_peak_[c], peaks[c]);
    }

    // --- receive: finished frames from the Dante ring to the capture ring ---
    if (rxch) {
      const uint64_t start = next - 2 * P;
      float peaks[kMaxChannels] = {};
      for (uint32_t f = 0; f < P; ++f) {
        int32_t* src = drx_.data() + size_t((start + f) & dmask) * rxch;
        float* dst = rx + size_t((start + f) & mask) * rxch;
        for (uint32_t c = 0; c < rxch; ++c) {
          dst[c] = s32_to_float(src[c]);
          peaks[c] = std::max(peaks[c], std::fabs(dst[c]));
          src[c] = 0;  // consumed: a stopped stream must not replay a ring later
        }
      }
      for (uint32_t c = 0; c < rxch; ++c)
        if (peaks[c] > 0) peak_max(rx_peak_[c], peaks[c]);
      // Silence the capture slot half a ring ahead (== half a ring behind).
      const uint64_t z = next + ring / 2 - P;
      for (uint32_t f = 0; f < P; ++f)
        std::memset(rx + size_t((z + f) & mask) * rxch, 0, sizeof(float) * rxch);
    }

    if (now - last_reclaim > 1000000000LL) {
      reclaim_clients(now);
      report_clock_master();
      last_reclaim = now;
    }
    next += P;
  }
}

// Dante Controller's clock status needs the leader's identity. PTPv1 uses
// 6-byte clock UUIDs; Dante shows them as EUI-64 (ff:fe in the middle).
void Engine::report_clock_master() {
  if (!dante_) return;
  // Reported as soon as Virgil follows (or is) a leader, like Dante devices
  // that are still settling; the grandmaster is cleared when it is lost.
  unsigned b[6];
  const std::string gm = clock_->grandmaster();
  const bool have = std::sscanf(gm.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3],
                                &b[4], &b[5]) == 6;
  uint8_t id[8] = {};
  if (have) {
    id[0] = uint8_t(b[0]); id[1] = uint8_t(b[1]); id[2] = uint8_t(b[2]);
    id[3] = 0xff; id[4] = 0xfe;
    id[5] = uint8_t(b[3]); id[6] = uint8_t(b[4]); id[7] = uint8_t(b[5]);
  }
  if (have == reported_master_ && std::memcmp(id, reported_id_, 8) == 0) return;
  reported_master_ = have;
  std::memcpy(reported_id_, id, 8);
  vg_dante_set_clock_master(have ? id : nullptr);
}

void Engine::reclaim_clients(int64_t now) {
  for (uint32_t i = 0; i < kMaxTxClients; ++i) {
    ClientSlot& s = hdr_->clients[i];
    const uint32_t pid = s.pid.load(std::memory_order_acquire);
    if (pid == 0) continue;
    if (!process_alive(pid)) {
      VIRGIL_LOG_INFO("client slot %u (%s, pid %u) died; releasing", i, slot_name(s).c_str(), pid);
      s.active.store(0, std::memory_order_release);
      s.pid.store(0, std::memory_order_release);
      client_stalled_[i] = false;
      continue;
    }
    // A client whose heartbeat stalled stays active: its ring is consumed
    // every tick, so it contributes silence until it resumes. (Deactivating
    // it here used to mute an app for good after a long hiccup, since only
    // the app sets `active` again, on its next start.) Just say so.
    const bool stalled = s.active.load(std::memory_order_acquire) != 0 &&
                         now - s.heartbeat_ns.load(std::memory_order_relaxed) > 2000000000LL;
    if (stalled != client_stalled_[i]) {
      if (stalled)
        VIRGIL_LOG_WARN("client slot %u (%s, pid %u) stopped sending audio", i, slot_name(s).c_str(), pid);
      else
        VIRGIL_LOG_INFO("client slot %u (%s, pid %u) is sending audio again", i, slot_name(s).c_str(), pid);
      client_stalled_[i] = stalled;
    }
  }
}

void Engine::take_peaks(std::vector<float>* tx, std::vector<float>* rx) {
  auto drain = [](std::array<std::atomic<uint32_t>, kMaxChannels>& a, uint32_t n,
                  std::vector<float>* out) {
    out->assign(n, 0.f);
    for (uint32_t c = 0; c < n; ++c) {
      const uint32_t b = a[c].exchange(0, std::memory_order_relaxed);
      std::memcpy(&(*out)[c], &b, sizeof b);
    }
  };
  drain(tx_peak_, cfg_.tx_channels, tx);
  drain(rx_peak_, cfg_.rx_channels, rx);
}

std::string Engine::status_line() const {
  if (!hdr_) return "stopped";
  static const char* states[] = {"stopped", "free-run", "ptp-locked", "ptp-master"};
  const uint32_t st = hdr_->state.load();
  uint32_t clients = 0;
  for (const auto& c : hdr_->clients)
    if (c.pid.load() && c.active.load()) ++clients;
  char buf[200];
  std::snprintf(buf, sizeof buf, "clock %s offset %+.1f us | late ticks %" PRIu64
                " | clock steps %" PRIu64 " | apps playing %u",
                st < 4 ? states[st] : "?", double(hdr_->ptp_offset_ns.load()) / 1000.0,
                hdr_->late_ticks.load(), hdr_->clock_steps.load(), clients);
  return buf;
}

}  // namespace virgil
