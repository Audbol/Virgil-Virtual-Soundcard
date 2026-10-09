// Virgil Control for macOS: menu bar icon, status window with level meters
// and a settings panel, drawn like the Windows app (shared view code). It
// talks to virgild over its local control API (127.0.0.1:8480).
//
//   Virgil Control.app                  open the window (and the menu bar icon)
//   .../MacOS/Virgil Control --tray     menu bar icon only (used at login)
#import <AppKit/AppKit.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api.h"
#include "settings_model.h"
#include "view.h"

#ifndef VIRGIL_VERSION
#define VIRGIL_VERSION "dev"
#endif

namespace {

unsigned g_port = 8480;
vc::StatusView g_view;
std::mutex g_mutex;
vc::Status g_latest;  // guarded by g_mutex
std::atomic<bool> g_visible{false};
NSImage* g_logo = nil;

uint64_t now_ms() {
  using namespace std::chrono;
  return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

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
  if (a == vc::Action::StartService) [app performSelector:@selector(startService:) withObject:nil];
}
@end

// ---- application ---------------------------------------------------------------------------

@interface VCApp : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate>
@property(strong) NSWindow* window;
@property(strong) VCView* view;
@property(strong) NSStatusItem* item;
@property(strong) NSMenuItem* autostartItem;
@property BOOL startHidden;
@end

@implementation VCApp

- (void)applicationDidFinishLaunching:(NSNotification*)n {
  [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
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

  std::thread([self] { [self pollLoop]; }).detach();
  if (portable() && vc::http(g_port, "GET", "/api/status").status != 200) [self startService:nil];
  if (!self.startHidden) [self showWindow:nil];
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
  add(@"Restart audio engine", @selector(restartEngine:));
  [m addItem:[NSMenuItem separatorItem]];
  add(@"Open control panel in browser", @selector(openBrowser:));
  add(@"Show log", @selector(showLog:));
  self.autostartItem = add(@"Start at login", @selector(toggleAutostart:));
  add(@"About Virgil", @selector(about:));
  [m addItem:[NSMenuItem separatorItem]];
  add(@"Quit Virgil Control", @selector(quit:));
  return m;
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
  self.autostartItem.state = autostart_enabled() ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)pollLoop {
  bool was_reachable = true;
  std::string was_clock;
  while (true) {
    const auto r = vc::http(g_port, "GET", "/api/status");
    vc::Status s;
    if (r.status != 200 || !vc::parse_status(r.body, &s)) s = vc::Status();
    {
      std::lock_guard<std::mutex> l(g_mutex);
      g_latest = s;
    }
    const bool lost_service = was_reachable && !s.reachable;
    was_reachable = s.reachable;
    was_clock = s.clock;
    dispatch_async(dispatch_get_main_queue(), ^{
      [self applyStatus:lost_service];
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(g_visible ? 80 : 1000));
  }
}

- (void)applyStatus:(bool)lostService {
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
  (void)lostService;
}

- (void)showWindow:(id)sender {
  [NSApp activateIgnoringOtherApps:YES];
  [self.window makeKeyAndOrderFront:nil];
  g_visible = true;
}
- (BOOL)windowShouldClose:(NSWindow*)w {
  [w orderOut:nil];  // keep running in the menu bar
  g_visible = false;
  return NO;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a {
  return NO;
}
- (BOOL)applicationShouldHandleReopen:(NSApplication*)a hasVisibleWindows:(BOOL)v {
  [self showWindow:nil];
  return YES;
}

- (void)restartEngine:(id)sender {
  if (vc::http(g_port, "POST", "/api/restart").status != 200)
    alert(@"The Virgil service is not running, so the audio engine cannot be restarted.");
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

- (void)openBrowser:(id)sender {
  [[NSWorkspace sharedWorkspace]
      openURL:[NSURL URLWithString:ns("http://127.0.0.1:" + std::to_string(g_port) + "/")]];
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
  [NSApp terminate:nil];
}

// ---- settings panel ------------------------------------------------------------------------

- (void)showSettings:(id)sender {
  const auto c = vc::http(g_port, "GET", "/api/config");
  if (c.status != 200) {
    alert(@"The Virgil service is not running, so its settings cannot be changed now.");
    return;
  }
  const auto ifs = vc::parse_interfaces(vc::http(g_port, "GET", "/api/interfaces").body);
  vc::SettingsForm f = vc::load_settings(c.body, ifs);

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
  NSTextField* err = [NSTextField wrappingLabelWithString:@""];
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
  NSView* box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 320)];
  [box addSubview:grid];
  [grid.leadingAnchor constraintEqualToAnchor:box.leadingAnchor].active = YES;
  [grid.topAnchor constraintEqualToAnchor:box.topAnchor].active = YES;
  a.accessoryView = box;
  if (@available(macOS 10.14, *)) a.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  [NSApp activateIgnoringOtherApps:YES];

  while ([a runModal] == NSAlertFirstButtonReturn) {
    f.name = name.stringValue.UTF8String ?: "";
    f.iface_index = int(iface.indexOfSelectedItem);
    f.rate_index = int(rate.indexOfSelectedItem);
    f.tx_channels = unsigned(std::max(1, txch.intValue));
    f.rx_channels = unsigned(std::max(1, rxch.intValue));
    f.rx_latency_ms = rxl.stringValue.UTF8String ?: "4";
    f.tx_latency_ms = txl.stringValue.UTF8String ?: "4";
    f.local_clock = clock.indexOfSelectedItem == 1;
    f.master_capable = master.state == NSControlStateValueOn;
    std::string e;
    if (vc::save_settings(g_port, vc::apply_settings(c.body, f), &e)) break;
    err.stringValue = ns(e);
  }
}

@end

int main(int argc, const char** argv) {
  @autoreleasepool {
    bool tray = false;
    for (int i = 1; i < argc; ++i) {
      if (!std::strcmp(argv[i], "--tray")) tray = true;
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
