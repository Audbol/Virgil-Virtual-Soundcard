// Virgil Control for Windows: tray icon, status window with level meters, a
// settings dialog, and the Dropouts and diagnostics, Channel names and Edit
// configuration windows. It talks to virgild over its local control API
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
#include <commdlg.h>
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
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api.h"
#include "config_model.h"
#include "desktop_audio.h"
#include "desktop_audio_ui.h"
#include "diagnostics_model.h"
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
           WM_APP_SAVED = WM_APP + 5,  // to the settings dialog: its POST finished (lParam: Job*)
           // Results of the panels' requests (lParam: the result, owned by the receiver).
           WM_APP_DIAG = WM_APP + 20, WM_APP_REPORT = WM_APP + 21, WM_APP_CLEARED = WM_APP + 22,
           WM_APP_NAMES = WM_APP + 23, WM_APP_NAMES_SAVED = WM_APP + 24, WM_APP_CONF = WM_APP + 25,
           WM_APP_CONF_SAVED = WM_APP + 26;
const UINT kTrayId = 1;
enum {
  ID_OPEN = 100, ID_SETTINGS, ID_RESTART, ID_LOG, ID_AUTOSTART, ID_QUIT, ID_ABOUT, ID_DIAG, ID_STOP, ID_START,
  ID_DESKTOP, ID_NAMES, ID_EDITCONF
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
// Desktop audio (Windows apps <-> Dante), running for as long as this app.
std::unique_ptr<da::Engine> g_desktop;
// The modeless windows, one each (UI thread only); the message loop hands
// them their keyboard input (IsDialogMessage).
HWND g_diag_dlg, g_names_dlg, g_conf_dlg;
void show_diagnostics();
void show_channel_names();
void show_config_editor();
UINT dpi_for(HWND h);

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

const float kW = 700, kHeader = 76, kFooter = 56, kMeterH = 150, kPad = 22;

float strip_w(unsigned n) {
  // Strips get narrower with many channels so the window stays on screen.
  const unsigned total = std::max(1u, n);
  if (total <= 16) return 20;
  if (total <= 32) return 12;
  if (total <= 64) return 7;
  return 4;  // up to 128: keeps the window under about 850 pixels wide
}
float strip_gap(float w) { return w >= 12 ? 6 : w >= 7 ? 3 : 2; }
float group_w(unsigned n) {
  const float w = strip_w(n), gap = strip_gap(w);
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
  const float sw = strip_w(n), gap = strip_gap(sw), scale_w = 34;
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
    if (n <= 32 || c % (n <= 64 ? 4 : 8) == 0)
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
  // Buttons from the right (indices 1..): Stop, Restart, Settings, Desktop
  // audio (Dropouts and diagnostics are in Settings and the tray menu); laid out first so the text on the left knows its room.
  const bool stopped_now = s.state == "stopped";
  float text_r = W - kPad;
  if (s.reachable) {
    struct B {
      int id;
      const wchar_t* label;
    };
    std::vector<B> right;
    if (!stopped_now) right = {{ID_STOP, L"Stop"}, {ID_RESTART, L"Restart"}};
    right.push_back({ID_SETTINGS, L"Settings\u2026"});
    right.push_back({ID_DESKTOP, L"Desktop audio\u2026"});
    float bx = W - kPad;
    for (size_t k = 0; k < right.size() && 1 + k < g_buttons.size(); ++k) {
      Button& b = g_buttons[1 + k];
      b.id = right[k].id;
      b.label = button_label(b.id, right[k].label);
      const float w = std::max(64.f, text_width(b.label, g_f_button) + 28);
      b.primary = false;
      b.r = D2D1::RectF(bx - w, H - kFooter + 12, bx, H - 12);
      draw_button(b);
      bx -= w + 8;
    }
    text_r = bx - 4;
  }
  std::wstring apps;
  for (const auto& c : s.clients) {
    if (!apps.empty()) apps += L",  ";
    apps += widen(c.name) + (c.active ? L" (playing)" : L"");
  }
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
  vc::HttpResult r, r2, r3;  // LoadSettings: /api/config, /api/interfaces and /api/status
};

bool run_job(int id, JobKind kind, std::string body = std::string()) {
  if (busy()) return false;
  g_pending = id;
  InvalidateRect(g_hwnd, nullptr, FALSE);
  Job* job = new Job{kind, std::move(body), {}, {}, {}};
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
        if (job->r.status == 200) job->r3 = vc::http(port, "GET", "/api/status");  // remote access URLs
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
  AppendMenuW(m, MF_STRING, ID_DESKTOP, L"Desktop audio\u2026");
  // Like the web panel: nothing to restart while Virgil is stopped.
  AppendMenuW(m, MF_STRING | req | (stopped ? MF_GRAYED : 0), ID_RESTART,
              s.reachable ? L"Restart audio engine" : L"Start Virgil service");
  if (s.reachable)
    AppendMenuW(m, MF_STRING | req, stopped ? ID_START : ID_STOP, stopped ? L"Start Virgil" : L"Stop Virgil");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (s.reachable ? 0 : MF_GRAYED), ID_DIAG, L"Dropouts and diagnostics\u2026");
  AppendMenuW(m, MF_STRING | (s.reachable ? 0 : MF_GRAYED), ID_EDITCONF, L"Edit configuration\u2026");
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

// Virgil's channels as the service reports them, for the Desktop audio window.
da::ChannelInfo desktop_channels() {
  std::lock_guard<std::mutex> l(g_mutex);
  da::ChannelInfo c;
  if (!g_status.reachable) return c;
  c.tx = g_status.tx;
  c.rx = g_status.rx;
  c.tx_names = g_status.tx_names;
  c.rx_names = g_status.rx_names;
  return c;
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
    case ID_DIAG: show_diagnostics(); break;
    case ID_NAMES: show_channel_names(); break;
    case ID_EDITCONF: show_config_editor(); break;
    case ID_DESKTOP:
      // Modal; a second request brings the open window up.
      if (g_desktop) da::show_dialog(g_inst, IsWindowVisible(g_hwnd) ? g_hwnd : nullptr, g_icon_small, *g_desktop,
                                     desktop_channels);
      break;
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
  // From /api/status "remote": the web panel listens for other computers, at these URLs.
  bool remote_listening = false;
  std::vector<std::string> remote_urls;
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

// Channel count field: 0..128 (0 = that direction off). False if not a number.
bool read_channels(HWND d, int id, unsigned* out) {
  const std::wstring t = get_text(d, id);
  wchar_t* end = nullptr;
  const unsigned long n = std::wcstoul(t.c_str(), &end, 10);
  if (t.empty() || end == t.c_str() || *end || n > 128) return false;
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

// The password field follows the checkbox; below them, where other computers
// find the panel while it listens, and the warning that goes with it.
void settings_remote_update(HWND d, const SettingsData& data) {
  const bool on = IsDlgButtonChecked(d, IDC_REMOTE) == BST_CHECKED;
  EnableWindow(GetDlgItem(d, IDC_REMOTE_PW), on);
  std::wstring info;
  if (data.remote_listening) {
    std::wstring urls;
    for (const std::string& u : data.remote_urls) urls += (urls.empty() ? L"" : L"   ") + widen(u);
    info = urls.empty() ? L"Listening for other computers." : L"Open " + urls;
  }
  if (data.remote_listening || on)
    info += (info.empty() ? L"" : L"\n") + std::wstring(L"Plain HTTP: the password crosses the network unencrypted.");
  SetDlgItemTextW(d, IDC_REMOTE_INFO, info.c_str());
}

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
      SendDlgItemMessageW(d, IDC_TXSPIN, UDM_SETRANGE32, 0, 128);
      SendDlgItemMessageW(d, IDC_RXSPIN, UDM_SETRANGE32, 0, 128);
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
      CheckDlgButton(d, IDC_REMOTE, f.remote_access ? BST_CHECKED : BST_UNCHECKED);
      SendDlgItemMessageW(d, IDC_REMOTE_PW, EM_LIMITTEXT, 128, 0);
      SetDlgItemTextW(d, IDC_REMOTE_PW, widen(f.remote_password).c_str());
      settings_remote_update(d, data);
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
      // These windows save the configuration file themselves: the dialog
      // closes first, so it never applies its (then stale) copy over theirs.
      // show_settings opens the window the returned command names.
      if (LOWORD(wp) == IDC_NAMES || LOWORD(wp) == IDC_DIAGBTN || LOWORD(wp) == IDC_EDITCONF) {
        EndDialog(d, LOWORD(wp) == IDC_NAMES ? ID_NAMES : LOWORD(wp) == IDC_DIAGBTN ? ID_DIAG : ID_EDITCONF);
        return TRUE;
      }
      if (LOWORD(wp) == IDC_REMOTE) {
        settings_remote_update(d, *data);
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
          settings_error(d, L"Enter a channel count from 0 to 128 for each direction.");
          return TRUE;
        }
        if (!read_ms(d, IDC_RXLAT, &f.rx_latency_ms) || !read_ms(d, IDC_TXLAT, &f.tx_latency_ms)) {
          settings_error(d, L"Enter the latency in milliseconds, for example 4 or 3.5.");
          return TRUE;
        }
        f.local_clock = SendDlgItemMessageW(d, IDC_CLOCK, CB_GETCURSEL, 0, 0) == 1;
        f.master_capable = IsDlgButtonChecked(d, IDC_MASTER) == BST_CHECKED;
        f.remote_access = IsDlgButtonChecked(d, IDC_REMOTE) == BST_CHECKED;
        f.remote_password = narrow(get_text(d, IDC_REMOTE_PW));
        const std::string remote_problem = vc::remote_access_problem(f);
        if (!remote_problem.empty()) {
          settings_error(d, widen(remote_problem));
          EnableWindow(GetDlgItem(d, IDC_REMOTE_PW), TRUE);  // even with remote access off: let it be fixed
          SetFocus(GetDlgItem(d, IDC_REMOTE_PW));
          return TRUE;
        }
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
void show_settings(const std::string& conf, const std::vector<vc::Interface>& ifaces, const std::string& status) {
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
  // vc::Status leaves out the "remote" block; read it here.
  vc::Json j;
  if (vc::parse_json(status, &j)) {
    const vc::Json& remote = j["remote"];
    data.remote_listening = remote["listening"].boolean();
    const vc::Json& urls = remote["urls"];
    for (size_t i = 0; i < urls.size(); ++i)
      if (!urls[i].str().empty()) data.remote_urls.push_back(urls[i].str());
  }
  const INT_PTR r = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS),
                                    IsWindowVisible(g_hwnd) ? g_hwnd : nullptr, settings_proc,
                                    reinterpret_cast<LPARAM>(&data));
  g_settings_dlg = nullptr;
  if (r == ID_NAMES || r == ID_DIAG || r == ID_EDITCONF) command(int(r));
}

// ---- panels: shared helpers --------------------------------------------------------------
//
// Dropouts and diagnostics, Channel names and Edit configuration: modeless
// dialogs in the style of the Settings and Desktop audio windows, one
// instance each. Their requests run on worker threads (run_async) and come
// back as messages, so a slow service never freezes them.

const COLORREF kErrText = RGB(190, 40, 30), kOkText = RGB(30, 120, 50);
enum StatusKind { kStatusPlain, kStatusError, kStatusOk };

// Runs work(port, result) on a worker thread and posts the result to
// `target` as `msg` (lParam: T*, which the receiver deletes). Dropped if the
// window is gone by then.
template <class T, class F>
void run_async(HWND target, UINT msg, F work) {
  const unsigned port = g_port;
  std::thread([target, msg, port, work] {
    std::unique_ptr<T> r(new T());
    work(port, *r);
    if (PostMessageW(target, msg, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
  }).detach();
}

bool bring_up(HWND d) {
  if (!d) return false;
  ShowWindow(d, IsIconic(d) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(d);
  return true;
}

// Keyboard input for the modeless windows (Tab, Enter, Esc).
bool panel_dialog_message(MSG* m) {
  for (HWND d : {g_diag_dlg, g_names_dlg, g_conf_dlg})
    if (d && IsDialogMessageW(d, m)) return true;
  return false;
}

int dlu_x(HWND d, int v) {
  RECT r{0, 0, v, 0};
  MapDialogRect(d, &r);
  return r.right;
}
int dlu_y(HWND d, int v) {
  RECT r{0, 0, 0, v};
  MapDialogRect(d, &r);
  return r.bottom;
}
void place(HDWP& dwp, HWND d, int id, int x, int y, int w, int h) {
  HWND c = GetDlgItem(d, id);
  if (c && dwp) dwp = DeferWindowPos(dwp, c, nullptr, x, y, std::max(0, w), std::max(0, h), SWP_NOZORDER | SWP_NOACTIVATE);
}

std::wstring window_text(HWND h) {
  const int n = GetWindowTextLengthW(h);
  if (n <= 0) return {};
  std::wstring s(size_t(n) + 1, L'\0');
  const int got = GetWindowTextW(h, &s[0], n + 1);
  s.resize(size_t(std::max(0, got)));
  return s;
}
// "\r\n" -> "\n" (edit controls use CRLF; virgil.conf and the API use LF).
std::string to_lf(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i)
    if (!(s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n')) o += s[i];
  return o;
}
template <class S>
S to_crlf(const S& s) {
  S o;
  o.reserve(s.size() + s.size() / 16);
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) o += '\r';
    o += s[i];
  }
  return o;
}

// A GDI font like the dialog's, in another face / weight.
HFONT font_like(HWND d, const wchar_t* face, int weight) {
  LOGFONTW lf{};
  HFONT base = reinterpret_cast<HFONT>(SendMessageW(d, WM_GETFONT, 0, 0));
  if (!base || !GetObjectW(base, sizeof lf, &lf)) {
    lf.lfHeight = -MulDiv(9, int(dpi_for(d)), 72);
    wcsncpy(lf.lfFaceName, L"Segoe UI", LF_FACESIZE - 1);
  }
  if (face) {
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    lf.lfFaceName[LF_FACESIZE - 1] = 0;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
  }
  lf.lfWeight = weight;
  lf.lfQuality = CLEARTYPE_QUALITY;
  return CreateFontIndirectW(&lf);
}

void set_icon(HWND d) {
  if (g_icon_small) SendMessageW(d, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_icon_small));
  if (HICON big = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_VIRGIL)))
    SendMessageW(d, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
}

// The window's size when it opened (its template) is its smallest size.
void min_track(HWND d, LPARAM lp, const SIZE& min) {
  if (min.cx <= 0) return;
  auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
  mm->ptMinTrackSize.x = min.cx;
  mm->ptMinTrackSize.y = min.cy;
  (void)d;
}
SIZE window_size(HWND d) {
  RECT r;
  GetWindowRect(d, &r);
  return SIZE{r.right - r.left, r.bottom - r.top};
}

// Status line colours (WM_CTLCOLORSTATIC), as in the Desktop audio window.
INT_PTR status_color(WPARAM wp, int kind) {
  SetTextColor(reinterpret_cast<HDC>(wp), kind == kStatusError ? kErrText
                                          : kind == kStatusOk  ? kOkText
                                                               : GetSysColor(COLOR_WINDOWTEXT));
  SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
  return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
}

// Save As (comdlg32 is loaded when needed, so the link line stays as it is).
bool ask_save_path(HWND owner, const std::wstring& suggested, std::wstring* out) {
  using Fn = BOOL(WINAPI*)(LPOPENFILENAMEW);
  static Fn fn = [] {
    HMODULE m = LoadLibraryW(L"comdlg32.dll");
    return m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, "GetSaveFileNameW"))) : Fn(nullptr);
  }();
  if (!fn) return false;
  std::vector<wchar_t> buf(32768, L'\0');
  wcsncpy(buf.data(), suggested.c_str(), buf.size() - 1);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = DWORD(buf.size());
  ofn.lpstrDefExt = L"txt";
  ofn.lpstrTitle = L"Save diagnostics report";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;
  if (!fn(&ofn)) return false;
  *out = buf.data();
  return true;
}
bool write_file(const std::wstring& path, const std::string& bytes) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD n = 0;
  const bool ok = WriteFile(f, bytes.data(), DWORD(bytes.size()), &n, nullptr) && n == bytes.size();
  return CloseHandle(f) && ok;
}

D2D1_COLOR_F sys_color(int idx) {
  const COLORREF c = GetSysColor(idx);
  return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, 1.f);
}

// ---- Dropouts and diagnostics ------------------------------------------------------------

const wchar_t* kDiagSummaryClass = L"VirgilDiagSummary";
const float kPillH = 24, kChartH = 64, kChartPad = 6;

struct DiagRow {
  std::wstring time, text;
  int image = 2;  // dot: 0 amber (Warn), 1 red (Error), 2 grey (Info)
};

struct DiagState {
  vc::Diagnostics d;
  bool have = false;      // d holds a reply
  std::string error;      // the last refresh failed with this
  bool fetching = false, again = false, saving = false, clearing = false;
  std::vector<DiagRow> rows;
  std::wstring status;
  int status_kind = kStatusPlain;
  bool status_is_fetch = false;  // the status line shows a failed refresh
  SIZE min{};
  int summary_h = -1;     // pixels, as laid out
  HFONT mono = nullptr, normal = nullptr;
  // The summary (headline, chart, legend, hints), drawn with Direct2D.
  ID2D1DCRenderTarget* rt = nullptr;
  ID2D1SolidColorBrush* brush = nullptr;
  IDWriteTextFormat *f_head = nullptr, *f_text = nullptr, *f_small = nullptr;
  D2D1_RECT_F chart{};    // chart area in the summary, DIPs
  int hover = -1;         // minute under the mouse
  HWND tip = nullptr;
  bool tracking = false;
};
DiagState* g_dg;

void diag_release_d2d(DiagState& st) {
  release(st.brush);
  release(st.rt);
}

IDWriteTextFormat* diag_wrap_format(float size) {
  IDWriteTextFormat* f = make_format(L"ui", size, DWRITE_FONT_WEIGHT_NORMAL);
  if (f) {
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  }
  return f;
}
IDWriteTextLayout* text_layout(const std::wstring& s, IDWriteTextFormat* f, float w) {
  IDWriteTextLayout* l = nullptr;
  if (f) g_dw->CreateTextLayout(s.c_str(), UINT32(s.size()), f, std::max(1.f, w), 10000.f, &l);
  return l;
}

// Lays out the summary `W` DIPs wide and returns its height; draws it too
// when `rt` is set.
float diag_summary(DiagState& st, ID2D1RenderTarget* rt, float W) {
  if (!st.f_head) {
    st.f_head = make_format(L"ui", 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    st.f_text = diag_wrap_format(12.f);
    st.f_small = make_format(L"ui", 11.5f, DWRITE_FONT_WEIGHT_NORMAL);
  }
  ID2D1SolidColorBrush* b = st.brush;
  const D2D1_COLOR_F text_c = sys_color(COLOR_WINDOWTEXT), muted_c = sys_color(COLOR_GRAYTEXT);
  auto draw_text = [&](const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    if (!rt || !f || s.empty()) return;
    b->SetColor(c);
    rt->DrawText(s.c_str(), UINT32(s.size()), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
  };
  const vc::Diagnostics& d = st.d;

  // Headline pill, and "since ..." beside it.
  std::wstring head;
  unsigned pill = kMuted;
  if (st.have) {
    head = widen(d.headline);
    pill = d.headline_severity == vc::Severity::Ok      ? kOk
           : d.headline_severity == vc::Severity::Error ? kBad
                                                        : kWarn;
  } else {
    head = st.error.empty() ? L"Loading…" : L"No answer from the Virgil service";
    pill = st.error.empty() ? kMuted : kBad;
  }
  const float pw = text_width(head, st.f_head) + 26;
  if (rt) {
    b->SetColor(rgb(pill));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, pw, kPillH), kPillH / 2, kPillH / 2), b);
    draw_text(head, st.f_head, D2D1::RectF(0, 0, pw, kPillH), rgb(pill == kBad ? 0xFFFFFF : 0x15171A));
    if (st.have) draw_text(widen(d.since_text), st.f_small, D2D1::RectF(pw + 10, 0, W, kPillH), muted_c);
  }
  float y = kPillH + 10;

  // The last hour, per minute: stacked bars on a panel in the app's colours.
  const D2D1_RECT_F panel = D2D1::RectF(0, y, W, y + kChartH + 2 * kChartPad);
  st.chart = D2D1::RectF(panel.left + kChartPad, panel.top + kChartPad, panel.right - kChartPad, panel.bottom - kChartPad);
  const float cw = st.chart.right - st.chart.left, ch = st.chart.bottom - st.chart.top;
  if (rt) {
    b->SetColor(rgb(kBg));
    rt->FillRoundedRectangle(D2D1::RoundedRect(panel, 4, 4), b);
    if (st.hover >= 0 && st.hover < int(d.minutes.size())) {
      const float mw = cw / float(vc::kDiagMinutes);
      b->SetColor(rgb(kLine));
      rt->FillRectangle(D2D1::RectF(st.chart.left + float(st.hover) * mw, st.chart.top,
                                    st.chart.left + float(st.hover + 1) * mw, st.chart.bottom), b);
    }
    b->SetColor(rgb(kSegOff));
    rt->FillRectangle(D2D1::RectF(st.chart.left, st.chart.bottom - 1, st.chart.right, st.chart.bottom), b);
    for (const vc::DiagBar& bar : vc::diag_chart_layout(d, cw, ch, 2)) {
      b->SetColor(rgb(vc::diag_category_color(bar.category)));
      rt->FillRectangle(D2D1::RectF(st.chart.left + bar.x, st.chart.top + bar.y, st.chart.left + bar.x + bar.w,
                                    st.chart.top + bar.y + bar.h), b);
    }
  }
  y = panel.bottom + 6;

  // Legend.
  const float lh = 16;
  if (rt) {
    const std::wstring lead = L"last 60 minutes, per minute:";
    float x = 0;
    draw_text(lead, st.f_small, D2D1::RectF(x, y, W, y + lh), muted_c);
    x += text_width(lead, st.f_small) + 10;
    for (int c = 0; c < vc::kDiagCategories; ++c) {
      const vc::DiagCategory cat = vc::DiagCategory(c);
      b->SetColor(rgb(vc::diag_category_color(cat)));
      rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y + lh / 2 - 4, x + 8, y + lh / 2 + 4), 1.5f, 1.5f), b);
      x += 12;
      const std::wstring label = widen(vc::diag_category_label(cat));
      draw_text(label, st.f_small, D2D1::RectF(x, y, W, y + lh), text_c);
      x += text_width(label, st.f_small) + 14;
    }
  }
  y += lh + 8;

  // Hints, as bullets.
  for (const std::string& h : st.have ? d.hints : std::vector<std::string>()) {
    IDWriteTextLayout* l = text_layout(widen(h), st.f_text, W - 16);
    if (!l) continue;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    if (rt) {
      b->SetColor(text_c);
      rt->DrawText(L"•", 1, st.f_text, D2D1::RectF(2, y, 14, y + m.height), b);
      rt->DrawTextLayout(D2D1::Point2F(16, y), l, b);
    }
    release(l);
    y += m.height + 4;
  }
  return y;
}

float px_scale(HWND h) { return float(dpi_for(h)) / 96.f; }

void diag_paint(HWND h, HDC dc) {
  RECT rc;
  GetClientRect(h, &rc);
  DiagState* st = g_dg;
  if (!st) {
    FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
    return;
  }
  const float dpi = float(dpi_for(h));
  if (!st->rt) {
    const D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    if (FAILED(g_d2d->CreateDCRenderTarget(&p, &st->rt)) ||
        FAILED(st->rt->CreateSolidColorBrush(sys_color(COLOR_WINDOWTEXT), &st->brush))) {
      diag_release_d2d(*st);
      FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
      return;
    }
  }
  st->rt->SetDpi(dpi, dpi);
  if (FAILED(st->rt->BindDC(dc, &rc))) return;
  st->rt->BeginDraw();
  st->rt->Clear(sys_color(COLOR_BTNFACE));
  diag_summary(*st, st->rt, float(rc.right) * 96.f / dpi);
  if (st->rt->EndDraw() == HRESULT(D2DERR_RECREATE_TARGET)) diag_release_d2d(*st);
}

void diag_tip_set(HWND h, DiagState& st, int minute, POINT screen) {
  if (!st.tip) {
    st.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, h, nullptr, g_inst, nullptr);
    if (!st.tip) return;
    TTTOOLINFOW ti{};
    ti.cbSize = sizeof ti;
    ti.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_IDISHWND;
    ti.hwnd = h;
    ti.uId = reinterpret_cast<UINT_PTR>(h);
    ti.lpszText = const_cast<wchar_t*>(L"");
    SendMessageW(st.tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
  }
  TTTOOLINFOW ti{};
  ti.cbSize = sizeof ti;
  ti.hwnd = h;
  ti.uId = reinterpret_cast<UINT_PTR>(h);
  if (minute < 0 || minute >= int(st.d.minutes.size())) {
    SendMessageW(st.tip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
    return;
  }
  std::wstring t = widen(st.d.minutes[size_t(minute)].tooltip);
  ti.lpszText = &t[0];
  SendMessageW(st.tip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
  if (screen.x != LONG_MIN) SendMessageW(st.tip, TTM_TRACKPOSITION, 0, MAKELPARAM(screen.x + 14, screen.y + 20));
  SendMessageW(st.tip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
}

// Mouse at (x, y) client pixels over the summary, or (-1, -1): gone.
void diag_hover(HWND h, int x, int y) {
  DiagState* st = g_dg;
  if (!st) return;
  int minute = -1;
  if (x >= 0 && st->have) {
    const float s = px_scale(h), xd = float(x) / s, yd = float(y) / s;
    if (yd >= st->chart.top && yd < st->chart.bottom)
      minute = vc::diag_chart_minute_at(xd - st->chart.left, st->chart.right - st->chart.left);
    if (minute >= int(st->d.minutes.size())) minute = -1;
  }
  if (x >= 0 && !st->tracking) {
    TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
    st->tracking = TrackMouseEvent(&t) != FALSE;
  }
  if (x < 0) st->tracking = false;
  if (minute != st->hover) InvalidateRect(h, nullptr, FALSE);
  st->hover = minute;
  POINT p{x, y};
  ClientToScreen(h, &p);
  diag_tip_set(h, *st, minute, p);
}

LRESULT CALLBACK diag_summary_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      diag_paint(h, dc);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_MOUSEMOVE:
      diag_hover(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_MOUSELEAVE:
      diag_hover(h, -1, -1);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

void diag_status(HWND d, const std::wstring& text, int kind, bool fetch = false) {
  if (!g_dg) return;
  g_dg->status = text;
  g_dg->status_kind = kind;
  g_dg->status_is_fetch = fetch;
  SetDlgItemTextW(d, IDC_DG_STATUS, text.c_str());
}

void diag_layout(HWND d) {
  DiagState* st = g_dg;
  if (!st) return;
  RECT rc;
  GetClientRect(d, &rc);
  const int W = rc.right, H = rc.bottom, m = dlu_x(d, 7), mv = dlu_y(d, 7);
  const int bh = dlu_y(d, 15), bw = dlu_x(d, 46), sw = dlu_x(d, 62), gap = dlu_x(d, 4);
  const float s = px_scale(d);
  const int sum_h = int(std::ceil(diag_summary(*st, nullptr, float(W - 2 * m) / s) * s));
  st->summary_h = sum_h;
  const int row_y = H - mv - bh;
  const int ev_label_y = mv + sum_h + dlu_y(d, 5), ev_y = ev_label_y + dlu_y(d, 11);
  HDWP dwp = BeginDeferWindowPos(7);
  place(dwp, d, IDC_DG_SUMMARY, m, mv, W - 2 * m, sum_h);
  place(dwp, d, IDC_DG_EVLABEL, m, ev_label_y, W - 2 * m, dlu_y(d, 9));
  place(dwp, d, IDC_DG_EVENTS, m, ev_y, W - 2 * m, std::max(dlu_y(d, 30), row_y - dlu_y(d, 6) - ev_y));
  int x = W - m - bw;
  place(dwp, d, IDCANCEL, x, row_y, bw, bh);
  x -= gap + bw;
  place(dwp, d, IDC_DG_CLEAR, x, row_y, bw, bh);
  x -= gap + sw;
  place(dwp, d, IDC_DG_SAVE, x, row_y, sw, bh);
  place(dwp, d, IDC_DG_STATUS, m, row_y, x - gap - m, bh);
  if (dwp) EndDeferWindowPos(dwp);
  InvalidateRect(GetDlgItem(d, IDC_DG_SUMMARY), nullptr, FALSE);
  ListView_SetColumnWidth(GetDlgItem(d, IDC_DG_EVENTS), 1, LVSCW_AUTOSIZE_USEHEADER);
}

// Small round severity dots for the events list (32-bit, premultiplied alpha).
HIMAGELIST diag_dots(int size) {
  HIMAGELIST il = ImageList_Create(size, size, ILC_COLOR32, 3, 0);
  if (!il) return nullptr;
  for (unsigned c : {kWarn, kBad, kMuted}) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb || !bits) continue;
    auto* px = static_cast<uint32_t*>(bits);
    const float r = float(size) * 0.22f, cx = float(size) / 2.f, cy = float(size) / 2.f;
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) {
        const float dist = std::hypot(float(x) + 0.5f - cx, float(y) + 0.5f - cy);
        const float a = std::max(0.f, std::min(1.f, r + 0.5f - dist));
        const uint32_t A = uint32_t(a * 255.f + 0.5f), R = uint32_t(((c >> 16) & 0xFF) * a + 0.5f),
                       G = uint32_t(((c >> 8) & 0xFF) * a + 0.5f), B = uint32_t((c & 0xFF) * a + 0.5f);
        px[y * size + x] = (A << 24) | (R << 16) | (G << 8) | B;
      }
    ImageList_Add(il, hb, nullptr);
    DeleteObject(hb);
  }
  return il;
}

void diag_rows(HWND d) {
  DiagState* st = g_dg;
  if (!st) return;
  st->rows.clear();
  for (const vc::DiagEvent& e : st->d.events) {
    if (st->rows.size() >= vc::kDiagShownEvents) break;
    DiagRow r;
    r.time = widen(e.time_text);
    r.text = widen(e.text) + (e.duration_text.empty() ? L"" : L" (" + widen(e.duration_text) + L")");
    r.image = e.severity == vc::Severity::Warn ? 0 : e.severity == vc::Severity::Error ? 1 : 2;
    st->rows.push_back(std::move(r));
  }
  HWND lv = GetDlgItem(d, IDC_DG_EVENTS);
  ListView_SetItemCountEx(lv, int(st->rows.size()), LVSICF_NOSCROLL);
  InvalidateRect(lv, nullptr, FALSE);
}

struct DiagReport {
  bool ok = false;
  std::string text, error;
};
struct DiagCleared {
  bool ok = false;
  std::string error;
};
struct DiagFetched {
  bool ok = false;
  vc::Diagnostics d;
  std::string error;
};

void diag_fetch(HWND d) {
  DiagState* st = g_dg;
  if (!st) return;
  if (st->fetching) {
    st->again = true;  // e.g. right after Clear: fetch once more when this one is back
    return;
  }
  st->fetching = true;
  run_async<DiagFetched>(d, WM_APP_DIAG, [](unsigned port, DiagFetched& r) {
    r.ok = vc::fetch_diagnostics(port, &r.d, &r.error);
  });
}

INT_PTR CALLBACK diag_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
  DiagState* st = g_dg;
  switch (msg) {
    case WM_INITDIALOG: {
      st = g_dg = new DiagState();
      g_diag_dlg = d;
      set_icon(d);
      st->min = window_size(d);
      st->mono = font_like(d, g_mono_font.c_str(), FW_NORMAL);
      st->normal = reinterpret_cast<HFONT>(SendMessageW(d, WM_GETFONT, 0, 0));
      HWND lv = GetDlgItem(d, IDC_DG_EVENTS);
      ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
      ListView_SetImageList(lv, diag_dots(MulDiv(16, int(dpi_for(d)), 96)), LVSIL_SMALL);
      // Wide enough for "2026-10-12 14:05:12" in the monospace font, and the dot.
      int time_w = dlu_x(d, 90);
      if (HDC dc = GetDC(lv)) {
        HGDIOBJ old = SelectObject(dc, st->mono);
        SIZE sz{};
        if (GetTextExtentPoint32W(dc, L"2026-10-12 14:05:12", 19, &sz))
          time_w = sz.cx + MulDiv(16 + 22, int(dpi_for(d)), 96);
        SelectObject(dc, old);
        ReleaseDC(lv, dc);
      }
      LVCOLUMNW c{};
      c.mask = LVCF_TEXT | LVCF_WIDTH;
      c.cx = time_w;
      c.pszText = const_cast<wchar_t*>(L"Time");
      ListView_InsertColumn(lv, 0, &c);
      c.cx = dlu_x(d, 200);
      c.pszText = const_cast<wchar_t*>(L"What happened");
      ListView_InsertColumn(lv, 1, &c);
      diag_layout(d);
      diag_fetch(d);
      SetTimer(d, 1, 2000, nullptr);  // refresh while the window is up
      return TRUE;
    }
    case WM_DESTROY:
      KillTimer(d, 1);
      if (st) {
        diag_release_d2d(*st);
        release(st->f_head);
        release(st->f_text);
        release(st->f_small);
        if (st->mono) DeleteObject(st->mono);
        delete st;
      }
      g_dg = nullptr;
      if (g_diag_dlg == d) g_diag_dlg = nullptr;
      break;
    case WM_GETMINMAXINFO:
      if (st) min_track(d, lp, st->min);
      return TRUE;
    case WM_SIZE:
      diag_layout(d);
      return TRUE;
    case WM_TIMER:
      if (st && IsWindowVisible(d) && !IsIconic(d)) diag_fetch(d);
      return TRUE;
    case WM_APP_DIAG: {
      std::unique_ptr<DiagFetched> r(reinterpret_cast<DiagFetched*>(lp));
      if (!st || !r) return TRUE;
      st->fetching = false;
      if (r->ok) {
        st->d = std::move(r->d);
        st->have = true;
        st->error.clear();
        if (st->status_is_fetch) diag_status(d, L"", kStatusPlain);
        diag_rows(d);
      } else {
        st->error = r->error.empty() ? "Could not reach the Virgil service." : r->error;
        diag_status(d, L"Not up to date: " + widen(st->error), kStatusError, true);
      }
      const float s = px_scale(d);
      RECT rc;
      GetClientRect(d, &rc);
      const int h = int(std::ceil(diag_summary(*st, nullptr, float(rc.right - 2 * dlu_x(d, 7)) / s) * s));
      if (h != st->summary_h) diag_layout(d);  // hints came or went
      InvalidateRect(GetDlgItem(d, IDC_DG_SUMMARY), nullptr, FALSE);
      if (st->hover >= 0) {
        POINT none{LONG_MIN, 0};
        diag_tip_set(GetDlgItem(d, IDC_DG_SUMMARY), *st, st->hover, none);
      }
      if (st->again) {
        st->again = false;
        diag_fetch(d);
      }
      return TRUE;
    }
    case WM_APP_REPORT: {
      std::unique_ptr<DiagReport> r(reinterpret_cast<DiagReport*>(lp));
      if (!st || !r) return TRUE;
      st->saving = false;
      EnableWindow(GetDlgItem(d, IDC_DG_SAVE), TRUE);
      if (!r->ok) {
        diag_status(d, L"Could not get the report: " + widen(r->error), kStatusError);
        return TRUE;
      }
      diag_status(d, L"", kStatusPlain);
      std::wstring path;
      if (!ask_save_path(d, widen(vc::default_report_file_name()), &path)) return TRUE;
      if (write_file(path, to_crlf(r->text))) diag_status(d, L"Report saved to " + path, kStatusOk);
      else diag_status(d, L"Could not write " + path, kStatusError);
      return TRUE;
    }
    case WM_APP_CLEARED: {
      std::unique_ptr<DiagCleared> r(reinterpret_cast<DiagCleared*>(lp));
      if (!st || !r) return TRUE;
      st->clearing = false;
      EnableWindow(GetDlgItem(d, IDC_DG_CLEAR), TRUE);
      if (r->ok) diag_status(d, L"Cleared.", kStatusPlain);
      else diag_status(d, L"Could not clear: " + widen(r->error), kStatusError);
      diag_fetch(d);
      return TRUE;
    }
    case WM_CTLCOLORSTATIC:
      if (st && GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == IDC_DG_STATUS) return status_color(wp, st->status_kind);
      break;
    case WM_NOTIFY: {
      const NMHDR* n = reinterpret_cast<const NMHDR*>(lp);
      if (!st || n->idFrom != IDC_DG_EVENTS) break;
      if (n->code == LVN_GETDISPINFOW) {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(lp);
        const int i = di->item.iItem;
        if (i < 0 || size_t(i) >= st->rows.size()) return TRUE;
        const DiagRow& row = st->rows[size_t(i)];
        if ((di->item.mask & LVIF_TEXT) && di->item.pszText && di->item.cchTextMax > 0) {
          const std::wstring& t = di->item.iSubItem == 0 ? row.time : row.text;
          wcsncpy(di->item.pszText, t.c_str(), size_t(di->item.cchTextMax) - 1);
          di->item.pszText[di->item.cchTextMax - 1] = 0;
        }
        if (di->item.mask & LVIF_IMAGE) di->item.iImage = row.image;
        return TRUE;
      }
      if (n->code == LVN_GETEMPTYMARKUP) {
        auto* em = reinterpret_cast<NMLVEMPTYMARKUP*>(lp);
        em->dwFlags = EMF_CENTERED;
        wcsncpy(em->szMarkup, st->have ? L"Nothing yet." : L"Loading…", L_MAX_URL_LENGTH - 1);
        SetWindowLongPtrW(d, DWLP_MSGRESULT, TRUE);
        return TRUE;
      }
      if (n->code == NM_CUSTOMDRAW) {  // times in the monospace font
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
        LRESULT r = CDRF_DODEFAULT;
        switch (cd->nmcd.dwDrawStage) {
          case CDDS_PREPAINT: r = CDRF_NOTIFYITEMDRAW; break;
          case CDDS_ITEMPREPAINT: r = CDRF_NOTIFYSUBITEMDRAW; break;
          case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
            SelectObject(cd->nmcd.hdc, cd->iSubItem == 0 && st->mono ? st->mono : st->normal);
            r = CDRF_NEWFONT;
            break;
          default: break;
        }
        SetWindowLongPtrW(d, DWLP_MSGRESULT, r);
        return TRUE;
      }
      break;
    }
    case WM_COMMAND:
      if (!st) break;
      switch (LOWORD(wp)) {
        case IDCANCEL:
          DestroyWindow(d);
          return TRUE;
        case IDC_DG_SAVE:
          if (st->saving) return TRUE;
          st->saving = true;
          EnableWindow(GetDlgItem(d, IDC_DG_SAVE), FALSE);
          diag_status(d, L"Getting the report…", kStatusPlain);
          run_async<DiagReport>(d, WM_APP_REPORT, [](unsigned port, DiagReport& r) {
            r.ok = vc::fetch_report(port, &r.text, &r.error);
          });
          return TRUE;
        case IDC_DG_CLEAR:
          if (st->clearing) return TRUE;
          st->clearing = true;
          EnableWindow(GetDlgItem(d, IDC_DG_CLEAR), FALSE);
          run_async<DiagCleared>(d, WM_APP_CLEARED, [](unsigned port, DiagCleared& r) {
            r.ok = vc::reset_diagnostics(port, &r.error);
          });
          return TRUE;
      }
      break;
  }
  return FALSE;
}

void show_diagnostics() {
  if (bring_up(g_diag_dlg)) return;
  HWND d = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_DIAG), nullptr, diag_proc, 0);
  if (d) bring_up(d);
}

// ---- Channel names ------------------------------------------------------------------------

// A scrolling column of numbered name fields (one per channel); its edits
// report to the dialog through it.
const wchar_t* kNameGridClass = L"VirgilNameGrid";
const int kTxEditBase = 3000, kRxEditBase = 3200;  // control ids: base + channel index

struct NameGrid {
  int base = kTxEditBase;
  std::vector<HWND> nums, edits;
  std::vector<char> bad;  // the field's name cannot be saved: drawn red
  int scroll = 0;         // pixels
};
NameGrid* grid_of(HWND g) { return g ? reinterpret_cast<NameGrid*>(GetWindowLongPtrW(g, GWLP_USERDATA)) : nullptr; }

int grid_row_h(HWND g) { return dlu_y(GetParent(g), 16); }

void grid_layout(HWND g) {
  NameGrid* ng = grid_of(g);
  if (!ng) return;
  HWND d = GetParent(g);
  RECT rc;
  GetClientRect(g, &rc);
  const int row = grid_row_h(g), m = dlu_x(d, 4), num_w = dlu_x(d, 16), eh = dlu_y(d, 13);
  const int content = int(ng->edits.size()) * row + 2 * dlu_y(d, 3);
  ng->scroll = std::max(0, std::min(ng->scroll, content - int(rc.bottom)));
  SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content - 1), UINT(std::max(0L, rc.bottom)),
                ng->scroll, 0};
  SetScrollInfo(g, SB_VERT, &si, TRUE);
  GetClientRect(g, &rc);  // the scroll bar may have come or gone
  HDWP dwp = BeginDeferWindowPos(int(ng->edits.size() * 2));
  for (size_t i = 0; i < ng->edits.size() && dwp; ++i) {
    const int y = dlu_y(d, 3) + int(i) * row - ng->scroll;
    dwp = DeferWindowPos(dwp, ng->nums[i], nullptr, m, y, num_w, row, SWP_NOZORDER | SWP_NOACTIVATE);
    if (dwp)
      dwp = DeferWindowPos(dwp, ng->edits[i], nullptr, m + num_w + dlu_x(d, 4), y + (row - eh) / 2,
                           std::max(10, int(rc.right) - (2 * m + num_w + dlu_x(d, 4))), eh,
                           SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (dwp) EndDeferWindowPos(dwp);
}

void grid_scroll_to(HWND g, int pos) {
  NameGrid* ng = grid_of(g);
  if (!ng || pos == ng->scroll) return;
  ng->scroll = pos;
  grid_layout(g);
  InvalidateRect(g, nullptr, TRUE);
}

// Fields for `n` channels, filled with `names` and the placeholders.
void grid_fill(HWND g, unsigned n, const std::vector<std::string>& names, bool tx,
               const std::vector<std::string>& live) {
  NameGrid* ng = grid_of(g);
  if (!ng) return;
  for (HWND h : ng->nums) DestroyWindow(h);
  for (HWND h : ng->edits) DestroyWindow(h);
  ng->nums.clear();
  ng->edits.clear();
  ng->bad.assign(n, 0);
  ng->scroll = 0;
  HFONT font = reinterpret_cast<HFONT>(SendMessageW(GetParent(g), WM_GETFONT, 0, 0));
  for (unsigned i = 0; i < n; ++i) {
    HWND num = CreateWindowExW(0, L"STATIC", std::to_wstring(i + 1).c_str(),
                               WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX, 0, 0, 0, 0, g, nullptr,
                               g_inst, nullptr);
    HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", widen(i < names.size() ? names[i] : std::string()).c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, g,
                             reinterpret_cast<HMENU>(INT_PTR(ng->base + int(i))), g_inst, nullptr);
    SendMessageW(num, WM_SETFONT, WPARAM(font), FALSE);
    SendMessageW(e, WM_SETFONT, WPARAM(font), FALSE);
    SendMessageW(e, EM_LIMITTEXT, vc::kMaxNameLength, 0);
    const std::wstring cue = widen(vc::channel_name_placeholder(live, tx, i));
    SendMessageW(e, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(cue.c_str()));
    ng->nums.push_back(num);
    ng->edits.push_back(e);
  }
  grid_layout(g);
  InvalidateRect(g, nullptr, TRUE);
}

LRESULT CALLBACK name_grid_proc(HWND g, UINT msg, WPARAM wp, LPARAM lp) {
  NameGrid* ng = grid_of(g);
  switch (msg) {
    case WM_NCCREATE: {
      auto* n = new NameGrid();
      n->base = GetDlgCtrlID(g) == IDC_NM_RXGRID ? kRxEditBase : kTxEditBase;
      SetWindowLongPtrW(g, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(n));
      break;
    }
    case WM_NCDESTROY:
      delete ng;
      SetWindowLongPtrW(g, GWLP_USERDATA, 0);
      break;
    case WM_SIZE:
      grid_layout(g);
      return 0;
    case WM_VSCROLL: {
      if (!ng) return 0;
      SCROLLINFO si{};
      si.cbSize = sizeof si;
      si.fMask = SIF_ALL;
      GetScrollInfo(g, SB_VERT, &si);
      const int row = grid_row_h(g);
      int pos = ng->scroll;
      switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= row; break;
        case SB_LINEDOWN: pos += row; break;
        case SB_PAGEUP: pos -= int(si.nPage); break;
        case SB_PAGEDOWN: pos += int(si.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        default: return 0;
      }
      grid_scroll_to(g, std::max(0, pos));
      return 0;
    }
    case WM_MOUSEWHEEL:
      if (ng) grid_scroll_to(g, std::max(0, ng->scroll - GET_WHEEL_DELTA_WPARAM(wp) * 3 * grid_row_h(g) / WHEEL_DELTA));
      return 0;
    case WM_COMMAND:
      if (ng && HIWORD(wp) == EN_SETFOCUS) {  // keep the field being edited in view
        const int i = LOWORD(wp) - ng->base;
        RECT rc;
        GetClientRect(g, &rc);
        const int row = grid_row_h(g), top = dlu_y(GetParent(g), 3) + i * row;
        if (top < ng->scroll) grid_scroll_to(g, std::max(0, top - dlu_y(GetParent(g), 3)));
        else if (top + row > ng->scroll + rc.bottom) grid_scroll_to(g, top + row + dlu_y(GetParent(g), 3) - rc.bottom);
      }
      return SendMessageW(GetParent(g), WM_COMMAND, wp, lp);
    case WM_CTLCOLOREDIT: {
      const int i = ng ? GetDlgCtrlID(reinterpret_cast<HWND>(lp)) - ng->base : -1;
      if (i >= 0 && size_t(i) < ng->bad.size() && ng->bad[size_t(i)]) {
        static HBRUSH pink = CreateSolidBrush(RGB(253, 228, 225));
        SetTextColor(reinterpret_cast<HDC>(wp), RGB(150, 20, 10));
        SetBkColor(reinterpret_cast<HDC>(wp), RGB(253, 228, 225));
        return reinterpret_cast<LRESULT>(pink);
      }
      break;
    }
    case WM_CTLCOLORSTATIC:  // channel numbers
      SetTextColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_GRAYTEXT));
      SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
  }
  return DefWindowProcW(g, msg, wp, lp);
}

struct NamesState {
  bool loaded = false, loading = false, saving = false;
  vc::ChannelNames base;  // as loaded (or last saved): saving writes only what changed since
  std::wstring status;
  int status_kind = kStatusPlain;
  bool tx_err = false, rx_err = false;
  SIZE min{};
};
NamesState* g_nm;

struct NamesLoaded {
  bool ok = false;
  vc::ChannelNames names;
  std::string error;
};
struct NamesSaved {
  bool ok = false;
  std::string error;
};

void names_status(HWND d, const std::wstring& text, int kind) {
  if (!g_nm) return;
  g_nm->status = text;
  g_nm->status_kind = kind;
  SetDlgItemTextW(d, IDC_NM_STATUS, text.c_str());
}

vc::ChannelNames names_edited(HWND d) {
  vc::ChannelNames e = g_nm->base;
  for (int dir = 0; dir < 2; ++dir) {
    const bool tx = dir == 0;
    NameGrid* ng = grid_of(GetDlgItem(d, tx ? IDC_NM_TXGRID : IDC_NM_RXGRID));
    std::vector<std::string>& v = e.names(tx);
    v.assign(tx ? e.tx : e.rx, std::string());
    for (size_t i = 0; ng && i < ng->edits.size() && i < v.size(); ++i) v[i] = narrow(window_text(ng->edits[i]));
  }
  return e;
}

bool names_changed(HWND d) {
  const vc::ChannelNames e = names_edited(d);
  for (int dir = 0; dir < 2; ++dir) {
    const bool tx = dir == 0;
    const auto &a = e.names(tx), &b = g_nm->base.names(tx);
    for (size_t i = 0; i < a.size(); ++i)
      if (vc::trim_name(a[i]) != vc::trim_name(i < b.size() ? b[i] : std::string())) return true;
  }
  return false;
}

// Marks the fields whose names cannot be saved, says why under each column
// (the focused field's problem, else the first), and enables Save.
void names_validate(HWND d) {
  NamesState* st = g_nm;
  if (!st) return;
  const std::vector<vc::NameError> errors = st->loaded ? vc::validate_channel_names(names_edited(d)) : std::vector<vc::NameError>();
  HWND focus = GetFocus();
  for (int dir = 0; dir < 2; ++dir) {
    const bool tx = dir == 0;
    NameGrid* ng = grid_of(GetDlgItem(d, tx ? IDC_NM_TXGRID : IDC_NM_RXGRID));
    if (!ng) continue;
    std::wstring first, focused;
    unsigned count = 0;
    for (size_t i = 0; i < ng->edits.size(); ++i) {
      const std::string err = vc::name_error(errors, tx, unsigned(i));
      const char bad = err.empty() ? 0 : 1;
      if (bad) {
        const std::wstring line = fmt(L"Channel %u: ", unsigned(i + 1)) + widen(err);
        if (first.empty()) first = line;
        if (ng->edits[i] == focus) focused = line;
        ++count;
      }
      if (ng->bad[i] != bad) {
        ng->bad[i] = bad;
        InvalidateRect(ng->edits[i], nullptr, TRUE);
      }
    }
    std::wstring msg = focused.empty() ? first : focused;
    if (count > 1) msg += fmt(L" (%u fields to fix)", count);
    SetDlgItemTextW(d, tx ? IDC_NM_TXERR : IDC_NM_RXERR, msg.c_str());
    (tx ? st->tx_err : st->rx_err) = count > 0;
  }
  EnableWindow(GetDlgItem(d, IDOK), st->loaded && !st->saving && errors.empty());
}

void names_layout(HWND d) {
  RECT rc;
  GetClientRect(d, &rc);
  const int W = rc.right, H = rc.bottom, m = dlu_x(d, 7), mv = dlu_y(d, 7), gap = dlu_x(d, 4);
  const int bh = dlu_y(d, 15), bw = dlu_x(d, 50);
  const int col = (W - 3 * m) / 2, row_y = H - mv - bh;
  const int help_h = dlu_y(d, 26), help_y = row_y - dlu_y(d, 5) - help_h;
  const int err_h = dlu_y(d, 17), err_y = help_y - dlu_y(d, 3) - err_h;
  const int grid_y = mv + dlu_y(d, 11), grid_h = err_y - dlu_y(d, 2) - grid_y;
  HDWP dwp = BeginDeferWindowPos(10);
  for (int dir = 0; dir < 2; ++dir) {
    const int x = m + dir * (col + m);
    place(dwp, d, dir ? IDC_NM_RXLABEL : IDC_NM_TXLABEL, x, mv, col, dlu_y(d, 9));
    place(dwp, d, dir ? IDC_NM_RXGRID : IDC_NM_TXGRID, x, grid_y, col, grid_h);
    place(dwp, d, dir ? IDC_NM_RXERR : IDC_NM_TXERR, x, err_y, col, err_h);
  }
  place(dwp, d, IDC_NM_HELP, m, help_y, W - 2 * m, help_h);
  place(dwp, d, IDCANCEL, W - m - bw, row_y, bw, bh);
  place(dwp, d, IDOK, W - m - 2 * bw - gap, row_y, bw, bh);
  place(dwp, d, IDC_NM_STATUS, m, row_y, W - 2 * m - 2 * bw - 2 * gap, bh);
  if (dwp) EndDeferWindowPos(dwp);
}

void names_load(HWND d) {
  NamesState* st = g_nm;
  if (!st || st->loading) return;
  st->loading = true;
  names_status(d, L"Loading…", kStatusPlain);
  run_async<NamesLoaded>(d, WM_APP_NAMES, [](unsigned port, NamesLoaded& r) {
    vc::ConfigText c;
    r.ok = vc::fetch_config(port, &c, &r.error);
    if (r.ok) r.names = vc::load_channel_names(c.text);
  });
}

INT_PTR CALLBACK names_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
  NamesState* st = g_nm;
  switch (msg) {
    case WM_INITDIALOG:
      st = g_nm = new NamesState();
      g_names_dlg = d;
      set_icon(d);
      st->min = window_size(d);
      SetDlgItemTextW(d, IDC_NM_TXLABEL, L"Transmit (playback → network)");
      SetDlgItemTextW(d, IDC_NM_RXLABEL, L"Receive (network → capture)");
      SetDlgItemTextW(d, IDC_NM_HELP,
                      L"Leave a field empty for the default (TX 1, RX 1, …). Names changed in Dante Controller "
                      L"appear here too. Up to 31 characters, unique per direction, without = . @ or \".");
      EnableWindow(GetDlgItem(d, IDOK), FALSE);
      names_layout(d);
      names_load(d);
      return TRUE;
    case WM_DESTROY:
      delete st;
      g_nm = nullptr;
      if (g_names_dlg == d) g_names_dlg = nullptr;
      break;
    case WM_GETMINMAXINFO:
      if (st) min_track(d, lp, st->min);
      return TRUE;
    case WM_SIZE:
      names_layout(d);
      return TRUE;
    case WM_APP_NAMES: {
      std::unique_ptr<NamesLoaded> r(reinterpret_cast<NamesLoaded*>(lp));
      if (!st || !r) return TRUE;
      st->loading = false;
      if (!r->ok) {
        names_status(d, L"Could not load the channel names: " + widen(r->error), kStatusError);
        return TRUE;
      }
      st->base = r->names;
      st->base.tx_names.resize(st->base.tx);
      st->base.rx_names.resize(st->base.rx);
      std::vector<std::string> live_tx, live_rx;
      {
        std::lock_guard<std::mutex> l(g_mutex);
        live_tx = g_status.tx_names;
        live_rx = g_status.rx_names;
      }
      grid_fill(GetDlgItem(d, IDC_NM_TXGRID), st->base.tx, st->base.tx_names, true, live_tx);
      grid_fill(GetDlgItem(d, IDC_NM_RXGRID), st->base.rx, st->base.rx_names, false, live_rx);
      auto label = [](const wchar_t* what, unsigned n) {
        return std::wstring(what) + (n ? fmt(L"  —  %u channel%s", n, n == 1 ? L"" : L"s") : std::wstring(L"  —  off"));
      };
      SetDlgItemTextW(d, IDC_NM_TXLABEL, label(L"Transmit (playback → network)", st->base.tx).c_str());
      SetDlgItemTextW(d, IDC_NM_RXLABEL, label(L"Receive (network → capture)", st->base.rx).c_str());
      st->loaded = true;
      names_status(d, L"", kStatusPlain);
      names_validate(d);
      return TRUE;
    }
    case WM_APP_NAMES_SAVED: {
      std::unique_ptr<NamesSaved> r(reinterpret_cast<NamesSaved*>(lp));
      if (!st || !r) return TRUE;
      st->saving = false;
      if (r->ok) {
        // What was saved is the new base (as the daemon has it: trimmed).
        st->base = names_edited(d);
        for (auto* v : {&st->base.tx_names, &st->base.rx_names})
          for (std::string& n : *v) n = vc::trim_name(n);
        names_status(d, L"Saved. The audio engine restarts to use the new names.", kStatusOk);
      } else {
        names_status(d, widen(r->error), kStatusError);
      }
      names_validate(d);
      return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
      const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
      if (st && id == IDC_NM_STATUS) return status_color(wp, st->status_kind);
      if (id == IDC_NM_TXERR || id == IDC_NM_RXERR) return status_color(wp, kStatusError);
      if (id == IDC_NM_HELP) {
        SetTextColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_GRAYTEXT));
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
      }
      break;
    }
    case WM_COMMAND: {
      if (!st) break;
      const int id = LOWORD(wp);
      if (id >= kTxEditBase && id < kRxEditBase + 128) {
        if (HIWORD(wp) == EN_CHANGE || HIWORD(wp) == EN_SETFOCUS) names_validate(d);
        return TRUE;
      }
      if (id == IDOK) {
        if (!st->loaded || st->saving) return TRUE;
        const vc::ChannelNames edited = names_edited(d), base = st->base;
        if (!vc::validate_channel_names(edited).empty()) return TRUE;
        st->saving = true;
        EnableWindow(GetDlgItem(d, IDOK), FALSE);
        names_status(d, L"Saving…", kStatusPlain);
        run_async<NamesSaved>(d, WM_APP_NAMES_SAVED, [base, edited](unsigned port, NamesSaved& r) {
          r.ok = vc::save_channel_names(port, base, edited, &r.error);
        });
        return TRUE;
      }
      if (id == IDCANCEL) {
        if (st->loaded && !st->saving && names_changed(d) &&
            MessageBoxW(d, L"Close without saving the changed names?", L"Channel names",
                        MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK)
          return TRUE;
        DestroyWindow(d);
        return TRUE;
      }
      break;
    }
  }
  return FALSE;
}

void show_channel_names() {
  if (bring_up(g_names_dlg)) return;
  HWND d = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_NAMES), nullptr, names_proc, 0);
  if (d) bring_up(d);
}

// ---- Edit configuration -------------------------------------------------------------------

struct ConfState {
  bool loading = false, saving = false, keep_status = false;
  std::string version;  // of the text in the editor (X-Virgil-Config-Version)
  HFONT mono = nullptr;
  std::wstring status;
  int status_kind = kStatusPlain;
  SIZE min{};
};
ConfState* g_cf;

struct ConfLoaded {
  bool ok = false;
  vc::ConfigText c;
  std::string error;
};
struct ConfSaved {
  vc::SaveResult r;
};

void conf_status(HWND d, const std::wstring& text, int kind) {
  if (!g_cf) return;
  g_cf->status = text;
  g_cf->status_kind = kind;
  SetDlgItemTextW(d, IDC_CF_STATUS, text.c_str());
}

void conf_buttons(HWND d) {
  const bool idle = g_cf && !g_cf->loading && !g_cf->saving;
  EnableWindow(GetDlgItem(d, IDOK), idle);
  EnableWindow(GetDlgItem(d, IDC_CF_RELOAD), idle);
}

void conf_layout(HWND d) {
  RECT rc;
  GetClientRect(d, &rc);
  const int W = rc.right, H = rc.bottom, m = dlu_x(d, 7), mv = dlu_y(d, 7), gap = dlu_x(d, 4);
  const int bh = dlu_y(d, 15), bw = dlu_x(d, 52), aw = dlu_x(d, 72), row_y = H - mv - bh;
  const int text_y = mv + dlu_y(d, 12);
  HDWP dwp = BeginDeferWindowPos(6);
  place(dwp, d, IDC_CF_NOTE, m, mv, W - 2 * m, dlu_y(d, 9));
  place(dwp, d, IDC_CF_TEXT, m, text_y, W - 2 * m, row_y - dlu_y(d, 6) - text_y);
  int x = W - m - bw;
  place(dwp, d, IDCANCEL, x, row_y, bw, bh);
  x -= gap + bw;
  place(dwp, d, IDC_CF_RELOAD, x, row_y, bw, bh);
  x -= gap + aw;
  place(dwp, d, IDOK, x, row_y, aw, bh);
  place(dwp, d, IDC_CF_STATUS, m, row_y, x - gap - m, bh);
  if (dwp) EndDeferWindowPos(dwp);
}

// `keep_status`: after a save, its message stays up while the text reloads.
void conf_load(HWND d, bool keep_status) {
  ConfState* st = g_cf;
  if (!st || st->loading || st->saving) return;
  st->loading = true;
  st->keep_status = keep_status;
  if (!keep_status) conf_status(d, L"Loading…", kStatusPlain);
  conf_buttons(d);
  run_async<ConfLoaded>(d, WM_APP_CONF, [](unsigned port, ConfLoaded& r) { r.ok = vc::fetch_config(port, &r.c, &r.error); });
}

bool conf_modified(HWND d) { return SendDlgItemMessageW(d, IDC_CF_TEXT, EM_GETMODIFY, 0, 0) != 0; }

INT_PTR CALLBACK conf_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
  ConfState* st = g_cf;
  switch (msg) {
    case WM_INITDIALOG:
      st = g_cf = new ConfState();
      g_conf_dlg = d;
      set_icon(d);
      st->min = window_size(d);
      st->mono = font_like(d, g_mono_font.c_str(), FW_NORMAL);
      if (st->mono) SendDlgItemMessageW(d, IDC_CF_TEXT, WM_SETFONT, WPARAM(st->mono), FALSE);
      SendDlgItemMessageW(d, IDC_CF_TEXT, EM_SETLIMITTEXT, 0, 0);  // no 32 K limit
      conf_layout(d);
      conf_load(d, false);
      return TRUE;
    case WM_DESTROY:
      if (st) {
        if (st->mono) DeleteObject(st->mono);
        delete st;
      }
      g_cf = nullptr;
      if (g_conf_dlg == d) g_conf_dlg = nullptr;
      break;
    case WM_GETMINMAXINFO:
      if (st) min_track(d, lp, st->min);
      return TRUE;
    case WM_SIZE:
      conf_layout(d);
      return TRUE;
    case WM_APP_CONF: {
      std::unique_ptr<ConfLoaded> r(reinterpret_cast<ConfLoaded*>(lp));
      if (!st || !r) return TRUE;
      st->loading = false;
      conf_buttons(d);
      if (!r->ok) {
        conf_status(d, L"Could not load the configuration: " + widen(r->error), kStatusError);
        return TRUE;
      }
      st->version = r->c.version;
      SetDlgItemTextW(d, IDC_CF_TEXT, to_crlf(widen(to_lf(r->c.text))).c_str());
      SendDlgItemMessageW(d, IDC_CF_TEXT, EM_SETMODIFY, FALSE, 0);
      if (!st->keep_status) conf_status(d, L"", kStatusPlain);
      return TRUE;
    }
    case WM_APP_CONF_SAVED: {
      std::unique_ptr<ConfSaved> s(reinterpret_cast<ConfSaved*>(lp));
      if (!st || !s) return TRUE;
      st->saving = false;
      conf_buttons(d);
      const vc::SaveResult& r = s->r;
      if (r.ok) {
        conf_status(d, widen(vc::saved_message(r)), kStatusOk);
        conf_load(d, true);  // the file as virgild now has it, with its new version
      } else if (r.conflict) {
        conf_status(d, L"Not saved: the configuration was changed elsewhere since it was loaded.", kStatusError);
        if (MessageBoxW(d,
                        L"The configuration was changed elsewhere (for example a rename in Dante Controller, or "
                        L"another editor) since it was loaded here, so it was not saved.\n\n"
                        L"Reload it? Your edits here are replaced.",
                        L"Edit configuration", MB_YESNO | MB_ICONWARNING) == IDYES)
          conf_load(d, false);
      } else {
        conf_status(d, widen(r.error.empty() ? std::string("Could not save the configuration.") : r.error),
                    kStatusError);
      }
      return TRUE;
    }
    case WM_CTLCOLORSTATIC:
      if (st && GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == IDC_CF_STATUS) return status_color(wp, st->status_kind);
      break;
    case WM_COMMAND: {
      if (!st) break;
      switch (LOWORD(wp)) {
        case IDOK: {
          if (st->loading || st->saving) return TRUE;
          const std::string text = to_lf(narrow(window_text(GetDlgItem(d, IDC_CF_TEXT))));
          const std::string version = st->version;
          st->saving = true;
          conf_buttons(d);
          conf_status(d, L"Saving…", kStatusPlain);
          run_async<ConfSaved>(d, WM_APP_CONF_SAVED, [text, version](unsigned port, ConfSaved& r) {
            r.r = vc::save_config_text(port, text, version);
          });
          return TRUE;
        }
        case IDC_CF_RELOAD:
          if (conf_modified(d) &&
              MessageBoxW(d, L"Replace your edits with the configuration as Virgil has it now?", L"Edit configuration",
                          MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK)
            return TRUE;
          conf_load(d, false);
          return TRUE;
        case IDCANCEL:
          if (conf_modified(d) && !st->saving &&
              MessageBoxW(d, L"Close without applying your edits?", L"Edit configuration",
                          MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK)
            return TRUE;
          DestroyWindow(d);
          return TRUE;
      }
      break;
    }
  }
  return FALSE;
}

void show_config_editor() {
  if (bring_up(g_conf_dlg)) return;
  HWND d = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_CONF), nullptr, conf_proc, 0);
  if (d) bring_up(d);
}

void register_panel_classes() {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.hInstance = g_inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpfnWndProc = diag_summary_proc;
  wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
  wc.lpszClassName = kDiagSummaryClass;
  RegisterClassExW(&wc);
  wc.lpfnWndProc = name_grid_proc;
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kNameGridClass;
  RegisterClassExW(&wc);
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
      show_settings(job->r.body, vc::parse_interfaces(job->r2.body), job->r3.body);
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
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS | ICC_LISTVIEW_CLASSES |
                                            ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);
  create_device_independent();
  register_panel_classes();
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
  // Desktop audio runs in this app (it starts at login, in the user's
  // session, where per-app capture works). Its own threads; never blocks here.
  g_desktop.reset(new da::Engine());
  g_desktop->start(da::load_settings(nullptr));
  std::thread poller(poll_thread);
  if (!tray_only) show_window();

  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    if (panel_dialog_message(&m)) continue;
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  g_quit = true;
  poller.join();
  g_desktop->shutdown();  // releases its Virgil playback slot
  g_desktop.reset();
  discard_device();
  if (g_icon_small) DestroyIcon(g_icon_small);
  CloseHandle(single);
  CoUninitialize();
  return 0;
}
