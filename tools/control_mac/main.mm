// Virgil Control for macOS: menu bar icon, status window with level meters
// and a settings panel, drawn like the Windows app (shared view code). It
// talks to virgild over its local control API (127.0.0.1:8480).
//
//   Virgil Control.app                  open the window (and the menu bar icon)
//   .../MacOS/Virgil Control --tray     menu bar icon only (used at login)
//   .../MacOS/Virgil Control --desktop-audio   also open the Desktop audio window
#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api.h"
#include "desktop_audio_engine.h"
#include "desktop_audio_mac.h"
#include "settings_model.h"
#include "view.h"
#include "windows_mac.h"

#ifndef VIRGIL_VERSION
#define VIRGIL_VERSION "dev"
#endif

namespace {

unsigned g_port = 8480;
vc::StatusView g_view;
std::mutex g_mutex;
vc::Status g_latest;  // guarded by g_mutex
std::atomic<bool> g_visible{false};
std::atomic<bool> g_quitting{false};
// Posted by a second launch so the running copy shows its window instead.
NSString* const kShowNotification = @"org.virgil.control.show";
NSImage* g_logo = nil;
// Desktop audio (this Mac's apps <-> Dante), running for as long as this app.
std::unique_ptr<desk::Engine> g_desktop;
bool g_open_desktop = false;  // --desktop-audio

// Virgil's channels as the service reports them, for the Desktop audio window.
damac::ChannelInfo desktop_channels() {
  std::lock_guard<std::mutex> l(g_mutex);
  damac::ChannelInfo c;
  if (!g_latest.reachable) return c;
  c.tx = g_latest.tx;
  c.rx = g_latest.rx;
  c.tx_names = g_latest.tx_names;
  c.rx_names = g_latest.rx_names;
  return c;
}

uint64_t now_ms() {
  using namespace std::chrono;
  return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Invalid UTF-8 sequences (an app or channel name the daemon passed through
// as raw bytes) become U+FFFD instead of blanking the whole string.
std::string utf8_sanitize(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      out += char(c);
      ++i;
      continue;
    }
    size_t len = 0;
    unsigned cp = 0, min = 0;
    if ((c & 0xE0) == 0xC0) len = 2, cp = c & 0x1F, min = 0x80;
    else if ((c & 0xF0) == 0xE0) len = 3, cp = c & 0x0F, min = 0x800;
    else if ((c & 0xF8) == 0xF0) len = 4, cp = c & 0x07, min = 0x10000;
    bool ok = len != 0 && i + len <= n;
    for (size_t k = 1; ok && k < len; ++k) {
      const unsigned char d = static_cast<unsigned char>(s[i + k]);
      if ((d & 0xC0) != 0x80) ok = false;
      else cp = (cp << 6) | (d & 0x3F);
    }
    if (ok && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
    if (ok) {
      out.append(s, i, len);
      i += len;
    } else {
      out += "\xEF\xBF\xBD";
      ++i;
    }
  }
  return out;
}

NSString* ns(const std::string& s) {
  if (s.empty()) return @"";
  NSString* r = [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
  if (!r) {
    const std::string clean = utf8_sanitize(s);
    r = [[NSString alloc] initWithBytes:clean.data() length:clean.size() encoding:NSUTF8StringEncoding];
  }
  return r ?: @"";
}

// UNUserNotificationCenter raises an exception in a process without a bundle
// identifier; Virgil Control always runs from its .app, but be safe.
bool notifications_available() {
  if (@available(macOS 10.14, *)) return [[NSBundle mainBundle] bundleIdentifier] != nil;
  return false;
}

// Banner notification (service lost, clock lost), like the tray balloons on
// Windows and the desktop notifications on Linux. `ident` is per kind of
// event, so a repeat replaces the earlier banner instead of piling up. If the
// user did not allow notifications, the system drops the request.
void notify(NSString* ident, NSString* title, NSString* text) {
  if (!notifications_available()) return;
  if (@available(macOS 10.14, *)) {
    UNMutableNotificationContent* c = [[UNMutableNotificationContent alloc] init];
    c.title = title;
    c.body = text;
    c.sound = [UNNotificationSound defaultSound];
    UNNotificationRequest* r = [UNNotificationRequest requestWithIdentifier:ident content:c trigger:nil];
    [[UNUserNotificationCenter currentNotificationCenter] addNotificationRequest:r withCompletionHandler:nil];
  }
}

// Runs `block` on the main thread for a background request that may end in a
// modal alert. Not dispatch_get_main_queue(): that queue is serial, so a modal
// run inside one of its blocks would hold back every later main-queue block
// (the status updates) until the alert is closed.
void on_main(void (^block)(void)) {
  CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, block);
  CFRunLoopWakeUp(CFRunLoopGetMain());
}

// A queue for the blocking control API requests the user starts.
dispatch_queue_t background() { return dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0); }

// Consecutive failed polls before the service counts as not running, so one
// slow reply (engine restart, busy machine) does not flash "not running".
constexpr int kFailuresUntilUnreachable = 3;

NSColor* color(unsigned c, float a = 1.f) {
  return [NSColor colorWithSRGBRed:((c >> 16) & 255) / 255.0
                             green:((c >> 8) & 255) / 255.0
                              blue:(c & 255) / 255.0
                             alpha:a];
}

NSFont* font(vc::Font f) {
  switch (f) {
    case vc::Font::Title: return [NSFont systemFontOfSize:17 weight:NSFontWeightSemibold];
    case vc::Font::Big: return [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
    case vc::Font::Small: return [NSFont systemFontOfSize:10.5];
    case vc::Font::Mono:
      if (@available(macOS 10.15, *)) return [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
      return [NSFont fontWithName:@"Menlo" size:11] ?: [NSFont systemFontOfSize:11];
    default: return [NSFont systemFontOfSize:12];
  }
}

// ---- CoreGraphics canvas (the view is flipped: y grows downwards) -------------------------

class MacCanvas : public vc::Canvas {
 public:
  void fill(const vc::Rect& r, unsigned c, float a) override {
    [color(c, a) setFill];
    NSRectFill(rect(r));
  }
  void fill_round(const vc::Rect& r, float rad, unsigned c, float a) override {
    [color(c, a) setFill];
    [[NSBezierPath bezierPathWithRoundedRect:rect(r) xRadius:rad yRadius:rad] fill];
  }
  void stroke_round(const vc::Rect& r, float rad, unsigned c) override {
    [color(c) setStroke];
    NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(rect(r), 0.5, 0.5) xRadius:rad yRadius:rad];
    p.lineWidth = 1;
    [p stroke];
  }
  void hline(float x0, float x1, float y, unsigned c) override {
    [color(c) setFill];
    NSRectFill(NSMakeRect(x0, std::floor(y), x1 - x0, 1));
  }
  void dot(float cx, float cy, float rad, unsigned c) override {
    [color(c) setFill];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - rad, cy - rad, 2 * rad, 2 * rad)] fill];
  }
  void text(const std::string& s, vc::Font f, const vc::Rect& r, unsigned c, vc::Align a) override {
    if (s.empty()) return;
    NSDictionary* attrs = @{NSFontAttributeName : font(f), NSForegroundColorAttributeName : color(c)};
    NSString* str = ns(s);
    const NSSize sz = [str sizeWithAttributes:attrs];
    CGFloat x = r.l;
    if (a == vc::Align::Center) x = r.l + (r.r - r.l - sz.width) / 2;
    if (a == vc::Align::Right) x = r.r - sz.width;
    const CGFloat y = r.t + (r.b - r.t - sz.height) / 2;
    [NSGraphicsContext saveGraphicsState];
    NSRectClip(rect(r));
    [str drawAtPoint:NSMakePoint(x, y) withAttributes:attrs];
    [NSGraphicsContext restoreGraphicsState];
  }
  float text_width(const std::string& s, vc::Font f) override {
    if (s.empty()) return 0;
    return float([ns(s) sizeWithAttributes:@{NSFontAttributeName : font(f)}].width);
  }
  void logo(const vc::Rect& r) override {
    if (!g_logo) return;
    [g_logo drawInRect:rect(r)
              fromRect:NSZeroRect
             operation:NSCompositingOperationSourceOver
              fraction:1
        respectFlipped:YES
                 hints:nil];
  }

 private:
  static NSRect rect(const vc::Rect& r) { return NSMakeRect(r.l, r.t, r.r - r.l, r.b - r.t); }
};

// ---- helpers -------------------------------------------------------------------------------

std::string bundle_folder() {
  // .../Folder/Virgil Control.app -> .../Folder
  return std::string([[[[NSBundle mainBundle] bundlePath] stringByDeletingLastPathComponent] UTF8String]);
}
bool exists(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}
bool portable() {
  const std::string d = bundle_folder();
  return d.rfind("/Applications", 0) != 0 && exists(d + "/virgild");
}

void alert(NSString* text) {
  NSAlert* a = [[NSAlert alloc] init];
  a.messageText = @"Virgil";
  a.informativeText = text;
  [NSApp activateIgnoringOtherApps:YES];
  [a runModal];
}

std::string launch_agent_path() {
  return std::string([NSHomeDirectory() UTF8String]) + "/Library/LaunchAgents/org.virgil.control.plist";
}
bool autostart_enabled() { return exists(launch_agent_path()); }
void set_autostart(bool on) {
  const std::string p = launch_agent_path();
  if (!on) {
    unlink(p.c_str());
    return;
  }
  NSString* exe = [[NSBundle mainBundle] executablePath];
  NSDictionary* plist = @{
    @"Label" : @"org.virgil.control",
    @"ProgramArguments" : @[ exe, @"--tray" ],
    @"RunAtLoad" : @YES,
    @"LimitLoadToSessionType" : @"Aqua",
  };
  [[NSFileManager defaultManager] createDirectoryAtPath:ns(p.substr(0, p.find_last_of('/')))
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
  [plist writeToFile:ns(p) atomically:YES];
}

// Remote access as /api/status reports it ("remote"; vc::Status leaves it out).
struct RemoteInfo {
  bool listening = false;
  std::vector<std::string> urls;
};
RemoteInfo parse_remote(const std::string& status_body) {
  RemoteInfo ri;
  vc::Json j;
  if (!vc::parse_json(status_body, &j)) return ri;
  const vc::Json& r = j["remote"];
  ri.listening = r["listening"].boolean();
  const vc::Json& u = r["urls"];
  for (size_t i = 0; i < u.size(); ++i) {
    const std::string url = vc::valid_utf8(u[i].str());
    if (!url.empty()) ri.urls.push_back(url);
  }
  return ri;
}

}  // namespace

// ---- status view ---------------------------------------------------------------------------

@interface VCView : NSView
@end

@implementation VCView
- (BOOL)isFlipped {
  return YES;
}
- (void)drawRect:(NSRect)dirty {
  MacCanvas c;
  g_view.paint(c, float(self.bounds.size.width), float(self.bounds.size.height));
}
- (void)updateTrackingAreas {
  for (NSTrackingArea* t in self.trackingAreas) [self removeTrackingArea:t];
  [self addTrackingArea:[[NSTrackingArea alloc]
                            initWithRect:self.bounds
                                 options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                         NSTrackingActiveAlways | NSTrackingInVisibleRect
                                   owner:self
                                userInfo:nil]];
  [super updateTrackingAreas];
}
- (NSPoint)local:(NSEvent*)e {
  return [self convertPoint:e.locationInWindow fromView:nil];
}
- (void)mouseMoved:(NSEvent*)e {
  const NSPoint p = [self local:e];
  if (g_view.hover(float(p.x), float(p.y))) {
    [(g_view.over_button() ? [NSCursor pointingHandCursor] : [NSCursor arrowCursor]) set];
    self.needsDisplay = YES;
  }
}
- (void)mouseExited:(NSEvent*)e {
  if (g_view.hover(-1, -1)) self.needsDisplay = YES;
  [[NSCursor arrowCursor] set];
}
- (void)mouseDown:(NSEvent*)e {
  const NSPoint p = [self local:e];
  if (g_view.press(float(p.x), float(p.y))) self.needsDisplay = YES;
}
- (void)mouseUp:(NSEvent*)e {
  const NSPoint p = [self local:e];
  const vc::Action a = g_view.release(float(p.x), float(p.y));
  self.needsDisplay = YES;
  id app = [NSApp delegate];
  if (a == vc::Action::Settings) [app performSelector:@selector(showSettings:) withObject:nil];
  if (a == vc::Action::Restart) [app performSelector:@selector(restartEngine:) withObject:nil];
  if (a == vc::Action::Stop) [app performSelector:@selector(stopVirgil:) withObject:nil];
  if (a == vc::Action::Start) [app performSelector:@selector(startVirgil:) withObject:nil];
  if (a == vc::Action::StartService) [app performSelector:@selector(startService:) withObject:nil];
  if (a == vc::Action::Diagnostics) [app performSelector:@selector(openDiagnostics:) withObject:nil];
  if (a == vc::Action::DesktopAudio) [app performSelector:@selector(showDesktopAudio:) withObject:nil];
}
@end

// ---- application ---------------------------------------------------------------------------

@interface VCApp : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate, UNUserNotificationCenterDelegate>
@property(strong) NSWindow* window;
@property(strong) VCView* view;
@property(strong) NSStatusItem* item;
@property(strong) NSMenuItem* autostartItem;
@property(strong) NSMenuItem* runItem;  // Stop Virgil / Start Virgil
@property BOOL startHidden;
// A start/stop/restart request or a settings save is on its way (main thread only).
@property BOOL busy;
// The settings are being fetched, or the settings dialog is open (or saving).
@property BOOL settingsLoading;
@property(strong) NSAlert* settingsAlert;
@property(strong) NSTextField* settingsError;
@property(copy) std::string (^settingsCollect)(void);  // the form's fields as virgil.conf text
@property(copy) std::string (^settingsProblem)(void);  // why the form cannot be saved, or ""
@end

@implementation VCApp

- (void)applicationDidFinishLaunching:(NSNotification*)n {
  // One copy only: the login item starts the executable directly, so opening
  // the app from Applications could otherwise add a second menu bar icon
  // (and Quit would seem not to work while the other copy keeps running).
  NSString* bid = [[NSBundle mainBundle] bundleIdentifier];
  const pid_t me = getpid();
  for (NSRunningApplication* a in [NSRunningApplication runningApplicationsWithBundleIdentifier:bid ?: @""]) {
    if (a.processIdentifier == me) continue;
    if (!self.startHidden)
      [[NSDistributedNotificationCenter defaultCenter] postNotificationName:kShowNotification
                                                                     object:nil
                                                                   userInfo:nil
                                                         deliverImmediately:YES];
    _exit(0);
  }
  [[NSDistributedNotificationCenter defaultCenter] addObserver:self
                                                      selector:@selector(showWindow:)
                                                          name:kShowNotification
                                                        object:nil];

  // Desktop audio runs in this app (it starts at login, in the user's
  // session, where the apps and the sound devices are).
  g_desktop.reset(new desk::Engine(desk::make_system_backend()));
  g_desktop->start(desk::load_settings(desk::settings_path(), nullptr));
  g_view.set_desktop_audio(true);

  [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
  NSApp.mainMenu = [self buildMainMenu];
  NSString* png = [[NSBundle mainBundle] pathForResource:@"virgil-256" ofType:@"png"];
  g_logo = png ? [[NSImage alloc] initWithContentsOfFile:png] : [NSApp applicationIconImage];

  self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, g_view.width(), g_view.height())
                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                      NSWindowStyleMaskMiniaturizable
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
  self.window.title = @"Virgil";
  self.window.delegate = self;
  self.window.releasedWhenClosed = NO;
  if (@available(macOS 10.14, *)) self.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  self.view = [[VCView alloc] initWithFrame:NSMakeRect(0, 0, g_view.width(), g_view.height())];
  self.window.contentView = self.view;
  [self.window center];

  self.item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
  NSImage* icon = [g_logo copy];
  icon.size = NSMakeSize(18, 18);
  self.item.button.image = icon;
  self.item.button.toolTip = @"Virgil";
  self.item.menu = [self buildMenu];
  // Ask once (the system remembers the answer; later calls do not prompt).
  // Refused or unavailable: notify() requests are simply dropped.
  if (notifications_available()) {
    if (@available(macOS 10.14, *)) {
      UNUserNotificationCenter* nc = [UNUserNotificationCenter currentNotificationCenter];
      nc.delegate = self;
      [nc requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                        completionHandler:^(BOOL granted, NSError* error){
                        }];
    }
  }

  std::thread([self] { [self pollLoop]; }).detach();
  if (portable() && vc::http(g_port, "GET", "/api/status").status != 200) [self startService:nil];
  if (!self.startHidden) [self showWindow:nil];
  if (g_open_desktop) [self showDesktopAudio:nil];
}

// The app menu bar while the window is open: Quit (Cmd-Q), Settings (Cmd-,),
// Close (Cmd-W) and an Edit menu so copy and paste work in the settings fields.
- (NSMenu*)buildMainMenu {
  NSMenu* bar = [[NSMenu alloc] init];
  NSMenu* app = [[NSMenu alloc] initWithTitle:@"Virgil"];
  [[app addItemWithTitle:@"About Virgil" action:@selector(about:) keyEquivalent:@""] setTarget:self];
  [app addItem:[NSMenuItem separatorItem]];
  [[app addItemWithTitle:@"Settings…" action:@selector(showSettings:) keyEquivalent:@","] setTarget:self];
  [app addItem:[NSMenuItem separatorItem]];
  [app addItemWithTitle:@"Hide Virgil" action:@selector(hide:) keyEquivalent:@"h"];
  [app addItem:[NSMenuItem separatorItem]];
  [[app addItemWithTitle:@"Quit Virgil Control" action:@selector(quit:) keyEquivalent:@"q"] setTarget:self];
  [bar addItemWithTitle:@"" action:nil keyEquivalent:@""].submenu = app;

  NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
  [edit addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
  [edit addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"Z"];
  [edit addItem:[NSMenuItem separatorItem]];
  [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
  [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
  [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
  [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
  [bar addItemWithTitle:@"Edit" action:nil keyEquivalent:@""].submenu = edit;

  NSMenu* win = [[NSMenu alloc] initWithTitle:@"Window"];
  [win addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
  [win addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
  [bar addItemWithTitle:@"Window" action:nil keyEquivalent:@""].submenu = win;
  NSApp.windowsMenu = win;
  return bar;
}

- (NSMenu*)buildMenu {
  NSMenu* m = [[NSMenu alloc] init];
  m.delegate = self;
  auto add = [&](NSString* title, SEL sel) {
    NSMenuItem* i = [m addItemWithTitle:title action:sel keyEquivalent:@""];
    i.target = self;
    return i;
  };
  add(@"Open Virgil", @selector(showWindow:));
  add(@"Settings…", @selector(showSettings:));
  add(@"Desktop audio…", @selector(showDesktopAudio:));
  add(@"Restart audio engine", @selector(restartEngine:));
  self.runItem = add(@"Stop Virgil", @selector(toggleRunning:));
  [m addItem:[NSMenuItem separatorItem]];
  add(@"Dropouts and diagnostics…", @selector(openDiagnostics:));
  add(@"Channel names…", @selector(showChannelNames:));
  add(@"Edit configuration…", @selector(showConfigEditor:));
  add(@"Show log", @selector(showLog:));
  self.autostartItem = add(@"Start at login", @selector(toggleAutostart:));
  add(@"About Virgil", @selector(about:));
  [m addItem:[NSMenuItem separatorItem]];
  add(@"Quit Virgil Control", @selector(quit:));
  return m;
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
  self.autostartItem.state = autostart_enabled() ? NSControlStateValueOn : NSControlStateValueOff;
  const vc::Status& s = g_view.status();
  self.runItem.title = s.state == "stopped" ? @"Start Virgil" : @"Stop Virgil";
}

// Menu items (status item menu and the app menu) are enabled here: NSMenu
// asks before showing them (autoenablesItems).
- (BOOL)validateMenuItem:(NSMenuItem*)item {
  const vc::Status& s = g_view.status();
  const SEL a = item.action;
  if (a == @selector(toggleRunning:)) return s.reachable && !self.busy;
  if (a == @selector(restartEngine:)) return s.reachable && s.state != "stopped" && !self.busy;
  if (a == @selector(showSettings:)) return !self.busy || self.settingsAlert != nil;
  return YES;
}

- (void)pollLoop {
  // false at first: no "service stopped" notice when it was not running at launch.
  bool was_reachable = false;
  std::string was_clock;
  uint64_t lost_since = 0;
  bool lost_told = false;
  bool no_leader_told = false;
  int failures = 0;
  while (!g_quitting) {
    const auto r = vc::http(g_port, "GET", "/api/status");
    vc::Status s;
    const bool ok = r.status == 200 && vc::parse_status(r.body, &s);
    failures = ok ? 0 : failures + 1;
    // A missed poll or two keeps the last good status on screen.
    const bool unreachable = !ok && (!was_reachable || failures >= kFailuresUntilUnreachable);
    if (ok || unreachable) {
      if (!ok) s = vc::Status();
      {
        std::lock_guard<std::mutex> l(g_mutex);
        g_latest = s;
      }
      int event = 0;  // 1 = service lost, 2 = Dante clock lost, 3 = no clock leader
      if (was_reachable && !s.reachable) event = 1;
      // Not when Virgil was stopped on purpose (or is restarting).
      if (s.reachable && s.state == "running" && was_clock == "ptp-locked" && s.clock != "ptp-locked" &&
          !lost_since)
        lost_since = now_ms();
      if (s.state != "running") lost_since = 0;
      if (s.clock == "ptp-locked") lost_since = 0, lost_told = false;
      if (lost_since && !lost_told && now_ms() - lost_since > 5000) {
        lost_told = true;
        event = 2;
      }
      // Once per stretch without a leader (as on Windows and Linux).
      if ((s.reachable && s.state != "running") || s.clock == "ptp-locked") no_leader_told = false;
      if (!event && !no_leader_told && !lost_since && vc::clock_notice(s).show) {
        no_leader_told = true;
        event = 3;
      }
      was_reachable = s.reachable;
      was_clock = s.clock;
      dispatch_async(dispatch_get_main_queue(), ^{
        [self applyStatus:event];
      });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(g_visible ? 80 : 1000));
  }
}

- (void)applyStatus:(int)event {
  vc::Status s;
  {
    std::lock_guard<std::mutex> l(g_mutex);
    s = g_latest;
  }
  const unsigned old_tx = g_view.status().tx, old_rx = g_view.status().rx;
  g_view.update(s, now_ms());
  if (s.tx != old_tx || s.rx != old_rx) {
    NSRect f = self.window.frame;
    const NSRect content = NSMakeRect(0, 0, g_view.width(), g_view.height());
    const NSRect nf = [self.window frameRectForContentRect:content];
    f.origin.y += f.size.height - nf.size.height;
    f.size = nf.size;
    [self.window setFrame:f display:YES];
  }
  self.item.button.toolTip = ns(vc::tray_tooltip(s));
  if (g_visible) self.view.needsDisplay = YES;
  if (event == 1)
    notify(@"org.virgil.control.service", @"Virgil service stopped",
           @"Audio to and from the Dante network has stopped.");
  if (event == 2)
    notify(@"org.virgil.control.clock", @"Dante clock lost",
           @"Virgil is running on its own clock; check the network connection.");
  if (event == 3) {
    const vc::ClockNotice n = vc::clock_notice(s);
    std::string body = n.title + ".";
    for (const auto& h : n.hints) body += "\n" + h;
    notify(@"org.virgil.control.clock", ns(n.headline), ns(body));
  }
}

// Show the banners even while the window is in front; clicking one opens it.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions options))completionHandler {
  if (@available(macOS 11.0, *)) {
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList |
                      UNNotificationPresentationOptionSound);
  } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    completionHandler(UNNotificationPresentationOptionAlert | UNNotificationPresentationOptionSound);
#pragma clang diagnostic pop
  }
}
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
  on_main(^{
    [self showWindow:nil];
  });
  completionHandler();
}

// While the window is open the app is a regular one (Dock icon, menu bar,
// Cmd-Tab, Cmd-Q); closed, it lives on as the menu bar icon only.
- (void)showWindow:(id)sender {
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  [NSApp activateIgnoringOtherApps:YES];
  [self.window makeKeyAndOrderFront:nil];
  g_visible = true;
}
- (BOOL)windowShouldClose:(NSWindow*)w {
  [w orderOut:nil];  // keep running in the menu bar
  g_visible = false;
  [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
  return NO;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)a {
  return NSTerminateNow;
}
- (void)applicationWillTerminate:(NSNotification*)n {
  g_quitting = true;
  if (g_desktop) g_desktop->shutdown();  // releases its Virgil playback slot, closes its taps
  // The poll thread is still running: leave without static destructors.
  _exit(0);
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a {
  return NO;
}
- (BOOL)applicationShouldHandleReopen:(NSApplication*)a hasVisibleWindows:(BOOL)v {
  [self showWindow:nil];
  return YES;
}

// POSTs to the control API off the main thread; `failure` is shown if the
// service does not answer. Further requests wait until this one is done.
- (void)post:(const char*)path failure:(NSString*)failure {
  if (self.busy) return;
  self.busy = YES;
  const std::string p = path;
  dispatch_async(background(), ^{
    const bool ok = vc::http(g_port, "POST", p).status == 200;
    on_main(^{
      self.busy = NO;
      if (!ok) alert(failure);
    });
  });
}

- (void)stopVirgil:(id)sender {
  if (self.busy) return;
  NSAlert* a = [[NSAlert alloc] init];
  a.messageText = @"Stop Virgil?";
  a.informativeText = @"Audio to and from the Dante network stops, Virgil leaves the network and apps cannot use "
                      @"its soundcard until you start it again. It stays stopped after a restart of the Mac.";
  [a addButtonWithTitle:@"Stop"];
  [a addButtonWithTitle:@"Cancel"];
  [NSApp activateIgnoringOtherApps:YES];
  if ([a runModal] != NSAlertFirstButtonReturn) return;
  [self post:"/api/stop" failure:@"The Virgil service is not running."];
}
- (void)startVirgil:(id)sender {
  [self post:"/api/start" failure:@"The Virgil service is not running."];
}
- (void)toggleRunning:(id)sender {
  if (g_view.status().state == "stopped") [self startVirgil:sender];
  else [self stopVirgil:sender];
}

- (void)restartEngine:(id)sender {
  [self post:"/api/restart" failure:@"The Virgil service is not running, so the audio engine cannot be restarted."];
}

- (void)startService:(id)sender {
  if (portable()) {
    const std::string d = bundle_folder();
    NSTask* t = [[NSTask alloc] init];
    t.executableURL = [NSURL fileURLWithPath:ns(d + "/virgild")];
    NSMutableArray* args =
        [@[ @"--log", ns(d + "/virgild.log"), @"--control-port", ns(std::to_string(g_port)) ] mutableCopy];
    if (exists(d + "/virgil.conf")) [args addObjectsFromArray:@[ @"-c", ns(d + "/virgil.conf") ]];
    t.arguments = args;
    NSError* err = nil;
    if (![t launchAndReturnError:&err]) alert(@"Could not start virgild from this folder.");
    return;
  }
  NSDictionary* error = nil;
  NSAppleScript* s = [[NSAppleScript alloc]
      initWithSource:@"do shell script \"launchctl kickstart -k system/org.virgil.virgild || "
                     @"launchctl load -w /Library/LaunchDaemons/org.virgil.virgild.plist\" with administrator "
                     @"privileges"];
  if (![s executeAndReturnError:&error] && ![error[NSAppleScriptErrorNumber] isEqual:@(-128)])
    alert(@"Could not start the Virgil service. See the log (Show log) for details.");
}

// Native windows (windows_mac.mm), one of each.
- (void)openDiagnostics:(id)sender {
  vcmac::show_diagnostics(g_port);
}
- (void)showChannelNames:(id)sender {
  vcmac::show_channel_names(g_port, desktop_channels);
}
- (void)showConfigEditor:(id)sender {
  vcmac::show_config_editor(g_port);
}

- (void)showLog:(id)sender {
  NSString* p = portable() ? ns(bundle_folder() + "/virgild.log") : @"/Library/Logs/Virgil/virgild.log";
  if (![[NSFileManager defaultManager] fileExistsAtPath:p]) {
    alert([NSString stringWithFormat:@"No log yet at %@.", p]);
    return;
  }
  // Console shows the log live.
  NSURL* console = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:@"com.apple.Console"];
  if (console) {
    [[NSWorkspace sharedWorkspace] openURLs:@[ [NSURL fileURLWithPath:p] ]
                       withApplicationAtURL:console
                              configuration:[NSWorkspaceOpenConfiguration configuration]
                          completionHandler:nil];
  } else {
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:p]];
  }
}

- (void)showDesktopAudio:(id)sender {
  if (g_desktop) damac::show(*g_desktop, desktop_channels);
}

- (void)toggleAutostart:(id)sender {
  set_autostart(!autostart_enabled());
}

- (void)about:(id)sender {
  [NSApp activateIgnoringOtherApps:YES];
  [NSApp orderFrontStandardAboutPanelWithOptions:@{
    @"ApplicationName" : @"Virgil",
    @"ApplicationVersion" : @VIRGIL_VERSION,
    @"Credits" : [[NSAttributedString alloc]
        initWithString:@"Virtual Interface Routing Gateway for Inferno-based Low-latency audio.\n\nA virtual "
                       @"soundcard for Dante® networks, built on Inferno. Independent project: not "
                       @"affiliated with or endorsed by Audinate. Dante is a registered trademark of Audinate "
                       @"Pty Ltd. Licensed under the GPLv3."],
  }];
}

- (void)quit:(id)sender {
  g_quitting = true;
  [NSApp terminate:nil];
  _exit(0);  // not reached unless something vetoed termination
}

// ---- settings panel ------------------------------------------------------------------------

- (void)showSettings:(id)sender {
  // Already open: bring it forward (while it saves it is hidden; nothing to do).
  if (self.settingsAlert) {
    if (self.settingsAlert.window.visible) {
      [NSApp activateIgnoringOtherApps:YES];
      [self.settingsAlert.window makeKeyAndOrderFront:nil];
    }
    return;
  }
  if (self.settingsLoading || self.busy) return;
  self.settingsLoading = YES;
  dispatch_async(background(), ^{
    const vc::HttpResult c = vc::http(g_port, "GET", "/api/config");
    std::vector<vc::Interface> ifs;
    RemoteInfo remote;
    if (c.status == 200) {
      ifs = vc::parse_interfaces(vc::http(g_port, "GET", "/api/interfaces").body);
      const vc::HttpResult st = vc::http(g_port, "GET", "/api/status");
      if (st.status == 200) remote = parse_remote(st.body);
    }
    on_main(^{
      self.settingsLoading = NO;
      if (c.status != 200) {
        alert(@"The Virgil service is not running, so its settings cannot be changed now.");
        return;
      }
      [self openSettings:c.body interfaces:ifs remote:remote];
    });
  });
}

// Runs the settings dialog again after a failed save (with the error shown),
// until it is applied or cancelled. The save itself runs off the main thread.
- (void)runSettings {
  NSAlert* a = self.settingsAlert;
  NSModalResponse response;
  for (;;) {
    [NSApp activateIgnoringOtherApps:YES];
    response = [a runModal];
    if (response != NSAlertFirstButtonReturn) break;
    // Checked here (the daemon would refuse it too), keeping the dialog open.
    const std::string problem = self.settingsProblem ? self.settingsProblem() : std::string();
    if (problem.empty()) break;
    self.settingsError.stringValue = ns(problem);
  }
  if (response != NSAlertFirstButtonReturn) {
    self.settingsAlert = nil;
    self.settingsError = nil;
    self.settingsCollect = nil;
    self.settingsProblem = nil;
    if (response == NSAlertThirdButtonReturn) [self showChannelNames:nil];
    if (response == NSAlertThirdButtonReturn + 1) [self openDiagnostics:nil];
    if (response == NSAlertThirdButtonReturn + 2) [self showConfigEditor:nil];
    return;
  }
  const std::string conf = self.settingsCollect();
  self.busy = YES;
  dispatch_async(background(), ^{
    std::string e;
    const bool ok = vc::save_settings(g_port, conf, &e);
    on_main(^{
      self.busy = NO;
      if (ok) {
        self.settingsAlert = nil;
        self.settingsError = nil;
        self.settingsCollect = nil;
        self.settingsProblem = nil;
        return;
      }
      self.settingsError.stringValue = ns(e);
      [self runSettings];
    });
  });
}

- (void)openSettings:(const std::string&)conf
          interfaces:(const std::vector<vc::Interface>&)ifs
              remote:(const RemoteInfo&)remote {
  const vc::SettingsForm f = vc::load_settings(conf, ifs);

  auto label = [](NSString* s) {
    NSTextField* t = [NSTextField labelWithString:s];
    t.alignment = NSTextAlignmentRight;
    return t;
  };
  auto popup = [](const std::vector<std::string>& items, int sel) {
    NSPopUpButton* p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (const auto& i : items) [p addItemWithTitle:ns(i)];
    if (sel >= 0 && sel < int(items.size())) [p selectItemAtIndex:sel];
    return p;
  };
  auto combo = [](const char* const* items, int n, const std::string& value) {
    NSComboBox* cb = [[NSComboBox alloc] initWithFrame:NSZeroRect];
    for (int i = 0; i < n; ++i) [cb addItemWithObjectValue:@(items[i])];
    cb.stringValue = ns(value);
    return cb;
  };

  NSTextField* name = [NSTextField textFieldWithString:ns(f.name)];
  NSPopUpButton* iface = popup(f.iface_labels(), f.iface_index);
  NSPopUpButton* rate = popup(std::vector<std::string>(vc::kRateLabels, vc::kRateLabels + 4), f.rate_index);
  NSTextField* txch = [NSTextField textFieldWithString:ns(std::to_string(f.tx_channels))];
  NSTextField* rxch = [NSTextField textFieldWithString:ns(std::to_string(f.rx_channels))];
  NSStackView* ch = [NSStackView stackViewWithViews:@[ txch, [NSTextField labelWithString:@"/"], rxch ]];
  [txch.widthAnchor constraintEqualToConstant:50].active = YES;
  [rxch.widthAnchor constraintEqualToConstant:50].active = YES;
  NSComboBox* rxl = combo(vc::kRxLatencies, 6, f.rx_latency_ms);
  NSComboBox* txl = combo(vc::kTxLatencies, 6, f.tx_latency_ms);
  NSPopUpButton* clock =
      popup({"Follow the Dante clock leader (PTP)", "Local clock only (testing)"}, f.local_clock ? 1 : 0);
  NSButton* master = [NSButton checkboxWithTitle:@"Become clock leader when no Dante device provides one"
                                          target:nil
                                          action:nil];
  master.state = f.master_capable ? NSControlStateValueOn : NSControlStateValueOff;
  // The web panel from other computers ([device] remote_access / remote_password).
  NSButton* remoteOn = [NSButton checkboxWithTitle:@"Allow remote access from other computers (web browser)"
                                            target:nil
                                            action:nil];
  remoteOn.state = f.remote_access ? NSControlStateValueOn : NSControlStateValueOff;
  NSSecureTextField* password = [[NSSecureTextField alloc] initWithFrame:NSZeroRect];
  password.stringValue = ns(f.remote_password);
  password.placeholderString = @"At least 8 characters";
  NSView* remoteInfo = [NSGridCell emptyContentView];
  if (remote.listening) {
    std::string t = "Open from another computer:";
    for (const auto& u : remote.urls) t += "\n" + u;
    t += "\nPlain HTTP: the password crosses the network unencrypted.";
    NSTextField* l = [NSTextField wrappingLabelWithString:ns(t)];
    l.preferredMaxLayoutWidth = 300;
    l.selectable = YES;
    l.textColor = [NSColor secondaryLabelColor];
    l.font = [NSFont systemFontOfSize:11];
    remoteInfo = l;
  }
  NSTextField* err = [NSTextField wrappingLabelWithString:@""];
  err.preferredMaxLayoutWidth = 300;
  err.textColor = color(vc::color::kBad);

  NSGridView* grid = [NSGridView gridViewWithViews:@[
    @[ label(@"Name in Dante Controller:"), name ],
    @[ label(@"Network interface:"), iface ],
    @[ label(@"Sample rate:"), rate ],
    @[ label(@"Channels out / in:"), ch ],
    @[ label(@"Receive latency (ms):"), rxl ],
    @[ label(@"Transmit latency (ms, 3.5 min):"), txl ],
    @[ label(@"Clock:"), clock ],
    @[ [NSGridCell emptyContentView], master ],
    @[ [NSGridCell emptyContentView], remoteOn ],
    @[ label(@"Remote password:"), password ],
    @[ [NSGridCell emptyContentView], remoteInfo ],
    @[ [NSGridCell emptyContentView], err ],
  ]];
  grid.rowSpacing = 8;
  grid.columnSpacing = 10;
  [grid columnAtIndex:1].width = 300;
  grid.translatesAutoresizingMaskIntoConstraints = NO;

  NSAlert* a = [[NSAlert alloc] init];
  a.messageText = @"Virgil Settings";
  a.informativeText = @"Applying restarts the audio engine; connected apps stay attached if the format is unchanged.";
  [a addButtonWithTitle:@"Apply"];
  [a addButtonWithTitle:@"Cancel"];
  [a addButtonWithTitle:@"Channel names…"];
  [a addButtonWithTitle:@"Dropouts and diagnostics…"];
  [a addButtonWithTitle:@"Edit configuration…"];
  const NSSize fit = grid.fittingSize;
  NSView* box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, std::max<CGFloat>(520, std::ceil(fit.width)),
                                                         std::ceil(fit.height) + 4)];
  [box addSubview:grid];
  [grid.leadingAnchor constraintEqualToAnchor:box.leadingAnchor].active = YES;
  [grid.topAnchor constraintEqualToAnchor:box.topAnchor].active = YES;
  a.accessoryView = box;
  if (@available(macOS 10.14, *)) a.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];

  const std::string base = conf;  // a copy: blocks would capture the reference itself
  self.settingsAlert = a;
  self.settingsError = err;
  // The form as the fields have it now.
  vc::SettingsForm (^form)(void) = ^vc::SettingsForm(void) {
    vc::SettingsForm g = f;
    g.name = name.stringValue.UTF8String ?: "";
    g.iface_index = int(iface.indexOfSelectedItem);
    g.rate_index = int(rate.indexOfSelectedItem);
    // 0 is allowed (receive or transmit only); the daemon insists on one direction.
    g.tx_channels = unsigned(std::max(0, txch.intValue));
    g.rx_channels = unsigned(std::max(0, rxch.intValue));
    g.rx_latency_ms = rxl.stringValue.UTF8String ?: "4";
    g.tx_latency_ms = txl.stringValue.UTF8String ?: "4";
    g.local_clock = clock.indexOfSelectedItem == 1;
    g.master_capable = master.state == NSControlStateValueOn;
    g.remote_access = remoteOn.state == NSControlStateValueOn;
    g.remote_password = password.stringValue.UTF8String ?: "";
    return g;
  };
  self.settingsCollect = ^std::string(void) {
    return vc::apply_settings(base, form());
  };
  self.settingsProblem = ^std::string(void) {
    return vc::remote_access_problem(form());
  };
  [self runSettings];
}

@end

int main(int argc, const char** argv) {
  @autoreleasepool {
    bool tray = false;
    for (int i = 1; i < argc; ++i) {
      if (!std::strcmp(argv[i], "--tray")) tray = true;
      if (!std::strcmp(argv[i], "--desktop-audio")) g_open_desktop = true;
      if (!std::strcmp(argv[i], "--port") && i + 1 < argc) g_port = unsigned(std::atoi(argv[++i]));
    }
    NSApplication* app = [NSApplication sharedApplication];
    VCApp* d = [[VCApp alloc] init];
    d.startHidden = tray;
    app.delegate = d;
    [app run];
  }
  return 0;
}
