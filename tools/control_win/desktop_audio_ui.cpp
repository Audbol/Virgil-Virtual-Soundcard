#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include "desktop_audio_ui.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <memory>
#include <thread>

#include "resource.h"
#include "wasapi.h"

namespace da {

const wchar_t* const kHelpText =
    L"Desktop audio sends sound from Windows programs to Virgil's Dante transmit channels, with no driver "
    L"and no extra software:\n\n"
    L"\u2022 Output device: everything Windows plays on that device (for example your speakers), captured "
    L"like a recording of it. \"Default\" follows the device chosen in Windows' sound settings.\n"
    L"\u2022 App: one program, such as chrome.exe or spotify.exe, with the processes it starts. Needs "
    L"Windows 10 version 2004 or later.\n"
    L"\u2022 All apps except Virgil: everything, without what Virgil Control itself plays (no echo when you "
    L"also listen to Dante on this PC). Windows 10 version 2004 or later.\n\n"
    L"Each source goes to a pair of transmit channels (or one, mixed to mono); sources on the same channels "
    L"are mixed. Route those channels in Dante Controller like any other.\n\n"
    L"Dante receive channels can play on a Windows output device too. Do not play them on a device you also "
    L"capture: they would echo back into Dante.\n\n"
    L"Captured audio still plays on its own device. To hear it only through Dante, let those apps play on an "
    L"output you do not listen to, or turn that device down.\n\n"
    L"The audio is converted to Virgil's sample rate and follows the small difference between the clock of the "
    L"PC's sound hardware and the Dante clock. It adds roughly 60 to 80 ms of latency, so use it for music, "
    L"video calls and system sounds, not for live monitoring.\n\n"
    L"Desktop audio runs while Virgil Control runs (it starts with Windows) and pauses while Virgil is stopped. "
    L"Settings are saved in %APPDATA%\\Virgil\\desktop-audio.conf.";

namespace {

const UINT WM_DA_INVENTORY = WM_APP + 40;  // lParam: Inventory* (the receiver owns it)
enum { kColSource, kColChannels, kColGain, kColLevel, kColStatus };

struct Inventory {
  std::vector<wa::Endpoint> devices;
  std::vector<wa::AudioApp> apps;
};

// Devices and apps with audio sessions, listed on a worker thread (COM calls
// into the audio service must not hold up the window).
void load_inventory(HWND target) {
  std::thread([target] {
    std::unique_ptr<Inventory> inv(new Inventory);
    {
      wa::ComScope com;
      if (SUCCEEDED(com.hr)) {
        inv->devices = wa::render_endpoints();
        inv->apps = wa::audio_apps();
      }
    }
    if (PostMessageW(target, WM_DA_INVENTORY, 0, reinterpret_cast<LPARAM>(inv.get()))) inv.release();
  }).detach();
}

std::wstring get_text(HWND d, int id) {
  wchar_t buf[512];
  GetDlgItemTextW(d, id, buf, 512);
  return buf;
}

// Gain field in dB ("-6", "-6,5", "+3", "" = 0).
bool parse_gain(std::wstring t, double* out) {
  std::replace(t.begin(), t.end(), L',', L'.');
  while (!t.empty() && t.back() == L' ') t.pop_back();
  while (!t.empty() && t.front() == L' ') t.erase(t.begin());
  if (t.empty()) {
    *out = 0;
    return true;
  }
  wchar_t* end = nullptr;
  const double v = std::wcstod(t.c_str(), &end);
  if (end == t.c_str() || *end || !std::isfinite(v)) return false;
  *out = std::max(vc::kMinGainDb, std::min(vc::kMaxGainDb, v));
  return true;
}
std::wstring gain_text(double db) {
  wchar_t b[32];
  if (db == std::floor(db)) _snwprintf(b, 31, L"%+.0f", db);
  else _snwprintf(b, 31, L"%+.1f", db);
  b[31] = 0;
  return db == 0 ? std::wstring(L"0") : std::wstring(b);
}

float lin_to_pos(float lin) {
  if (!(lin > 0)) return 0;
  const float db = 20.f * std::log10(lin);
  return std::max(0.f, std::min(1.f, (db + 60.f) / 60.f));
}

std::wstring device_name(const std::string& id, const std::string& saved, const Inventory* inv) {
  if (id.empty() || id == "default") return L"Default output device";
  const std::wstring wid = widen(id);
  if (inv)
    for (const auto& e : inv->devices)
      if (e.id == wid) return e.name;
  return saved.empty() ? wid : widen(saved);
}

std::wstring source_label(const vc::DesktopAudioSource& s, const Inventory* inv) {
  switch (s.type) {
    case vc::DesktopAudioSource::kApp: return L"App: " + widen(s.app);
    case vc::DesktopAudioSource::kAllApps: return L"All apps except Virgil";
    default: return (s.device == "default" || s.device.empty() ? L"" : L"Output: ") + device_name(s.device, s.name, inv);
  }
}

std::wstring channels_short(unsigned first, bool mono) {
  wchar_t b[32];
  if (mono) _snwprintf(b, 31, L"%u (mono)", first);
  else _snwprintf(b, 31, L"%u + %u", first, first + 1);
  b[31] = 0;
  return b;
}

// Stereo pairs from every channel, then single channels (mono). Item data:
// first * 2 + mono. A configured choice Virgil does not have (now) is kept.
void fill_channels(HWND combo, unsigned count, const std::vector<std::string>& names, unsigned first, bool mono) {
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  const bool known = count > 0;
  if (!known) count = 128;  // Virgil not reachable: anything goes
  auto name = [&](unsigned ch) -> std::wstring {
    return ch >= 1 && ch <= names.size() ? widen(names[ch - 1]) : std::wstring();
  };
  int sel = -1;
  auto add = [&](unsigned f, bool m, const std::wstring& extra) {
    std::wstring t = channels_short(f, m);
    if (!extra.empty()) t += L"   " + extra;
    const int i = int(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str())));
    SendMessageW(combo, CB_SETITEMDATA, WPARAM(i), LPARAM(f * 2 + (m ? 1 : 0)));
    if (f == first && m == mono) sel = i;
  };
  for (unsigned f = 1; f + 1 <= count; ++f) {
    const std::wstring a = name(f), b = name(f + 1);
    add(f, false, a.empty() && b.empty() ? L"" : L"(" + a + L" / " + b + L")");
  }
  for (unsigned f = 1; f <= count; ++f) {
    const std::wstring a = name(f);
    add(f, true, a.empty() ? L"" : L"(" + a + L")");
  }
  if (sel < 0 && first >= 1) add(first, mono, known ? L"(not available)" : L"");
  SendMessageW(combo, CB_SETCURSEL, WPARAM(std::max(0, sel)), 0);
}

bool combo_channels(HWND combo, unsigned* first, bool* mono) {
  const LRESULT i = SendMessageW(combo, CB_GETCURSEL, 0, 0);
  if (i == CB_ERR) return false;
  const LRESULT v = SendMessageW(combo, CB_GETITEMDATA, WPARAM(i), 0);
  *first = unsigned(v / 2);
  *mono = (v & 1) != 0;
  return *first >= 1;
}

// Output device combo: Default, then every active device; a configured one
// that is unplugged stays, marked. ids[i] is item i's id.
void fill_devices(HWND combo, const Inventory* inv, const std::string& cur, const std::string& cur_name,
                  std::vector<std::string>* ids) {
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  ids->clear();
  int sel = 0;
  auto add = [&](const std::wstring& label, const std::string& id) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    ids->push_back(id);
  };
  add(L"Default output device (follows Windows)", "default");
  bool found = cur.empty() || cur == "default";
  if (inv)
    for (const auto& e : inv->devices) {
      const std::string id = narrow(e.id);
      if (id == cur) {
        sel = int(ids->size());
        found = true;
      }
      add(e.name, id);
    }
  if (!found) {
    sel = int(ids->size());
    add((cur_name.empty() ? widen(cur) : widen(cur_name)) + (inv ? L"  (not connected)" : L""), cur);
  }
  SendMessageW(combo, CB_SETCURSEL, WPARAM(sel), 0);
}

// ---- the source dialog ------------------------------------------------------------

struct SourceDlg {
  vc::DesktopAudioSource src;
  std::shared_ptr<Inventory> inv;
  std::vector<std::string> ids;
  ChannelInfo ch;
};

void source_enable(HWND d) {
  const bool supported = wa::process_loopback_supported();
  const bool dev = IsDlgButtonChecked(d, IDC_DS_DEV) == BST_CHECKED;
  const bool app = IsDlgButtonChecked(d, IDC_DS_APP) == BST_CHECKED;
  EnableWindow(GetDlgItem(d, IDC_DS_DEVICE), dev);
  EnableWindow(GetDlgItem(d, IDC_DS_APPNAME), app && supported);
  EnableWindow(GetDlgItem(d, IDC_DS_REFRESH), app && supported);
  EnableWindow(GetDlgItem(d, IDC_DS_APP), supported || app);
  EnableWindow(GetDlgItem(d, IDC_DS_ALL), supported || IsDlgButtonChecked(d, IDC_DS_ALL) == BST_CHECKED);
}

void source_fill_apps(HWND d, const SourceDlg& s) {
  HWND combo = GetDlgItem(d, IDC_DS_APPNAME);
  const std::wstring keep = get_text(d, IDC_DS_APPNAME);
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  std::wstring playing;
  if (s.inv)
    for (const auto& a : s.inv->apps) {
      SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(a.exe.c_str()));
      if (a.active) playing += (playing.empty() ? L"" : L", ") + a.exe;
    }
  SetDlgItemTextW(d, IDC_DS_APPNAME, keep.c_str());
  std::wstring hint;
  if (!wa::process_loopback_supported())
    hint = std::wstring(L"App capture ") + wa::kNeedsWin2004 + L" (this is build " +
           std::to_wstring(wa::windows_build()) + L").";
  else if (!s.inv)
    hint = L"Looking for apps\u2026";
  else
    hint = (playing.empty() ? std::wstring(L"No app is playing sound right now.") : L"Playing now: " + playing + L".") +
           L" Or type the program's file name, e.g. spotify.exe.";
  SetDlgItemTextW(d, IDC_DS_APPHINT, hint.c_str());
}

INT_PTR CALLBACK source_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
  SourceDlg* s = reinterpret_cast<SourceDlg*>(GetWindowLongPtrW(d, DWLP_USER));
  switch (msg) {
    case WM_INITDIALOG: {
      SetWindowLongPtrW(d, DWLP_USER, lp);
      s = reinterpret_cast<SourceDlg*>(lp);
      if (HWND owner = GetParent(d)) {
        const HICON ic = reinterpret_cast<HICON>(SendMessageW(owner, WM_GETICON, ICON_SMALL, 0));
        if (ic) SendMessageW(d, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(ic));
      }
      const int radio = s->src.type == vc::DesktopAudioSource::kApp      ? IDC_DS_APP
                        : s->src.type == vc::DesktopAudioSource::kAllApps ? IDC_DS_ALL
                                                                          : IDC_DS_DEV;
      CheckRadioButton(d, IDC_DS_DEV, IDC_DS_ALL, radio);
      fill_devices(GetDlgItem(d, IDC_DS_DEVICE), s->inv.get(), s->src.device, s->src.name, &s->ids);
      SetDlgItemTextW(d, IDC_DS_APPNAME, widen(s->src.app).c_str());
      SendDlgItemMessageW(d, IDC_DS_APPNAME, CB_LIMITTEXT, 120, 0);
      source_fill_apps(d, *s);
      fill_channels(GetDlgItem(d, IDC_DS_CH), s->ch.tx, s->ch.tx_names, s->src.channel, s->src.mono);
      SetDlgItemTextW(d, IDC_DS_GAIN, gain_text(s->src.gain_db).c_str());
      CheckDlgButton(d, IDC_DS_MUTE, s->src.mute ? BST_CHECKED : BST_UNCHECKED);
      source_enable(d);
      if (!s->inv) load_inventory(d);
      return TRUE;
    }
    case WM_DA_INVENTORY: {
      std::unique_ptr<Inventory> inv(reinterpret_cast<Inventory*>(lp));
      if (!s || !inv) return TRUE;
      // Keep the device chosen in the combo across the refill.
      const LRESULT i = SendDlgItemMessageW(d, IDC_DS_DEVICE, CB_GETCURSEL, 0, 0);
      std::string cur = i >= 0 && size_t(i) < s->ids.size() ? s->ids[size_t(i)] : s->src.device;
      s->inv = std::shared_ptr<Inventory>(inv.release());
      fill_devices(GetDlgItem(d, IDC_DS_DEVICE), s->inv.get(), cur, s->src.name, &s->ids);
      source_fill_apps(d, *s);
      return TRUE;
    }
    case WM_CTLCOLORSTATIC:
      if (GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == IDC_DS_ERR) {
        SetTextColor(reinterpret_cast<HDC>(wp), RGB(190, 40, 30));
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
      }
      break;
    case WM_COMMAND: {
      if (!s) break;
      const int id = LOWORD(wp);
      if (id == IDC_DS_DEV || id == IDC_DS_APP || id == IDC_DS_ALL) {
        CheckRadioButton(d, IDC_DS_DEV, IDC_DS_ALL, id);
        source_enable(d);
        if (id == IDC_DS_APP) SetFocus(GetDlgItem(d, IDC_DS_APPNAME));
        return TRUE;
      }
      if (id == IDC_DS_DEVICE && HIWORD(wp) == CBN_SELCHANGE) {
        CheckRadioButton(d, IDC_DS_DEV, IDC_DS_ALL, IDC_DS_DEV);
        source_enable(d);
        return TRUE;
      }
      if (id == IDC_DS_REFRESH) {
        SetDlgItemTextW(d, IDC_DS_APPHINT, L"Looking for apps\u2026");
        load_inventory(d);
        return TRUE;
      }
      if (id == IDCANCEL) {
        EndDialog(d, 0);
        return TRUE;
      }
      if (id == IDOK) {
        vc::DesktopAudioSource r = s->src;
        if (IsDlgButtonChecked(d, IDC_DS_APP) == BST_CHECKED) {
          std::wstring exe = get_text(d, IDC_DS_APPNAME);
          while (!exe.empty() && (exe.back() == L' ' || exe.back() == L'"')) exe.pop_back();
          while (!exe.empty() && (exe.front() == L' ' || exe.front() == L'"')) exe.erase(exe.begin());
          const size_t slash = exe.find_last_of(L"\\/");
          if (slash != std::wstring::npos) exe = exe.substr(slash + 1);  // a full path pasted in
          if (!exe.empty() && exe.find(L'.') == std::wstring::npos) exe += L".exe";
          if (exe.empty()) {
            SetDlgItemTextW(d, IDC_DS_ERR, L"Choose an app, or type its program file name (for example spotify.exe).");
            return TRUE;
          }
          r.type = vc::DesktopAudioSource::kApp;
          r.app = narrow(exe);
        } else if (IsDlgButtonChecked(d, IDC_DS_ALL) == BST_CHECKED) {
          r.type = vc::DesktopAudioSource::kAllApps;
        } else {
          r.type = vc::DesktopAudioSource::kDevice;
          const LRESULT i = SendDlgItemMessageW(d, IDC_DS_DEVICE, CB_GETCURSEL, 0, 0);
          r.device = i >= 0 && size_t(i) < s->ids.size() ? s->ids[size_t(i)] : std::string("default");
          r.name = r.device == "default" ? std::string() : narrow(device_name(r.device, r.name, s->inv.get()));
        }
        if (r.type != vc::DesktopAudioSource::kApp) r.app.clear();
        if (r.type != vc::DesktopAudioSource::kDevice) {
          r.device = "default";
          r.name.clear();
        }
        if (!combo_channels(GetDlgItem(d, IDC_DS_CH), &r.channel, &r.mono)) {
          SetDlgItemTextW(d, IDC_DS_ERR, L"Choose the transmit channels.");
          return TRUE;
        }
        if (!parse_gain(get_text(d, IDC_DS_GAIN), &r.gain_db)) {
          SetDlgItemTextW(d, IDC_DS_ERR, L"Enter the gain in dB, for example -6 (from -60 to +12).");
          return TRUE;
        }
        r.mute = IsDlgButtonChecked(d, IDC_DS_MUTE) == BST_CHECKED;
        s->src = r;
        EndDialog(d, 1);
        return TRUE;
      }
      break;
    }
  }
  return FALSE;
}

// ---- the main window --------------------------------------------------------------

struct Main {
  HINSTANCE inst = nullptr;
  HICON icon = nullptr;
  Engine* engine = nullptr;
  ChannelSource channels = nullptr;
  vc::DesktopAudioSettings cfg;
  std::shared_ptr<Inventory> inv;
  ChannelInfo ch;
  std::vector<std::string> out_ids;
  std::vector<float> level;
  std::vector<std::wstring> shown;
  std::wstring shown_status, shown_out, save_error;
  float out_level = 0;
  DWORD last_tick = 0, last_channels = 0;
  Status::Phase phase = Status::kOff;
  bool status_error = false, out_error = false, filling = false;
  HBRUSH off = nullptr, green = nullptr, yellow = nullptr, red = nullptr;
};

HWND g_open = nullptr;

Main* main_data(HWND d) { return reinterpret_cast<Main*>(GetWindowLongPtrW(d, DWLP_USER)); }

ChannelInfo current_channels(Main& m) {
  ChannelInfo c = m.channels ? m.channels() : ChannelInfo();
  return c;
}

void fill_output(HWND d, Main& m) {
  m.filling = true;
  const vc::DesktopAudioOutput& o = m.cfg.output;
  CheckDlgButton(d, IDC_DA_OUT_ENABLE, o.enabled ? BST_CHECKED : BST_UNCHECKED);
  fill_channels(GetDlgItem(d, IDC_DA_OUT_CH), m.ch.rx, m.ch.rx_names, o.channel, o.mono);
  fill_devices(GetDlgItem(d, IDC_DA_OUT_DEV), m.inv.get(), o.device, o.name, &m.out_ids);
  SetDlgItemTextW(d, IDC_DA_OUT_GAIN, gain_text(o.gain_db).c_str());
  m.filling = false;
}

void fill_list(HWND d, Main& m) {
  HWND list = GetDlgItem(d, IDC_DA_LIST);
  const int keep = int(SendMessageW(list, LVM_GETNEXTITEM, WPARAM(-1), LVNI_SELECTED));
  SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
  m.level.assign(m.cfg.sources.size(), 0.f);
  m.shown.assign(m.cfg.sources.size(), std::wstring());
  for (size_t i = 0; i < m.cfg.sources.size(); ++i) {
    const vc::DesktopAudioSource& s = m.cfg.sources[i];
    std::wstring label = source_label(s, m.inv.get());
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = int(i);
    it.pszText = &label[0];
    SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
    auto set = [&](int col, std::wstring t) {
      LVITEMW si{};
      si.iSubItem = col;
      si.pszText = &t[0];
      SendMessageW(list, LVM_SETITEMTEXTW, WPARAM(i), reinterpret_cast<LPARAM>(&si));
    };
    set(kColChannels, channels_short(s.channel, s.mono));
    set(kColGain, s.mute ? std::wstring(L"muted") : gain_text(s.gain_db));
  }
  if (keep >= 0 && size_t(keep) < m.cfg.sources.size()) {
    LVITEMW st{};
    st.stateMask = st.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(list, LVM_SETITEMSTATE, WPARAM(keep), reinterpret_cast<LPARAM>(&st));
  }
  const bool sel = SendMessageW(list, LVM_GETNEXTITEM, WPARAM(-1), LVNI_SELECTED) >= 0;
  EnableWindow(GetDlgItem(d, IDC_DA_EDIT), sel);
  EnableWindow(GetDlgItem(d, IDC_DA_REMOVE), sel);
  EnableWindow(GetDlgItem(d, IDC_DA_ADD), m.cfg.sources.size() < vc::kMaxDesktopSources);
}

// Applies the edited settings right away and saves them.
void commit(HWND d, Main& m) {
  m.engine->apply(m.cfg);
  std::wstring err;
  m.save_error = save_settings(m.cfg, &err) ? std::wstring() : err;
  m.shown_status.clear();  // the status line is redone on the next tick
}

int selected(HWND d) {
  return int(SendDlgItemMessageW(d, IDC_DA_LIST, LVM_GETNEXTITEM, WPARAM(-1), LVNI_SELECTED));
}

void edit_source(HWND d, Main& m, int index) {
  SourceDlg s;
  if (index >= 0 && size_t(index) < m.cfg.sources.size()) {
    s.src = m.cfg.sources[size_t(index)];
  } else {
    index = -1;
    // A new source: the first odd stereo pair no other source uses.
    const unsigned count = m.ch.tx ? m.ch.tx : 128;
    s.src.channel = 1;
    for (unsigned first = 1; first + 1 <= count; first += 2) {
      bool clash = false;
      for (const auto& o : m.cfg.sources)
        if (first < o.channel + o.width() && o.channel < first + 2) clash = true;
      if (!clash) {
        s.src.channel = first;
        break;
      }
    }
  }
  s.inv = m.inv;
  s.ch = m.ch;
  if (DialogBoxParamW(m.inst, MAKEINTRESOURCEW(IDD_DA_SOURCE), d, source_proc, reinterpret_cast<LPARAM>(&s)) != 1)
    return;
  if (index < 0) m.cfg.sources.push_back(s.src);
  else m.cfg.sources[size_t(index)] = s.src;
  // A new source means the user wants desktop audio on.
  if (index < 0 && !m.cfg.enabled) {
    m.cfg.enabled = true;
    CheckDlgButton(d, IDC_DA_ENABLE, BST_CHECKED);
  }
  if (s.inv && s.inv != m.inv) m.inv = s.inv;  // refreshed in there
  commit(d, m);
  fill_list(d, m);
}

void read_output(HWND d, Main& m) {
  vc::DesktopAudioOutput o = m.cfg.output;
  o.enabled = IsDlgButtonChecked(d, IDC_DA_OUT_ENABLE) == BST_CHECKED;
  combo_channels(GetDlgItem(d, IDC_DA_OUT_CH), &o.channel, &o.mono);
  const LRESULT i = SendDlgItemMessageW(d, IDC_DA_OUT_DEV, CB_GETCURSEL, 0, 0);
  if (i >= 0 && size_t(i) < m.out_ids.size()) {
    o.device = m.out_ids[size_t(i)];
    o.name = o.device == "default" ? std::string() : narrow(device_name(o.device, o.name, m.inv.get()));
  }
  double g = o.gain_db;
  if (parse_gain(get_text(d, IDC_DA_OUT_GAIN), &g)) o.gain_db = g;
  else SetDlgItemTextW(d, IDC_DA_OUT_GAIN, gain_text(o.gain_db).c_str());
  if (o == m.cfg.output) return;
  m.cfg.output = o;
  if (o.enabled && !m.cfg.enabled) {
    m.cfg.enabled = true;
    CheckDlgButton(d, IDC_DA_ENABLE, BST_CHECKED);
  }
  commit(d, m);
}

void tick(HWND d, Main& m) {
  const DWORD now = GetTickCount();
  const float dt = m.last_tick ? std::min(0.25f, (now - m.last_tick) / 1000.f) : 0.f;
  m.last_tick = now;
  const Status st = m.engine->status();

  // Channel names and counts: Virgil's own while connected, else the
  // service's report. Combos follow when they change.
  if (now - m.last_channels > 1000) {
    m.last_channels = now;
    ChannelInfo c;
    if (st.connected) {
      c.tx = st.tx;
      c.rx = st.rx;
      c.tx_names = st.tx_names;
      c.rx_names = st.rx_names;
    } else {
      c = current_channels(m);
    }
    const bool changed = c.rx != m.ch.rx || c.rx_names != m.ch.rx_names;
    m.ch = c;
    if (changed && !SendDlgItemMessageW(d, IDC_DA_OUT_CH, CB_GETDROPPEDSTATE, 0, 0)) {
      m.filling = true;
      fill_channels(GetDlgItem(d, IDC_DA_OUT_CH), m.ch.rx, m.ch.rx_names, m.cfg.output.channel, m.cfg.output.mono);
      m.filling = false;
    }
  }

  // Status line.
  std::wstring line = m.save_error.empty() ? st.text : L"Not saved: " + m.save_error;
  bool err = st.phase == Status::kError || !m.save_error.empty();
  if (!st.app_capture_supported)
    for (const auto& s : m.cfg.sources)
      if (s.type != vc::DesktopAudioSource::kDevice) {
        line += std::wstring(L" App capture ") + wa::kNeedsWin2004 + L".";
        err = true;
        break;
      }
  if (line != m.shown_status) {
    m.shown_status = line;
    m.phase = st.phase;
    m.status_error = err;
    SetDlgItemTextW(d, IDC_DA_STATUS, line.c_str());
  }

  // Sources: status text and meters.
  HWND list = GetDlgItem(d, IDC_DA_LIST);
  for (size_t i = 0; i < m.cfg.sources.size(); ++i) {
    const SourceStatus ss = i < st.sources.size() ? st.sources[i] : SourceStatus();
    std::wstring t = (ss.error ? L"\u26A0 " : L"") + ss.text;
    if (t != m.shown[i]) {
      m.shown[i] = t;
      LVITEMW si{};
      si.iSubItem = kColStatus;
      si.pszText = &t[0];
      SendMessageW(list, LVM_SETITEMTEXTW, WPARAM(i), reinterpret_cast<LPARAM>(&si));
    }
    const float v = lin_to_pos(ss.peak);
    const float shown = std::max(v, m.level[i] - dt * 0.6f);
    if (shown != m.level[i]) {
      m.level[i] = shown;
      RECT rc{};
      rc.top = kColLevel;
      rc.left = LVIR_BOUNDS;
      if (SendMessageW(list, LVM_GETSUBITEMRECT, WPARAM(i), reinterpret_cast<LPARAM>(&rc))) InvalidateRect(list, &rc, FALSE);
    }
  }

  // Output.
  const float ov = lin_to_pos(st.output.peak);
  m.out_level = std::max(ov, m.out_level - dt * 0.6f);
  SendDlgItemMessageW(d, IDC_DA_OUT_METER, PBM_SETPOS, WPARAM(m.out_level * 1000), 0);
  const std::wstring ot = m.cfg.output.enabled ? st.output.text : std::wstring();
  if (ot != m.shown_out || st.output.error != m.out_error) {
    m.shown_out = ot;
    m.out_error = st.output.error;
    SetDlgItemTextW(d, IDC_DA_OUT_STATE, ot.c_str());
  }
}

void draw_level(HWND list, Main& m, NMLVCUSTOMDRAW* cd) {
  const int item = int(cd->nmcd.dwItemSpec);
  RECT rc{};
  rc.top = kColLevel;
  rc.left = LVIR_BOUNDS;
  if (!SendMessageW(list, LVM_GETSUBITEMRECT, WPARAM(item), reinterpret_cast<LPARAM>(&rc))) return;
  const bool sel = (SendMessageW(list, LVM_GETITEMSTATE, WPARAM(item), LVIS_SELECTED) & LVIS_SELECTED) != 0;
  FillRect(cd->nmcd.hdc, &rc,
           GetSysColorBrush(!sel ? COLOR_WINDOW : GetFocus() == list ? COLOR_HIGHLIGHT : COLOR_BTNFACE));
  RECT bar = {rc.left + 4, rc.top + (rc.bottom - rc.top) / 2 - 3, rc.right - 4, rc.top + (rc.bottom - rc.top) / 2 + 3};
  if (bar.right <= bar.left) return;
  FillRect(cd->nmcd.hdc, &bar, m.off);
  const float v = size_t(item) < m.level.size() ? m.level[size_t(item)] : 0.f;
  if (v <= 0) return;
  RECT on = bar;
  on.right = bar.left + LONG(float(bar.right - bar.left) * v);
  // -60..0 dBFS: green, yellow above -18 dBFS, red above -6.
  FillRect(cd->nmcd.hdc, &on, v > 0.9f ? m.red : v > 0.7f ? m.yellow : m.green);
}

INT_PTR CALLBACK main_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
  Main* m = main_data(d);
  switch (msg) {
    case WM_INITDIALOG: {
      SetWindowLongPtrW(d, DWLP_USER, lp);
      m = reinterpret_cast<Main*>(lp);
      g_open = d;
      if (m->icon) SendMessageW(d, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(m->icon));
      m->off = CreateSolidBrush(RGB(0x2A, 0x2E, 0x34));
      m->green = CreateSolidBrush(RGB(0x4E, 0xA8, 0x5A));
      m->yellow = CreateSolidBrush(RGB(0xD9, 0xA9, 0x3F));
      m->red = CreateSolidBrush(RGB(0xD2, 0x55, 0x3F));
      m->cfg = m->engine->settings();
      m->ch = current_channels(*m);
      m->last_channels = GetTickCount();
      CheckDlgButton(d, IDC_DA_ENABLE, m->cfg.enabled ? BST_CHECKED : BST_UNCHECKED);
      HWND list = GetDlgItem(d, IDC_DA_LIST);
      SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                   LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
      RECT rc;
      GetClientRect(list, &rc);
      const int w = rc.right - rc.left - GetSystemMetrics(SM_CXVSCROLL);
      const struct {
        const wchar_t* name;
        int pct;
      } cols[] = {{L"Source", 30}, {L"Channels", 14}, {L"Gain", 9}, {L"Level", 14}, {L"Status", 33}};
      for (int c = 0; c < 5; ++c) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<wchar_t*>(cols[c].name);
        col.cx = w * cols[c].pct / 100;
        col.iSubItem = c;
        SendMessageW(list, LVM_INSERTCOLUMNW, WPARAM(c), reinterpret_cast<LPARAM>(&col));
      }
      SendDlgItemMessageW(d, IDC_DA_OUT_METER, PBM_SETRANGE32, 0, 1000);
      fill_list(d, *m);
      fill_output(d, *m);
      tick(d, *m);
      SetTimer(d, 1, 60, nullptr);
      load_inventory(d);
      return TRUE;
    }
    case WM_DESTROY:
      KillTimer(d, 1);
      if (m) {
        HBRUSH* brushes[] = {&m->off, &m->green, &m->yellow, &m->red};
        for (HBRUSH* b : brushes) {
          if (*b) DeleteObject(*b);
          *b = nullptr;
        }
      }
      if (g_open == d) g_open = nullptr;
      break;
    case WM_TIMER:
      if (m) tick(d, *m);
      return TRUE;
    case WM_DA_INVENTORY: {
      std::unique_ptr<Inventory> inv(reinterpret_cast<Inventory*>(lp));
      if (!m || !inv) return TRUE;
      m->inv = std::shared_ptr<Inventory>(inv.release());
      fill_output(d, *m);
      fill_list(d, *m);  // device names
      return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
      const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
      if (m && (id == IDC_DA_STATUS || id == IDC_DA_OUT_STATE)) {
        COLORREF c = GetSysColor(COLOR_WINDOWTEXT);
        if (id == IDC_DA_STATUS)
          c = m->status_error ? RGB(190, 40, 30)
              : m->phase == Status::kRunning ? RGB(30, 120, 50)
              : m->phase == Status::kWaiting ? RGB(160, 100, 0)
                                             : GetSysColor(COLOR_GRAYTEXT);
        else if (m->out_error)
          c = RGB(190, 40, 30);
        SetTextColor(reinterpret_cast<HDC>(wp), c);
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
      }
      break;
    }
    case WM_NOTIFY: {
      const NMHDR* h = reinterpret_cast<const NMHDR*>(lp);
      if (!m || h->idFrom != IDC_DA_LIST) break;
      if (h->code == NM_CUSTOMDRAW) {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
        LRESULT r = CDRF_DODEFAULT;
        switch (cd->nmcd.dwDrawStage) {
          case CDDS_PREPAINT: r = CDRF_NOTIFYITEMDRAW; break;
          case CDDS_ITEMPREPAINT: r = CDRF_NOTIFYSUBITEMDRAW; break;
          case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
            if (cd->iSubItem == kColLevel) {
              draw_level(h->hwndFrom, *m, cd);
              r = CDRF_SKIPDEFAULT;
            }
            break;
          default: break;
        }
        SetWindowLongPtrW(d, DWLP_MSGRESULT, r);
        return TRUE;
      }
      if (h->code == NM_DBLCLK) {
        const int i = selected(d);
        if (i >= 0) edit_source(d, *m, i);
        return TRUE;
      }
      if (h->code == LVN_ITEMCHANGED) {
        const bool sel = selected(d) >= 0;
        EnableWindow(GetDlgItem(d, IDC_DA_EDIT), sel);
        EnableWindow(GetDlgItem(d, IDC_DA_REMOVE), sel);
        return TRUE;
      }
      if (h->code == LVN_KEYDOWN && reinterpret_cast<const NMLVKEYDOWN*>(lp)->wVKey == VK_DELETE) {
        PostMessageW(d, WM_COMMAND, MAKEWPARAM(IDC_DA_REMOVE, BN_CLICKED), 0);
        return TRUE;
      }
      break;
    }
    case WM_COMMAND: {
      if (!m) break;
      const int id = LOWORD(wp), code = HIWORD(wp);
      switch (id) {
        case IDC_DA_ENABLE:
          m->cfg.enabled = IsDlgButtonChecked(d, IDC_DA_ENABLE) == BST_CHECKED;
          commit(d, *m);
          return TRUE;
        case IDC_DA_ADD:
          if (m->cfg.sources.size() < vc::kMaxDesktopSources) edit_source(d, *m, -1);
          return TRUE;
        case IDC_DA_EDIT:
          if (selected(d) >= 0) edit_source(d, *m, selected(d));
          return TRUE;
        case IDC_DA_REMOVE: {
          const int i = selected(d);
          if (i < 0 || size_t(i) >= m->cfg.sources.size()) return TRUE;
          m->cfg.sources.erase(m->cfg.sources.begin() + i);
          commit(d, *m);
          fill_list(d, *m);
          return TRUE;
        }
        case IDC_DA_OUT_ENABLE:
          if (!m->filling) read_output(d, *m);
          return TRUE;
        case IDC_DA_OUT_CH:
        case IDC_DA_OUT_DEV:
          if (code == CBN_SELCHANGE && !m->filling) read_output(d, *m);
          return TRUE;
        case IDC_DA_OUT_GAIN:
          if (code == EN_KILLFOCUS && !m->filling) read_output(d, *m);
          return TRUE;
        case IDC_DA_HELP:
          MessageBoxW(d, kHelpText, L"Desktop audio", MB_ICONINFORMATION);
          return TRUE;
        case IDOK:  // Enter: take the gain field, stay open
          if (GetFocus() == GetDlgItem(d, IDC_DA_OUT_GAIN)) read_output(d, *m);
          return TRUE;
        case IDCANCEL:
          read_output(d, *m);  // a gain typed but not yet left
          EndDialog(d, 0);
          return TRUE;
        default:
          break;
      }
      break;
    }
  }
  return FALSE;
}

}  // namespace

HWND open_dialog() { return g_open; }

void show_dialog(HINSTANCE inst, HWND owner, HICON icon, Engine& engine, ChannelSource channels) {
  if (g_open) {
    ShowWindow(g_open, SW_SHOWNORMAL);
    SetForegroundWindow(g_open);
    return;
  }
  Main m;
  m.inst = inst;
  m.icon = icon;
  m.engine = &engine;
  m.channels = channels;
  DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_DESKTOP), owner, main_proc, reinterpret_cast<LPARAM>(&m));
  g_open = nullptr;
}

}  // namespace da
