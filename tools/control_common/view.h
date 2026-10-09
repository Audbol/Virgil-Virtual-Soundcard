// Virgil Control (macOS, Linux): the status window, drawn the same way as the
// Windows app (header, stacked meter bridge, footer with buttons) through a
// small Canvas interface that each platform implements (Cairo, CoreGraphics).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "api.h"

namespace vc {

struct Rect {
  float l = 0, t = 0, r = 0, b = 0;
  bool contains(float x, float y) const { return x >= l && x < r && y >= t && y < b; }
};

enum class Font { Title, Sub, Label, Small, Mono, Button, Big };
enum class Align { Left, Center, Right };

// Colours as 0xRRGGBB.
namespace color {
constexpr unsigned kBg = 0x15171A, kPanel = 0x1B1E22, kLine = 0x2A2E34, kText = 0xE9E5DD, kMuted = 0x8C9097,
                   kDim = 0x5C6168, kEmber = 0xE07A2E, kOk = 0x6DB476, kWarn = 0xD9A93F, kBad = 0xD2553F,
                   kSegOff = 0x23272C;
}

class Canvas {
 public:
  virtual ~Canvas() = default;
  virtual void fill(const Rect& r, unsigned rgb, float alpha = 1.f) = 0;
  virtual void fill_round(const Rect& r, float radius, unsigned rgb, float alpha = 1.f) = 0;
  virtual void stroke_round(const Rect& r, float radius, unsigned rgb) = 0;
  virtual void hline(float x0, float x1, float y, unsigned rgb) = 0;
  virtual void dot(float cx, float cy, float radius, unsigned rgb) = 0;
  // UTF-8 text, vertically centred in `r`, clipped to it.
  virtual void text(const std::string& s, Font f, const Rect& r, unsigned rgb, Align a) = 0;
  virtual float text_width(const std::string& s, Font f) = 0;
  virtual void logo(const Rect& r) = 0;
};

enum class Action { None, Settings, Restart, StartService };

class StatusView {
 public:
  // Feed the latest status (from the poll thread), with a monotonic ms clock.
  void update(const Status& s, uint64_t now_ms);
  void paint(Canvas& c, float width, float height);
  // Mouse: returns true if the view needs repainting.
  bool hover(float x, float y);
  bool press(float x, float y);
  Action release(float x, float y);
  // Pointer over a button (for the hand cursor).
  bool over_button() const { return hover_ >= 0; }

  // Natural size for the current channel counts.
  float width() const;
  float height() const;
  const Status& status() const { return status_; }

 private:
  struct Meter {
    float level = 0, hold = 0;
    uint64_t hold_t = 0, clip_t = 0;
  };
  struct Button {
    Rect r;
    std::string label;
    Action action = Action::None;
    bool primary = false;
  };
  void update_meters(std::vector<Meter>& m, const std::vector<float>& vals, unsigned n, float dt);
  void draw_group(Canvas& c, float x, float y, const std::string& caption, unsigned n, const std::vector<Meter>& m);
  void draw_button(Canvas& c, const Button& b, int index);

  Status status_;
  std::vector<Meter> mtx_, mrx_;
  uint64_t now_ = 0, last_ = 0;
  std::vector<Button> buttons_;
  int hover_ = -1, down_ = -1;
};

// One-line summary for a tray tooltip.
std::string tray_tooltip(const Status& s);

}  // namespace vc
