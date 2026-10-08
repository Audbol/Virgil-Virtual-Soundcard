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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api.h"
#include "resource.h"

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
const UINT WM_APP_STATUS = WM_APP + 1, WM_APP_TRAY = WM_APP + 2, WM_APP_SHOW = WM_APP + 3;
const UINT kTrayId = 1;
enum { ID_OPEN = 100, ID_SETTINGS, ID_RESTART, ID_LOG, ID_AUTOSTART, ID_QUIT, ID_CONFIG_FOLDER };

HINSTANCE g_inst;
HWND g_hwnd;
unsigned g_port = 8480;
UINT g_taskbar_created;
std::mutex g_mutex;
vc::Status g_status;         // guarded by g_mutex
std::atomic<bool> g_visible{false};
std::atomic<bool> g_quit{false};
bool g_told_about_tray = false;

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
IDWriteTextFormat *g_f_title, *g_f_sub, *g_f_sub_c, *g_f_label, *g_f_small, *g_f_mono, *g_f_button, *g_f_big;
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
  bool hover = false, down = false, primary = false, visible = false;
};
std::vector<Button> g_buttons;  // rebuilt each paint

const float kW = 640, kHeader = 76, kFooter = 56, kMeterH = 210, kPad = 22;

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
  return std::max(kW, kPad * 2 + group_w(s.tx) + 44 + group_w(s.rx));
}
float window_h() { return kHeader + 34 + kMeterH + 40 + kFooter; }

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
                      DWORD now) {
  const float sw = strip_w(n), gap = sw >= 12 ? 6 : 3, scale_w = 34;
  text(caption, g_f_label, D2D1::RectF(x, y - 26, x + 400, y - 8), kMuted);
  const float top = y, h = kMeterH, x0 = x + scale_w;
  // dB scale.
  const int marks[] = {0, -6, -12, -18, -24, -36, -48, -60};
  for (int db : marks) {
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
  const unsigned bg = b.primary ? kEmber : (b.down ? 0x2C3137 : b.hover ? 0x262A30 : kPanel);
  fill_round(b.r, 4, bg);
  if (!b.primary) stroke_round(b.r, 4, b.hover ? 0x4A5058 : 0x383D44);
  text(b.label, g_f_button, b.r, b.primary ? 0x1A0D06 : kText);
}

struct ClockView {
  unsigned color;
  std::wstring line1, line2;
};
ClockView clock_view(const vc::Status& s) {
  if (!s.reachable) return {kBad, L"Service not running", L""};
  if (s.state == "error") return {kBad, L"Audio engine error", widen(s.error)};
  if (s.state == "waiting-for-network") return {kWarn, L"Waiting for network", widen(s.iface)};
  if (s.state != "running") return {kWarn, L"Starting\u2026", L""};
  if (!s.dante) return {kBad, L"Dante stack not running", L""};
  if (s.clock == "ptp-locked")
    return {kOk, L"Locked to " + widen(s.grandmaster), fmt(L"offset %+.1f \u00b5s", s.offset_us)};
  if (s.clock == "ptp-master") return {kEmber, L"Clock leader", L"no other Dante clock present"};
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
  std::wstring sub = L"Dante virtual soundcard";
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

  g_buttons.resize(3);
  for (auto& b : g_buttons) b.visible = false;

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
  } else {
    // Meter bridge.
    const float my = kHeader + 34 + 14;
    const float bridge = group_w(s.tx) + 44 + group_w(s.rx);
    const float bx = std::max(kPad, (W - bridge) / 2);
    draw_meter_group(bx, my, fmt(L"To network  \u2014  %u channels", s.tx).c_str(), s.tx, g_mtx, now);
    draw_meter_group(bx + group_w(s.tx) + 44, my, fmt(L"From network  \u2014  %u channels", s.rx).c_str(), s.rx,
                     g_mrx, now);
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
  if (s.reachable)
    text(apps.empty() ? L"No apps connected" : apps, g_f_label,
         D2D1::RectF(kPad, H - kFooter, W - 280, H - kFooter / 2 - 2), apps.empty() ? kMuted : kText);
  if (s.reachable)
    text(L"Virgil " + widen(s.version) + (s.clock_steps > 1 ? fmt(L"   \u00b7   clock re-synced %llu\u00d7", s.clock_steps - 1) : L""),
         g_f_sub, D2D1::RectF(kPad, H - kFooter / 2 - 4, W - 280, H - 8), kDim);
  if (s.reachable) {
    Button& b1 = g_buttons[1];
    b1.id = ID_SETTINGS;
    b1.label = L"Settings\u2026";
    b1.primary = false;
    b1.r = D2D1::RectF(W - kPad - 236, H - kFooter + 12, W - kPad - 128, H - 12);
    draw_button(b1);
    Button& b2 = g_buttons[2];
    b2.id = ID_RESTART;
    b2.label = L"Restart engine";
    b2.primary = false;
    b2.r = D2D1::RectF(W - kPad - 120, H - kFooter + 12, W - kPad, H - 12);
    draw_button(b2);
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

void restart_engine() {
  bool reachable;
  {
    std::lock_guard<std::mutex> l(g_mutex);
    reachable = g_status.reachable;
  }
  if (!reachable) return start_service();
  vc::http(g_port, "POST", "/api/restart");
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
  nid.hIcon = static_cast<HICON>(LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_VIRGIL), IMAGE_ICON,
                                            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
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

void show_settings();

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
  AppendMenuW(m, MF_STRING | (s.reachable ? 0 : MF_GRAYED), ID_SETTINGS, L"Settings\u2026");
  AppendMenuW(m, MF_STRING, ID_RESTART, s.reachable ? L"Restart audio engine" : L"Start Virgil service");
  AppendMenuW(m, MF_STRING, ID_LOG, L"Open log");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
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
  switch (id) {
    case ID_OPEN: show_window(); break;
    case ID_SETTINGS: show_settings(); break;
    case ID_RESTART: restart_engine(); break;
    case ID_LOG: ShellExecuteW(g_hwnd, L"open", log_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case ID_AUTOSTART: set_autostart(!autostart_enabled()); break;
    case ID_QUIT: DestroyWindow(g_hwnd); break;
  }
}

// ---- status polling ------------------------------------------------------------------

std::wstring tray_text(const vc::Status& s) {
  const ClockView cv = clock_view(s);
  std::wstring t = L"Virgil \u2014 " + cv.line1;
  if (s.reachable) {
    unsigned playing = 0;
    for (const auto& c : s.clients) playing += c.active;
    if (playing) t += fmt(L"\n%u app(s) playing", playing);
  }
  return t.substr(0, 127);
}

void poll_thread() {
  bool was_reachable = true;
  std::string was_clock;
  DWORD lost_since = 0;
  bool lost_told = false;
  while (!g_quit) {
    const auto r = vc::http(g_port, "GET", "/api/status");
    vc::Status s;
    if (r.status != 200 || !vc::parse_status(r.body, &s)) s = vc::Status();
    {
      std::lock_guard<std::mutex> l(g_mutex);
      // Meters are drained per request by the daemon; keep a frame's worth.
      g_status = s;
    }
    // Notifications on state changes worth knowing about while hidden.
    if (was_reachable && !s.reachable)
      PostMessageW(g_hwnd, WM_APP_STATUS, 1, 0);
    if (s.reachable && was_clock == "ptp-locked" && s.clock != "ptp-locked" && !lost_since)
      lost_since = GetTickCount();
    if (s.clock == "ptp-locked") lost_since = 0, lost_told = false;
    if (lost_since && !lost_told && GetTickCount() - lost_since > 5000) {
      lost_told = true;
      PostMessageW(g_hwnd, WM_APP_STATUS, 2, 0);
    }
    was_reachable = s.reachable;
    was_clock = s.clock;
    PostMessageW(g_hwnd, WM_APP_STATUS, 0, 0);
    Sleep(g_visible ? 80 : 1000);
  }
}

// ---- settings dialog -------------------------------------------------------------------

struct SettingsData {
  std::string conf;
  std::vector<vc::Interface> ifaces;
};

void combo_add(HWND d, int id, const std::wstring& s) { SendDlgItemMessageW(d, id, CB_ADDSTRING, 0, (LPARAM)s.c_str()); }
std::wstring get_text(HWND d, int id) {
  wchar_t buf[512];
  GetDlgItemTextW(d, id, buf, 512);
  return buf;
}
std::wstring ms_text(const std::string& us) {
  const double ms = std::atof(us.c_str()) / 1000.0;
  wchar_t b[32];
  swprintf(b, 32, L"%g", ms);
  return b;
}

INT_PTR CALLBACK settings_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
  static SettingsData* data;
  switch (msg) {
    case WM_INITDIALOG: {
      data = reinterpret_cast<SettingsData*>(lp);
      const std::string& c = data->conf;
      HICON ic = static_cast<HICON>(LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_VIRGIL), IMAGE_ICON, 16, 16, 0));
      SendMessageW(d, WM_SETICON, ICON_SMALL, (LPARAM)ic);
      std::string name = vc::conf_get(c, "device", "name");
      SetDlgItemTextW(d, IDC_NAME, widen(name.empty() ? "Virgil" : name).c_str());
      SendDlgItemMessageW(d, IDC_NAME, EM_LIMITTEXT, 31, 0);
      const std::string cur_if = vc::conf_get(c, "device", "interface");
      combo_add(d, IDC_IFACE, L"Automatic (prefers wired Ethernet)");
      int sel = 0, i = 1;
      for (const auto& f : data->ifaces) {
        if (f.loopback) continue;
        combo_add(d, IDC_IFACE, widen(f.name + "  (" + f.address + ")" + (f.virt ? "  virtual" : "")));
        if (!cur_if.empty() && (cur_if == f.name || cur_if == f.address)) sel = i;
        ++i;
      }
      SendDlgItemMessageW(d, IDC_IFACE, CB_SETCURSEL, sel, 0);
      const char* rates[] = {"44100", "48000", "88200", "96000"};
      std::string rate = vc::conf_get(c, "device", "sample_rate");
      if (rate.empty()) rate = "48000";
      for (int k = 0; k < 4; ++k) {
        combo_add(d, IDC_RATE, widen(std::string(rates[k]) + " Hz"));
        if (rate == rates[k]) SendDlgItemMessageW(d, IDC_RATE, CB_SETCURSEL, k, 0);
      }
      std::string tx = vc::conf_get(c, "device", "tx_channels"), rx = vc::conf_get(c, "device", "rx_channels");
      SetDlgItemTextW(d, IDC_TXCH, widen(tx.empty() ? "8" : tx).c_str());
      SetDlgItemTextW(d, IDC_RXCH, widen(rx.empty() ? "8" : rx).c_str());
      for (const wchar_t* v : {L"1", L"2", L"4", L"5", L"10", L"20"}) combo_add(d, IDC_RXLAT, v);
      for (const wchar_t* v : {L"3.5", L"4", L"5", L"6", L"10", L"20"}) combo_add(d, IDC_TXLAT, v);
      const std::string rl = vc::conf_get(c, "device", "latency_us"), tl = vc::conf_get(c, "device", "tx_latency_us");
      SetDlgItemTextW(d, IDC_RXLAT, rl.empty() ? L"4" : ms_text(rl).c_str());
      SetDlgItemTextW(d, IDC_TXLAT, tl.empty() ? L"4" : ms_text(tl).c_str());
      combo_add(d, IDC_CLOCK, L"Follow the Dante clock leader (PTP)");
      combo_add(d, IDC_CLOCK, L"Local clock only (testing)");
      SendDlgItemMessageW(d, IDC_CLOCK, CB_SETCURSEL, vc::conf_get(c, "device", "clock") == "free" ? 1 : 0, 0);
      const std::string mc = vc::conf_get(c, "ptp", "master_capable");
      CheckDlgButton(d, IDC_MASTER, (mc == "true" || mc == "1" || mc == "yes") ? BST_CHECKED : BST_UNCHECKED);
      return TRUE;
    }
    case WM_CTLCOLORSTATIC:
      if (GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == IDC_ERR) {
        SetTextColor(reinterpret_cast<HDC>(wp), RGB(190, 40, 30));
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
      }
      break;
    case WM_COMMAND:
      if (LOWORD(wp) == IDCANCEL) {
        EndDialog(d, 0);
        return TRUE;
      }
      if (LOWORD(wp) == IDOK) {
        std::string c = data->conf;
        c = vc::conf_set(c, "device", "name", narrow(get_text(d, IDC_NAME)));
        const int si = int(SendDlgItemMessageW(d, IDC_IFACE, CB_GETCURSEL, 0, 0));
        std::string iface;
        int i = 1;
        for (const auto& f : data->ifaces) {
          if (f.loopback) continue;
          if (i++ == si) iface = f.name;
        }
        c = vc::conf_set(c, "device", "interface", iface);
        const char* rates[] = {"44100", "48000", "88200", "96000"};
        const int ri = int(SendDlgItemMessageW(d, IDC_RATE, CB_GETCURSEL, 0, 0));
        c = vc::conf_set(c, "device", "sample_rate", rates[ri < 0 || ri > 3 ? 1 : ri]);
        c = vc::conf_set(c, "device", "tx_channels", narrow(get_text(d, IDC_TXCH)));
        c = vc::conf_set(c, "device", "rx_channels", narrow(get_text(d, IDC_RXCH)));
        auto us = [](const std::wstring& ms) { return std::to_string(long(std::wcstod(ms.c_str(), nullptr) * 1000 + 0.5)); };
        c = vc::conf_set(c, "device", "latency_us", us(get_text(d, IDC_RXLAT)));
        c = vc::conf_set(c, "device", "tx_latency_us", us(get_text(d, IDC_TXLAT)));
        c = vc::conf_set(c, "device", "clock",
                         SendDlgItemMessageW(d, IDC_CLOCK, CB_GETCURSEL, 0, 0) == 1 ? "free" : "ptp");
        c = vc::conf_set(c, "ptp", "master_capable", IsDlgButtonChecked(d, IDC_MASTER) ? "true" : "false");
        SetDlgItemTextW(d, IDC_ERR, L"Applying\u2026");
        const auto r = vc::http(g_port, "POST", "/api/config", c);
        if (r.status == 200) {
          EndDialog(d, 1);
        } else {
          vc::Json j;
          std::string err = "Could not reach the Virgil service.";
          if (r.status && vc::parse_json(r.body, &j)) err = j["error"].str("HTTP " + std::to_string(r.status));
          SetDlgItemTextW(d, IDC_ERR, widen(err).c_str());
        }
        return TRUE;
      }
      break;
  }
  return FALSE;
}

void show_settings() {
  SettingsData data;
  const auto c = vc::http(g_port, "GET", "/api/config");
  if (c.status != 200) {
    MessageBoxW(g_hwnd, L"The Virgil service is not running, so its settings cannot be changed now.", L"Virgil",
                MB_ICONINFORMATION);
    return;
  }
  data.conf = c.body;
  data.ifaces = vc::parse_interfaces(vc::http(g_port, "GET", "/api/interfaces").body);
  DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), IsWindowVisible(g_hwnd) ? g_hwnd : nullptr,
                  settings_proc, reinterpret_cast<LPARAM>(&data));
}

// ---- window --------------------------------------------------------------------------------

UINT dpi_for(HWND h) {
  using Fn = UINT(WINAPI*)(HWND);
  static Fn fn = reinterpret_cast<Fn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
  return fn && h ? fn(h) : 96;
}

void resize_to_content(HWND h) {
  vc::Status s;
  {
    std::lock_guard<std::mutex> l(g_mutex);
    s = g_status;
  }
  const float scale = g_dpi / 96.f;
  RECT r{0, 0, LONG(window_w(s) * scale), LONG(window_h() * scale)};
  AdjustWindowRectEx(&r, DWORD(GetWindowLongW(h, GWL_STYLE)), FALSE, 0);
  RECT cur;
  GetWindowRect(h, &cur);
  if (cur.right - cur.left != r.right - r.left || cur.bottom - cur.top != r.bottom - r.top)
    SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

Button* hit(float x, float y) {
  for (auto& b : g_buttons)
    if (b.visible && x >= b.r.left && x <= b.r.right && y >= b.r.top && y <= b.r.bottom) return &b;
  return nullptr;
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
      const float x = GET_X_LPARAM(lp) * 96.f / g_dpi, y = GET_Y_LPARAM(lp) * 96.f / g_dpi;
      Button* b = hit(x, y);
      for (auto& o : g_buttons) o.hover = &o == b;
      SetCursor(LoadCursorW(nullptr, b ? IDC_HAND : IDC_ARROW));
      TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
      TrackMouseEvent(&t);
      return 0;
    }
    case WM_MOUSELEAVE:
      for (auto& o : g_buttons) o.hover = o.down = false;
      return 0;
    case WM_LBUTTONDOWN: {
      Button* b = hit(GET_X_LPARAM(lp) * 96.f / g_dpi, GET_Y_LPARAM(lp) * 96.f / g_dpi);
      if (b) b->down = true;
      SetCapture(h);
      return 0;
    }
    case WM_LBUTTONUP: {
      ReleaseCapture();
      Button* b = hit(GET_X_LPARAM(lp) * 96.f / g_dpi, GET_Y_LPARAM(lp) * 96.f / g_dpi);
      const bool was_down = b && b->down;
      for (auto& o : g_buttons) o.down = false;
      if (was_down) command(b->id);
      return 0;
    }
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
  INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
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
  wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_VIRGIL), IMAGE_ICON, 16, 16, 0));
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kWindowClass;
  RegisterClassExW(&wc);
  g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r{0, 0, LONG(kW), LONG(window_h())};
  AdjustWindowRectEx(&r, style, FALSE, 0);
  g_hwnd = CreateWindowExW(0, kWindowClass, L"Virgil", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                           r.bottom - r.top, nullptr, nullptr, inst, nullptr);
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
  CloseHandle(single);
  CoUninitialize();
  return 0;
}
