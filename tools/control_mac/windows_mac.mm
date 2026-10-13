#import <AppKit/AppKit.h>

#include "windows_mac.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "api.h"
#include "config_model.h"
#include "diagnostics_model.h"
#include "view.h"  // vc::color

namespace {

NSString* ns(const std::string& s) {
  if (s.empty()) return @"";
  const std::string v = vc::valid_utf8(s);
  return [[NSString alloc] initWithBytes:v.data() length:v.size() encoding:NSUTF8StringEncoding] ?: @"";
}
std::string str(NSString* s) {
  const char* c = s.UTF8String;
  return c ? std::string(c) : std::string();
}
NSColor* rgb(unsigned c, CGFloat a = 1) {
  return [NSColor colorWithSRGBRed:((c >> 16) & 255) / 255.0 green:((c >> 8) & 255) / 255.0 blue:(c & 255) / 255.0 alpha:a];
}

// Runs on the main thread, also while a modal dialog is open (see main.mm).
void on_main(void (^block)(void)) {
  CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, block);
  CFRunLoopWakeUp(CFRunLoopGetMain());
}
dispatch_queue_t background() { return dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0); }

NSFont* mono_font(CGFloat size) {
  if (@available(macOS 10.15, *)) return [NSFont monospacedSystemFontOfSize:size weight:NSFontWeightRegular];
  return [NSFont fontWithName:@"Menlo" size:size] ?: [NSFont systemFontOfSize:size];
}

unsigned severity_color(vc::Severity s) {
  switch (s) {
    case vc::Severity::Ok: return vc::color::kOk;
    case vc::Severity::Warn: return vc::color::kWarn;
    case vc::Severity::Error: return vc::color::kBad;
    default: return vc::color::kMuted;
  }
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
// A status line: red for errors, the normal colour otherwise.
void set_status(NSTextField* t, const std::string& s, bool error) {
  t.stringValue = ns(s);
  t.textColor = error ? rgb(vc::color::kBad) : [NSColor secondaryLabelColor];
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
// Footer: buttons on the left, others on the right.
NSStackView* footer(NSArray<NSView*>* leading, NSArray<NSView*>* trailing) {
  NSStackView* foot = [[NSStackView alloc] initWithFrame:NSZeroRect];
  foot.orientation = NSUserInterfaceLayoutOrientationHorizontal;
  foot.spacing = 8;
  for (NSView* v in leading) [foot addView:v inGravity:NSStackViewGravityLeading];
  for (NSView* v in trailing) [foot addView:v inGravity:NSStackViewGravityTrailing];
  return foot;
}

NSWindow* make_window(NSString* title, NSRect frame, id<NSWindowDelegate> delegate) {
  NSWindow* w = [[NSWindow alloc]
      initWithContentRect:frame
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                          NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  w.title = title;
  w.releasedWhenClosed = NO;
  w.delegate = delegate;
  w.autorecalculatesKeyViewLoop = YES;
  if (@available(macOS 10.14, *)) w.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
  return w;
}

void bring_forward(NSWindow* w) {
  [NSApp activateIgnoringOtherApps:YES];
  if (w.miniaturized) [w deminiaturize:nil];
  [w makeKeyAndOrderFront:nil];
}

}  // namespace

// ---- small views ------------------------------------------------------------------------

// A document view with y down, so scroll views start at the top.
@interface VWFlippedView : NSView
@end
@implementation VWFlippedView
- (BOOL)isFlipped {
  return YES;
}
@end

// The headline pill: rounded, tinted with the severity colour.
@interface VWPill : NSView
- (void)setText:(const std::string&)text color:(unsigned)rgbColor;
@end

@implementation VWPill {
  NSString* _text;
  unsigned _color;
}
- (instancetype)initWithFrame:(NSRect)f {
  if ((self = [super initWithFrame:f])) {
    _text = @"";
    _color = vc::color::kMuted;
  }
  return self;
}
- (NSDictionary*)attrs {
  return @{
    NSFontAttributeName : [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold],
    NSForegroundColorAttributeName : rgb(_color),
  };
}
- (void)setText:(const std::string&)text color:(unsigned)c {
  NSString* t = ns(text);
  if ([t isEqualToString:_text] && c == _color) return;
  _text = t;
  _color = c;
  [self invalidateIntrinsicContentSize];
  self.needsDisplay = YES;
}
- (NSSize)intrinsicContentSize {
  const NSSize s = [_text sizeWithAttributes:[self attrs]];
  return NSMakeSize(std::ceil(s.width) + 22, std::ceil(s.height) + 6);
}
- (void)drawRect:(NSRect)dirty {
  const NSRect b = self.bounds;
  const CGFloat r = b.size.height / 2;
  [rgb(_color, 0.18) setFill];
  [[NSBezierPath bezierPathWithRoundedRect:b xRadius:r yRadius:r] fill];
  [rgb(_color, 0.6) setStroke];
  NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(b, 0.5, 0.5) xRadius:r yRadius:r];
  p.lineWidth = 1;
  [p stroke];
  NSDictionary* a = [self attrs];
  const NSSize s = [_text sizeWithAttributes:a];
  [_text drawAtPoint:NSMakePoint((b.size.width - s.width) / 2, (b.size.height - s.height) / 2) withAttributes:a];
}
@end

// The per-minute chart of the last hour: stacked bars as diag_chart_layout
// places them (y down, hence flipped), a tooltip per minute.
@interface VWDiagChart : NSView <NSViewToolTipOwner>
- (void)setDiagnostics:(const vc::Diagnostics&)d;
@end

@implementation VWDiagChart {
  vc::Diagnostics _d;
}
- (BOOL)isFlipped {
  return YES;
}
- (void)setDiagnostics:(const vc::Diagnostics&)d {
  _d = d;
  self.needsDisplay = YES;
}
- (void)drawRect:(NSRect)dirty {
  const NSRect b = self.bounds;
  [rgb(vc::color::kPanel) setFill];
  [[NSBezierPath bezierPathWithRoundedRect:b xRadius:4 yRadius:4] fill];
  const float w = float(b.size.width), h = float(b.size.height);
  [rgb(vc::color::kLine) setFill];
  NSRectFill(NSMakeRect(0, h - 1, w, 1));
  const float top = 4;  // headroom above the tallest bar
  for (const vc::DiagBar& bar : vc::diag_chart_layout(_d, w, h - top)) {
    [rgb(vc::diag_category_color(bar.category)) setFill];
    NSRectFill(NSMakeRect(bar.x, bar.y + top, bar.w, bar.h));
  }
}
// One tooltip rectangle per minute; the text is looked up when it shows,
// so it follows the refreshes.
- (void)rebuildTips {
  [self removeAllToolTips];
  const CGFloat w = self.bounds.size.width, h = self.bounds.size.height;
  if (w <= 0 || h <= 0) return;
  const CGFloat step = w / vc::kDiagMinutes;
  for (int i = 0; i < vc::kDiagMinutes; ++i)
    [self addToolTipRect:NSMakeRect(step * i, 0, step, h) owner:self userData:(void*)(intptr_t)i];
}
- (void)setFrameSize:(NSSize)size {
  [super setFrameSize:size];
  [self rebuildTips];
}
- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  [self rebuildTips];
}
- (NSString*)view:(NSView*)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)point userData:(void*)data {
  int i = vc::diag_chart_minute_at(float(point.x), float(self.bounds.size.width));
  if (i < 0) i = int(intptr_t(data));
  if (i < 0 || size_t(i) >= _d.minutes.size()) return @"";
  return ns(_d.minutes[size_t(i)].tooltip);
}
@end

// ---- Dropouts and diagnostics -------------------------------------------------------------

@interface VWDiagnostics : NSObject <NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate>
- (instancetype)initWithPort:(unsigned)port;
- (void)open;
- (void)refresh;
@property(readonly) NSWindow* window;
@end

namespace {
VWDiagnostics* g_diagnostics = nil;  // the open window, if any
}

@implementation VWDiagnostics {
  unsigned _port;
  NSWindow* _w;
  VWPill* _pill;
  NSTextField *_since, *_hints, *_error;
  VWDiagChart* _chart;
  NSTableView* _table;
  NSButton *_save, *_clear;
  NSTimer* _timer;
  vc::Diagnostics _d;
  std::vector<vc::DiagEvent> _events;  // shown (at most kDiagShownEvents)
  std::string _events_key;
  bool _have, _loading, _closed;
}

- (instancetype)initWithPort:(unsigned)port {
  if ((self = [super init])) _port = port;
  return self;
}
- (NSWindow*)window {
  return _w;
}

- (void)open {
  _w = make_window(@"Dropouts and diagnostics", NSMakeRect(0, 0, 700, 560), self);
  _w.contentMinSize = NSMakeSize(680, 420);

  _pill = [[VWPill alloc] initWithFrame:NSZeroRect];
  [_pill setText:"Loading…" color:vc::color::kMuted];
  _since = dim(label(""));
  NSStackView* head = hstack(@[ _pill, _since ], 10);

  _chart = [[VWDiagChart alloc] initWithFrame:NSMakeRect(0, 0, 660, 64)];
  _chart.translatesAutoresizingMaskIntoConstraints = NO;
  [_chart.heightAnchor constraintEqualToConstant:64].active = YES;

  NSFont* small = [NSFont systemFontOfSize:11];
  NSMutableArray<NSView*>* legend = [NSMutableArray array];
  [legend addObject:dim(label("last 60 minutes, per minute:"))];
  ((NSTextField*)legend[0]).font = small;
  for (int c = 0; c < vc::kDiagCategories; ++c) {
    const vc::DiagCategory cat = vc::DiagCategory(c);
    NSMutableAttributedString* s = [[NSMutableAttributedString alloc] init];
    [s appendAttributedString:[[NSAttributedString alloc]
                                  initWithString:@"■ "
                                      attributes:@{
                                        NSFontAttributeName : small,
                                        NSForegroundColorAttributeName : rgb(vc::diag_category_color(cat)),
                                      }]];
    [s appendAttributedString:[[NSAttributedString alloc]
                                  initWithString:ns(vc::diag_category_label(cat))
                                      attributes:@{
                                        NSFontAttributeName : small,
                                        NSForegroundColorAttributeName : [NSColor secondaryLabelColor],
                                      }]];
    [legend addObject:[NSTextField labelWithAttributedString:s]];
  }
  NSStackView* legendRow = hstack(legend, 10);

  _hints = wrapping("", 640);
  _error = wrapping("", 640);
  _error.textColor = rgb(vc::color::kBad);

  NSTableView* table = _table = [[NSTableView alloc] initWithFrame:NSZeroRect];
  struct Col {
    NSString* key;
    NSString* title;
    CGFloat width;
  } cols[] = {{@"time", @"Time", 140}, {@"severity", @"", 18}, {@"text", @"What happened", 460}};
  for (const Col& c : cols) {
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:c.key];
    col.title = c.title;
    col.width = c.width;
    col.minWidth = c.width < 40 ? c.width : 40;
    if ([c.key isEqualToString:@"text"]) col.resizingMask = NSTableColumnAutoresizingMask;
    else col.resizingMask = NSTableColumnUserResizingMask;
    [table addTableColumn:col];
  }
  table.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
  table.rowHeight = 20;
  table.allowsMultipleSelection = NO;
  table.allowsEmptySelection = YES;
  table.usesAlternatingRowBackgroundColors = YES;
  table.dataSource = self;
  table.delegate = self;
  NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
  scroll.documentView = table;
  scroll.hasVerticalScroller = YES;
  scroll.borderType = NSBezelBorder;
  [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:140].active = YES;
  [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow - 50
                     forOrientation:NSLayoutConstraintOrientationVertical];

  _save = [NSButton buttonWithTitle:@"Save report…" target:self action:@selector(saveReport:)];
  _clear = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clear:)];
  NSButton* close = [NSButton buttonWithTitle:@"Close" target:self action:@selector(close:)];
  close.keyEquivalent = @"\033";
  NSStackView* foot = footer(@[ _save, _clear ], @[ close ]);

  NSStackView* v = vstack(@[ head, _chart, legendRow, _hints, scroll, _error, foot ], 10);
  v.distribution = NSStackViewDistributionFill;
  [v setCustomSpacing:4 afterView:_chart];
  for (NSView* full : @[ _chart, scroll, foot ]) [full.widthAnchor constraintEqualToAnchor:v.widthAnchor].active = YES;
  NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 700, 560)];
  pin(v, content, 16);
  _w.contentView = content;

  __weak VWDiagnostics* weak = self;
  _timer = [NSTimer timerWithTimeInterval:2.0
                                  repeats:YES
                                    block:^(NSTimer* t) {
                                      VWDiagnostics* me = weak;
                                      if (me && (me.window.occlusionState & NSWindowOcclusionStateVisible)) [me refresh];
                                    }];
  [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
  [_w center];
  bring_forward(_w);
  [self refresh];
}

- (void)refresh {
  if (_closed || _loading) return;
  _loading = true;
  const unsigned port = _port;
  VWDiagnostics* me = self;
  dispatch_async(background(), ^{
    auto d = std::make_shared<vc::Diagnostics>();
    std::string err;
    const bool ok = vc::fetch_diagnostics(port, d.get(), &err);
    on_main(^{
      [me loaded:(ok ? d : std::shared_ptr<vc::Diagnostics>()) error:err];
    });
  });
}

- (void)loaded:(std::shared_ptr<vc::Diagnostics>)d error:(const std::string&)err {
  _loading = false;
  if (_closed) return;
  if (!d) {
    set_status(_error, err, true);
    if (!_have) [_pill setText:"Not available" color:vc::color::kMuted];
    return;
  }
  _error.stringValue = @"";
  _d = *d;
  _have = true;
  [_pill setText:_d.headline color:severity_color(_d.headline_severity)];
  _since.stringValue = ns(_d.since_text);
  [_chart setDiagnostics:_d];
  std::string hints;
  for (const auto& h : _d.hints) hints += (hints.empty() ? "" : "\n") + std::string("• ") + h;
  _hints.stringValue = ns(hints);
  _hints.hidden = hints.empty();

  // The list only when it changed (keeps the selection and scrolling).
  std::vector<vc::DiagEvent> ev(_d.events.begin(),
                                _d.events.begin() + std::min(_d.events.size(), vc::kDiagShownEvents));
  std::string key;
  for (const auto& e : ev)
    key += e.time_text + '\x1f' + e.text + '\x1f' + e.duration_text + '\x1f' + char('0' + int(e.severity)) + '\x1e';
  if (key != _events_key || ev.empty() != _events.empty()) {
    _events_key = key;
    _events = std::move(ev);
    [_table reloadData];
  }
}

// ---- table ----
- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
  return _events.empty() ? 1 : NSInteger(_events.size());
}
- (NSView*)tableView:(NSTableView*)tableView viewForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
  NSString* c = column.identifier;
  NSTextField* t = label("");
  t.lineBreakMode = NSLineBreakByTruncatingTail;
  if (_events.empty()) {
    if ([c isEqualToString:@"text"]) {
      t.stringValue = _have ? @"Nothing yet." : @"Loading…";
      t.textColor = [NSColor secondaryLabelColor];
    }
    return t;
  }
  if (row < 0 || size_t(row) >= _events.size()) return t;
  const vc::DiagEvent& e = _events[size_t(row)];
  if ([c isEqualToString:@"time"]) {
    t.stringValue = ns(e.time_text);
    t.font = mono_font(11);
    t.textColor = [NSColor secondaryLabelColor];
  } else if ([c isEqualToString:@"severity"]) {
    t.stringValue = @"●";
    t.alignment = NSTextAlignmentCenter;
    t.textColor = rgb(severity_color(e.severity));
  } else {
    const std::string s = e.duration_text.empty() ? e.text : e.text + " (" + e.duration_text + ")";
    t.stringValue = ns(s);
    t.toolTip = ns(s);
  }
  return t;
}
- (BOOL)tableView:(NSTableView*)tableView shouldSelectRow:(NSInteger)row {
  return !_events.empty();
}

// ---- buttons ----
- (void)saveReport:(id)sender {
  _save.enabled = NO;
  const unsigned port = _port;
  VWDiagnostics* me = self;
  dispatch_async(background(), ^{
    std::string text, err;
    const bool ok = vc::fetch_report(port, &text, &err);
    on_main(^{
      [me reportFetched:ok text:text error:err];
    });
  });
}
- (void)reportFetched:(bool)ok text:(const std::string&)text error:(const std::string&)err {
  if (_closed) return;
  _save.enabled = YES;
  if (!ok) {
    set_status(_error, err, true);
    return;
  }
  NSSavePanel* panel = [NSSavePanel savePanel];
  panel.title = @"Save report";
  panel.nameFieldStringValue = ns(vc::default_report_file_name());
  panel.canCreateDirectories = YES;
  NSData* data = [NSData dataWithBytes:text.data() length:text.size()];
  NSTextField* errLabel = _error;
  __weak NSSavePanel* weakPanel = panel;
  [panel beginSheetModalForWindow:_w
                completionHandler:^(NSModalResponse r) {
                  NSURL* url = weakPanel.URL;
                  if (r != NSModalResponseOK || !url) return;
                  NSError* e = nil;
                  if (![data writeToURL:url options:NSDataWritingAtomic error:&e])
                    set_status(errLabel, "Could not save the report: " + str(e.localizedDescription ?: @"unknown error"),
                               true);
                  else
                    set_status(errLabel, "Report saved to " + str(url.path ?: @"") + ".", false);
                }];
}
- (void)clear:(id)sender {
  _clear.enabled = NO;
  const unsigned port = _port;
  VWDiagnostics* me = self;
  dispatch_async(background(), ^{
    std::string err;
    const bool ok = vc::reset_diagnostics(port, &err);
    on_main(^{
      [me cleared:ok error:err];
    });
  });
}
- (void)cleared:(bool)ok error:(const std::string&)err {
  if (_closed) return;
  _clear.enabled = YES;
  if (!ok) {
    set_status(_error, err, true);
    return;
  }
  [self refresh];
}
- (void)close:(id)sender {
  [_w performClose:nil];
}
- (void)windowWillClose:(NSNotification*)n {
  _closed = true;
  [_timer invalidate];
  _timer = nil;
  _table.dataSource = nil;
  _table.delegate = nil;
  _w.delegate = nil;
  VWDiagnostics* keep = g_diagnostics;
  if (g_diagnostics == self) g_diagnostics = nil;
  on_main(^{
    (void)keep;  // released later: AppKit is still inside the window's close
  });
}
@end

// ---- Channel names ------------------------------------------------------------------------

@interface VWChannelNames : NSObject <NSWindowDelegate, NSTextFieldDelegate>
- (instancetype)initWithPort:(unsigned)port live:(damac::ChannelSource)live;
- (void)open;
@property(readonly) NSWindow* window;
@end

namespace {
VWChannelNames* g_names = nil;

const char* const kNamesHelp =
    "Leave a field empty for the default (TX 1, RX 1, …). Names changed in Dante Controller appear here too. "
    "Up to 31 characters, unique per direction, without = . @ or \".";
}  // namespace

@implementation VWChannelNames {
  unsigned _port;
  damac::ChannelSource _live;
  NSWindow* _w;
  NSScrollView* _scroll[2];  // 0 = transmit, 1 = receive
  NSTextField* _caption[2];
  std::vector<NSTextField*> _fields[2], _errors[2];
  NSTextField* _status;
  NSButton* _save;
  vc::ChannelNames _base;  // as loaded: only changes from this are saved
  bool _loaded, _saving, _closed;
}

- (instancetype)initWithPort:(unsigned)port live:(damac::ChannelSource)live {
  if ((self = [super init])) {
    _port = port;
    _live = std::move(live);
  }
  return self;
}
- (NSWindow*)window {
  return _w;
}

- (NSScrollView*)makeScroll {
  NSScrollView* s = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 380, 360)];
  s.hasVerticalScroller = YES;
  s.hasHorizontalScroller = NO;
  s.autohidesScrollers = YES;
  s.borderType = NSBezelBorder;
  s.drawsBackground = NO;
  s.translatesAutoresizingMaskIntoConstraints = NO;
  [s.heightAnchor constraintGreaterThanOrEqualToConstant:200].active = YES;
  [s.widthAnchor constraintGreaterThanOrEqualToConstant:380].active = YES;
  [s setContentHuggingPriority:NSLayoutPriorityDefaultLow - 50 forOrientation:NSLayoutConstraintOrientationVertical];
  VWFlippedView* doc = [[VWFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 360, 40)];
  NSTextField* t = dim(label("Loading…"));
  t.frame = NSMakeRect(10, 10, 300, 20);
  [doc addSubview:t];
  s.documentView = doc;
  return s;
}

- (void)open {
  _w = make_window(@"Channel names", NSMakeRect(0, 0, 860, 600), self);
  _w.contentMinSize = NSMakeSize(860, 360);

  NSTextField* help = dim(wrapping(kNamesHelp, 780));
  for (int d = 0; d < 2; ++d) {
    _scroll[d] = [self makeScroll];
    _caption[d] = label(d == 0 ? "Transmit (to the network)" : "Receive (from the network)");
    _caption[d].font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
  }
  NSStackView* tx = vstack(@[ _caption[0], _scroll[0] ], 6);
  NSStackView* rx = vstack(@[ _caption[1], _scroll[1] ], 6);
  for (int d = 0; d < 2; ++d) {
    NSStackView* col = d == 0 ? tx : rx;
    col.distribution = NSStackViewDistributionFill;
    [_scroll[d].widthAnchor constraintEqualToAnchor:col.widthAnchor].active = YES;
  }
  NSStackView* grids = hstack(@[ tx, rx ], 16);
  grids.alignment = NSLayoutAttributeTop;
  grids.distribution = NSStackViewDistributionFillEqually;
  [grids setContentHuggingPriority:NSLayoutPriorityDefaultLow - 50
                    forOrientation:NSLayoutConstraintOrientationVertical];
  [tx.heightAnchor constraintEqualToAnchor:grids.heightAnchor].active = YES;
  [rx.heightAnchor constraintEqualToAnchor:grids.heightAnchor].active = YES;

  _status = wrapping("", 780);
  _save = [NSButton buttonWithTitle:@"Save" target:self action:@selector(save:)];
  _save.keyEquivalent = @"\r";
  _save.enabled = NO;
  NSButton* cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];
  cancel.keyEquivalent = @"\033";
  NSStackView* foot = footer(@[], @[ cancel, _save ]);

  NSStackView* v = vstack(@[ help, grids, _status, foot ], 10);
  v.distribution = NSStackViewDistributionFill;
  for (NSView* full : @[ grids, foot ]) [full.widthAnchor constraintEqualToAnchor:v.widthAnchor].active = YES;
  NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 860, 600)];
  pin(v, content, 16);
  _w.contentView = content;
  [_w center];
  bring_forward(_w);
  [self load];
}

// The configuration afresh (Dante Controller renames are written there).
- (void)load {
  const unsigned port = _port;
  VWChannelNames* me = self;
  dispatch_async(background(), ^{
    vc::ConfigText cfg;
    std::string err;
    const bool ok = vc::fetch_config(port, &cfg, &err);
    on_main(^{
      [me loaded:ok config:cfg.text error:err];
    });
  });
}

- (void)loaded:(bool)ok config:(const std::string&)conf error:(const std::string&)err {
  if (_closed) return;
  if (!ok) {
    set_status(_status, err, true);
    return;
  }
  _base = vc::load_channel_names(conf);
  damac::ChannelInfo live;
  if (_live) live = _live();
  [self buildGrid:true live:live.tx_names];
  [self buildGrid:false live:live.rx_names];
  _loaded = true;
  _status.stringValue = @"";
  [self validate];
  if (!_fields[0].empty()) [_w makeFirstResponder:_fields[0][0]];
  else if (!_fields[1].empty()) [_w makeFirstResponder:_fields[1][0]];
}

- (void)buildGrid:(bool)tx live:(const std::vector<std::string>&)live {
  const int d = tx ? 0 : 1;
  const unsigned n = tx ? _base.tx : _base.rx;
  const std::vector<std::string>& names = _base.names(tx);
  _fields[d].clear();
  _errors[d].clear();
  _caption[d].stringValue = ns(std::string(tx ? "Transmit (to the network)" : "Receive (from the network)") + ", " +
                               std::to_string(n) + " channel" + (n == 1 ? "" : "s"));
  VWFlippedView* doc = [[VWFlippedView alloc] initWithFrame:NSZeroRect];
  if (n == 0) {
    NSTextField* t = dim(label(tx ? "No transmit channels." : "No receive channels."));
    t.frame = NSMakeRect(10, 10, 300, 20);
    [doc addSubview:t];
    doc.frame = NSMakeRect(0, 0, 360, 40);
    _scroll[d].documentView = doc;
    return;
  }
  NSMutableArray<NSArray<NSView*>*>* rows = [NSMutableArray arrayWithCapacity:n];
  for (unsigned i = 0; i < n; ++i) {
    NSTextField* num = label(std::to_string(i + 1));
    num.font = mono_font(11);
    num.textColor = [NSColor secondaryLabelColor];
    num.alignment = NSTextAlignmentRight;
    NSTextField* f = [NSTextField textFieldWithString:ns(i < names.size() ? names[i] : std::string())];
    f.placeholderString = ns(vc::channel_name_placeholder(live, tx, i));
    f.delegate = self;
    f.lineBreakMode = NSLineBreakByClipping;
    f.cell.scrollable = YES;
    f.cell.wraps = NO;
    [f.widthAnchor constraintEqualToConstant:170].active = YES;
    NSTextField* e = label("");
    e.font = [NSFont systemFontOfSize:11];
    e.textColor = rgb(vc::color::kBad);
    e.lineBreakMode = NSLineBreakByTruncatingTail;
    [e.widthAnchor constraintEqualToConstant:130].active = YES;
    _fields[d].push_back(f);
    _errors[d].push_back(e);
    [rows addObject:@[ num, f, e ]];
  }
  NSGridView* g = [NSGridView gridViewWithViews:rows];
  g.rowSpacing = 6;
  g.columnSpacing = 8;
  g.rowAlignment = NSGridRowAlignmentFirstBaseline;
  [g columnAtIndex:0].width = 28;
  [g columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
  g.translatesAutoresizingMaskIntoConstraints = NO;
  [doc addSubview:g];
  [g.leadingAnchor constraintEqualToAnchor:doc.leadingAnchor constant:8].active = YES;
  [g.topAnchor constraintEqualToAnchor:doc.topAnchor constant:8].active = YES;
  const NSSize fit = g.fittingSize;
  doc.frame = NSMakeRect(0, 0, std::ceil(fit.width) + 16, std::ceil(fit.height) + 16);
  _scroll[d].documentView = doc;
  [doc scrollRectToVisible:NSMakeRect(0, 0, 1, 1)];
}

- (vc::ChannelNames)edited {
  vc::ChannelNames e = _base;
  for (int d = 0; d < 2; ++d) {
    auto& v = e.names(d == 0);
    v.resize(d == 0 ? e.tx : e.rx);
    for (size_t i = 0; i < _fields[d].size() && i < v.size(); ++i) v[i] = str(_fields[d][i].stringValue);
  }
  return e;
}

- (void)validate {
  if (!_loaded) return;
  const auto errors = vc::validate_channel_names([self edited]);
  for (int d = 0; d < 2; ++d)
    for (size_t i = 0; i < _errors[d].size(); ++i) {
      NSString* m = ns(vc::name_error(errors, d == 0, unsigned(i)));
      if (![_errors[d][i].stringValue isEqualToString:m]) {
        _errors[d][i].stringValue = m;
        _errors[d][i].toolTip = m.length ? m : nil;
      }
    }
  _save.enabled = !_saving && errors.empty();
  if (!_saving) {
    if (errors.empty()) _status.stringValue = @"";
    else set_status(_status, "Correct the names marked in red to save.", true);
  }
}

- (void)controlTextDidChange:(NSNotification*)n {
  [self validate];
}

- (void)save:(id)sender {
  if (!_loaded || _saving) return;
  const vc::ChannelNames edited = [self edited];
  if (!vc::validate_channel_names(edited).empty()) return;
  _saving = true;
  _save.enabled = NO;
  set_status(_status, "Saving…", false);
  const unsigned port = _port;
  const vc::ChannelNames base = _base;
  VWChannelNames* me = self;
  dispatch_async(background(), ^{
    std::string err;
    const bool ok = vc::save_channel_names(port, base, edited, &err);
    on_main(^{
      [me saved:ok error:err];
    });
  });
}
- (void)saved:(bool)ok error:(const std::string&)err {
  _saving = false;
  if (_closed) return;
  if (ok) {
    [_w performClose:nil];
    return;
  }
  set_status(_status, err, true);
  _save.enabled = YES;
}
- (void)cancel:(id)sender {
  [_w performClose:nil];
}
- (void)windowWillClose:(NSNotification*)n {
  _closed = true;
  for (int d = 0; d < 2; ++d)
    for (NSTextField* f : _fields[d]) f.delegate = nil;
  _w.delegate = nil;
  VWChannelNames* keep = g_names;
  if (g_names == self) g_names = nil;
  on_main(^{
    (void)keep;
  });
}
@end

// ---- Edit configuration -------------------------------------------------------------------

@interface VWConfigEditor : NSObject <NSWindowDelegate>
- (instancetype)initWithPort:(unsigned)port;
- (void)open;
@property(readonly) NSWindow* window;
@end

namespace {
VWConfigEditor* g_config = nil;
}

@implementation VWConfigEditor {
  unsigned _port;
  NSWindow* _w;
  NSTextView* _text;
  NSTextField* _status;
  NSButton *_apply, *_reload;
  std::string _version;  // X-Virgil-Config-Version of the loaded text
  bool _busy, _closed;
}

- (instancetype)initWithPort:(unsigned)port {
  if ((self = [super init])) _port = port;
  return self;
}
- (NSWindow*)window {
  return _w;
}

- (void)open {
  _w = make_window(@"Edit configuration", NSMakeRect(0, 0, 760, 600), self);
  _w.contentMinSize = NSMakeSize(600, 320);

  NSTextField* note = dim(label("See virgil.conf.example for every option."));

  NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 728, 460)];
  scroll.hasVerticalScroller = YES;
  scroll.hasHorizontalScroller = YES;
  scroll.autohidesScrollers = YES;
  scroll.borderType = NSBezelBorder;
  const NSSize cs = scroll.contentSize;
  NSTextView* tv = _text = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, cs.width, cs.height)];
  tv.minSize = NSMakeSize(0, cs.height);
  tv.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
  tv.verticallyResizable = YES;
  tv.horizontallyResizable = YES;  // long lines scroll instead of wrapping
  tv.autoresizingMask = NSViewWidthSizable;
  tv.textContainer.containerSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
  tv.textContainer.widthTracksTextView = NO;
  tv.richText = NO;
  tv.importsGraphics = NO;
  tv.allowsUndo = YES;
  tv.usesFindBar = YES;
  tv.incrementalSearchingEnabled = YES;
  // Plain text: no "smart" changes to quotes, dashes and the like.
  tv.automaticQuoteSubstitutionEnabled = NO;
  tv.automaticDashSubstitutionEnabled = NO;
  tv.automaticTextReplacementEnabled = NO;
  tv.automaticSpellingCorrectionEnabled = NO;
  tv.automaticLinkDetectionEnabled = NO;
  tv.automaticDataDetectionEnabled = NO;
  tv.continuousSpellCheckingEnabled = NO;
  tv.grammarCheckingEnabled = NO;
  tv.smartInsertDeleteEnabled = NO;
  tv.font = mono_font(12);
  tv.textColor = [NSColor textColor];
  tv.backgroundColor = [NSColor textBackgroundColor];
  tv.editable = NO;  // until loaded
  scroll.documentView = tv;
  [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:160].active = YES;
  [scroll setContentHuggingPriority:NSLayoutPriorityDefaultLow - 50
                     forOrientation:NSLayoutConstraintOrientationVertical];

  _status = wrapping("Loading…", 560);
  _status.textColor = [NSColor secondaryLabelColor];
  _apply = [NSButton buttonWithTitle:@"Apply & restart" target:self action:@selector(apply:)];
  _reload = [NSButton buttonWithTitle:@"Reload" target:self action:@selector(reload:)];
  NSButton* close = [NSButton buttonWithTitle:@"Close" target:self action:@selector(close:)];
  _apply.enabled = NO;
  NSStackView* foot = footer(@[ _reload ], @[ close, _apply ]);

  NSStackView* v = vstack(@[ note, scroll, _status, foot ], 10);
  v.distribution = NSStackViewDistributionFill;
  for (NSView* full : @[ scroll, foot ]) [full.widthAnchor constraintEqualToAnchor:v.widthAnchor].active = YES;
  NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 760, 600)];
  pin(v, content, 16);
  _w.contentView = content;
  [_w center];
  bring_forward(_w);
  [self loadKeepingStatus:NO];
}

- (void)setBusy:(bool)busy {
  _busy = busy;
  _apply.enabled = !busy && _text.editable;
  _reload.enabled = !busy;
}

- (void)loadKeepingStatus:(BOOL)keep {
  if (_busy) return;
  [self setBusy:true];
  if (!keep) set_status(_status, "Loading…", false);
  const unsigned port = _port;
  VWConfigEditor* me = self;
  dispatch_async(background(), ^{
    vc::ConfigText cfg;
    std::string err;
    const bool ok = vc::fetch_config(port, &cfg, &err);
    on_main(^{
      [me loaded:ok config:cfg error:err keepStatus:keep];
    });
  });
}

- (void)loaded:(bool)ok config:(const vc::ConfigText&)cfg error:(const std::string&)err keepStatus:(BOOL)keep {
  if (_closed) return;
  if (!ok) {
    [self setBusy:false];
    set_status(_status, err, true);
    return;
  }
  _version = cfg.version;
  _text.editable = YES;
  _text.string = ns(cfg.text);
  _text.font = mono_font(12);
  [_text.undoManager removeAllActions];
  [self setBusy:false];
  if (!keep) _status.stringValue = @"";
}

- (void)apply:(id)sender {
  if (_busy) return;
  [self setBusy:true];
  set_status(_status, "Saving…", false);
  const std::string text = str(_text.string);
  const std::string version = _version;
  const unsigned port = _port;
  VWConfigEditor* me = self;
  dispatch_async(background(), ^{
    const vc::SaveResult r = vc::save_config_text(port, text, version);
    on_main(^{
      [me saved:r];
    });
  });
}

- (void)saved:(const vc::SaveResult&)r {
  if (_closed) return;
  [self setBusy:false];
  if (r.ok) {
    set_status(_status, vc::saved_message(r), false);
    [self loadKeepingStatus:YES];  // the new version (and what virgild made of it)
    return;
  }
  if (r.conflict) {
    set_status(_status, "Not saved: the configuration was changed elsewhere meanwhile.", true);
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = @"The configuration was changed elsewhere";
    a.informativeText = @"Since you opened it, the configuration was changed (by Stop, a rename in Dante "
                        @"Controller or another editor). Reload it and make your changes again?";
    [a addButtonWithTitle:@"Reload"];
    [a addButtonWithTitle:@"Keep editing"];
    VWConfigEditor* me = self;
    [a beginSheetModalForWindow:_w
              completionHandler:^(NSModalResponse resp) {
                if (resp == NSAlertFirstButtonReturn) [me loadKeepingStatus:NO];
              }];
    return;
  }
  set_status(_status, r.error, true);
}

- (void)reload:(id)sender {
  [self loadKeepingStatus:NO];
}
- (void)close:(id)sender {
  [_w performClose:nil];
}
- (void)windowWillClose:(NSNotification*)n {
  _closed = true;
  _w.delegate = nil;
  VWConfigEditor* keep = g_config;
  if (g_config == self) g_config = nil;
  on_main(^{
    (void)keep;
  });
}
@end

// ---- entry points -------------------------------------------------------------------------

namespace vcmac {

void show_diagnostics(unsigned port) {
  if (g_diagnostics) {
    bring_forward(g_diagnostics.window);
    [g_diagnostics refresh];
    return;
  }
  g_diagnostics = [[VWDiagnostics alloc] initWithPort:port];
  [g_diagnostics open];
}

void show_channel_names(unsigned port, damac::ChannelSource live) {
  if (g_names) {
    bring_forward(g_names.window);
    return;
  }
  g_names = [[VWChannelNames alloc] initWithPort:port live:std::move(live)];
  [g_names open];
}

void show_config_editor(unsigned port) {
  if (g_config) {
    bring_forward(g_config.window);
    return;
  }
  g_config = [[VWConfigEditor alloc] initWithPort:port];
  [g_config open];
}

}  // namespace vcmac
