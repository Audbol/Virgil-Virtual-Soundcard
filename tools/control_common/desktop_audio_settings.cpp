#include "desktop_audio_settings.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace vc {

namespace {

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return s.substr(a, b - a);
}

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// A value: "quoted" (kept verbatim up to the closing quote), or bare with a
// comment (" # ..." / " ; ...") cut off.
std::string value_of(const std::string& raw) {
  std::string v = trim(raw);
  if (!v.empty() && v[0] == '"') {
    const size_t end = v.find('"', 1);
    return end == std::string::npos ? v.substr(1) : v.substr(1, end - 1);
  }
  for (size_t i = 1; i < v.size(); ++i)
    if ((v[i] == '#' || v[i] == ';') && (v[i - 1] == ' ' || v[i - 1] == '\t')) {
      v = trim(v.substr(0, i));
      break;
    }
  if (!v.empty() && (v[0] == '#' || v[0] == ';')) v.clear();
  return v;
}

bool parse_bool(const std::string& v, bool* out) {
  const std::string l = lower(v);
  if (l == "true" || l == "yes" || l == "on" || l == "1") return *out = true, true;
  if (l == "false" || l == "no" || l == "off" || l == "0") return *out = false, true;
  return false;
}

// Locale-independent: '.' is the decimal separator.
bool parse_double(const std::string& v, double* out) {
  if (v.empty() || v.size() > 32) return false;
  std::istringstream is(v);
  is.imbue(std::locale::classic());
  double d = 0;
  is >> d;
  if (is.fail() || !std::isfinite(d)) return false;
  is >> std::ws;
  if (!is.eof()) return false;
  *out = d;
  return true;
}

bool parse_uint(const std::string& v, long* out) {
  if (v.empty() || v.size() > 9) return false;
  size_t i = v[0] == '-' || v[0] == '+' ? 1 : 0;
  if (i == v.size()) return false;
  for (size_t k = i; k < v.size(); ++k)
    if (!std::isdigit(static_cast<unsigned char>(v[k]))) return false;
  *out = std::strtol(v.c_str(), nullptr, 10);
  return true;
}

std::string fmt_double(double d) {
  std::ostringstream os;
  os.imbue(std::locale::classic());
  os << d;
  return os.str();
}

// Strings written quoted must not contain quotes or line breaks.
std::string clean(const std::string& s) {
  std::string r;
  for (char c : s)
    if (c != '"' && c != '\r' && c != '\n') r += c;
  return r;
}

}  // namespace

const char* source_type_key(DesktopAudioSource::Type t) {
  switch (t) {
    case DesktopAudioSource::kApp: return "app";
    case DesktopAudioSource::kAllApps: return "all-apps";
    default: return "device";
  }
}

double db_to_gain(double db) {
  if (!(db > kMinGainDb)) return 0;
  return std::pow(10.0, std::min(db, kMaxGainDb) / 20.0);
}

bool DesktopAudioSettings::same_routes(const DesktopAudioSettings& o) const {
  if (enabled != o.enabled || sources.size() != o.sources.size() || !output.same_route(o.output)) return false;
  for (size_t i = 0; i < sources.size(); ++i)
    if (!sources[i].same_route(o.sources[i])) return false;
  return true;
}

bool parse_desktop_audio(const std::string& text, DesktopAudioSettings* out, std::vector<std::string>* warnings) {
  if (!out) return false;
  *out = DesktopAudioSettings();
  auto warn = [&](int line, const std::string& msg) {
    if (warnings) warnings->push_back("line " + std::to_string(line) + ": " + msg);
  };
  enum Section { kNone, kMain, kSource, kOutput, kUnknown } section = kNone;
  // A source is kept when its section ends, if it has what it needs.
  bool have_source = false, source_ok = true;
  int source_line = 0;
  DesktopAudioSource cur;
  auto finish_source = [&] {
    if (!have_source) return;
    have_source = false;
    if (!source_ok) return;
    if (cur.type == DesktopAudioSource::kApp && cur.app.empty()) {
      warn(source_line, "source without an app (app = name.exe): skipped");
      return;
    }
    if (cur.type == DesktopAudioSource::kDevice && cur.device.empty()) cur.device = "default";
    if (out->sources.size() >= kMaxDesktopSources) {
      warn(source_line, "more than " + std::to_string(kMaxDesktopSources) + " sources: skipped");
      return;
    }
    out->sources.push_back(cur);
  };

  size_t pos = 0;
  int line_no = 0;
  // A UTF-8 byte order mark (Notepad) is not part of the first line.
  if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) pos = 3;
  while (pos <= text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    const std::string line = trim(text.substr(pos, eol - pos));
    pos = eol + 1;
    ++line_no;
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line[0] == '[') {
      const size_t close = line.find(']');
      const std::string name = lower(trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
      finish_source();
      if (name == "desktop-audio" || name == "desktop_audio" || name == "general") {
        section = kMain;
      } else if (name == "source") {
        section = kSource;
        cur = DesktopAudioSource();
        have_source = true;
        source_ok = true;
        source_line = line_no;
      } else if (name == "output") {
        section = kOutput;
      } else {
        section = kUnknown;
        warn(line_no, "unknown section [" + name + "] ignored");
      }
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      warn(line_no, "not a 'key = value' line: ignored");
      continue;
    }
    const std::string key = lower(trim(line.substr(0, eq)));
    const std::string v = value_of(line.substr(eq + 1));
    auto bad = [&] { warn(line_no, "invalid value for " + key + ": '" + v + "' ignored"); };
    bool b = false;
    double d = 0;
    long n = 0;
    if (section == kMain) {
      if (key == "enabled") {
        if (parse_bool(v, &b)) out->enabled = b;
        else bad();
      } else {
        warn(line_no, "unknown key " + key + " ignored");
      }
    } else if (section == kSource || section == kOutput) {
      const bool src = section == kSource;
      if (src && key == "type") {
        const std::string t = lower(v);
        if (t == "device" || t == "system") cur.type = DesktopAudioSource::kDevice;
        else if (t == "app" || t == "process") cur.type = DesktopAudioSource::kApp;
        else if (t == "all-apps" || t == "all_apps" || t == "all") cur.type = DesktopAudioSource::kAllApps;
        else {
          warn(line_no, "unknown source type '" + v + "': source skipped");
          source_ok = false;
        }
      } else if (key == "device") {
        (src ? cur.device : out->output.device) = v.empty() ? "default" : v;
      } else if (key == "name") {
        (src ? cur.name : out->output.name) = v;
      } else if (src && key == "app") {
        cur.app = v;
      } else if (key == "channel") {
        if (parse_uint(v, &n)) {
          if (n < 1 || n > 64) warn(line_no, "channel " + v + " out of range 1-64: clamped");
          (src ? cur.channel : out->output.channel) = unsigned(std::max(1L, std::min(64L, n)));
        } else {
          bad();
        }
      } else if (key == "mono") {
        if (parse_bool(v, &b)) (src ? cur.mono : out->output.mono) = b;
        else bad();
      } else if (key == "gain_db") {
        if (parse_double(v, &d)) (src ? cur.gain_db : out->output.gain_db) = std::max(kMinGainDb, std::min(kMaxGainDb, d));
        else bad();
      } else if (src && key == "mute") {
        if (parse_bool(v, &b)) cur.mute = b;
        else bad();
      } else if (!src && key == "enabled") {
        if (parse_bool(v, &b)) out->output.enabled = b;
        else bad();
      } else {
        warn(line_no, "unknown key " + key + " ignored");
      }
    } else if (section == kNone) {
      warn(line_no, "key outside a section ignored");
    }
  }
  finish_source();
  return true;
}

std::string format_desktop_audio(const DesktopAudioSettings& s) {
  std::string o;
  o += "# Virgil desktop audio (Virgil Control): Windows audio to Dante and back.\n";
  o += "# Edited by Virgil Control's Desktop audio window; see README.md.\n\n";
  o += "[desktop-audio]\n";
  o += std::string("enabled = ") + (s.enabled ? "true" : "false") + "\n";
  for (const DesktopAudioSource& src : s.sources) {
    o += "\n[source]\n";
    o += std::string("type = ") + source_type_key(src.type) + "\n";
    if (src.type == DesktopAudioSource::kDevice) {
      o += "device = " + clean(src.device.empty() ? "default" : src.device) + "\n";
      if (!src.name.empty()) o += "name = \"" + clean(src.name) + "\"\n";
    }
    if (src.type == DesktopAudioSource::kApp) o += "app = \"" + clean(src.app) + "\"\n";
    o += "channel = " + std::to_string(src.channel) + "\n";
    o += std::string("mono = ") + (src.mono ? "true" : "false") + "\n";
    o += "gain_db = " + fmt_double(src.gain_db) + "\n";
    o += std::string("mute = ") + (src.mute ? "true" : "false") + "\n";
  }
  const DesktopAudioOutput& out = s.output;
  o += "\n[output]\n";
  o += std::string("enabled = ") + (out.enabled ? "true" : "false") + "\n";
  o += "device = " + clean(out.device.empty() ? "default" : out.device) + "\n";
  if (!out.name.empty()) o += "name = \"" + clean(out.name) + "\"\n";
  o += "channel = " + std::to_string(out.channel) + "\n";
  o += std::string("mono = ") + (out.mono ? "true" : "false") + "\n";
  o += "gain_db = " + fmt_double(out.gain_db) + "\n";
  return o;
}

}  // namespace vc
