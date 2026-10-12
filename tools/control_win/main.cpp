// Virgil Control for Windows: tray icon, status window with level meters, and
// a settings dialog. It talks to virgild over its local control API
// (127.0.0.1:8480) and starts a portable virgild next to it if no service runs.
//
//   virgil-control.exe          open the window (and the tray icon)
//   virgil-control.exe --tray   start in the tray only (used at login)
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <cwchar>
#include <initializer_list>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api.h"
#include "resource.h"
#include "settings_model.h"

namespace {

// ---- small helpers ------------------------------------------------------------------

template <class T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
  return w;
}
std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}
std::wstring fmt(const wchar_t* f, ...) {
  wchar_t buf[512];
  va_list ap;
  va_start(ap, f);
  _vsnwprintf(buf, 511, f, ap);
  va_end(ap);
  buf[511] = 0;
  return buf;
}
std::wstring exe_dir() {
  wchar_t p[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
  std::wstring s(p, n);
  return s.substr(0, s.find_last_of(L"\\/"));
}
bool file_exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

D2D1_COLOR_F rgb(unsigned hex, float a = 1.f) {
  return D2D1::ColorF(((hex >> 16) & 0xFF) / 255.f, ((hex >> 8) & 0xFF) / 255.f, (hex & 0xFF) / 255.f, a);
}

// Palette: warm charcoal, like the front panel of a rack unit.
const unsigned kBg = 0x15171A, kPanel = 0x1B1E22, kLine = 0x2A2E34, kText = 0xE9E5DD,
               kMuted = 0x8C9097, kDim = 0x5C6168, kEmber = 0xE07A2E, kOk = 0x6DB476,
               kWarn = 0xD9A93F, kBad = 0xD2553F, kSegOff = 0x23272C;

// ---- app state ---------------------------------------------------------------------------

const wchar_t* kWindowClass = L"VirgilControlWindow";
const UINT WM_APP_STATUS = WM_APP + 1, WM_APP_TRAY = WM_APP + 2, WM_APP_SHOW = WM_APP + 3,
           WM_APP_DONE = WM_APP + 4,   // a worker's request finished (lParam: Job*)
           WM_APP_SAVED = WM_APP + 5;  // to the settings dialog: its POST finished (lParam: Job*)
const UINT kTrayId = 1;
enum {
  ID_OPEN = 100, ID_SETTINGS, ID_RESTART, ID_LOG, ID_AUTOSTART, ID_QUIT, ID_ABOUT, ID_DIAG, ID_STOP, ID_START,
  ID_BROWSER
};

HINSTANCE g_inst;
HWND g_hwnd;
unsigned g_port = 8480;
UINT g_taskbar_created;
std::mutex g_mutex;
vc::Status g_status;         // guarded by g_mutex
std::atomic<bool> g_visible{false};
std::atomic<bool> g_quit{false};
bool g_told_about_tray = false;
HICON g_icon_small;          // tray, window and dialog icon (loaded once)
HWND g_settings_dlg;         // the open settings dialog, if any (UI thread only)
int g_pending = 0;           // command id whose request is in flight, 0 = none (UI thread only)

// Meter display state (UI thread only).
struct MeterState {
  float level = 0;  // displayed, with release
  float hold = 0;
  DWORD hold_t = 0;
  DWORD clip_t = 0;
};
std::vector<MeterState> g_mtx, g_mrx;
DWORD g_last_frame = 0;

// ---- Direct2D resources --------------------------------------------------------------

ID2D1Factory* g_d2d;
IDWriteFactory* g_dw;
IWICImagingFactory* g_wic;
ID2D1HwndRenderTarget* g_rt;
ID2D1SolidColorBrush* g_brush;
ID2D1Bitmap* g_logo;
IDWriteTextFormat *g_f_title, *g_f_sub, *g_f_sub_c, *g_f_label, *g_f_small, *g_f_small_l, *g_f_mono, *g_f_button, *g_f_big;
float g_dpi = 96.f;

// First installed family of a preference list (Segoe UI/Consolas ship with
// Windows; the fallbacks keep text visible on unusual systems).
std::wstring pick_family(std::initializer_list<const wchar_t*> names) {
  IDWriteFontCollection* fc = nullptr;
  std::wstring found = *names.begin();
  if (SUCCEEDED(g_dw->GetSystemFontCollection(&fc, FALSE))) {
    for (const wchar_t* n : names) {
      UINT32 idx;
      BOOL exists = FALSE;
      if (SUCCEEDED(fc->FindFamilyName(n, &idx, &exists)) && exists) {
        found = n;
        break;
      }
    }
  }
  release(fc);
  return found;
}
std::wstring g_ui_font, g_mono_font;

IDWriteTextFormat* make_format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT w,
                               DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
  IDWriteTextFormat* f = nullptr;
  if (!std::wcscmp(family, L"ui")) family = g_ui_font.c_str();
  if (!std::wcscmp(family, L"mono")) family = g_mono_font.c_str();
  g_dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                         L"en-us", &f);
  if (f) {
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  }
  return f;
}

void create_device_independent() {
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                      reinterpret_cast<IUnknown**>(&g_dw));
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
  g_ui_font = pick_family({L"Segoe UI", L"Segoe UI Variable Text", L"Tahoma", L"Arial", L"DejaVu Sans"});
  g_mono_font = pick_family({L"Cascadia Mono", L"Consolas", L"Lucida Console", L"DejaVu Sans Mono"});
  g_f_title = make_format(L"ui", 17.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  g_f_sub = make_format(L"ui", 12.f, DWRITE_FONT_WEIGHT_NORMAL);
  g_f_label = make_format(L"ui", 12.f, DWRITE_FONT_WEIGHT_NORMAL);
  g_f_sub_c = make_format(L"ui", 12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
  g_f_small = make_format(L"ui", 10.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
  g_f_small_l = make_format(L"ui", 10.5f, DWRITE_FONT_WEIGHT_NORMAL);
  g_f_mono = make_format(L"mono", 11.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
  g_f_button = make_format(L"ui", 12.f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
  g_f_big = make_format(L"ui", 15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void discard_device() {
  release(g_logo);
  release(g_brush);
  release(g_rt);
}

void load_logo() {
  if (!g_wic || !g_rt) return;
  HICON ic = static_cast<HICON>(LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_VIRGIL), IMAGE_ICON, 64, 64, 0));
  if (!ic) return;
  IWICBitmap* wb = nullptr;
  IWICFormatConverter* conv = nullptr;
  if (SUCCEEDED(g_wic->CreateBitmapFromHICON(ic, &wb)) && SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
      SUCCEEDED(conv->Initialize(wb, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                 WICBitmapPaletteTypeCustom)))
    g_rt->CreateBitmapFromWicBitmap(conv, nullptr, &g_logo);
  release(conv);
  release(wb);
  DestroyIcon(ic);
}

bool ensure_device(HWND hwnd) {
  if (g_rt) return true;
  RECT rc;
  GetClientRect(hwnd, &rc);
  D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties();
  p.dpiX = p.dpiY = g_dpi;
  if (FAILED(g_d2d->CreateHwndRenderTarget(
          p, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(UINT(rc.right), UINT(rc.bottom))), &g_rt)))
    return false;
  g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  g_rt->CreateSolidColorBrush(rgb(kText), &g_brush);
  load_logo();
  return true;
}

// ---- drawing primitives ----------------------------------------------------------------

void fill(const D2D1_RECT_F& r, unsigned c, float a = 1.f) {
  g_brush->SetColor(rgb(c, a));
  g_rt->FillRectangle(r, g_brush);
}
void fill_round(const D2D1_RECT_F& r, float rad, unsigned c, float a = 1.f) {
  g_brush->SetColor(rgb(c, a));
  g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g_brush);
}
void stroke_round(const D2D1_RECT_F& r, float rad, unsigned c, float w = 1.f) {
  g_brush->SetColor(rgb(c));
  g_rt->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g_brush, w);
}
void hline(float x0, float x1, float y, unsigned c) {
  g_brush->SetColor(rgb(c));
  g_rt->DrawLine(D2D1::Point2F(x0, y), D2D1::Point2F(x1, y), g_brush, 1.f);
}
void text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, unsigned c) {
  if (!f || s.empty()) return;
  g_brush->SetColor(rgb(c));
  g_rt->DrawText(s.c_str(), UINT32(s.size()), f, r, g_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
float text_width(const std::wstring& s, IDWriteTextFormat* f) {
  IDWriteTextLayout* l = nullptr;
  float w = 0;
  if (SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), UINT32(s.size()), f, 2000.f, 100.f, &l))) {
    DWRITE_TEXT_METRICS m;
    l->GetMetrics(&m);
    w = m.widthIncludingTrailingWhitespace;
  }
  release(l);
  return w;
}

// ---- layout & buttons --------------------------------------------------------------------

struct Button {
  D2D1_RECT_F r{};
  std::wstring label;
  int id = 0;
  bool primary = false, visible = false, disabled = false;
};
std::vector<Button> g_buttons;  // rebuilt each paint
// Hover and press are kept by command id, not by slot: the buttons change
// between running and stopped, and a release must trigger what was pressed.
int g_hover_id = 0, g_press_id = 0;

// Commands that talk to the service; disabled while one of them is in flight.
bool is_request(int id) { return id == ID_STOP || id == ID_START || id == ID_RESTART || id == ID_SETTINGS; }
bool busy() { return g_pending != 0; }
// A button's label, or what it is doing while its request is in flight.
const wchar_t* button_label(int id, const wchar_t* idle) {
  if (id != g_pending) return idle;
  if (id == ID_STOP) return L"Stopping\u2026";
  if (id == ID_START) return L"Starting\u2026";
  if (id == ID_RESTART) return L"Restarting\u2026";
  return idle;
}

const float kW = 640, kHeader = 76, kFooter = 56, kMeterH = 150, kPad = 22;

float strip_w(unsigned n) {
  // Strips get narrower with many channels so the window stays on screen.
  const unsigned total = std::max(1u, n);
  if (total <= 16) return 20;
  if (total <= 32) return 12;
  return 7;
}
float group_w(unsigned n) {
  const float w = strip_w(n), gap = w >= 12 ? 6 : 3;
  return 34 + float(std::max(1u, n)) * (w + gap) - gap;
}
float window_w(const vc::Status& s) {
  return std::max(kW, kPad * 2 + std::max(group_w(s.tx), group_w(s.rx)));
}
// Two meter groups stacked: from the network on top, to the network below.
const float kGroupH = 34 + kMeterH + 30;
// With the "no clock leader" notice each meter group gives up this much
// height for it, so the window keeps its size (as tools/control_common/view.cpp).
const float kNoticeShrink = 44;
float window_h() { return kHeader + 14 + 2 * kGroupH + 10 + kFooter; }

// ---- painting -----------------------------------------------------------------------------

float lin_to_pos(float lin) {
  if (!(lin > 0)) return 0;
  const float db = 20.f * std::log10(lin);
  return std::max(0.f, std::min(1.f, (db + 60.f) / 60.f));
}

void update_meters(std::vector<MeterState>& m, const std::vector<float>& vals, unsigned n, DWORD now,
                   float dt) {
  m.resize(n);
  for (unsigned i = 0; i < n; ++i) {
    const float v = i < vals.size() ? lin_to_pos(vals[i]) : 0;
    // Instant attack, ~26 dB/s release, 1.5 s peak hold, 2 s clip lamp.
    m[i].level = std::max(v, m[i].level - dt * 0.44f);
    if (v >= m[i].hold || now - m[i].hold_t > 1500) {
      m[i].hold = v;
      m[i].hold_t = now;
    }
    if (i < vals.size() && vals[i] >= 0.989f) m[i].clip_t = now;  // >= -0.1 dBFS
  }
}

void draw_meter_group(float x, float y, const wchar_t* caption, unsigned n, const std::vector<MeterState>& m,
                      DWORD now, float meter_h) {
  const float sw = strip_w(n), gap = sw >= 12 ? 6 : 3, scale_w = 34;
  text(caption, g_f_label, D2D1::RectF(x, y - 26, x + 400, y - 8), kMuted);
  const float top = y, h = meter_h, x0 = x + scale_w;
  // dB scale.
  const int marks[] = {0, -6, -12, -18, -24, -36, -48, -60};
  for (int db : marks) {
    if (h < 130 && (db == -6 || db == -18)) continue;  // shortened for the clock notice
    const float yy = top + h * (1.f - (db + 60.f) / 60.f);
    text(db == 0 ? L"0" : std::to_wstring(-db), g_f_mono, D2D1::RectF(x, yy - 7, x + scale_w - 8, yy + 7),
         kDim);
    hline(x + scale_w - 5, x + scale_w - 2, yy, kLine);
  }
  // Segmented strips: 30 segments, -60..0 dBFS.
  const int segs = 30;
  const float seg_h = h / segs;
  for (unsigned c = 0; c < std::max(1u, n); ++c) {
    const float sx = x0 + float(c) * (sw + gap);
    const MeterState st = c < m.size() ? m[c] : MeterState{};
    const int lit = int(std::ceil(st.level * segs - 0.01f));
    const int hold = int(std::ceil(st.hold * segs - 0.01f)) - 1;
    for (int k = 0; k < segs; ++k) {
      const float sy = top + h - float(k + 1) * seg_h;
      const float thr_db = -60.f + 60.f * float(k + 1) / segs;
      const unsigned on = thr_db > -6.f ? kBad : thr_db > -18.f ? kWarn : kOk;
      const D2D1_RECT_F r = D2D1::RectF(sx, sy + 1, sx + sw, sy + seg_h);
      if (k < lit) fill(r, on);
      else if (k == hold && st.hold > 0.02f) fill(r, on, 0.85f);
      else fill(r, kSegOff);
    }
    // Clip lamp.
    const bool clip = st.clip_t && now - st.clip_t < 2000;
    fill(D2D1::RectF(sx, top - 7, sx + sw, top - 3), clip ? kBad : kSegOff);
    if (n <= 32 || c % 4 == 0)
      text(std::to_wstring(c + 1), g_f_small, D2D1::RectF(sx - 6, top + h + 4, sx + sw + 6, top + h + 18), kMuted);
  }
}

void draw_button(Button& b) {
  b.visible = true;
  b.disabled = busy() && is_request(b.id);
  const bool hover = !b.disabled && b.id == g_hover_id, down = hover && b.id == g_press_id;
  const unsigned bg = b.primary ? kEmber : (down ? 0x2C3137 : hover ? 0x262A30 : kPanel);
  fill_round(b.r, 4, bg, b.disabled && b.primary ? 0.5f : 1.f);
  if (!b.primary) stroke_round(b.r, 4, hover ? 0x4A5058 : 0x383D44);
  text(b.label, g_f_button, b.r, b.primary ? 0x1A0D06 : b.disabled ? kDim : kText);
}

struct ClockView {
  unsigned color;
  std::wstring line1, line2;
};
ClockView clock_view(const vc::Status& s) {
  if (!s.reachable) return {kBad, L"Service not running", L""};
  if (s.state == "error") return {kBad, L"Audio engine error", widen(s.error)};
  if (s.state == "stopped") return {kDim, L"Stopped", L"off the Dante network"};
  if (s.state == "waiting-for-network") return {kWarn, L"Waiting for network", widen(s.iface)};
  if (s.state != "running") return {kWarn, L"Starting\u2026", L""};
  if (!s.dante) return {kBad, L"Dante stack not running", L""};
  if (s.clock == "ptp-locked")
    return {kOk, L"Locked to " + widen(s.grandmaster), fmt(L"offset %+.1f \u00b5s", s.offset_us)};
  if (s.clock == "ptp-master") return {kEmber, L"Clock leader", L"no other Dante clock present"};
  if (s.clock_source == "free")
    return {kWarn, L"Local clock only", L"not in sync with Dante (Settings \u203a Clock)"};
  const vc::ClockNotice n = vc::clock_notice(s);
  if (n.show) return {kWarn, widen(n.headline), L"running on the local clock"};
  return {kWarn, L"Free-running", s.grandmaster.empty() ? L"no Dante clock leader found" : L"acquiring " + widen(s.grandmaster)};
}

void paint(HWND hwnd) {
  if (!ensure_device(hwnd)) return;
  vc::Status s;
  {
    std::lock_guard<std::mutex> l(g_mutex);
    s = g_status;
  }
  const DWORD now = GetTickCount();
  const float dt = g_last_frame ? std::min(0.2f, (now - g_last_frame) / 1000.f) : 0.f;
  g_last_frame = now;
  update_meters(g_mtx, s.meter_tx, s.tx, now, dt);
  update_meters(g_mrx, s.meter_rx, s.rx, now, dt);

  const D2D1_SIZE_F sz = g_rt->GetSize();
  const float W = sz.width, H = sz.height;
  g_rt->BeginDraw();
  g_rt->Clear(rgb(kBg));

  // Header.
  fill(D2D1::RectF(0, 0, W, kHeader), kPanel);
  hline(0, W, kHeader - 0.5f, kLine);
  if (g_logo) g_rt->DrawBitmap(g_logo, D2D1::RectF(kPad, 16, kPad + 44, 60));
  const float tx = kPad + 58;
  text(widen(s.reachable ? s.name : "Virgil"), g_f_title, D2D1::RectF(tx, 14, W / 2 + 60, 38), kText);
  std::wstring sub = L"Virtual Interface Routing Gateway for Inferno-based Low-latency audio";
  if (s.reachable && !s.address.empty())
    sub = widen(s.address) + fmt(L"   \u00b7   %.1f kHz   \u00b7   %u out / %u in   \u00b7   %.1f ms", s.rate / 1000.0,
                                 s.tx, s.rx, s.latency_us / 1000.0);
  text(sub, g_f_sub, D2D1::RectF(tx, 40, W / 2 + 140, 60), kMuted);

  const ClockView cv = clock_view(s);
  const float cw = std::max(text_width(cv.line1, g_f_label), text_width(cv.line2, g_f_sub)) + 24;
  const float cx = W - kPad - cw;
  g_brush->SetColor(rgb(cv.color));
  g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 4, cv.line2.empty() ? 38.f : 28.f), 4, 4), g_brush);
  text(cv.line1, g_f_label, D2D1::RectF(cx + 16, cv.line2.empty() ? 28.f : 18.f, W - kPad, cv.line2.empty() ? 48.f : 38.f),
       kText);
  if (!cv.line2.empty()) text(cv.line2, g_f_sub, D2D1::RectF(cx + 16, 40, W - kPad, 58), kMuted);

  g_buttons.assign(6, Button());

  if (!s.reachable) {
    text(L"The Virgil service is not running.", g_f_big, D2D1::RectF(0, kHeader + 70, W, kHeader + 100), kText);
    text(L"It normally starts with Windows. Start it now, or check the log for errors.", g_f_sub_c,
         D2D1::RectF(0, kHeader + 100, W, kHeader + 120), kMuted);
    Button& b = g_buttons[0];
    b.id = ID_RESTART;
    b.label = L"Start service";
    b.primary = true;
    b.r = D2D1::RectF(W / 2 - 70, kHeader + 140, W / 2 + 70, kHeader + 172);
    draw_button(b);
  } else if (s.state == "stopped") {
    text(L"Virgil is stopped.", g_f_big, D2D1::RectF(0, kHeader + 70, W, kHeader + 100), kText);
    text(L"It is off the Dante network, its ports are free and apps cannot use the soundcard.", g_f_sub_c,
         D2D1::RectF(0, kHeader + 100, W, kHeader + 120), kMuted);
    Button& b = g_buttons[0];
    b.id = ID_START;
    b.label = button_label(ID_START, L"Start Virgil");
    b.primary = true;
    b.r = D2D1::RectF(W / 2 - 70, kHeader + 140, W / 2 + 70, kHeader + 172);
    draw_button(b);
  } else {
    // Meter bridge: what arrives from the network (the apps' inputs) on top,
    // what the apps send to the network below. Scales line up on the left.
    const vc::ClockNotice notice = vc::clock_notice(s);
    const float shrink = notice.show ? kNoticeShrink : 0;
    const float mh = kMeterH - shrink, gh = kGroupH - shrink;
    const float my = kHeader + 14 + 34;
    const float bx = std::max(kPad, (W - std::max(group_w(s.tx), group_w(s.rx))) / 2);
    draw_meter_group(bx, my, fmt(L"From network  \u2014  %u channels", s.rx).c_str(), s.rx, g_mrx, now, mh);
    draw_meter_group(bx, my + gh, fmt(L"To network  \u2014  %u channels", s.tx).c_str(), s.tx, g_mtx, now, mh);
    if (notice.show) {
      // "No Dante clock leader found on <adapter> (<ip>)" and the likely fixes.
      const float top = my + gh + mh + 24, bottom = H - kFooter - 24;
      const D2D1_RECT_F box = D2D1::RectF(kPad, top, W - kPad, bottom);
      fill_round(box, 4, kPanel);
      stroke_round(box, 4, kLine);
      g_brush->SetColor(rgb(kWarn));
      g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kPad + 14, top + 15), 4, 4), g_brush);
      text(widen(notice.title), g_f_label, D2D1::RectF(kPad + 26, top + 5, W - kPad - 10, top + 25), kText);
      float y = top + 26;
      for (const std::string& h : notice.hints) {
        text(widen(h), g_f_small_l, D2D1::RectF(kPad + 26, y, W - kPad - 10, y + 15), kMuted);
        y += 15;
      }
    }
    if (!s.error.empty())
      text(widen(s.error), g_f_sub, D2D1::RectF(kPad, H - kFooter - 22, W - kPad, H - kFooter - 4), kBad);
  }

  // Footer.
  fill(D2D1::RectF(0, H - kFooter, W, H), kPanel);
  hline(0, W, H - kFooter + 0.5f, kLine);
  std::wstring apps;
  for (const auto& c : s.clients) {
    if (!apps.empty()) apps += L",  ";
    apps += widen(c.name) + (c.active ? L" (playing)" : L"");
  }
  // Leave room for the footer buttons (4 while running, 2 when stopped).
  const bool stopped_now = s.state == "stopped";
  const float text_r = W - kPad - (stopped_now ? 200 : 352);
  if (stopped_now) apps.clear();
  if (s.reachable)
    text(stopped_now ? L"Stopped" : apps.empty() ? L"No apps connected" : apps, g_f_label,
         D2D1::RectF(kPad, H - kFooter, text_r, H - kFooter / 2 - 2), apps.empty() ? kMuted : kText);
  if (s.reachable) {
    // Version, and how many dropouts the last hour had (details: Dropouts).
    const unsigned long long n = s.dropouts_last_hour;
    text(L"Virgil " + widen(s.version) + L"   \u00b7   " +
             (n ? fmt(L"%llu dropout%s in the last hour", n, n == 1 ? L"" : L"s") : std::wstring(L"no dropouts")),
         g_f_sub, D2D1::RectF(kPad, H - kFooter / 2 - 4, text_r, H - 8), n ? kWarn : kDim);
  }
  if (s.reachable) {
    // Buttons from the right (indices 1..): Stop, Restart, Settings, Dropouts.
    const bool stopped = s.state == "stopped";
    struct B {
      int id;
      const wchar_t* label;
    };
    std::vector<B> right;
    if (!stopped) right = {{ID_STOP, L"Stop"}, {ID_RESTART, L"Restart"}};
    right.push_back({ID_SETTINGS, L"Settings\u2026"});
    right.push_back({ID_DIAG, L"Dropouts\u2026"});
    float bx = W - kPad;
    for (size_t k = 0; k < right.size(); ++k) {
      Button& b = g_buttons[1 + k];
      b.id = right[k].id;
      b.label = button_label(b.id, right[k].label);
      const float w = std::max(64.f, text_width(b.label, g_f_button) + 28);
      b.primary = false;
      b.r = D2D1::RectF(bx - w, H - kFooter + 12, bx, H - 12);
      draw_button(b);
      bx -= w + 8;
    }
  }

  if (g_rt->EndDraw() == HRESULT(D2DERR_RECREATE_TARGET)) discard_device();
}

// ---- service control -------------------------------------------------------------------

void start_portable_daemon() {
  const std::wstring dir = exe_dir();
  const std::wstring daemon = dir + L"\\virgild.exe";
  const std::wstring conf = dir + L"\\virgil.conf";
  std::wstring cmd = L"\"" + daemon + L"\" --log \"" + dir + L"\\virgild.log\" --control-port " +
                     std::to_wstring(g_port);
  if (file_exists(conf)) cmd += L" -c \"" + conf + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, dir.c_str(), &si,
                     &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

bool service_installed() {
  SC_HANDLE m = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!m) return false;
  SC_HANDLE s = OpenServiceW(m, L"Virgil", SERVICE_QUERY_STATUS);
  const bool ok = s != nullptr;
  if (s) CloseServiceHandle(s);
  CloseServiceHandle(m);
  return ok;
}

void start_service() {
  if (service_installed()) {
    // Starting a service needs administrator rights: ask through UAC.
    ShellExecuteW(g_hwnd, L"runas", L"sc.exe", L"start Virgil", nullptr, SW_HIDE);
  } else if (file_exists(exe_dir() + L"\\virgild.exe")) {
    start_portable_daemon();
  } else {
    MessageBoxW(g_hwnd, L"virgild is not installed. Reinstall Virgil.", L"Virgil", MB_ICONWARNING);
  }
}

// Requests to the service run on a worker thread so a slow or hung service
// never freezes the window; the result comes back as WM_APP_DONE.
enum class JobKind { Stop, Start, Restart, LoadSettings, SaveSettings };
struct Job {
  JobKind kind;
  std::string body;      // SaveSettings: the configuration to POST
  vc::HttpResult r, r2;  // LoadSettings: /api/config and /api/interfaces
};

bool run_job(int id, JobKind kind, std::string body = std::string()) {
  if (busy()) return false;
  g_pending = id;
  InvalidateRect(g_hwnd, nullptr, FALSE);
  Job* job = new Job{kind, std::move(body), {}, {}};
  const HWND target = g_hwnd;
  const unsigned port = g_port;
  std::thread([job, target, port] {
    switch (job->kind) {
      case JobKind::Stop: job->r = vc::http(port, "POST", "/api/stop"); break;
      case JobKind::Start: job->r = vc::http(port, "POST", "/api/start"); break;
      case JobKind::Restart: job->r = vc::http(port, "POST", "/api/restart"); break;
      case JobKind::LoadSettings:
        job->r = vc::http(port, "GET", "/api/config");
        if (job->r.status == 200) job->r2 = vc::http(port, "GET", "/api/interfaces");
        break;
      case JobKind::SaveSettings: job->r = vc::http(port, "POST", "/api/config", job->body); break;
    }
    if (!PostMessageW(target, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(job))) delete job;  // window gone
  }).detach();
  return true;
}

// The service's error message from a failed request.
std::wstring request_error(const vc::HttpResult& r) {
  if (!r.status) return L"Could not reach the Virgil service.";
  vc::Json j;
  std::string err = "HTTP " + std::to_string(r.status);
  if (vc::parse_json(r.body, &j)) err = j["error"].str(err);
  return widen(err);
}

void restart_engine() {
  bool reachable;
  {
    std::lock_guard<std::mutex> l(g_mutex);
    reachable = g_status.reachable;
  }
  if (!reachable) return start_service();
  run_job(ID_RESTART, JobKind::Restart);
}

std::wstring log_path() {
  std::string conf;
  {
    std::lock_guard<std::mutex> l(g_mutex);
    conf = g_status.config_path;
  }
  if (!conf.empty()) {
    const std::wstring w = widen(conf);
    const std::wstring p = w.substr(0, w.find_last_of(L"\\/")) + L"\\virgild.log";
    if (file_exists(p)) return p;
  }
  wchar_t pd[MAX_PATH];
  if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, pd)))
    return std::wstring(pd) + L"\\Virgil\\virgild.log";
  return exe_dir() + L"\\virgild.log";
}

// ---- start with Windows --------------------------------------------------------------

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
bool autostart_enabled() {
  wchar_t buf[1024];
  DWORD n = sizeof buf;
  return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Virgil Control", RRF_RT_REG_SZ, nullptr, buf, &n) ==
         ERROR_SUCCESS;
}
void set_autostart(bool on) {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
  if (on) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring v = L"\"" + std::wstring(exe) + L"\" --tray";
    RegSetValueExW(k, L"Virgil Control", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                   DWORD((v.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(k, L"Virgil Control");
  }
  RegCloseKey(k);
}
// First run: start with Windows by default (the tray icon is the app).
void first_run_defaults() {
  HKEY k;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Virgil", 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k,
                      nullptr) != ERROR_SUCCESS)
    return;
  DWORD v = 0, n = sizeof v;
  if (RegQueryValueExW(k, L"Initialized", nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &n) != ERROR_SUCCESS) {
    set_autostart(true);
    v = 1;
    RegSetValueExW(k, L"Initialized", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
  }
  RegCloseKey(k);
}

// ---- tray ---------------------------------------------------------------------------------

std::wstring g_tip;
void tray_add() {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof nid;
  nid.hWnd = g_hwnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_APP_TRAY;
  nid.hIcon = g_icon_small;  // loaded once in wWinMain; re-adding after TaskbarCreated reuses it
  wcsncpy(nid.szTip, g_tip.empty() ? L"Virgil" : g_tip.c_str(), 127);
  Shell_NotifyIconW(NIM_ADD, &nid);
}
void tray_remove() {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof nid;
  nid.hWnd = g_hwnd;
  nid.uID = kTrayId;
  Shell_NotifyIconW(NIM_DELETE, &nid);
}
void tray_tip(const std::wstring& tip) {
  if (tip == g_tip) return;
  g_tip = tip;
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof nid;
  nid.hWnd = g_hwnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_TIP;
  wcsncpy(nid.szTip, tip.c_str(), 127);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}
void tray_balloon(const std::wstring& title, const std::wstring& msg, bool warn) {
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof nid;
  nid.hWnd = g_hwnd;
  nid.uID = kTrayId;
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = warn ? NIIF_WARNING : NIIF_INFO;
  wcsncpy(nid.szInfoTitle, title.c_str(), 63);
  wcsncpy(nid.szInfo, msg.c_str(), 255);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void show_window() {
  ShowWindow(g_hwnd, SW_SHOWNORMAL);
  SetForegroundWindow(g_hwnd);
  g_visible = true;
}
void hide_window() {
  ShowWindow(g_hwnd, SW_HIDE);
  g_visible = false;
  if (!g_told_about_tray) {
    g_told_about_tray = true;
    tray_balloon(L"Virgil is still running", L"Virgil Control stays in the notification area. Right-click the icon for options.", false);
  }
}

// Dropout details and the channel name editor live in the control panel.
void open_page(const wchar_t* anchor) {
  const std::wstring url = L"http://127.0.0.1:" + std::to_wstring(g_port) + L"/" + anchor;
  ShellExecuteW(g_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void tray_menu() {
  vc::Status s;
  {
    std::lock_guard<std::mutex> l(g_mutex);
    s = g_status;
  }
  HMENU m = CreatePopupMenu();
  const ClockView cv = clock_view(s);
  AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (widen(s.reachable ? s.name : "Virgil") + L":  " + cv.line1).c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_OPEN, L"Open Virgil Control");
  const bool stopped = s.reachable && s.state == "stopped";
  const UINT req = busy() ? MF_GRAYED : 0;
  AppendMenuW(m, MF_STRING | (s.reachable && (!busy() || g_settings_dlg) ? 0 : MF_GRAYED), ID_SETTINGS,
              L"Settings\u2026");
  // Like the web panel: nothing to restart while Virgil is stopped.
  AppendMenuW(m, MF_STRING | req | (stopped ? MF_GRAYED : 0), ID_RESTART,
              s.reachable ? L"Restart audio engine" : L"Start Virgil service");
  if (s.reachable)
    AppendMenuW(m, MF_STRING | req, stopped ? ID_START : ID_STOP, stopped ? L"Start Virgil" : L"Stop Virgil");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (s.reachable ? 0 : MF_GRAYED), ID_DIAG, L"Dropouts and diagnostics\u2026");
  AppendMenuW(m, MF_STRING | (s.reachable ? 0 : MF_GRAYED), ID_BROWSER, L"Open control panel in browser");
  AppendMenuW(m, MF_STRING, ID_LOG, L"Open log");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
  AppendMenuW(m, MF_STRING, ID_ABOUT, L"About Virgil\u2026");
  AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit Virgil Control");
  SetMenuDefaultItem(m, ID_OPEN, FALSE);
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(g_hwnd);
  TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
  PostMessageW(g_hwnd, WM_NULL, 0, 0);
  DestroyMenu(m);
}

void command(int id) {
  if (id == ID_SETTINGS && g_settings_dlg) {  // already open: bring it up instead of a second one
    SetForegroundWindow(g_settings_dlg);
    return;
  }
  if (is_request(id) && busy()) return;
  switch (id) {
    case ID_OPEN: show_window(); break;
    case ID_SETTINGS: run_job(ID_SETTINGS, JobKind::LoadSettings); break;  // the dialog opens when loaded
    case ID_DIAG: open_page(L"#diagnostics"); break;
    case ID_BROWSER: open_page(L""); break;
    case ID_RESTART: restart_engine(); break;
    case ID_STOP: {
      static bool asking = false;  // the tray menu stays usable while the question is up
      if (asking) break;
      asking = true;
      const int answer = MessageBoxW(
          g_hwnd,
          L"Audio to and from the Dante network stops, Virgil leaves the network (its ports are "
          L"free, e.g. for Dante Virtual Soundcard) and apps cannot use its soundcard until you "
          L"start it again. It stays stopped after a restart of the computer.",
          L"Stop Virgil?", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
      asking = false;
      if (answer == IDOK) run_job(ID_STOP, JobKind::Stop);
      break;
    }
    case ID_START: run_job(ID_START, JobKind::Start); break;
    case ID_LOG: ShellExecuteW(g_hwnd, L"open", log_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case ID_AUTOSTART: set_autostart(!autostart_enabled()); break;
    case ID_QUIT: DestroyWindow(g_hwnd); break;
    case ID_ABOUT: {
      std::string ver;
      {
        std::lock_guard<std::mutex> l(g_mutex);
        ver = g_status.version;
      }
      const std::wstring msg =
          L"Virgil " + widen(ver) +
          L"\nVirtual Interface Routing Gateway for Inferno-based Low-latency audio\n\n"
          L"Free software under the GNU GPL v3, with no warranty. Network compatibility comes "
          L"from Inferno, an independent open-source project.\n\n"
          L"Virgil is not affiliated with, authorized or endorsed by Audinate. Dante is a "
          L"registered trademark of Audinate Pty Ltd. ASIO is a trademark and software of "
          L"Steinberg Media Technologies GmbH. See NOTICE.md in the install folder.";
      MessageBoxW(g_hwnd, msg.c_str(), L"About Virgil", MB_ICONINFORMATION);
      break;
    }
  }
}

// ---- status polling ------------------------------------------------------------------

std::wstring tray_text(const vc::Status& s) {
  const ClockView cv = clock_view(s);
  const vc::ClockNotice notice = vc::clock_notice(s);
  std::wstring t = L"Virgil \u2014 " + (notice.show ? widen(notice.title) : cv.line1);
  if (s.reachable) {
    unsigned playing = 0;
    for (const auto& c : s.clients) playing += c.active;
    if (playing) t += fmt(L"\n%u %s playing", playing, playing == 1 ? L"app" : L"apps");
  }
  return t.substr(0, 127);
}

// Polls in a row that must fail before the service counts as gone: one lost
// or slow request (e.g. while the engine restarts) is not worth an alarm.
const int kFailuresUntilGone = 3;

void poll_thread() {
  // Not reachable yet: if the service is still starting (login, portable
  // start) the first failures are no news and must not raise a balloon.
  bool was_reachable = false;
  int failures = 0;
  std::string was_clock;
  DWORD lost_since = 0;
  bool lost_told = false;
  bool no_leader_told = false;  // once per run of the engine
  while (!g_quit) {
    const auto r = vc::http(g_port, "GET", "/api/status");
    vc::Status s;
    const bool ok = r.status == 200 && vc::parse_status(r.body, &s);
    failures = ok ? 0 : failures + 1;
    {
      std::lock_guard<std::mutex> l(g_mutex);
      if (ok || failures >= kFailuresUntilGone) {
        // Meters are drained per request by the daemon; keep a frame's worth.
        g_status = ok ? s : vc::Status();
      } else {
        // A missed poll: keep showing the last status, but let the meters fall.
        g_status.meter_tx.clear();
        g_status.meter_rx.clear();
      }
      s = g_status;
    }
    // Notifications on state changes worth knowing about while hidden.
    if (was_reachable && !s.reachable)
      PostMessageW(g_hwnd, WM_APP_STATUS, 1, 0);
    // Not when Virgil was stopped on purpose (or is restarting).
    if (s.reachable && s.state == "running" && was_clock == "ptp-locked" && s.clock != "ptp-locked" && !lost_since)
      lost_since = GetTickCount();
    if (s.state != "running") lost_since = 0;
    if (s.clock == "ptp-locked") lost_since = 0, lost_told = false;
    if (lost_since && !lost_told && GetTickCount() - lost_since > 5000) {
      lost_told = true;
      PostMessageW(g_hwnd, WM_APP_STATUS, 2, 0);
    }
    // No Dante clock leader heard at all (not a lost lock: that is above).
    if ((s.reachable && s.state != "running") || s.clock == "ptp-locked") no_leader_told = false;
    if (!no_leader_told && !lost_since && vc::clock_notice(s).show) {
      no_leader_told = true;
      PostMessageW(g_hwnd, WM_APP_STATUS, 3, 0);
    }
    was_reachable = s.reachable;
    was_clock = s.clock;
    PostMessageW(g_hwnd, WM_APP_STATUS, 0, 0);
    Sleep(g_visible ? 80 : 1000);
  }
}

// ---- settings dialog -------------------------------------------------------------------

// Per dialog, in DWLP_USER (owned by show_settings, which outlives the dialog).
struct SettingsData {
  std::string conf;     // /api/config as loaded; the form is applied onto it
  vc::SettingsForm form;
  bool saving = false;  // POST in flight
};

SettingsData* settings_data(HWND d) { return reinterpret_cast<SettingsData*>(GetWindowLongPtrW(d, DWLP_USER)); }

void combo_add(HWND d, int id, const std::wstring& s) {
  SendDlgItemMessageW(d, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
}
std::wstring get_text(HWND d, int id) {
  wchar_t buf[512];
  GetDlgItemTextW(d, id, buf, 512);
  return buf;
}

// Channel count field: 0..64 (0 = that direction off). False if not a number.
bool read_channels(HWND d, int id, unsigned* out) {
  const std::wstring t = get_text(d, id);
  wchar_t* end = nullptr;
  const unsigned long n = std::wcstoul(t.c_str(), &end, 10);
  if (t.empty() || end == t.c_str() || *end || n > 64) return false;
  *out = unsigned(n);
  return true;
}

// Latency field in ms. Accepts ',' as the decimal separator ("3,5") as well as
// '.', and hands the model a '.' string (it parses in the C locale).
bool read_ms(HWND d, int id, std::string* out) {
  std::wstring t = get_text(d, id);
  std::replace(t.begin(), t.end(), L',', L'.');
  while (!t.empty() && t.back() == L' ') t.pop_back();
  while (!t.empty() && t.front() == L' ') t.erase(t.begin());
  wchar_t* end = nullptr;
  const double v = std::wcstod(t.c_str(), &end);
  if (t.empty() || end == t.c_str() || *end || !(v > 0)) return false;
  *out = narrow(t);
  return true;
}

void settings_error(HWND d, const std::wstring& msg) { SetDlgItemTextW(d, IDC_ERR, msg.c_str()); }

INT_PTR CALLBACK settings_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG: {
      SetWindowLongPtrW(d, DWLP_USER, lp);
      g_settings_dlg = d;
      const SettingsData& data = *reinterpret_cast<SettingsData*>(lp);
      const vc::SettingsForm& f = data.form;
      if (g_icon_small) SendMessageW(d, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_icon_small));
      SetDlgItemTextW(d, IDC_NAME, widen(f.name).c_str());
      SendDlgItemMessageW(d, IDC_NAME, EM_LIMITTEXT, 31, 0);
      // Includes a "not connected" entry for a configured interface that is
      // missing right now, so Apply keeps it instead of resetting to Automatic.
      for (const auto& l : f.iface_labels()) combo_add(d, IDC_IFACE, widen(l));
      SendDlgItemMessageW(d, IDC_IFACE, CB_SETCURSEL, WPARAM(f.iface_index), 0);
      for (const char* l : vc::kRateLabels) combo_add(d, IDC_RATE, widen(l));
      SendDlgItemMessageW(d, IDC_RATE, CB_SETCURSEL, WPARAM(f.rate_index), 0);
      SendDlgItemMessageW(d, IDC_TXSPIN, UDM_SETRANGE32, 0, 64);
      SendDlgItemMessageW(d, IDC_RXSPIN, UDM_SETRANGE32, 0, 64);
      SetDlgItemTextW(d, IDC_TXCH, std::to_wstring(f.tx_channels).c_str());
      SetDlgItemTextW(d, IDC_RXCH, std::to_wstring(f.rx_channels).c_str());
      for (const char* v : vc::kRxLatencies) combo_add(d, IDC_RXLAT, widen(v));
      for (const char* v : vc::kTxLatencies) combo_add(d, IDC_TXLAT, widen(v));
      SetDlgItemTextW(d, IDC_RXLAT, widen(f.rx_latency_ms).c_str());
      SetDlgItemTextW(d, IDC_TXLAT, widen(f.tx_latency_ms).c_str());
      combo_add(d, IDC_CLOCK, L"Follow the Dante clock leader (PTP)");
      combo_add(d, IDC_CLOCK, L"Local clock only (testing)");
      SendDlgItemMessageW(d, IDC_CLOCK, CB_SETCURSEL, f.local_clock ? 1 : 0, 0);
      CheckDlgButton(d, IDC_MASTER, f.master_capable ? BST_CHECKED : BST_UNCHECKED);
      std::string path;
      {
        std::lock_guard<std::mutex> l(g_mutex);
        path = g_status.config_path;
      }
      if (!path.empty()) SetDlgItemTextW(d, IDC_CONFPATH, (L"Configuration: " + widen(path)).c_str());
      return TRUE;
    }
    case WM_DESTROY:
      if (g_settings_dlg == d) g_settings_dlg = nullptr;
      break;
    case WM_CTLCOLORSTATIC:
      if (GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == IDC_ERR) {
        SetTextColor(reinterpret_cast<HDC>(wp), RGB(190, 40, 30));
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
      }
      break;
    case WM_APP_SAVED: {  // from the main window: our POST finished
      SettingsData* data = settings_data(d);
      const Job* job = reinterpret_cast<const Job*>(lp);
      if (!data || !job) return TRUE;
      data->saving = false;
      if (job->r.status == 200) {
        EndDialog(d, 1);
      } else {
        settings_error(d, request_error(job->r));
        EnableWindow(GetDlgItem(d, IDOK), TRUE);
      }
      return TRUE;
    }
    case WM_COMMAND: {
      SettingsData* data = settings_data(d);
      if (!data) break;
      if (LOWORD(wp) == IDC_NAMES) {  // the editor lives in the control panel
        open_page(L"#channels");
        EndDialog(d, 0);
        return TRUE;
      }
      if (LOWORD(wp) == IDCANCEL) {  // a POST in flight still completes; its result is dropped
        EndDialog(d, 0);
        return TRUE;
      }
      if (LOWORD(wp) == IDOK) {
        if (data->saving) return TRUE;
        vc::SettingsForm f = data->form;
        f.name = narrow(get_text(d, IDC_NAME));
        f.iface_index = std::max(0, int(SendDlgItemMessageW(d, IDC_IFACE, CB_GETCURSEL, 0, 0)));
        f.rate_index = int(SendDlgItemMessageW(d, IDC_RATE, CB_GETCURSEL, 0, 0));
        if (!read_channels(d, IDC_TXCH, &f.tx_channels) || !read_channels(d, IDC_RXCH, &f.rx_channels)) {
          settings_error(d, L"Enter a channel count from 0 to 64 for each direction.");
          return TRUE;
        }
        if (!read_ms(d, IDC_RXLAT, &f.rx_latency_ms) || !read_ms(d, IDC_TXLAT, &f.tx_latency_ms)) {
          settings_error(d, L"Enter the latency in milliseconds, for example 4 or 3.5.");
          return TRUE;
        }
        f.local_clock = SendDlgItemMessageW(d, IDC_CLOCK, CB_GETCURSEL, 0, 0) == 1;
        f.master_capable = IsDlgButtonChecked(d, IDC_MASTER) == BST_CHECKED;
        if (!run_job(ID_SETTINGS, JobKind::SaveSettings, vc::apply_settings(data->conf, f))) {
          settings_error(d, L"Virgil is busy with another request. Try again in a moment.");
          return TRUE;
        }
        data->saving = true;
        EnableWindow(GetDlgItem(d, IDOK), FALSE);
        settings_error(d, L"Applying…");
        return TRUE;
      }
      break;
    }
  }
  return FALSE;
}

// Opens the (modal) dialog with what LoadSettings fetched.
void show_settings(const std::string& conf, const std::vector<vc::Interface>& ifaces) {
  if (g_settings_dlg) {
    SetForegroundWindow(g_settings_dlg);
    return;
  }
  SettingsData data;
  data.conf = conf;
  data.form = vc::load_settings(conf, ifaces);
  // An explicit 0 (direction off) is a valid setting; show it as such.
  if (vc::conf_get(conf, "device", "tx_channels") == "0") data.form.tx_channels = 0;
  if (vc::conf_get(conf, "device", "rx_channels") == "0") data.form.rx_channels = 0;
  DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), IsWindowVisible(g_hwnd) ? g_hwnd : nullptr,
                  settings_proc, reinterpret_cast<LPARAM>(&data));
  g_settings_dlg = nullptr;
}

// ---- window --------------------------------------------------------------------------------

UINT dpi_for(HWND h) {
  using Fn = UINT(WINAPI*)(HWND);
  static Fn fn = reinterpret_cast<Fn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
  return fn && h ? fn(h) : 96;
}

// The frame for a client rect at the window's DPI. AdjustWindowRectEx uses the
// system DPI, which is wrong on other monitors under PerMonitorV2;
// AdjustWindowRectExForDpi (Windows 10 1607+) is looked up at run time.
void adjust_for_dpi(RECT* r, DWORD style, DWORD ex_style, UINT dpi) {
  using Fn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
  static Fn fn = reinterpret_cast<Fn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi")));
  if (!fn || !fn(r, style, FALSE, ex_style, dpi)) AdjustWindowRectEx(r, style, FALSE, ex_style);
}

void resize_to_content(HWND h) {
  vc::Status s;
  {
    std::lock_guard<std::mutex> l(g_mutex);
    s = g_status;
  }
  const float scale = g_dpi / 96.f;
  RECT r{0, 0, LONG(window_w(s) * scale), LONG(window_h() * scale)};
  adjust_for_dpi(&r, DWORD(GetWindowLongW(h, GWL_STYLE)), DWORD(GetWindowLongW(h, GWL_EXSTYLE)), UINT(g_dpi));
  RECT cur;
  GetWindowRect(h, &cur);
  if (cur.right - cur.left != r.right - r.left || cur.bottom - cur.top != r.bottom - r.top)
    SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// The enabled button under a client point (pixels), as its command id (0: none).
int hit(LPARAM lp) {
  const float x = GET_X_LPARAM(lp) * 96.f / g_dpi, y = GET_Y_LPARAM(lp) * 96.f / g_dpi;
  for (const auto& b : g_buttons)
    if (b.visible && !b.disabled && x >= b.r.left && x <= b.r.right && y >= b.r.top && y <= b.r.bottom)
      return b.id;
  return 0;
}

// A request from run_job finished (UI thread).
void job_done(std::unique_ptr<Job> job) {
  g_pending = 0;
  InvalidateRect(g_hwnd, nullptr, FALSE);
  switch (job->kind) {
    case JobKind::Stop:
    case JobKind::Start:
    case JobKind::Restart:
      if (job->r.status != 200) {
        const wchar_t* what = job->kind == JobKind::Stop    ? L"Could not stop Virgil"
                              : job->kind == JobKind::Start ? L"Could not start Virgil"
                                                            : L"Could not restart the audio engine";
        tray_balloon(what, request_error(job->r), true);
      }
      break;
    case JobKind::LoadSettings:
      if (job->r.status != 200) {
        MessageBoxW(g_hwnd, L"The Virgil service is not running, so its settings cannot be changed now.",
                    L"Virgil", MB_ICONINFORMATION);
        break;
      }
      // Modal: runs its own message loop until the dialog closes.
      show_settings(job->r.body, vc::parse_interfaces(job->r2.body));
      break;
    case JobKind::SaveSettings:
      if (g_settings_dlg) SendMessageW(g_settings_dlg, WM_APP_SAVED, 0, reinterpret_cast<LPARAM>(job.get()));
      break;
  }
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == g_taskbar_created && g_taskbar_created) {
    tray_add();
    return 0;
  }
  switch (msg) {
    case WM_CREATE: {
      g_dpi = float(dpi_for(h));
      BOOL dark = TRUE;
      DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
      SetTimer(h, 1, 33, nullptr);
      return 0;
    }
    case WM_TIMER:
      if (g_visible) InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_APP_STATUS: {
      vc::Status s;
      {
        std::lock_guard<std::mutex> l(g_mutex);
        s = g_status;
      }
      tray_tip(tray_text(s));
      if (wp == 1) tray_balloon(L"Virgil service stopped", L"Dante audio from this computer has stopped.", true);
      if (wp == 2) tray_balloon(L"Dante clock lost", L"Virgil is no longer locked to the Dante clock leader.", true);
      if (wp == 3) {
        const vc::ClockNotice n = vc::clock_notice(s);
        if (n.show) {
          // As many whole hints as the balloon holds (255 characters).
          std::wstring msg = widen(n.title) + L".";
          for (const std::string& h : n.hints)
            if (msg.size() + 1 + widen(h).size() <= 255) msg += L" " + widen(h);
          tray_balloon(widen(n.headline), msg, true);
        }
      }
      if (g_visible) resize_to_content(h);
      return 0;
    }
    case WM_APP_TRAY:
      switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT:
          if (IsWindowVisible(h)) hide_window();
          else show_window();
          return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
          tray_menu();
          return 0;
      }
      return 0;
    case WM_APP_SHOW:
      show_window();
      return 0;
    case WM_APP_DONE:
      job_done(std::unique_ptr<Job>(reinterpret_cast<Job*>(lp)));
      return 0;
    case WM_COMMAND:
      command(LOWORD(wp));
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(h, &ps);
      paint(h);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
      return 0;
    case WM_DPICHANGED: {
      g_dpi = float(HIWORD(wp));
      discard_device();
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      g_hover_id = hit(lp);
      SetCursor(LoadCursorW(nullptr, g_hover_id ? IDC_HAND : IDC_ARROW));
      TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
      TrackMouseEvent(&t);
      return 0;
    }
    case WM_MOUSELEAVE:
      g_hover_id = 0;
      if (GetCapture() != h) g_press_id = 0;
      return 0;
    case WM_LBUTTONDOWN:
      g_press_id = hit(lp);  // the command pressed, whatever the buttons become meanwhile
      SetCapture(h);
      return 0;
    case WM_LBUTTONUP: {
      const int pressed = g_press_id;
      g_press_id = 0;
      ReleaseCapture();
      // Only if released over a button that still does what was pressed.
      if (pressed && hit(lp) == pressed) command(pressed);
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (reinterpret_cast<HWND>(lp) != h) g_press_id = 0;
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) return TRUE;  // set in WM_MOUSEMOVE
      break;
    case WM_CLOSE:
      hide_window();
      return 0;
    case WM_DESTROY:
      g_quit = true;
      tray_remove();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdline, int) {
  g_inst = inst;
  const std::wstring args = cmdline ? cmdline : L"";
  const bool tray_only = args.find(L"--tray") != std::wstring::npos;
  const size_t pp = args.find(L"--port ");
  if (pp != std::wstring::npos) g_port = unsigned(_wtoi(args.c_str() + pp + 7));

  // One instance: a second launch just brings the window up.
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\VirgilControl");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(kWindowClass, nullptr))
      if (!tray_only) PostMessageW(other, WM_APP_SHOW, 0, 0);
    return 0;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS};
  InitCommonControlsEx(&icc);
  create_device_independent();
  first_run_defaults();

  // Portable download: start virgild from this folder if nothing answers.
  if (vc::http(g_port, "GET", "/api/status").status != 200 && !service_installed() &&
      file_exists(exe_dir() + L"\\virgild.exe"))
    start_portable_daemon();

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = inst;
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_VIRGIL));
  // One small icon for the window, the tray (also re-added after Explorer
  // restarts) and the settings dialog; destroyed at exit.
  g_icon_small = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_VIRGIL), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
  wc.hIconSm = g_icon_small;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kWindowClass;
  RegisterClassExW(&wc);
  g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r{0, 0, LONG(kW), LONG(window_h())};
  AdjustWindowRectEx(&r, style, FALSE, 0);
  g_hwnd = CreateWindowExW(0, kWindowClass, L"Virgil", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                           r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  // The size above is in 96-DPI pixels; now that the window has its monitor's
  // DPI (g_dpi, from WM_CREATE), size it for that.
  resize_to_content(g_hwnd);
  tray_add();
  std::thread poller(poll_thread);
  if (!tray_only) show_window();

  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  g_quit = true;
  poller.join();
  discard_device();
  if (g_icon_small) DestroyIcon(g_icon_small);
  CloseHandle(single);
  CoUninitialize();
  return 0;
}
