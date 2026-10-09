#include "view.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace vc {

namespace {

using namespace color;

const float kW = 640, kHeader = 76, kFooter = 56, kMeterH = 150, kPad = 22;
const float kGroupH = 34 + kMeterH + 30;

std::string fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

float strip_w(unsigned n) {
  const unsigned total = std::max(1u, n);
  if (total <= 16) return 20;
  if (total <= 32) return 12;
  return 7;
}
float group_w(unsigned n) {
  const float w = strip_w(n), gap = w >= 12 ? 6 : 3;
  return 34 + float(std::max(1u, n)) * (w + gap) - gap;
}
float lin_to_pos(float lin) {
  if (!(lin > 0)) return 0;
  const float db = 20.f * std::log10(lin);
  return std::max(0.f, std::min(1.f, (db + 60.f) / 60.f));
}

struct ClockView {
  unsigned color;
  std::string line1, line2;
};
ClockView clock_view(const Status& s) {
  if (!s.reachable) return {kBad, "Service not running", ""};
  if (s.state == "error") return {kBad, "Audio engine error", s.error};
  if (s.state == "waiting-for-network") return {kWarn, "Waiting for network", s.iface};
  if (s.state != "running") return {kWarn, "Starting…", ""};
  if (!s.dante) return {kBad, "Dante stack not running", ""};
  if (s.clock == "ptp-locked") return {kOk, "Locked to " + s.grandmaster, fmt("offset %+.1f µs", s.offset_us)};
  if (s.clock == "ptp-master") return {kEmber, "Clock leader", "no other Dante clock present"};
  return {kWarn, "Free-running", s.grandmaster.empty() ? "no Dante clock leader found" : "acquiring " + s.grandmaster};
}

}  // namespace

float StatusView::width() const {
  return std::max(kW, kPad * 2 + std::max(group_w(status_.tx), group_w(status_.rx)));
}
float StatusView::height() const { return kHeader + 14 + 2 * kGroupH + 10 + kFooter; }

void StatusView::update(const Status& s, uint64_t now_ms) {
  status_ = s;
  now_ = now_ms;
  const float dt = last_ ? std::min(0.2f, float(now_ms - last_) / 1000.f) : 0.f;
  last_ = now_ms;
  update_meters(mtx_, s.meter_tx, s.tx, dt);
  update_meters(mrx_, s.meter_rx, s.rx, dt);
}

void StatusView::update_meters(std::vector<Meter>& m, const std::vector<float>& vals, unsigned n, float dt) {
  m.resize(n);
  for (unsigned i = 0; i < n; ++i) {
    const float v = i < vals.size() ? lin_to_pos(vals[i]) : 0;
    // Instant attack, ~26 dB/s release, 1.5 s peak hold, 2 s clip lamp.
    m[i].level = std::max(v, m[i].level - dt * 0.44f);
    if (v >= m[i].hold || now_ - m[i].hold_t > 1500) {
      m[i].hold = v;
      m[i].hold_t = now_;
    }
    if (i < vals.size() && vals[i] >= 0.989f) m[i].clip_t = now_;  // >= -0.1 dBFS
  }
}

void StatusView::draw_group(Canvas& c, float x, float y, const std::string& caption, unsigned n,
                            const std::vector<Meter>& m) {
  const float sw = strip_w(n), gap = sw >= 12 ? 6 : 3, scale_w = 34;
  c.text(caption, Font::Label, Rect{x, y - 26, x + 400, y - 8}, kMuted, Align::Left);
  const float top = y, h = kMeterH, x0 = x + scale_w;
  const int marks[] = {0, -6, -12, -18, -24, -36, -48, -60};
  for (int db : marks) {
    const float yy = top + h * (1.f - (db + 60.f) / 60.f);
    c.text(db == 0 ? "0" : std::to_string(-db), Font::Mono, Rect{x, yy - 7, x + scale_w - 8, yy + 7}, kDim,
           Align::Right);
    c.hline(x + scale_w - 5, x + scale_w - 2, yy, kLine);
  }
  const int segs = 30;
  const float seg_h = h / segs;
  for (unsigned ch = 0; ch < std::max(1u, n); ++ch) {
    const float sx = x0 + float(ch) * (sw + gap);
    const Meter st = ch < m.size() ? m[ch] : Meter{};
    const int lit = int(std::ceil(st.level * segs - 0.01f));
    const int hold = int(std::ceil(st.hold * segs - 0.01f)) - 1;
    for (int k = 0; k < segs; ++k) {
      const float sy = top + h - float(k + 1) * seg_h;
      const float thr_db = -60.f + 60.f * float(k + 1) / segs;
      const unsigned on = thr_db > -6.f ? kBad : thr_db > -18.f ? kWarn : kOk;
      const Rect r{sx, sy + 1, sx + sw, sy + seg_h};
      if (k < lit) c.fill(r, on);
      else if (k == hold && st.hold > 0.02f) c.fill(r, on, 0.85f);
      else c.fill(r, kSegOff);
    }
    const bool clip = st.clip_t && now_ - st.clip_t < 2000;
    c.fill(Rect{sx, top - 7, sx + sw, top - 3}, clip ? kBad : kSegOff);
    if (n <= 32 || ch % 4 == 0)
      c.text(std::to_string(ch + 1), Font::Small, Rect{sx - 6, top + h + 4, sx + sw + 6, top + h + 18}, kMuted,
             Align::Center);
  }
}

void StatusView::draw_button(Canvas& c, const Button& b, int index) {
  const bool hov = hover_ == index, down = down_ == index;
  const unsigned bg = b.primary ? kEmber : (down ? 0x2C3137 : hov ? 0x262A30 : kPanel);
  c.fill_round(b.r, 4, bg);
  if (!b.primary) c.stroke_round(b.r, 4, hov ? 0x4A5058 : 0x383D44);
  c.text(b.label, Font::Button, b.r, b.primary ? 0x1A0D06 : kText, Align::Center);
}

void StatusView::paint(Canvas& c, float W, float H) {
  const Status& s = status_;
  c.fill(Rect{0, 0, W, H}, kBg);

  // Header.
  c.fill(Rect{0, 0, W, kHeader}, kPanel);
  c.hline(0, W, kHeader - 0.5f, kLine);
  c.logo(Rect{kPad, 16, kPad + 44, 60});
  const float tx = kPad + 58;
  c.text(s.reachable && !s.name.empty() ? s.name : "Virgil", Font::Title, Rect{tx, 14, W / 2 + 60, 38}, kText,
         Align::Left);
  std::string sub = "Virtual Interface Routing Gateway for Inferno-based Low-latency audio";
  if (s.reachable && !s.address.empty())
    sub = s.address + fmt("   ·   %.1f kHz   ·   %u out / %u in   ·   %.1f ms", s.rate / 1000.0, s.tx,
                          s.rx, s.latency_us / 1000.0);
  c.text(sub, Font::Sub, Rect{tx, 40, W / 2 + 140, 60}, kMuted, Align::Left);

  const ClockView cv = clock_view(s);
  const float cw = std::max(c.text_width(cv.line1, Font::Label), c.text_width(cv.line2, Font::Sub)) + 24;
  const float cx = W - kPad - cw;
  c.dot(cx + 4, cv.line2.empty() ? 38.f : 28.f, 4, cv.color);
  c.text(cv.line1, Font::Label,
         Rect{cx + 16, cv.line2.empty() ? 28.f : 18.f, W - kPad, cv.line2.empty() ? 48.f : 38.f}, kText,
         Align::Left);
  if (!cv.line2.empty()) c.text(cv.line2, Font::Sub, Rect{cx + 16, 40, W - kPad, 58}, kMuted, Align::Left);

  buttons_.clear();
  if (!s.reachable) {
    c.text("The Virgil service is not running.", Font::Big, Rect{0, kHeader + 70, W, kHeader + 100}, kText,
           Align::Center);
    c.text("It normally starts with the computer. Start it now, or check the log for errors.", Font::Sub,
           Rect{0, kHeader + 100, W, kHeader + 120}, kMuted, Align::Center);
    buttons_.push_back({Rect{W / 2 - 70, kHeader + 140, W / 2 + 70, kHeader + 172}, "Start service",
                        Action::StartService, true});
  } else {
    // Meter bridge: from the network (what apps record) on top, to the
    // network (what apps play) below.
    const float my = kHeader + 14 + 34;
    const float bx = std::max(kPad, (W - std::max(group_w(s.tx), group_w(s.rx))) / 2);
    draw_group(c, bx, my, fmt("From network  —  %u channels", s.rx), s.rx, mrx_);
    draw_group(c, bx, my + kGroupH, fmt("To network  —  %u channels", s.tx), s.tx, mtx_);
    if (!s.error.empty())
      c.text(s.error, Font::Sub, Rect{kPad, H - kFooter - 22, W - kPad, H - kFooter - 4}, kBad, Align::Left);
  }

  // Footer.
  c.fill(Rect{0, H - kFooter, W, H}, kPanel);
  c.hline(0, W, H - kFooter + 0.5f, kLine);
  if (s.reachable) {
    std::string apps;
    for (const auto& cl : s.clients) {
      if (!apps.empty()) apps += ",  ";
      apps += cl.name + (cl.active ? " (playing)" : "");
    }
    c.text(apps.empty() ? "No apps connected" : apps, Font::Label,
           Rect{kPad, H - kFooter, W - 280, H - kFooter / 2 - 2}, apps.empty() ? kMuted : kText, Align::Left);
    std::string ver = "Virgil " + s.version;
    if (s.clock_steps > 1) ver += fmt("   ·   clock re-synced %llu×", s.clock_steps - 1);
    c.text(ver, Font::Sub, Rect{kPad, H - kFooter / 2 - 4, W - 280, H - 8}, kDim, Align::Left);
    buttons_.push_back({Rect{W - kPad - 236, H - kFooter + 12, W - kPad - 128, H - 12}, "Settings…",
                        Action::Settings, false});
    buttons_.push_back({Rect{W - kPad - 120, H - kFooter + 12, W - kPad, H - 12}, "Restart engine",
                        Action::Restart, false});
  }
  for (size_t i = 0; i < buttons_.size(); ++i) draw_button(c, buttons_[i], int(i));
}

bool StatusView::hover(float x, float y) {
  int h = -1;
  for (size_t i = 0; i < buttons_.size(); ++i)
    if (buttons_[i].r.contains(x, y)) h = int(i);
  if (h == hover_) return false;
  hover_ = h;
  return true;
}

bool StatusView::press(float x, float y) {
  hover(x, y);
  down_ = hover_;
  return down_ >= 0;
}

Action StatusView::release(float x, float y) {
  const int d = down_;
  down_ = -1;
  hover(x, y);
  if (d >= 0 && d == hover_ && size_t(d) < buttons_.size()) return buttons_[size_t(d)].action;
  return Action::None;
}

std::string tray_tooltip(const Status& s) {
  if (!s.reachable) return "Virgil: service not running";
  std::string t = "Virgil — " + s.name + "\n";
  if (s.state == "error") t += "Audio engine error";
  else if (s.clock == "ptp-locked") t += "Locked to " + s.grandmaster;
  else if (s.clock == "ptp-master") t += "Clock leader";
  else t += "Free-running";
  unsigned playing = 0;
  for (const auto& c : s.clients) playing += c.active;
  if (playing) t += fmt("\n%u app(s) playing", playing);
  return t;
}

}  // namespace vc
