// desktop_audio_probe: checks Virgil's desktop audio capture on a Windows PC
// without Virgil (no service, no Virgil Control needed).
//
//   desktop_audio_probe list
//       Windows build, whether app capture is available, output devices
//       (index, default, format, id) and the apps with audio sessions.
//   desktop_audio_probe capture [source] [--seconds 5] [--out capture.wav]
//       source: --device default | <index from list> | <endpoint id>
//               --app chrome.exe   (process loopback of the app's process tree)
//               --pid 1234         (process loopback of that process tree)
//               --all-apps         (everything except this tool)
//       Writes a 32-bit float stereo WAV at the capture rate and prints the
//       packet statistics and the device clock measured against the PC's
//       performance counter (the drift Virgil's resampler follows).
//
// Built with the Windows cross build (build-win/tests/desktop_audio_probe.exe);
// not installed.
#include "wasapi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "virgil/platform.h"

namespace {

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
  return w;
}

int usage() {
  std::printf(
      "usage:\n"
      "  desktop_audio_probe list\n"
      "  desktop_audio_probe capture [--device default|<index>|<id> | --app name.exe | --pid N | --all-apps]\n"
      "                              [--seconds 5] [--out capture.wav]\n");
  return 2;
}

int list() {
  const DWORD build = wa::windows_build();
  std::printf("Windows build %lu; app capture (process loopback): %s\n\n", static_cast<unsigned long>(build),
              wa::process_loopback_supported() ? "available" : "not available (needs Windows 10 version 2004 or later)");
  HRESULT hr = S_OK;
  const std::vector<wa::Endpoint> eps = wa::render_endpoints(&hr);
  if (FAILED(hr)) std::printf("Cannot list output devices: %s\n", narrow(wa::hr_text(hr)).c_str());
  std::printf("Output devices:\n");
  for (size_t i = 0; i < eps.size(); ++i) {
    std::string format = "?";
    wa::Com<IMMDevice> d;
    wa::Com<IAudioClient> c;
    WAVEFORMATEX* mix = nullptr;
    wa::Format f;
    if (SUCCEEDED(wa::open_render_endpoint(eps[i].id, d.put())) &&
        SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, c.put_void())) &&
        SUCCEEDED(c->GetMixFormat(&mix))) {
      format = wa::describe_format(mix, &f) ? narrow(wa::format_text(f)) : "unsupported format";
      CoTaskMemFree(mix);
    }
    std::printf("  [%zu]%s %s  (%s)\n       id: %s\n", i, eps[i].is_default ? " (default)" : "", narrow(eps[i].name).c_str(),
                format.c_str(), narrow(eps[i].id).c_str());
  }
  std::printf("\nApps with audio sessions:\n");
  const std::vector<wa::AudioApp> apps = wa::audio_apps();
  if (apps.empty()) std::printf("  (none)\n");
  for (const auto& a : apps) {
    std::string roots;
    for (DWORD p : wa::root_processes(a.exe)) roots += (roots.empty() ? "" : ", ") + std::to_string(p);
    std::printf("  %-28s %s  session pid %lu, process tree roots: %s%s%s\n", narrow(a.exe).c_str(),
                a.active ? "playing" : "idle   ", static_cast<unsigned long>(a.pid), roots.empty() ? "-" : roots.c_str(),
                a.display.empty() ? "" : "  - ", narrow(a.display).c_str());
  }
  return 0;
}

void put32(std::FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
void put16(std::FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }

void wav_header(std::FILE* f, unsigned rate, unsigned ch, uint32_t data_bytes) {
  std::fseek(f, 0, SEEK_SET);
  std::fwrite("RIFF", 1, 4, f);
  put32(f, 36 + data_bytes);
  std::fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 3);  // IEEE float
  put16(f, uint16_t(ch));
  put32(f, rate);
  put32(f, rate * ch * 4);
  put16(f, uint16_t(ch * 4));
  put16(f, 32);
  std::fwrite("data", 1, 4, f);
  put32(f, data_bytes);
}

int capture(int argc, char** argv) {
  std::string device = "default", app, out = "capture.wav";
  long pid = -1;
  bool all = false, by_device = true;
  double seconds = 5;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    const bool more = i + 1 < argc;
    if (a == "--device" && more) device = argv[++i], by_device = true;
    else if (a == "--app" && more) app = argv[++i], by_device = false;
    else if (a == "--pid" && more) pid = std::strtol(argv[++i], nullptr, 10), by_device = false;
    else if (a == "--all-apps") all = true, by_device = false;
    else if (a == "--seconds" && more) seconds = std::atof(argv[++i]);
    else if (a == "--out" && more) out = argv[++i];
    else return usage();
  }
  if (!(seconds > 0) || seconds > 3600) seconds = 5;

  wa::Capture cap;
  std::wstring err;
  HRESULT hr;
  if (by_device) {
    std::wstring id = widen(device);
    char* end = nullptr;
    const long idx = std::strtol(device.c_str(), &end, 10);
    if (end && *end == 0 && !device.empty()) {
      const auto eps = wa::render_endpoints();
      if (idx < 0 || size_t(idx) >= eps.size()) {
        std::printf("No output device [%ld]; see 'desktop_audio_probe list'.\n", idx);
        return 1;
      }
      id = eps[size_t(idx)].id;
      std::printf("Capturing what %s plays\n", narrow(eps[size_t(idx)].name).c_str());
    } else {
      std::printf("Capturing what %s plays\n", device == "default" ? "the default output device" : device.c_str());
    }
    hr = cap.open_device(id, &err);
  } else {
    DWORD target = 0;
    if (all) {
      target = GetCurrentProcessId();
      std::printf("Capturing all apps (process loopback, excluding this tool)\n");
    } else if (pid >= 0) {
      target = DWORD(pid);
      std::printf("Capturing process tree of pid %lu (process loopback)\n", static_cast<unsigned long>(target));
    } else {
      const auto roots = wa::root_processes(widen(app));
      if (roots.empty()) {
        std::printf("%s is not running.\n", app.c_str());
        return 1;
      }
      target = roots[0];
      std::printf("Capturing %s, process tree of pid %lu%s (process loopback)\n", app.c_str(),
                  static_cast<unsigned long>(target), roots.size() > 1 ? " (the first of several)" : "");
    }
    hr = cap.open_process(target, !all, 48000, &err);
  }
  if (FAILED(hr)) {
    std::printf("Cannot capture: %s\n", narrow(err).c_str());
    return 1;
  }
  const wa::Format& f = cap.format();
  std::printf("Format: %s; %s\n", narrow(wa::format_text(f)).c_str(),
              cap.event() ? "event driven" : "polled (no loopback events)");
  std::FILE* wav = std::fopen(out.c_str(), "wb");
  if (!wav) {
    std::printf("Cannot write %s\n", out.c_str());
    return 1;
  }
  wav_header(wav, f.rate, 2, 0);
  if (FAILED(hr = cap.start())) {
    std::printf("Cannot start: %s\n", narrow(wa::hr_text(hr)).c_str());
    std::fclose(wav);
    return 1;
  }

  uint64_t packets = 0, frames = 0, silent = 0, discont = 0, stamped = 0, stamp_errors = 0;
  float peak = 0;
  int64_t first_end = 0;
  uint64_t frames_at_first = 0;
  int64_t last_end = 0;
  uint64_t frames_at_last = 0;
  size_t max_packet = 0;
  int64_t max_gap_ns = 0, last_packet_ns = 0;
  const int64_t t0 = virgil::mono_ns(), until = t0 + int64_t(seconds * 1e9);
  while (virgil::mono_ns() < until) {
    if (cap.event()) WaitForSingleObject(cap.event(), 10);
    else Sleep(5);
    hr = cap.drain(2, [&](const wa::Capture::Packet& p) {
      ++packets;
      if (p.flags & AUDCLNT_BUFFERFLAGS_SILENT) ++silent;
      if (p.flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discont;
      if (p.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ++stamp_errors;
      for (size_t i = 0; i < p.frames * 2; ++i) peak = std::max(peak, std::fabs(p.data[i]));
      std::fwrite(p.data, sizeof(float), p.frames * 2, wav);
      const int64_t now = virgil::mono_ns();
      if (last_packet_ns) max_gap_ns = std::max(max_gap_ns, now - last_packet_ns);
      last_packet_ns = now;
      max_packet = std::max(max_packet, p.frames);
      frames += p.frames;
      if (p.stamped) {
        ++stamped;
        if (!first_end) {
          first_end = p.end_ns;
          frames_at_first = frames;
        }
        last_end = p.end_ns;
        frames_at_last = frames;
      }
    });
    if (FAILED(hr)) {
      std::printf("Capture stopped: %s\n", narrow(wa::hr_text(hr)).c_str());
      break;
    }
  }
  cap.close();
  const uint64_t bytes = frames * 2 * sizeof(float);
  wav_header(wav, f.rate, 2, bytes > 0xFFFFFFF0ull ? 0xFFFFFFF0u : uint32_t(bytes));
  std::fclose(wav);

  const double secs = double(virgil::mono_ns() - t0) * 1e-9;
  std::printf("\n%llu packets, %llu frames (%.2f s of audio in %.2f s), largest packet %zu frames\n",
              (unsigned long long)packets, (unsigned long long)frames, double(frames) / f.rate, secs, max_packet);
  std::printf("silent packets %llu, discontinuities %llu, time stamped %llu (errors %llu), longest gap %.1f ms\n",
              (unsigned long long)silent, (unsigned long long)discont, (unsigned long long)stamped,
              (unsigned long long)stamp_errors, max_gap_ns * 1e-6);
  std::printf("peak level %.1f dBFS\n", peak > 0 ? 20 * std::log10(peak) : -999.0);
  if (last_end > first_end && frames_at_last > frames_at_first + f.rate) {
    const double measured = double(frames_at_last - frames_at_first) / (double(last_end - first_end) * 1e-9);
    std::printf("device clock: %.2f Hz against the performance counter (%+.0f ppm from %u Hz)\n", measured,
                (measured / f.rate - 1) * 1e6, f.rate);
  } else if (frames == 0) {
    std::printf("No audio arrived: play something on the source (a loopback of an idle device delivers nothing).\n");
  }
  std::printf("Wrote %s\n", out.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  wa::ComScope com;
  if (FAILED(com.hr)) {
    std::printf("CoInitializeEx failed: 0x%08lX\n", static_cast<unsigned long>(com.hr));
    return 1;
  }
  if (argc >= 2 && std::strcmp(argv[1], "list") == 0) return list();
  if (argc >= 2 && std::strcmp(argv[1], "capture") == 0) return capture(argc, argv);
  return usage();
}
