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

#include "asio_host.h"
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
    L"also listen to Dante on this PC). Windows 10 version 2004 or later.\n"
    L"\u2022 Audio interface inputs: the inputs of an audio interface (microphones, instruments), one transmit "
    L"channel each, with a few milliseconds of latency. Through the interface's own ASIO driver (Virgil Control "
    L"then uses that driver itself; other apps may not be able to use it at the same time), or WASAPI for any "
    L"recording device.\n\n"
    L"Output devices and apps go to a pair of transmit channels (or one, mixed to mono); sources on the same "
    L"channels are mixed. Route those channels in Dante Controller like any other.\n\n"
    L"Receive channels can also go straight to an audio interface's outputs, one each (ASIO or WASAPI). An "
    L"input source and the outputs can share one ASIO driver; only one ASIO driver can be used at a time.\n\n"
    L"Dante receive channels can play on a Windows output device too. Do not play them on a device you also "
    L"capture: they would echo back into Dante.\n\n"
    L"Captured audio still plays on its own device. To hear it only through Dante, let those apps play on an "
    L"output you do not listen to, or turn that device down.\n\n"
    L"The audio is converted to Virgil's sample rate and follows the small difference between the clock of the "
    L"PC's sound hardware and the Dante clock. Output devices and apps add roughly 60 to 80 ms of latency, so "
    L"use them for music, video calls and system sounds, not for live monitoring; audio interfaces through ASIO "
    L"add a few ms (WASAPI: some 30 ms).\n\n"
    L"Desktop audio runs while Virgil Control runs (it starts with Windows) and pauses while Virgil is stopped. "
    L"Settings are saved in %APPDATA%\\Virgil\\desktop-audio.conf.";

namespace {

const UINT WM_DA_INVENTORY = WM_APP + 40;  // lParam: Inventory* (the receiver owns it)
const UINT WM_DA_PROBE = WM_APP + 41;      // lParam: ProbeResult* (the receiver owns it)
enum { kColSource, kColChannels, kColGain, kColLevel, kColStatus };

struct Inventory {
  std::vector<wa::Endpoint> devices;  // output devices
  std::vector<wa::Endpoint> capture;  // recording devices
  std::vector<wa::AudioApp> apps;
  ah::DriverList asio;
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
        inv->capture = wa::capture_endpoints();
        inv->apps = wa::audio_apps();
      }
    }
    inv->asio = ah::list_drivers();
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

std::wstring range_text(unsigned first, unsigned n) {
  wchar_t b[32];
  if (n <= 1) _snwprintf(b, 31, L"%u", first);
  else _snwprintf(b, 31, L"%u-%u", first, first + n - 1);
  b[31] = 0;
  return b;
}

bool is_asio(const std::string& api) { return api != "wasapi"; }

// An interface by what the settings hold: ASIO driver name, or endpoint.
std::wstring iface_name(const std::string& api, const std::string& id, const std::string& saved, bool output,
                        const Inventory* inv) {
  if (is_asio(api)) return id.empty() ? std::wstring(L"(no ASIO driver)") : widen(id);
  if (id.empty() || id == "default") return output ? L"Default output device" : L"Default recording device";
  const std::wstring wid = widen(id);
  if (inv)
    for (const auto& e : output ? inv->devices : inv->capture)
      if (e.id == wid) return e.name;
  return saved.empty() ? wid : widen(saved);
}

std::wstring output_summary(const vc::DesktopAudioOutput& o, const Inventory* inv) {
  return iface_name(o.api, o.device, o.name, true, inv) + L" outputs " + range_text(o.first_output, o.outputs) +
         L" \u2190 RX " + range_text(o.channel, o.outputs) + (is_asio(o.api) ? L" (ASIO)" : L" (WASAPI)");
}

std::wstring source_label(const vc::DesktopAudioSource& s, const Inventory* inv) {
  switch (s.type) {
    case vc::DesktopAudioSource::kInput:
      return iface_name(s.api, s.device, s.name, false, inv) + L" inputs " + range_text(s.first_input, s.inputs) +
             L" \u2192 TX " + range_text(s.channel, s.inputs) + (is_asio(s.api) ? L" (ASIO)" : L" (WASAPI)");
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

// Runs of `width` channels (an interface's inputs or outputs, one each).
// Item data: first * 2.
void fill_range(HWND combo, unsigned count, const std::vector<std::string>& names, unsigned first, unsigned width) {
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  const bool known = count > 0;
  if (!known) count = 128;
  if (width < 1) width = 1;
  auto name = [&](unsigned ch) -> std::wstring {
    return ch >= 1 && ch <= names.size() ? widen(names[ch - 1]) : std::wstring();
  };
  int sel = -1;
  auto add = [&](unsigned f, const std::wstring& extra) {
    std::wstring t = range_text(f, width);
    if (!extra.empty()) t += L"   " + extra;
    const int i = int(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str())));
    SendMessageW(combo, CB_SETITEMDATA, WPARAM(i), LPARAM(f * 2));
    if (f == first) sel = i;
  };
  for (unsigned f = 1; f + width - 1 <= count; ++f) {
    const std::wstring a = name(f), b = name(f + width - 1);
    add(f, a.empty() && b.empty() ? L"" : width == 1 ? L"(" + a + L")" : L"(" + a + L" \u2026 " + b + L")");
  }
  if (sel < 0 && first >= 1) add(first, known ? L"(not available)" : L"");
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

// ---- audio interface controls (source dialog and interface output dialog) --------

struct ProbeResult {
  std::string api, device;  // what was probed
  bool ok = false;
  std::wstring error;
  unsigned channels = 0;    // inputs or outputs, as asked
  std::vector<std::string> names;
  unsigned rate = 0;
  long min_buffer = 0, max_buffer = 0, pref_buffer = 0, granularity = 0;
};

// The ASIO driver's or endpoint's channel count (and names), off the UI
// thread: loading an ASIO driver can take seconds.
void probe_device(HWND target, const std::string& api, const std::string& device, bool output) {
  std::thread([target, api, device, output] {
    std::unique_ptr<ProbeResult> r(new ProbeResult);
    r->api = api;
    r->device = device;
    if (is_asio(api)) {
      const ah::Probe p = ah::probe(widen(device));
      r->ok = p.ok;
      r->error = p.error;
      r->channels = output ? p.outputs : p.inputs;
      for (const auto& n : output ? p.output_names : p.input_names) r->names.push_back(narrow(n));
      r->rate = unsigned(std::lround(p.rate));
      r->min_buffer = p.min_buffer;
      r->max_buffer = p.max_buffer;
      r->pref_buffer = p.pref_buffer;
      r->granularity = p.granularity;
    } else {
      wa::ComScope com;
      if (SUCCEEDED(com.hr)) r->channels = wa::endpoint_channels(!output, widen(device), &r->rate, &r->error);
      r->ok = r->channels > 0;
      if (!r->ok && r->error.empty()) r->error = L"cannot open the device";
    }
    if (PostMessageW(target, WM_DA_PROBE, 0, reinterpret_cast<LPARAM>(r.get()))) r.release();
  }).detach();
}

struct IfacePart {
  bool output = false;       // the interface's outputs (else inputs)
  std::vector<std::string> ids;  // IDC_IF_DEV items
  std::unique_ptr<ProbeResult> info;  // of the selected device, when known
  bool probing = false;
};

std::string iface_api(HWND d) {
  return SendDlgItemMessageW(d, IDC_IF_API, CB_GETCURSEL, 0, 0) == 1 ? "wasapi" : "asio";
}
std::string iface_device(HWND d, const IfacePart& p) {
  const LRESULT i = SendDlgItemMessageW(d, IDC_IF_DEV, CB_GETCURSEL, 0, 0);
  return i >= 0 && size_t(i) < p.ids.size() ? p.ids[size_t(i)] : std::string();
}
unsigned get_uint(HWND d, int id, unsigned dflt) {
  BOOL ok = FALSE;
  const UINT v = GetDlgItemInt(d, id, &ok, FALSE);
  return ok ? unsigned(v) : dflt;
}

void iface_info_text(HWND d, IfacePart& p, const Inventory* inv) {
  const bool asio = iface_api(d) == "asio";
  std::wstring t;
  if (asio && !ah::built_in()) {
    t = ah::kNotBuiltIn;
  } else if (asio && inv && inv->asio.drivers.empty()) {
    t = sizeof(void*) == 8 ? L"No 64-bit ASIO driver is installed" : L"No ASIO driver is installed";
    if (!inv->asio.other_bitness.empty()) {
      t += L" (32-bit only: " + inv->asio.other_bitness.front() +
           (inv->asio.other_bitness.size() > 1 ? L", …" : L"") + L")";
    }
    t += L". Install the interface's ASIO driver, or use WASAPI.";
  } else if (!inv) {
    t = L"Looking for devices…";
  } else if (p.probing) {
    t = asio ? L"Asking the ASIO driver…" : L"Asking the device…";
  } else if (p.info && !p.info->ok) {
    t = p.info->error;
  } else if (p.info) {
    const ProbeResult& r = *p.info;
    wchar_t b[256];
    _snwprintf(b, 255, L"The %ls has %u %ls", asio ? L"interface" : L"device", r.channels,
               p.output ? (r.channels == 1 ? L"output" : L"outputs") : (r.channels == 1 ? L"input" : L"inputs"));
    b[255] = 0;
    t = b;
    if (r.rate) t += L", " + std::to_wstring(r.rate / 1000) + (r.rate % 1000 ? L"." + std::to_wstring(r.rate % 1000 / 100) : L"") + L" kHz";
    if (asio && r.pref_buffer > 0) {
      _snwprintf(b, 255, L", buffer %ld (%ld-%ld)", r.pref_buffer, r.min_buffer, r.max_buffer);
      b[255] = 0;
      t += b;
    }
    if (!r.names.empty()) {
      t += L": ";
      for (size_t i = 0; i < r.names.size() && i < 4; ++i) t += (i ? L", " : L"") + widen(r.names[i]);
      if (r.names.size() > 4) t += L", …";
    }
    t += L".";
    if (asio) t += L" Other apps may not be able to use this driver while Virgil does.";
  }
  SetDlgItemTextW(d, IDC_IF_INFO, t.c_str());
}

void iface_fill_devices(HWND d, IfacePart& p, const Inventory* inv, const std::string& cur, const std::string& cur_name) {
  HWND combo = GetDlgItem(d, IDC_IF_DEV);
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  p.ids.clear();
  int sel = -1;
  auto add = [&](const std::wstring& label, const std::string& id) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (id == cur) sel = int(p.ids.size());
    p.ids.push_back(id);
  };
  if (iface_api(d) == "asio") {
    if (inv)
      for (const auto& drv : inv->asio.drivers) add(drv.name, narrow(drv.name));
    if (sel < 0 && !cur.empty() && cur != "default") add(widen(cur) + (inv ? L"  (not installed)" : L""), cur);
  } else {
    add(p.output ? L"Default output device (follows Windows)" : L"Default recording device (follows Windows)", "default");
    if (inv)
      for (const auto& e : p.output ? inv->devices : inv->capture) add(e.name, narrow(e.id));
    if (sel < 0 && !cur.empty() && cur != "default")
      add((cur_name.empty() ? widen(cur) : widen(cur_name)) + (inv ? L"  (not connected)" : L""), cur);
  }
  SendMessageW(combo, CB_SETCURSEL, WPARAM(std::max(0, sel)), 0);
  EnableWindow(GetDlgItem(d, IDC_IF_BUFFER), iface_api(d) == "asio");
}

void iface_probe(HWND d, IfacePart& p, const Inventory* inv) {
  const std::string api = iface_api(d), dev = iface_device(d, p);
  if (p.info && p.info->api == api && p.info->device == dev) {
    iface_info_text(d, p, inv);
    return;
  }
  p.info.reset();
  p.probing = !dev.empty() && !(api == "asio" && !ah::built_in());
  if (p.probing) probe_device(d, api, dev, p.output);
  iface_info_text(d, p, inv);
}

// A probe result: kept if it is still for the selected device.
void iface_on_probe(HWND d, IfacePart& p, const Inventory* inv, ProbeResult* r) {
  std::unique_ptr<ProbeResult> own(r);
  if (!r || r->api != iface_api(d) || r->device != iface_device(d, p)) return;
  p.info = std::move(own);
  p.probing = false;
  if (p.info->ok && p.info->channels)
    SendDlgItemMessageW(d, IDC_IF_FIRST_SPIN, UDM_SETRANGE32, 1, LPARAM(p.info->channels));
  iface_info_text(d, p, inv);
}

const unsigned kBufferSizes[] = {0, 32, 48, 64, 96, 128, 192, 256, 384, 512, 1024, 2048};

void iface_init(HWND d, IfacePart& p, const Inventory* inv, const std::string& api, const std::string& device,
                const std::string& name, unsigned first, unsigned count, unsigned buffer) {
  HWND a = GetDlgItem(d, IDC_IF_API);
  SendMessageW(a, CB_RESETCONTENT, 0, 0);
  SendMessageW(a, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"ASIO"));
  SendMessageW(a, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WASAPI"));
  SendMessageW(a, CB_SETCURSEL, is_asio(api) ? 0 : 1, 0);
  HWND b = GetDlgItem(d, IDC_IF_BUFFER);
  SendMessageW(b, CB_RESETCONTENT, 0, 0);
  int sel = 0;
  for (unsigned v : kBufferSizes) {
    const std::wstring t = v ? std::to_wstring(v) + L" frames" : std::wstring(L"Driver default");
    const int i = int(SendMessageW(b, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str())));
    SendMessageW(b, CB_SETITEMDATA, WPARAM(i), LPARAM(v));
    if (v == buffer) sel = i;
  }
  if (buffer && sel == 0) {  // a size from the settings file not in the list
    const std::wstring t = std::to_wstring(buffer) + L" frames";
    sel = int(SendMessageW(b, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str())));
    SendMessageW(b, CB_SETITEMDATA, WPARAM(sel), LPARAM(buffer));
  }
  SendMessageW(b, CB_SETCURSEL, WPARAM(sel), 0);
  SendDlgItemMessageW(d, IDC_IF_FIRST_SPIN, UDM_SETRANGE32, 1, 256);
  SendDlgItemMessageW(d, IDC_IF_COUNT_SPIN, UDM_SETRANGE32, 1, LPARAM(vc::kMaxInterfaceInputs));
  SetDlgItemInt(d, IDC_IF_FIRST, first, FALSE);
  SetDlgItemInt(d, IDC_IF_COUNT, count, FALSE);
  std::string cur = device;
  if (is_asio(api) && (cur.empty() || cur == "default") && inv && !inv->asio.drivers.empty())
    cur = narrow(inv->asio.drivers.front().name);
  iface_fill_devices(d, p, inv, cur, name);
  iface_probe(d, p, inv);
}

// A new inventory: the device list again, keeping the choice.
void iface_on_inventory(HWND d, IfacePart& p, const Inventory* inv, const std::string& name) {
  std::string cur = iface_device(d, p);
  if (iface_api(d) == "asio" && cur.empty() && inv && !inv->asio.drivers.empty()) cur = narrow(inv->asio.drivers.front().name);
  iface_fill_devices(d, p, inv, cur, name);
  iface_probe(d, p, inv);
}

// WM_COMMAND for the interface controls; true if handled. *count_changed:
// the channel combo needs refilling.
bool iface_command(HWND d, IfacePart& p, const Inventory* inv, int id, int code, bool* touched) {
  *touched = false;
  if (id == IDC_IF_API && code == CBN_SELCHANGE) {
    std::string cur;
    if (iface_api(d) == "asio" && inv && !inv->asio.drivers.empty()) cur = narrow(inv->asio.drivers.front().name);
    else if (iface_api(d) == "wasapi") cur = "default";
    iface_fill_devices(d, p, inv, cur, std::string());
    iface_probe(d, p, inv);
    *touched = true;
    return true;
  }
  if (id == IDC_IF_DEV && code == CBN_SELCHANGE) {
    iface_probe(d, p, inv);
    *touched = true;
    return true;
  }
  if ((id == IDC_IF_FIRST || id == IDC_IF_COUNT) && code == EN_CHANGE) {
    *touched = true;
    return true;
  }
  if (id == IDC_IF_BUFFER && code == CBN_SELCHANGE) {
    *touched = true;
    return true;
  }
  return false;
}

// The interface controls into settings fields; false (and a reason) if incomplete.
bool iface_read(HWND d, const IfacePart& p, const Inventory* inv, std::string* api, std::string* device,
                std::string* name, unsigned* first, unsigned* count, unsigned* buffer, std::wstring* err) {
  *api = iface_api(d);
  *device = iface_device(d, p);
  if (device->empty()) {
    *err = *api == "asio" ? L"Choose an ASIO driver (or use WASAPI)." : L"Choose a device.";
    return false;
  }
  const unsigned f = get_uint(d, IDC_IF_FIRST, 0), n = get_uint(d, IDC_IF_COUNT, 0);
  if (f < 1 || f > 256) {
    *err = p.output ? L"Enter the first output (1 or more)." : L"Enter the first input (1 or more).";
    return false;
  }
  if (n < 1 || n > vc::kMaxInterfaceInputs) {
    *err = L"Enter how many channels (1 to " + std::to_wstring(vc::kMaxInterfaceInputs) + L").";
    return false;
  }
  if (p.info && p.info->ok && p.info->channels && f > p.info->channels) {
    *err = L"The device has " + std::to_wstring(p.info->channels) + (p.output ? L" outputs." : L" inputs.");
    return false;
  }
  *first = f;
  *count = n;
  const LRESULT bi = SendDlgItemMessageW(d, IDC_IF_BUFFER, CB_GETCURSEL, 0, 0);
  *buffer = *api == "asio" && bi >= 0 ? unsigned(SendDlgItemMessageW(d, IDC_IF_BUFFER, CB_GETITEMDATA, WPARAM(bi), 0)) : 0;
  *name = *api == "asio" || *device == "default" ? std::string()
                                                  : narrow(iface_name(*api, *device, std::string(), p.output, inv));
  return true;
}

// ---- the source dialog ------------------------------------------------------------

struct SourceDlg {
  vc::DesktopAudioSource src;
  std::shared_ptr<Inventory> inv;
  std::vector<std::string> ids;
  ChannelInfo ch;
  IfacePart part;
};

bool input_checked(HWND d) { return IsDlgButtonChecked(d, IDC_DS_INPUT) == BST_CHECKED; }

// The transmit channel combo: pairs / mono for desktop audio, a run of
// `count` for interface inputs. Keeps the current choice.
void source_fill_channels(HWND d, SourceDlg& s) {
  unsigned first = s.src.channel;
  bool mono = s.src.mono;
  combo_channels(GetDlgItem(d, IDC_DS_CH), &first, &mono);
  if (input_checked(d)) {
    const unsigned n = std::max(1u, std::min(vc::kMaxInterfaceInputs, get_uint(d, IDC_IF_COUNT, s.src.inputs)));
    fill_range(GetDlgItem(d, IDC_DS_CH), s.ch.tx, s.ch.tx_names, first, n);
  } else {
    fill_channels(GetDlgItem(d, IDC_DS_CH), s.ch.tx, s.ch.tx_names, first, mono);
  }
}

void source_enable(HWND d) {
  const bool supported = wa::process_loopback_supported();
  const bool dev = IsDlgButtonChecked(d, IDC_DS_DEV) == BST_CHECKED;
  const bool app = IsDlgButtonChecked(d, IDC_DS_APP) == BST_CHECKED;
  const bool in = input_checked(d);
  EnableWindow(GetDlgItem(d, IDC_DS_DEVICE), dev);
  EnableWindow(GetDlgItem(d, IDC_DS_APPNAME), app && supported);
  EnableWindow(GetDlgItem(d, IDC_DS_REFRESH), app && supported);
  EnableWindow(GetDlgItem(d, IDC_DS_APP), supported || app);
  EnableWindow(GetDlgItem(d, IDC_DS_ALL), supported || IsDlgButtonChecked(d, IDC_DS_ALL) == BST_CHECKED);
  const int iface[] = {IDC_IF_API, IDC_IF_DEV, IDC_IF_FIRST, IDC_IF_FIRST_SPIN, IDC_IF_COUNT, IDC_IF_COUNT_SPIN, IDC_IF_INFO};
  for (int id : iface) EnableWindow(GetDlgItem(d, id), in);
  EnableWindow(GetDlgItem(d, IDC_IF_BUFFER), in && iface_api(d) == "asio");
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
      const vc::DesktopAudioSource::Type ty = s->src.type;
      const int radio = ty == vc::DesktopAudioSource::kApp       ? IDC_DS_APP
                        : ty == vc::DesktopAudioSource::kAllApps ? IDC_DS_ALL
                        : ty == vc::DesktopAudioSource::kInput   ? IDC_DS_INPUT
                                                                 : IDC_DS_DEV;
      CheckRadioButton(d, IDC_DS_DEV, IDC_DS_INPUT, radio);
      const bool in = ty == vc::DesktopAudioSource::kInput;
      fill_devices(GetDlgItem(d, IDC_DS_DEVICE), s->inv.get(), in ? std::string("default") : s->src.device,
                   in ? std::string() : s->src.name, &s->ids);
      s->part.output = false;
      iface_init(d, s->part, s->inv.get(), in ? s->src.api : std::string("asio"), in ? s->src.device : std::string(),
                 in ? s->src.name : std::string(), s->src.first_input, s->src.inputs, s->src.buffer);
      SetDlgItemTextW(d, IDC_DS_APPNAME, widen(s->src.app).c_str());
      SendDlgItemMessageW(d, IDC_DS_APPNAME, CB_LIMITTEXT, 120, 0);
      source_fill_apps(d, *s);
      fill_channels(GetDlgItem(d, IDC_DS_CH), s->ch.tx, s->ch.tx_names, s->src.channel, s->src.mono);
      source_fill_channels(d, *s);
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
      iface_on_inventory(d, s->part, s->inv.get(), s->src.name);
      return TRUE;
    }
    case WM_DA_PROBE:
      if (s) iface_on_probe(d, s->part, s->inv.get(), reinterpret_cast<ProbeResult*>(lp));
      else delete reinterpret_cast<ProbeResult*>(lp);
      return TRUE;
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
      if (id == IDC_DS_DEV || id == IDC_DS_APP || id == IDC_DS_ALL || id == IDC_DS_INPUT) {
        const bool was_input = input_checked(d);
        CheckRadioButton(d, IDC_DS_DEV, IDC_DS_INPUT, id);
        source_enable(d);
        if (was_input != input_checked(d)) source_fill_channels(d, *s);
        if (id == IDC_DS_APP) SetFocus(GetDlgItem(d, IDC_DS_APPNAME));
        return TRUE;
      }
      if (id == IDC_DS_DEVICE && HIWORD(wp) == CBN_SELCHANGE) {
        const bool was_input = input_checked(d);
        CheckRadioButton(d, IDC_DS_DEV, IDC_DS_INPUT, IDC_DS_DEV);
        source_enable(d);
        if (was_input) source_fill_channels(d, *s);
        return TRUE;
      }
      bool touched = false;
      if (iface_command(d, s->part, s->inv.get(), id, HIWORD(wp), &touched)) {
        if (touched && !input_checked(d) && (HIWORD(wp) == CBN_SELCHANGE)) {
          CheckRadioButton(d, IDC_DS_DEV, IDC_DS_INPUT, IDC_DS_INPUT);
          source_enable(d);
          source_fill_channels(d, *s);
        } else if (id == IDC_IF_COUNT && input_checked(d)) {
          source_fill_channels(d, *s);
        }
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
        } else if (input_checked(d)) {
          r.type = vc::DesktopAudioSource::kInput;
          std::wstring err;
          if (!iface_read(d, s->part, s->inv.get(), &r.api, &r.device, &r.name, &r.first_input, &r.inputs, &r.buffer,
                          &err)) {
            SetDlgItemTextW(d, IDC_DS_ERR, err.c_str());
            return TRUE;
          }
        } else {
          r.type = vc::DesktopAudioSource::kDevice;
          const LRESULT i = SendDlgItemMessageW(d, IDC_DS_DEVICE, CB_GETCURSEL, 0, 0);
          r.device = i >= 0 && size_t(i) < s->ids.size() ? s->ids[size_t(i)] : std::string("default");
          r.name = r.device == "default" ? std::string() : narrow(device_name(r.device, r.name, s->inv.get()));
        }
        if (r.type != vc::DesktopAudioSource::kApp) r.app.clear();
        if (r.type != vc::DesktopAudioSource::kDevice && r.type != vc::DesktopAudioSource::kInput) {
          r.device = "default";
          r.name.clear();
        }
        if (r.type != vc::DesktopAudioSource::kInput) {
          r.api.clear();
          r.first_input = 1;
          r.inputs = 2;
          r.buffer = 0;
        }
        if (!combo_channels(GetDlgItem(d, IDC_DS_CH), &r.channel, &r.mono)) {
          SetDlgItemTextW(d, IDC_DS_ERR, L"Choose the transmit channels.");
          return TRUE;
        }
        if (!parse_gain(get_text(d, IDC_DS_GAIN), &r.gain_db)) {
          SetDlgItemTextW(d, IDC_DS_ERR, L"Enter the gain in dB, for example -6 (from -60 to +12).");
          return TRUE;
        }
        if (r.type == vc::DesktopAudioSource::kInput) r.mono = false;
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

// ---- the interface output dialog (receive channels -> an interface's outputs) ----------

struct OutputDlg {
  vc::DesktopAudioOutput out;
  std::shared_ptr<Inventory> inv;
  ChannelInfo ch;
  IfacePart part;
};

void ifout_fill_channels(HWND d, OutputDlg& o) {
  unsigned first = o.out.channel;
  bool mono = false;
  combo_channels(GetDlgItem(d, IDC_DS_CH), &first, &mono);
  const unsigned n = std::max(1u, std::min(vc::kMaxInterfaceInputs, get_uint(d, IDC_IF_COUNT, o.out.outputs)));
  fill_range(GetDlgItem(d, IDC_DS_CH), o.ch.rx, o.ch.rx_names, first, n);
}

INT_PTR CALLBACK ifout_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
  OutputDlg* o = reinterpret_cast<OutputDlg*>(GetWindowLongPtrW(d, DWLP_USER));
  switch (msg) {
    case WM_INITDIALOG: {
      SetWindowLongPtrW(d, DWLP_USER, lp);
      o = reinterpret_cast<OutputDlg*>(lp);
      if (HWND owner = GetParent(d)) {
        const HICON ic = reinterpret_cast<HICON>(SendMessageW(owner, WM_GETICON, ICON_SMALL, 0));
        if (ic) SendMessageW(d, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(ic));
      }
      o->part.output = true;
      iface_init(d, o->part, o->inv.get(), o->out.api.empty() ? std::string("asio") : o->out.api, o->out.device,
                 o->out.name, o->out.first_output, o->out.outputs, o->out.buffer);
      ifout_fill_channels(d, *o);
      if (!o->inv) load_inventory(d);
      return TRUE;
    }
    case WM_DA_INVENTORY: {
      std::unique_ptr<Inventory> inv(reinterpret_cast<Inventory*>(lp));
      if (!o || !inv) return TRUE;
      o->inv = std::shared_ptr<Inventory>(inv.release());
      iface_on_inventory(d, o->part, o->inv.get(), o->out.name);
      return TRUE;
    }
    case WM_DA_PROBE:
      if (o) iface_on_probe(d, o->part, o->inv.get(), reinterpret_cast<ProbeResult*>(lp));
      else delete reinterpret_cast<ProbeResult*>(lp);
      return TRUE;
    case WM_CTLCOLORSTATIC:
      if (GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == IDC_DS_ERR) {
        SetTextColor(reinterpret_cast<HDC>(wp), RGB(190, 40, 30));
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
      }
      break;
    case WM_COMMAND: {
      if (!o) break;
      const int id = LOWORD(wp);
      bool touched = false;
      if (iface_command(d, o->part, o->inv.get(), id, HIWORD(wp), &touched)) {
        if (id == IDC_IF_COUNT) ifout_fill_channels(d, *o);
        EnableWindow(GetDlgItem(d, IDC_IF_BUFFER), iface_api(d) == "asio");
        return TRUE;
      }
      if (id == IDCANCEL) {
        EndDialog(d, 0);
        return TRUE;
      }
      if (id == IDOK) {
        vc::DesktopAudioOutput r = o->out;
        std::wstring err;
        if (!iface_read(d, o->part, o->inv.get(), &r.api, &r.device, &r.name, &r.first_output, &r.outputs, &r.buffer,
                        &err)) {
          SetDlgItemTextW(d, IDC_DS_ERR, err.c_str());
          return TRUE;
        }
        bool mono = false;
        if (!combo_channels(GetDlgItem(d, IDC_DS_CH), &r.channel, &mono)) {
          SetDlgItemTextW(d, IDC_DS_ERR, L"Choose the receive channels.");
          return TRUE;
        }
        r.type = vc::DesktopAudioOutput::kInterface;
        r.mono = false;
        o->out = r;
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

// kDevice: channel pair and device combos; kInterface: a summary and the
// "Interface..." button.
void show_output_type(HWND d, Main& m) {
  const bool iface = m.cfg.output.type == vc::DesktopAudioOutput::kInterface;
  SendDlgItemMessageW(d, IDC_DA_OUT_TYPE, CB_SETCURSEL, iface ? 1 : 0, 0);
  ShowWindow(GetDlgItem(d, IDC_DA_OUT_CH), iface ? SW_HIDE : SW_SHOW);
  ShowWindow(GetDlgItem(d, IDC_DA_OUT_DEV), iface ? SW_HIDE : SW_SHOW);
  ShowWindow(GetDlgItem(d, IDC_DA_OUT_SUMMARY), iface ? SW_SHOW : SW_HIDE);
  ShowWindow(GetDlgItem(d, IDC_DA_OUT_SETUP), iface ? SW_SHOW : SW_HIDE);
  if (iface) SetDlgItemTextW(d, IDC_DA_OUT_SUMMARY, output_summary(m.cfg.output, m.inv.get()).c_str());
}

void fill_output(HWND d, Main& m) {
  m.filling = true;
  const vc::DesktopAudioOutput& o = m.cfg.output;
  CheckDlgButton(d, IDC_DA_OUT_ENABLE, o.enabled ? BST_CHECKED : BST_UNCHECKED);
  HWND type = GetDlgItem(d, IDC_DA_OUT_TYPE);
  if (SendMessageW(type, CB_GETCOUNT, 0, 0) == 0) {
    SendMessageW(type, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"an output device"));
    SendMessageW(type, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"audio interface outputs"));
  }
  const bool iface = o.type == vc::DesktopAudioOutput::kInterface;
  fill_channels(GetDlgItem(d, IDC_DA_OUT_CH), m.ch.rx, m.ch.rx_names, o.channel, iface ? false : o.mono);
  fill_devices(GetDlgItem(d, IDC_DA_OUT_DEV), m.inv.get(), iface ? std::string("default") : o.device,
               iface ? std::string() : o.name, &m.out_ids);
  SetDlgItemTextW(d, IDC_DA_OUT_GAIN, gain_text(o.gain_db).c_str());
  show_output_type(d, m);
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
    set(kColChannels, s.type == vc::DesktopAudioSource::kInput ? range_text(s.channel, s.inputs)
                                                               : channels_short(s.channel, s.mono));
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
  if (o.type == vc::DesktopAudioOutput::kDevice) {  // kInterface: set in its dialog
    combo_channels(GetDlgItem(d, IDC_DA_OUT_CH), &o.channel, &o.mono);
    const LRESULT i = SendDlgItemMessageW(d, IDC_DA_OUT_DEV, CB_GETCURSEL, 0, 0);
    if (i >= 0 && size_t(i) < m.out_ids.size()) {
      o.device = m.out_ids[size_t(i)];
      o.name = o.device == "default" ? std::string() : narrow(device_name(o.device, o.name, m.inv.get()));
    }
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

// The interface output dialog; true if it was confirmed (m.cfg.output updated).
bool edit_interface_output(HWND d, Main& m) {
  OutputDlg o;
  o.out = m.cfg.output;
  if (o.out.type != vc::DesktopAudioOutput::kInterface) {
    // From a stereo device output: start from the same receive channels.
    o.out.device.clear();
    o.out.name.clear();
    o.out.api = "asio";
    o.out.first_output = 1;
    o.out.outputs = 2;
    o.out.buffer = 0;
    // An ASIO input source's driver: the output most likely goes to the same interface.
    for (const auto& src : m.cfg.sources)
      if (src.type == vc::DesktopAudioSource::kInput && is_asio(src.api)) {
        o.out.device = src.device;
        o.out.buffer = src.buffer;
        break;
      }
  }
  o.inv = m.inv;
  o.ch = m.ch;
  if (DialogBoxParamW(m.inst, MAKEINTRESOURCEW(IDD_DA_IFOUT), d, ifout_proc, reinterpret_cast<LPARAM>(&o)) != 1)
    return false;
  if (o.inv && o.inv != m.inv) m.inv = o.inv;
  o.out.enabled = IsDlgButtonChecked(d, IDC_DA_OUT_ENABLE) == BST_CHECKED;
  double g = o.out.gain_db;
  if (parse_gain(get_text(d, IDC_DA_OUT_GAIN), &g)) o.out.gain_db = g;
  m.cfg.output = o.out;
  if (o.out.enabled && !m.cfg.enabled) {
    m.cfg.enabled = true;
    CheckDlgButton(d, IDC_DA_ENABLE, BST_CHECKED);
  }
  commit(d, m);
  fill_output(d, m);
  return true;
}

void output_type_changed(HWND d, Main& m) {
  const bool iface = SendDlgItemMessageW(d, IDC_DA_OUT_TYPE, CB_GETCURSEL, 0, 0) == 1;
  const bool was = m.cfg.output.type == vc::DesktopAudioOutput::kInterface;
  if (iface == was) return;
  if (iface) {
    if (!edit_interface_output(d, m)) show_output_type(d, m);  // cancelled: back to the device
    return;
  }
  vc::DesktopAudioOutput& o = m.cfg.output;
  o.type = vc::DesktopAudioOutput::kDevice;
  o.api.clear();
  o.device = "default";
  o.name.clear();
  o.mono = false;
  commit(d, m);
  fill_output(d, m);
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
      if (s.type == vc::DesktopAudioSource::kApp || s.type == vc::DesktopAudioSource::kAllApps) {
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
        case IDC_DA_OUT_TYPE:
          if (code == CBN_SELCHANGE && !m->filling) output_type_changed(d, *m);
          return TRUE;
        case IDC_DA_OUT_SETUP:
          edit_interface_output(d, *m);
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
