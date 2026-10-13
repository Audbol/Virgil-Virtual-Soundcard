#import <AppKit/AppKit.h>

#include "desktop_audio_mac.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

// Action targets (AppKit needs Objective-C objects for them); the window's
// state lives in C++ below.
@interface DATarget : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate>
@end

@interface DABlockTarget : NSObject
@property(nonatomic, copy) void (^handler)(id sender);
- (void)fire:(id)sender;
@end

@implementation DABlockTarget
- (void)fire:(id)sender {
  if (self.handler) self.handler(sender);
}
@end

namespace damac {

const char* const kHelpText =
    "Desktop audio sends sound from the apps on this Mac to Virgil's Dante transmit channels, with no driver "
    "and no extra software. Capturing needs macOS 14.2 or later; the first time, macOS asks whether Virgil "
    "Control may record this computer's audio.\n\n"
    "• Output device: everything that plays on that device (for example the built-in speakers). \"Default\" "
    "follows the output chosen in System Settings › Sound.\n"
    "• App: one app, such as Safari, Music or Spotify, with its helper processes. The list shows the apps "
    "that play sound now; you can also type an app's name.\n"
    "• All apps except Virgil: every app, without what Virgil Control itself plays and without apps that play "
    "on Virgil's own device (no echo when you also listen to Dante on this Mac).\n\n"
    "Each source goes to a pair of transmit channels (or one, mixed to mono); sources on the same channels are "
    "mixed. Route those channels in Dante Controller like any other.\n\n"
    "Dante receive channels can play on an output device too. Do not play them on a device you also capture: "
    "they would echo back into Dante.\n\n"
    "Captured audio still plays on its own device. To hear it only through Dante, turn that device down or let "
    "those apps play on an output you do not listen to.\n\n"
    "If a source stays silent while its app plays, allow Virgil Control in System Settings › Privacy & "
    "Security › Screen & System Audio Recording, then turn the source off and on.\n\n"
    "The audio is converted to Virgil's sample rate and follows the small difference between the clock of the "
    "Mac's sound hardware and the Dante clock. It adds roughly 60 ms of latency in each direction, so use it "
    "for music, video calls and system sounds, not for live monitoring.\n\n"
    "Desktop audio runs while Virgil Control runs (it can start at login) and pauses while Virgil is stopped. "
    "Settings are saved in ~/Library/Application Support/Virgil/desktop-audio.conf.";

namespace {

const unsigned kRed = 0xD2553F, kGreen = 0x6DB476, kAmber = 0xD9A93F;

NSString* ns(const std::string& s) {
  if (s.empty()) return @"";
  return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}
std::string str(NSString* s) {
  const char* c = s.UTF8String;
  return c ? std::string(c) : std::string();
}
NSColor* rgb(unsigned c) {
  return [NSColor colorWithSRGBRed:((c >> 16) & 255) / 255.0 green:((c >> 8) & 255) / 255.0 blue:(c & 255) / 255.0 alpha:1];
}

// Runs on the main thread, also while a modal dialog is open.
void on_main(void (^block)(void)) {
  CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, block);
  CFRunLoopWakeUp(CFRunLoopGetMain());
}

int64_t now_us() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Inventory {
  bool ok = false;
  std::string why;
  std::vector<desk::Device> devices;
  std::vector<desk::App> apps;
};

// Devices and apps, listed off the main thread; `done` runs on it.
void load_inventory(desk::Engine& engine, std::function<void(std::shared_ptr<Inventory>)> done) {
  desk::Backend* be = &engine.backend();
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    auto inv = std::make_shared<Inventory>();
    inv->ok = be->connect(&inv->why);
    if (inv->ok) {
      inv->devices = be->outputs();
      inv->apps = be->apps();
    }
    on_main(^{
      done(inv);
    });
  });
}

std::string fmtd(const char* f, double v) {
  char b[64];
  std::snprintf(b, sizeof b, f, v);
  return b;
}
std::string gain_text(double db) {
  if (db <= vc::kMinGainDb) return "off";
  if (db == 0) return "0 dB";
  return fmtd(db == std::floor(db) ? "%+.0f dB" : "%+.1f dB", db);
}
std::string gain_field_text(double db) { return fmtd(db == std::floor(db) ? "%.0f" : "%.1f", db); }

// A gain typed in: false if it is not a number.
bool parse_gain(NSString* s, double* db) {
  std::string t = str(s);
  for (char& c : t)
    if (c == ',') c = '.';
  const char* p = t.c_str();
  while (*p == ' ') ++p;
  if (*p == '+') ++p;
  char* end = nullptr;
  const double v = std::strtod(p, &end);
  if (end == p) return false;
  while (*end == ' ') ++end;
  if (*end && std::strcmp(end, "dB") != 0 && std::strcmp(end, "db") != 0) return false;
  *db = std::max(vc::kMinGainDb, std::min(vc::kMaxGainDb, v));
  return true;
}

float lin_to_pos(float lin) {
  if (!(lin > 0)) return 0;
  const float db = 20.f * std::log10(lin);
  return std::max(0.f, std::min(1.f, (db + 60.f) / 60.f));
}

std::string device_name(const std::string& key, const std::string& saved, const Inventory* inv) {
  if (key.empty() || key == "default") return "Default output device";
  if (inv)
    for (const auto& e : inv->devices)
      if (e.id == key) return e.name;
  return saved.empty() ? key : saved;
}

std::string app_name(const std::string& key, const Inventory* inv) {
  if (inv)
    for (const auto& a : inv->apps)
      if (a.id == key) return a.name;
  return key;
}

std::string source_label(const vc::DesktopAudioSource& s, const Inventory* inv) {
  switch (s.type) {
    case vc::DesktopAudioSource::kApp: return "App: " + app_name(s.app, inv);
    case vc::DesktopAudioSource::kAllApps: return "All apps except Virgil";
    default:
      return (s.device == "default" || s.device.empty() ? "" : "Output: ") + device_name(s.device, s.name, inv);
  }
}

std::string channels_short(unsigned first, bool mono) {
  char b[48];
  if (mono) std::snprintf(b, sizeof b, "%u (mono)", first);
  else std::snprintf(b, sizeof b, "%u + %u", first, first + 1);
  return b;
}

// ---- small AppKit helpers -----------------------------------------------------------------

NSPopUpButton* popup() {
  NSPopUpButton* p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
  p.autoenablesItems = NO;
  return p;
}
// Items keep their id in representedObject (titles may repeat).
void popup_add(NSPopUpButton* p, const std::string& title, const std::string& key) {
  NSMenuItem* i = [[NSMenuItem alloc] initWithTitle:ns(title) action:nil keyEquivalent:@""];
  i.representedObject = ns(key);
  i.enabled = YES;
  [p.menu addItem:i];
}
bool popup_select(NSPopUpButton* p, const std::string& key) {
  NSString* want = ns(key);
  for (NSMenuItem* i in p.itemArray)
    if ([i.representedObject isKindOfClass:[NSString class]] && [(NSString*)i.representedObject isEqualToString:want]) {
      [p selectItem:i];
      return true;
    }
  return false;
}
bool popup_value(NSPopUpButton* p, std::string* key) {
  NSObject* r = p.selectedItem.representedObject;
  if (![r isKindOfClass:[NSString class]]) return false;
  *key = str((NSString*)r);
  return true;
}

NSTextField* label(const std::string& s) { return [NSTextField labelWithString:ns(s)]; }
NSTextField* wrapping(const std::string& s, CGFloat width) {
  NSTextField* t = [NSTextField wrappingLabelWithString:ns(s)];
  t.preferredMaxLayoutWidth = width;
  return t;
}
NSTextField* dim(NSTextField* t) {
  t.textColor = [NSColor secondaryLabelColor];
  return t;
}
void set_text(NSTextField* t, const std::string& s, unsigned color) {
  t.stringValue = ns(s);
  t.textColor = color ? rgb(color) : [NSColor labelColor];
}

NSLevelIndicator* level_indicator() {
  NSLevelIndicator* m = [[NSLevelIndicator alloc] initWithFrame:NSMakeRect(0, 0, 90, 12)];
  m.levelIndicatorStyle = NSLevelIndicatorStyleContinuousCapacity;
  // -60..0 dBFS: yellow above -18 dBFS, red above -6.
  m.minValue = 0;
  m.maxValue = 1;
  m.warningValue = 0.7;
  m.criticalValue = 0.9;
  m.doubleValue = 0;
  return m;
}
// A meter centred in a cell of the given width.
NSView* meter_box(NSLevelIndicator* m, CGFloat width) {
  NSView* v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, 20)];
  m.translatesAutoresizingMaskIntoConstraints = NO;
  [v addSubview:m];
  [m.leadingAnchor constraintEqualToAnchor:v.leadingAnchor].active = YES;
  [m.trailingAnchor constraintEqualToAnchor:v.trailingAnchor].active = YES;
  [m.centerYAnchor constraintEqualToAnchor:v.centerYAnchor].active = YES;
  [m.heightAnchor constraintEqualToConstant:10].active = YES;
  [v.widthAnchor constraintEqualToConstant:width].active = YES;
  [v.heightAnchor constraintGreaterThanOrEqualToConstant:14].active = YES;
  return v;
}

NSStackView* vstack(NSArray<NSView*>* views, CGFloat spacing) {
  NSStackView* s = [NSStackView stackViewWithViews:views];
  s.orientation = NSUserInterfaceLayoutOrientationVertical;
  s.alignment = NSLayoutAttributeLeading;
  s.spacing = spacing;
  return s;
}
NSStackView* hstack(NSArray<NSView*>* views, CGFloat spacing) {
  NSStackView* s = [NSStackView stackViewWithViews:views];
  s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  s.alignment = NSLayoutAttributeCenterY;
  s.spacing = spacing;
  return s;
}
void pin(NSView* child, NSView* parent, CGFloat inset) {
  child.translatesAutoresizingMaskIntoConstraints = NO;
  [parent addSubview:child];
  [child.leadingAnchor constraintEqualToAnchor:parent.leadingAnchor constant:inset].active = YES;
  [child.trailingAnchor constraintEqualToAnchor:parent.trailingAnchor constant:-inset].active = YES;
  [child.topAnchor constraintEqualToAnchor:parent.topAnchor constant:inset].active = YES;
  [child.bottomAnchor constraintEqualToAnchor:parent.bottomAnchor constant:-inset].active = YES;
}

// Stereo pairs from every channel, then single channels (mono). Item id:
// "<first>:<mono>". A configured choice Virgil does not have (now) is kept.
void fill_channels(NSPopUpButton* p, unsigned count, const std::vector<std::string>& names, unsigned first, bool mono) {
  [p removeAllItems];
  const bool known = count > 0;
  if (!known) count = 128;  // Virgil not reachable: anything goes
  auto name = [&](unsigned ch) -> std::string { return ch >= 1 && ch <= names.size() ? names[ch - 1] : std::string(); };
  auto id_of = [](unsigned f, bool m) { return std::to_string(f) + ":" + (m ? "1" : "0"); };
  bool have = false;
  auto add = [&](unsigned f, bool m, const std::string& extra) {
    std::string t = channels_short(f, m);
    if (!extra.empty()) t += "   " + extra;
    popup_add(p, t, id_of(f, m));
    if (f == first && m == mono) have = true;
  };
  for (unsigned f = 1; f + 1 <= count; ++f) {
    const std::string a = name(f), b = name(f + 1);
    add(f, false, a.empty() && b.empty() ? "" : "(" + a + " / " + b + ")");
  }
  for (unsigned f = 1; f <= count; ++f) {
    const std::string a = name(f);
    add(f, true, a.empty() ? "" : "(" + a + ")");
  }
  if (!have && first >= 1) add(first, mono, known ? "(not available)" : "");
  if (!popup_select(p, id_of(first, mono)) && p.numberOfItems > 0) [p selectItemAtIndex:0];
}

bool popup_channels(NSPopUpButton* p, unsigned* first, bool* mono) {
  std::string key;
  if (!popup_value(p, &key)) return false;
  unsigned f = 0, m = 0;
  if (std::sscanf(key.c_str(), "%u:%u", &f, &m) != 2 || f < 1) return false;
  *first = f;
  *mono = m != 0;
  return true;
}

// Output device popup: Default, then every device; a configured one that is
// not there stays, marked. Item id = device id.
void fill_devices(NSPopUpButton* p, const Inventory* inv, const std::string& cur, const std::string& cur_name) {
  [p removeAllItems];
  popup_add(p, "Default output device (follows the system)", "default");
  bool found = cur.empty() || cur == "default";
  if (inv)
    for (const auto& e : inv->devices) {
      popup_add(p, e.name, e.id);
      if (e.id == cur) found = true;
    }
  if (!found) popup_add(p, (cur_name.empty() ? cur : cur_name) + (inv ? "  (not connected)" : ""), cur);
  popup_select(p, cur.empty() ? std::string("default") : cur);
}

NSButton* radio(const std::string& title, DABlockTarget* t) {
  return [NSButton radioButtonWithTitle:ns(title) target:t action:@selector(fire:)];
}
bool on(NSButton* b) { return b.state == NSControlStateValueOn; }

// ---- the source dialog ------------------------------------------------------------------------

struct SourceUi {
  NSButton *dev = nil, *app = nil, *all = nil, *refresh = nil, *mute = nil;
  NSPopUpButton *devp = nil, *ch = nil;
  NSComboBox* appc = nil;
  NSTextField *apphint = nil, *gain = nil, *err = nil;
  DABlockTarget *radio_target = nil, *refresh_target = nil;
  vc::DesktopAudioSource src;
  std::shared_ptr<Inventory> inv;
  desk::Capabilities caps;
  bool open = true;
};

void source_enable(SourceUi& s) {
  const bool dev = on(s.dev), app = on(s.app), all = on(s.all);
  s.devp.enabled = dev;
  s.appc.enabled = app && s.caps.app_capture;
  s.refresh.enabled = app && s.caps.app_capture;
  s.app.enabled = s.caps.app_capture || app;
  s.all.enabled = s.caps.all_apps || all;
}

void source_fill_apps(SourceUi& s) {
  NSString* keep = s.appc.stringValue ?: @"";
  [s.appc removeAllItems];
  std::string playing;
  if (s.inv)
    for (const auto& a : s.inv->apps) {
      [s.appc addItemWithObjectValue:ns(a.name)];
      if (a.playing) playing += (playing.empty() ? "" : ", ") + a.name;
    }
  s.appc.stringValue = keep;
  std::string hint;
  if (!s.caps.app_capture) hint = s.caps.app_hint + ".";
  else if (!s.inv) hint = "Looking for apps…";
  else if (!s.inv->ok) hint = s.inv->why;
  else
    hint = (playing.empty() ? std::string("No app is playing sound right now.") : "Playing now: " + playing + ".") +
           " Or type the app's name, e.g. Safari.";
  s.apphint.stringValue = ns(hint);
}

// Runs the (modal) source dialog; true and *src when OK was pressed.
// *inv: the device / app lists to start with, and the refreshed ones after.
bool edit_source_dialog(desk::Engine& engine, vc::DesktopAudioSource* src, std::shared_ptr<Inventory>* inv,
                        const ChannelInfo& ch) {
  auto s = std::make_shared<SourceUi>();
  s->src = *src;
  s->inv = *inv;
  s->caps = engine.backend().capabilities();
  std::weak_ptr<SourceUi> weak = s;

  s->radio_target = [[DABlockTarget alloc] init];
  s->radio_target.handler = ^(id sender) {
    auto u = weak.lock();
    if (!u) return;
    for (NSButton* b in @[ u->dev, u->app, u->all ]) b.state = b == sender ? NSControlStateValueOn : NSControlStateValueOff;
    source_enable(*u);
  };
  const CGFloat W = 470;
  s->dev = radio("Everything an output device plays", s->radio_target);
  s->devp = popup();
  fill_devices(s->devp, s->inv.get(), s->src.device, s->src.name);
  s->app = radio("One app (with its helper processes)", s->radio_target);
  s->appc = [[NSComboBox alloc] initWithFrame:NSMakeRect(0, 0, 330, 26)];
  s->appc.completes = YES;
  s->appc.numberOfVisibleItems = 12;
  s->appc.stringValue = ns(app_name(s->src.app, s->inv.get()));
  [s->appc.widthAnchor constraintEqualToConstant:330].active = YES;
  s->refresh_target = [[DABlockTarget alloc] init];
  s->refresh = [NSButton buttonWithTitle:@"Refresh" target:s->refresh_target action:@selector(fire:)];
  s->apphint = dim(wrapping("", W - 24));
  s->all = radio("All apps except Virgil (no echo of what Virgil itself plays)", s->radio_target);
  s->ch = popup();
  fill_channels(s->ch, ch.tx, ch.tx_names, s->src.channel, s->src.mono);
  s->gain = [NSTextField textFieldWithString:ns(gain_field_text(s->src.gain_db))];
  [s->gain.widthAnchor constraintEqualToConstant:60].active = YES;
  s->mute = [NSButton checkboxWithTitle:@"Mute" target:nil action:nil];
  s->mute.state = s->src.mute ? NSControlStateValueOn : NSControlStateValueOff;
  s->err = wrapping("", W);
  s->err.textColor = rgb(kRed);

  NSGridView* grid = [NSGridView gridViewWithViews:@[
    @[ label("Virgil transmit channels:"), s->ch ],
    @[ label("Gain (dB):"), hstack(@[ s->gain, s->mute ], 12) ],
  ]];
  grid.rowSpacing = 8;
  grid.columnSpacing = 10;
  NSStackView* devrow = hstack(@[ s->devp ], 0);
  devrow.edgeInsets = NSEdgeInsetsMake(0, 20, 0, 0);
  NSStackView* approw = hstack(@[ s->appc, s->refresh ], 8);
  approw.edgeInsets = NSEdgeInsetsMake(0, 20, 0, 0);
  NSStackView* hintrow = hstack(@[ s->apphint ], 0);
  hintrow.edgeInsets = NSEdgeInsetsMake(0, 20, 0, 0);
  NSStackView* v = vstack(@[ s->dev, devrow, s->app, approw, hintrow, s->all, grid, s->err ], 6);
  [v setCustomSpacing:12 afterView:devrow];
  [v setCustomSpacing:12 afterView:hintrow];
  [v setCustomSpacing:14 afterView:s->all];
  v.translatesAutoresizingMaskIntoConstraints = NO;
  [v.widthAnchor constraintEqualToConstant:W].active = YES;
  const NSSize fit = v.fittingSize;
  NSView* box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, W, std::ceil(fit.height) + 4)];
  [box addSubview:v];
  [v.leadingAnchor constraintEqualToAnchor:box.leadingAnchor].active = YES;
  [v.topAnchor constraintEqualToAnchor:box.topAnchor].active = YES;

  NSButton* r = s->src.type == vc::DesktopAudioSource::kApp      ? s->app
                : s->src.type == vc::DesktopAudioSource::kAllApps ? s->all
                                                                  : s->dev;
  r.state = NSControlStateValueOn;
  source_fill_apps(*s);
  source_enable(*s);

  desk::Engine* eng = &engine;
  auto refresh = [weak, eng] {
    if (auto u = weak.lock()) u->apphint.stringValue = @"Looking for apps…";
    load_inventory(*eng, [weak](std::shared_ptr<Inventory> inv) {
      auto u = weak.lock();
      if (!u || !u->open) return;
      std::string cur;
      if (!popup_value(u->devp, &cur)) cur = u->src.device;
      u->inv = inv;
      fill_devices(u->devp, inv.get(), cur, u->src.name);
      source_fill_apps(*u);
    });
  };
  s->refresh_target.handler = ^(id sender) {
    refresh();
  };
  if (!s->inv) refresh();

  NSAlert* a = [[NSAlert alloc] init];
  a.messageText = @"Desktop audio source";
  [a addButtonWithTitle:@"OK"];
  [a addButtonWithTitle:@"Cancel"];
  a.accessoryView = box;
  if (@available(macOS 10.14, *)) a.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  bool ok = false;
  for (;;) {
    [NSApp activateIgnoringOtherApps:YES];
    if ([a runModal] != NSAlertFirstButtonReturn) break;
    vc::DesktopAudioSource res = s->src;
    if (on(s->app)) {
      std::string t = str(s->appc.stringValue);
      while (!t.empty() && (t.back() == ' ' || t.back() == '"')) t.pop_back();
      while (!t.empty() && (t.front() == ' ' || t.front() == '"')) t.erase(t.begin());
      if (t.empty()) {
        s->err.stringValue = @"Choose an app, or type its name (for example Safari).";
        continue;
      }
      // A name from the list (or the one shown for the current app) stands for its id.
      std::string key = t;
      if (t == app_name(s->src.app, s->inv.get())) key = s->src.app;
      if (s->inv)
        for (const auto& ap : s->inv->apps)
          if (ap.name == t) {
            key = ap.id;
            break;
          }
      res.type = vc::DesktopAudioSource::kApp;
      res.app = key;
    } else if (on(s->all)) {
      res.type = vc::DesktopAudioSource::kAllApps;
    } else {
      res.type = vc::DesktopAudioSource::kDevice;
      std::string key;
      res.device = popup_value(s->devp, &key) ? key : std::string("default");
      res.name = res.device == "default" ? std::string() : device_name(res.device, res.name, s->inv.get());
    }
    if (res.type != vc::DesktopAudioSource::kApp) res.app.clear();
    if (res.type != vc::DesktopAudioSource::kDevice) {
      res.device = "default";
      res.name.clear();
    }
    if (!popup_channels(s->ch, &res.channel, &res.mono)) {
      s->err.stringValue = @"Choose the transmit channels.";
      continue;
    }
    if (!parse_gain(s->gain.stringValue, &res.gain_db)) {
      s->err.stringValue = @"Enter the gain in dB, for example -6 (from -60 to +12).";
      continue;
    }
    res.mute = on(s->mute);
    *src = res;
    ok = true;
    break;
  }
  if (s->inv) *inv = s->inv;  // refreshed in there
  s->open = false;
  return ok;
}

// ---- the main window ----------------------------------------------------------------------

struct Row {
  NSTextField *label = nil, *channels = nil, *gain = nil, *status = nil;
  NSLevelIndicator* meter = nil;
  NSView* meter_cell = nil;
  float level = 0;
  std::string shown;
};

struct Main {
  NSWindow* w = nil;
  DATarget* target = nil;
  NSButton *enable = nil, *add = nil, *edit = nil, *remove = nil, *out_enable = nil;
  NSTextField *status = nil, *out_gain = nil, *out_state = nil;
  NSStepper* out_step = nil;
  NSPopUpButton *out_ch = nil, *out_dev = nil;
  NSLevelIndicator* out_meter = nil;
  NSTableView* table = nil;
  NSTimer* timer = nil;
  desk::Engine* engine = nullptr;
  ChannelSource channels;
  vc::DesktopAudioSettings cfg;
  std::shared_ptr<Inventory> inv;
  ChannelInfo ch;
  std::vector<Row> rows;
  std::string shown_status, shown_out, save_error;
  float out_level = 0;
  int64_t last_tick = 0, last_channels = 0;
  bool filling = false;
};

Main* g_main = nullptr;  // the open window, if any

int selected(Main& m) {
  const NSInteger r = m.table.selectedRow;
  return r >= 0 && size_t(r) < m.rows.size() ? int(r) : -1;
}

void update_buttons(Main& m) {
  const bool sel = selected(m) >= 0;
  m.edit.enabled = sel;
  m.remove.enabled = sel;
  m.add.enabled = m.cfg.sources.size() < vc::kMaxDesktopSources;
}

NSTextField* cell_label(const std::string& s) {
  NSTextField* t = label(s);
  t.lineBreakMode = NSLineBreakByTruncatingTail;
  return t;
}

void fill_list(Main& m) {
  const int keep = selected(m);
  m.rows.assign(m.cfg.sources.size(), Row());
  for (size_t i = 0; i < m.cfg.sources.size(); ++i) {
    const vc::DesktopAudioSource& s = m.cfg.sources[i];
    Row& r = m.rows[i];
    r.label = cell_label(source_label(s, m.inv.get()));
    r.channels = cell_label(channels_short(s.channel, s.mono));
    r.gain = cell_label(s.mute ? "muted" : gain_text(s.gain_db));
    r.meter = level_indicator();
    r.meter_cell = meter_box(r.meter, 90);
    r.status = cell_label("");
  }
  [m.table reloadData];
  if (keep >= 0 && size_t(keep) < m.rows.size())
    [m.table selectRowIndexes:[NSIndexSet indexSetWithIndex:NSUInteger(keep)] byExtendingSelection:NO];
  update_buttons(m);
}

void set_out_gain(Main& m, double db) {
  m.out_gain.stringValue = ns(gain_field_text(db));
  m.out_step.doubleValue = db;
}

void fill_output(Main& m) {
  m.filling = true;
  const vc::DesktopAudioOutput& o = m.cfg.output;
  m.out_enable.state = o.enabled ? NSControlStateValueOn : NSControlStateValueOff;
  fill_channels(m.out_ch, m.ch.rx, m.ch.rx_names, o.channel, o.mono);
  fill_devices(m.out_dev, m.inv.get(), o.device, o.name);
  set_out_gain(m, o.gain_db);
  m.filling = false;
}

// Applies the edited settings right away and saves them.
void commit(Main& m) {
  m.engine->apply(m.cfg);
  std::string err;
  m.save_error = desk::save_settings(desk::settings_path(), m.cfg, &err) ? std::string() : err;
  m.shown_status.clear();  // redone on the next tick
}

void edit_source(Main& m, int index) {
  vc::DesktopAudioSource src;
  if (index >= 0 && size_t(index) < m.cfg.sources.size()) {
    src = m.cfg.sources[size_t(index)];
  } else {
    index = -1;
    // A new source: the first odd stereo pair no other source uses.
    const unsigned count = m.ch.tx ? m.ch.tx : 128;
    src.channel = 1;
    for (unsigned first = 1; first + 1 <= count; first += 2) {
      bool clash = false;
      for (const auto& o : m.cfg.sources)
        if (first < o.channel + o.width() && o.channel < first + 2) clash = true;
      if (!clash) {
        src.channel = first;
        break;
      }
    }
  }
  std::shared_ptr<Inventory> inv = m.inv;
  Main* const was = &m;
  const bool ok = edit_source_dialog(*m.engine, &src, &inv, m.ch);
  if (g_main != was) return;  // the window was closed meanwhile
  m.inv = inv;
  if (!ok) return;
  if (index < 0) m.cfg.sources.push_back(src);
  else m.cfg.sources[size_t(index)] = src;
  // A new source means the user wants desktop audio on.
  if (index < 0 && !m.cfg.enabled) {
    m.cfg.enabled = true;
    m.enable.state = NSControlStateValueOn;
  }
  commit(m);
  fill_list(m);
  if (index < 0 && !m.rows.empty())
    [m.table selectRowIndexes:[NSIndexSet indexSetWithIndex:m.rows.size() - 1] byExtendingSelection:NO];
  update_buttons(m);
}

void read_output(Main& m) {
  if (m.filling) return;
  vc::DesktopAudioOutput o = m.cfg.output;
  o.enabled = on(m.out_enable);
  popup_channels(m.out_ch, &o.channel, &o.mono);
  std::string key;
  if (popup_value(m.out_dev, &key)) {
    o.device = key;
    o.name = o.device == "default" ? std::string() : device_name(o.device, o.name, m.inv.get());
  }
  double db = o.gain_db;
  if (parse_gain(m.out_gain.stringValue, &db)) o.gain_db = db;
  set_out_gain(m, o.gain_db);
  if (o == m.cfg.output) return;
  m.cfg.output = o;
  if (o.enabled && !m.cfg.enabled) {
    m.cfg.enabled = true;
    m.enable.state = NSControlStateValueOn;
  }
  commit(m);
}

unsigned phase_color(desk::Status::Phase p, bool err) {
  if (err) return kRed;
  switch (p) {
    case desk::Status::kRunning: return kGreen;
    case desk::Status::kWaiting: return kAmber;
    default: return 0;
  }
}

void tick() {
  if (!g_main) return;
  Main& m = *g_main;
  const int64_t now = now_us();
  const float dt = m.last_tick ? std::min(0.25f, float(now - m.last_tick) / 1e6f) : 0.f;
  m.last_tick = now;
  const desk::Status st = m.engine->status();

  // Channel names and counts: Virgil's own while connected, else the
  // service's report. The popup follows when they change.
  if (now - m.last_channels > 1000000) {
    m.last_channels = now;
    ChannelInfo c;
    if (st.connected) {
      c.tx = st.tx;
      c.rx = st.rx;
      c.tx_names = st.tx_names;
      c.rx_names = st.rx_names;
    } else if (m.channels) {
      c = m.channels();
    }
    const bool changed = c.rx != m.ch.rx || c.rx_names != m.ch.rx_names;
    m.ch = c;
    if (changed) {
      m.filling = true;
      fill_channels(m.out_ch, m.ch.rx, m.ch.rx_names, m.cfg.output.channel, m.cfg.output.mono);
      m.filling = false;
    }
  }

  // Status line.
  const std::string line = m.save_error.empty() ? st.text : "Not saved: " + m.save_error;
  const bool err = st.phase == desk::Status::kError || !m.save_error.empty();
  const std::string key = line + (err ? "!" : "") + char('0' + st.phase);
  if (key != m.shown_status) {
    m.shown_status = key;
    set_text(m.status, line, phase_color(st.phase, err));
  }

  // Sources: status text and meters.
  for (size_t i = 0; i < m.rows.size(); ++i) {
    const desk::SourceStatus ss = i < st.sources.size() ? st.sources[i] : desk::SourceStatus();
    Row& r = m.rows[i];
    const std::string t = (ss.error ? "⚠ " : "") + ss.text;
    if (t != r.shown) {
      r.shown = t;
      set_text(r.status, t, ss.error ? kRed : 0);
      r.status.toolTip = ns(ss.text);
    }
    const float v = lin_to_pos(ss.peak);
    const float shown = std::max(v, r.level - dt * 0.6f);
    if (shown != r.level) {
      r.level = shown;
      r.meter.doubleValue = std::max(0.f, shown);
    }
  }

  // Output.
  m.out_level = std::max(lin_to_pos(st.output.peak), m.out_level - dt * 0.6f);
  m.out_meter.doubleValue = std::max(0.f, m.out_level);
  const std::string ot = m.cfg.output.enabled ? st.output.text : std::string();
  const std::string okey = ot + (st.output.error ? "!" : "");
  if (okey != m.shown_out) {
    m.shown_out = okey;
    set_text(m.out_state, ot, st.output.error ? kRed : 0);
    m.out_state.toolTip = ns(ot);
  }
}

void reload_inventory() {
  if (!g_main) return;
  load_inventory(*g_main->engine, [](std::shared_ptr<Inventory> inv) {
    if (!g_main) return;
    g_main->inv = inv;
    fill_output(*g_main);
    fill_list(*g_main);  // device and app names
  });
}

void show_help() {
  NSAlert* a = [[NSAlert alloc] init];
  a.messageText = @"Desktop audio";
  a.informativeText = ns(kHelpText);
  if (@available(macOS 10.14, *)) a.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  [a runModal];
}

// The window closes: nothing refers to it after this.
void teardown() {
  Main* m = g_main;
  if (!m) return;
  g_main = nullptr;
  [m->timer invalidate];
  m->timer = nil;
  m->table.dataSource = nil;
  m->table.delegate = nil;
  m->w.delegate = nil;
  on_main(^{
    delete m;  // later: AppKit is still inside the window's close
  });
}

NSBox* group(const std::string& title, NSView* content) {
  NSBox* b = [[NSBox alloc] initWithFrame:NSZeroRect];
  b.title = ns(title);
  b.titlePosition = NSAtTop;
  pin(content, b.contentView, 10);
  return b;
}

}  // namespace
}  // namespace damac

@implementation DATarget
- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
  return damac::g_main ? NSInteger(damac::g_main->rows.size()) : 0;
}
- (NSView*)tableView:(NSTableView*)tableView viewForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
  if (!damac::g_main || row < 0 || size_t(row) >= damac::g_main->rows.size()) return nil;
  const damac::Row& r = damac::g_main->rows[size_t(row)];
  NSString* c = column.identifier;
  if ([c isEqualToString:@"source"]) return r.label;
  if ([c isEqualToString:@"channels"]) return r.channels;
  if ([c isEqualToString:@"gain"]) return r.gain;
  if ([c isEqualToString:@"level"]) return r.meter_cell;
  return r.status;
}
- (void)tableViewSelectionDidChange:(NSNotification*)n {
  if (damac::g_main) damac::update_buttons(*damac::g_main);
}
- (void)tableDoubleClick:(id)sender {
  if (!damac::g_main) return;
  const NSInteger r = damac::g_main->table.clickedRow;
  if (r >= 0 && size_t(r) < damac::g_main->rows.size()) damac::edit_source(*damac::g_main, int(r));
}
- (void)toggleEnable:(id)sender {
  if (!damac::g_main || damac::g_main->filling) return;
  damac::g_main->cfg.enabled = damac::on(damac::g_main->enable);
  damac::commit(*damac::g_main);
}
- (void)add:(id)sender {
  if (damac::g_main && damac::g_main->cfg.sources.size() < vc::kMaxDesktopSources) damac::edit_source(*damac::g_main, -1);
}
- (void)edit:(id)sender {
  if (damac::g_main && damac::selected(*damac::g_main) >= 0)
    damac::edit_source(*damac::g_main, damac::selected(*damac::g_main));
}
- (void)remove:(id)sender {
  if (!damac::g_main) return;
  damac::Main& m = *damac::g_main;
  const int i = damac::selected(m);
  if (i < 0 || size_t(i) >= m.cfg.sources.size()) return;
  m.cfg.sources.erase(m.cfg.sources.begin() + i);
  damac::commit(m);
  damac::fill_list(m);
}
- (void)outChanged:(id)sender {
  if (damac::g_main) damac::read_output(*damac::g_main);
}
- (void)outStepped:(id)sender {
  if (!damac::g_main || damac::g_main->filling) return;
  damac::g_main->out_gain.stringValue = damac::ns(damac::gain_field_text(damac::g_main->out_step.doubleValue));
  damac::read_output(*damac::g_main);
}
- (void)help:(id)sender {
  damac::show_help();
}
- (void)close:(id)sender {
  if (damac::g_main) [damac::g_main->w performClose:nil];
}
- (void)windowWillClose:(NSNotification*)n {
  damac::teardown();
}
@end

namespace damac {

void show(desk::Engine& engine, ChannelSource channels) {
  [NSApp activateIgnoringOtherApps:YES];
  if (g_main) {
    [g_main->w makeKeyAndOrderFront:nil];
    return;
  }
  g_main = new Main;
  Main& m = *g_main;
  m.engine = &engine;
  m.channels = std::move(channels);
  m.cfg = engine.settings();
  if (m.channels) m.ch = m.channels();
  m.last_channels = now_us();
  DATarget* t = m.target = [[DATarget alloc] init];

  NSWindow* w = m.w = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(0, 0, 820, 560)
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                          NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  w.title = @"Desktop audio";
  w.releasedWhenClosed = NO;
  w.delegate = t;
  if (@available(macOS 10.14, *)) w.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];

  m.enable = [NSButton checkboxWithTitle:@"Send this Mac’s audio to Dante (desktop audio)"
                                  target:t
                                  action:@selector(toggleEnable:)];
  m.enable.state = m.cfg.enabled ? NSControlStateValueOn : NSControlStateValueOff;
  m.status = wrapping("", 780);

  // Sources.
  NSTableView* table = m.table = [[NSTableView alloc] initWithFrame:NSZeroRect];
  struct Col {
    NSString* key;
    NSString* title;
    CGFloat width;
  } cols[] = {{@"source", @"Source", 230}, {@"channels", @"Channels", 80}, {@"gain", @"Gain", 60},
              {@"level", @"Level", 96},    {@"status", @"Status", 300}};
  for (const Col& c : cols) {
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:c.key];
    col.title = c.title;
    col.width = c.width;
    col.minWidth = 40;
    if (c.key == cols[4].key) col.resizingMask = NSTableColumnAutoresizingMask;
    [table addTableColumn:col];
  }
  table.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
  table.rowHeight = 22;
  table.allowsMultipleSelection = NO;
  table.allowsEmptySelection = YES;
  table.usesAlternatingRowBackgroundColors = YES;
  table.dataSource = t;
  table.delegate = t;
  table.target = t;
  table.doubleAction = @selector(tableDoubleClick:);
  NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
  scroll.documentView = table;
  scroll.hasVerticalScroller = YES;
  scroll.borderType = NSBezelBorder;
  [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:150].active = YES;
  m.add = [NSButton buttonWithTitle:@"Add…" target:t action:@selector(add:)];
  m.edit = [NSButton buttonWithTitle:@"Edit…" target:t action:@selector(edit:)];
  m.remove = [NSButton buttonWithTitle:@"Remove" target:t action:@selector(remove:)];
  NSStackView* buttons = hstack(@[ m.add, m.edit, m.remove ], 6);
  NSStackView* sv = vstack(@[ scroll, buttons ], 6);
  [scroll.widthAnchor constraintEqualToAnchor:sv.widthAnchor].active = YES;
  NSBox* sources = group("This Mac’s audio to Virgil’s transmit channels", sv);

  // Output.
  m.out_enable = [NSButton checkboxWithTitle:@"Play receive channels" target:t action:@selector(outChanged:)];
  m.out_ch = popup();
  m.out_ch.target = t;
  m.out_ch.action = @selector(outChanged:);
  m.out_gain = [NSTextField textFieldWithString:@"0"];
  m.out_gain.target = t;
  m.out_gain.action = @selector(outChanged:);
  m.out_gain.cell.sendsActionOnEndEditing = YES;
  [m.out_gain.widthAnchor constraintEqualToConstant:56].active = YES;
  m.out_step = [[NSStepper alloc] initWithFrame:NSZeroRect];
  m.out_step.minValue = vc::kMinGainDb;
  m.out_step.maxValue = vc::kMaxGainDb;
  m.out_step.increment = 0.5;
  m.out_step.valueWraps = NO;
  m.out_step.target = t;
  m.out_step.action = @selector(outStepped:);
  m.out_dev = popup();
  m.out_dev.target = t;
  m.out_dev.action = @selector(outChanged:);
  m.out_meter = level_indicator();
  m.out_state = cell_label("");
  NSGridView* og = [NSGridView gridViewWithViews:@[
    @[ m.out_enable, m.out_ch, label("Gain (dB):"), hstack(@[ m.out_gain, m.out_step ], 4) ],
    @[ label("on"), m.out_dev, [NSGridCell emptyContentView], [NSGridCell emptyContentView] ],
    @[ meter_box(m.out_meter, 120), m.out_state, [NSGridCell emptyContentView], [NSGridCell emptyContentView] ],
  ]];
  og.rowSpacing = 8;
  og.columnSpacing = 10;
  [og mergeCellsInHorizontalRange:NSMakeRange(1, 3) verticalRange:NSMakeRange(1, 1)];
  [og mergeCellsInHorizontalRange:NSMakeRange(1, 3) verticalRange:NSMakeRange(2, 1)];
  [og cellAtColumnIndex:0 rowIndex:1].xPlacement = NSGridCellPlacementTrailing;
  NSBox* output = group("Dante receive channels to an output device", og);

  NSTextField* note = dim(wrapping(
      "Captured audio still plays on the device it was sent to. To hear it only through Dante, turn that device "
      "down or play those apps on an output you do not listen to.",
      780));

  NSButton* help = [NSButton buttonWithTitle:@"Help" target:t action:@selector(help:)];
  NSButton* close = [NSButton buttonWithTitle:@"Close" target:t action:@selector(close:)];
  NSStackView* foot = [[NSStackView alloc] initWithFrame:NSZeroRect];
  foot.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  [foot addView:help inGravity:NSStackViewGravityLeading];
  [foot addView:close inGravity:NSStackViewGravityTrailing];

  NSStackView* v = vstack(@[ m.enable, m.status, sources, output, note, foot ], 10);
  for (NSView* full : @[ sources, output, foot ]) [full.widthAnchor constraintEqualToAnchor:v.widthAnchor].active = YES;
  [v.widthAnchor constraintGreaterThanOrEqualToConstant:780].active = YES;
  NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 820, 560)];
  pin(v, content, 16);
  w.contentView = content;

  fill_list(m);
  fill_output(m);
  tick();
  m.timer = [NSTimer timerWithTimeInterval:0.1
                                   repeats:YES
                                     block:^(NSTimer* timer) {
                                       tick();
                                     }];
  [[NSRunLoop mainRunLoop] addTimer:m.timer forMode:NSRunLoopCommonModes];
  [w center];
  [w makeKeyAndOrderFront:nil];
  reload_inventory();
}

}  // namespace damac
