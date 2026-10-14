// Desktop audio backend for macOS: Core Audio, no driver.
//
//   output device   a process tap of that device's output (all processes),
//                   macOS 14.2 or later
//   one app         a stereo mixdown tap of the app's Core Audio processes
//                   (its helpers too, e.g. a browser's audio process)
//   all apps        a stereo global tap of every process except this one and
//                   those that play on a Virgil device (no echo into Dante)
//   interface inputs  an IOProc on a recording device (an audio interface),
//                   its input buffers: inputs first.. picked out across them
//                   (interleaved or not), a small I/O buffer (128 frames
//                   asked for). Any macOS version; needs the microphone
//                   permission (NSMicrophoneUsageDescription), asked for the
//                   first time from the main thread.
//   output          an IOProc on the output device (or the default one)
//   interface outputs the same, receive channels to the outputs first..
//                   (across the device's buffers), silence on the others
//
// Every tap is private, leaves the sound audible (CATapUnmuted) and is read
// through a private aggregate device whose clock is the tapped (or the
// default) output device; the aggregate and the tap go away with this
// process if it crashes. Taps need the "system audio recording" permission
// (NSAudioCaptureUsageDescription in the app's Info.plist): the system asks
// the first time, and a refused permission gives silence, not an error, so
// a tap that stays silent while its apps play says so in its status.
//
// IOProcs get float32 at the device's nominal rate; the engine's
// AdaptiveBridge converts to Virgil's rate and follows the drift.
#import <AppKit/AppKit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/HostTime.h>
#if __has_include(<CoreAudio/CATapDescription.h>)
#import <CoreAudio/CATapDescription.h>
#if __has_include(<CoreAudio/AudioHardwareTapping.h>)
#include <CoreAudio/AudioHardwareTapping.h>  // AudioHardwareCreateProcessTap
#endif
#define VIRGIL_HAVE_TAPS 1
#else
#define VIRGIL_HAVE_TAPS 0  // SDK older than macOS 14.2: output only
#endif
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <libproc.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "desktop_audio_engine.h"
#include "virgil/platform.h"

// Keys of AudioHardware.h (macOS 14.2 SDK), in case an SDK lacks one.
#ifndef kAudioAggregateDeviceTapListKey
#define kAudioAggregateDeviceTapListKey "taps"
#endif
#ifndef kAudioAggregateDeviceTapAutoStartKey
#define kAudioAggregateDeviceTapAutoStartKey "tapautostart"
#endif
#ifndef kAudioSubTapUIDKey
#define kAudioSubTapUIDKey "uid"
#endif
#ifndef kAudioSubTapDriftCompensationKey
#define kAudioSubTapDriftCompensationKey "drift"
#endif
#ifndef kAudioAggregateDeviceMainSubDeviceKey
#define kAudioAggregateDeviceMainSubDeviceKey "master"
#endif

// AVCaptureDevice's class methods used for the microphone permission
// (AVFoundation is loaded at run time, not linked).
@protocol VirgilCaptureAuthorization
+ (NSInteger)authorizationStatusForMediaType:(NSString*)mediaType;
+ (void)requestAccessForMediaType:(NSString*)mediaType completionHandler:(void (^)(BOOL granted))handler;
@end

namespace desk {

namespace {

constexpr AudioObjectPropertyElement kMain = 0;  // kAudioObjectPropertyElementMain (macOS 12+)
constexpr size_t kChunk = 1024;                  // frames per engine call
const char* const kOwnPrefix = "org.virgil.control.desktop-audio.";  // our aggregates' UIDs
const char* const kNeedsTaps = "Capturing other apps' audio needs macOS 14.2 or later";
const char* const kNeedsTapsDevice = "Capturing an output device needs macOS 14.2 or later";
const char* const kPermission =
    "Virgil Control may not be allowed to record audio: see System Settings › Privacy & Security › "
    "Screen & System Audio Recording";
const char* const kSilentNote =
    "Silent while the app plays: allow Virgil Control in System Settings › Privacy & Security › "
    "Screen & System Audio Recording (System Audio Recording Only), then turn the source off and on";

const char* const kMicDenied =
    "Virgil Control may not use audio inputs: allow it in System Settings › Privacy & Security › Microphone, "
    "then turn the source off and on";
const char* const kMicAsking = "Waiting for permission to use audio inputs: answer the macOS dialog";
const char* const kMicSilent =
    "Silent: Virgil Control may not be allowed to use audio inputs (System Settings › Privacy & Security › "
    "Microphone)";
constexpr UInt32 kLowLatencyFrames = 128;  // I/O buffer asked for interface inputs / outputs

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string os_text(OSStatus e) {
  const uint32_t u = uint32_t(e);
  const char c[4] = {char(u >> 24), char(u >> 16), char(u >> 8), char(u)};
  bool printable = true;
  for (char ch : c) printable = printable && ch >= 32 && ch < 127;
  return printable ? "'" + std::string(c, 4) + "'" : std::to_string(int(e));
}

AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel,
                                AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
  AudioObjectPropertyAddress a;
  a.mSelector = sel;
  a.mScope = scope;
  a.mElement = kMain;
  return a;
}

template <class T>
bool get(AudioObjectID obj, AudioObjectPropertySelector sel, T* out,
         AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal, UInt32 qsize = 0,
         const void* qual = nullptr) {
  const AudioObjectPropertyAddress a = addr(sel, scope);
  UInt32 size = sizeof(T);
  return AudioObjectGetPropertyData(obj, &a, qsize, qual, &size, out) == noErr && size == sizeof(T);
}

std::vector<AudioObjectID> get_ids(AudioObjectID obj, AudioObjectPropertySelector sel,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
  const AudioObjectPropertyAddress a = addr(sel, scope);
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(obj, &a, 0, nullptr, &size) != noErr || size < sizeof(AudioObjectID))
    return {};
  std::vector<AudioObjectID> v(size / sizeof(AudioObjectID));
  if (AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, v.data()) != noErr) return {};
  v.resize(size / sizeof(AudioObjectID));
  return v;
}

std::string cfstr(CFStringRef s) {
  if (!s) return std::string();
  if (const char* fast = CFStringGetCStringPtr(s, kCFStringEncodingUTF8)) return fast;
  const CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
  std::vector<char> buf(size_t(std::max<CFIndex>(max, 1)), '\0');
  if (!CFStringGetCString(s, buf.data(), CFIndex(buf.size()), kCFStringEncodingUTF8)) return std::string();
  return std::string(buf.data());
}

// A CFString property (the caller of AudioObjectGetPropertyData owns it).
std::string get_string(AudioObjectID obj, AudioObjectPropertySelector sel) {
  CFStringRef s = nullptr;
  if (!get(obj, sel, &s) || !s) return std::string();
  const std::string r = cfstr(s);
  CFRelease(s);
  return r;
}

NSString* ns(const std::string& s) { return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @""; }

// Channels (and streams) of a device in one direction.
unsigned channel_count(AudioObjectID dev, AudioObjectPropertyScope scope, unsigned* streams = nullptr) {
  if (streams) *streams = 0;
  const AudioObjectPropertyAddress a = addr(kAudioDevicePropertyStreamConfiguration, scope);
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size < offsetof(AudioBufferList, mBuffers))
    return 0;
  std::vector<uint64_t> mem((size + 7) / 8 + 1, 0);
  if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, mem.data()) != noErr) return 0;
  const AudioBufferList* l = reinterpret_cast<const AudioBufferList*>(mem.data());
  const AudioBuffer* b = l->mBuffers;
  unsigned n = 0;
  for (UInt32 i = 0; i < l->mNumberBuffers; ++i) n += b[i].mNumberChannels;
  if (streams) *streams = l->mNumberBuffers;
  return n;
}

struct OutDev {  // an output device, or (list_inputs) a recording device
  AudioObjectID obj = kAudioObjectUnknown;
  std::string uid, name;
  unsigned channels = 0;  // in the listed direction
};

bool own_device(const std::string& uid) { return uid.compare(0, std::strlen(kOwnPrefix), kOwnPrefix) == 0; }

// Devices that can play (our own aggregates left out).
std::vector<OutDev> list_outputs() {
  std::vector<OutDev> r;
  for (AudioObjectID dev : get_ids(kAudioObjectSystemObject, kAudioHardwarePropertyDevices)) {
    OutDev d;
    d.obj = dev;
    d.uid = get_string(dev, kAudioDevicePropertyDeviceUID);
    if (d.uid.empty() || own_device(d.uid)) continue;
    d.channels = channel_count(dev, kAudioObjectPropertyScopeOutput);
    if (d.channels == 0) continue;
    d.name = get_string(dev, kAudioObjectPropertyName);
    if (d.name.empty()) d.name = d.uid;
    r.push_back(d);
  }
  return r;
}

// Devices that can record (audio interfaces, microphones; our own aggregates left out).
std::vector<OutDev> list_inputs() {
  std::vector<OutDev> r;
  for (AudioObjectID dev : get_ids(kAudioObjectSystemObject, kAudioHardwarePropertyDevices)) {
    OutDev d;
    d.obj = dev;
    d.uid = get_string(dev, kAudioDevicePropertyDeviceUID);
    if (d.uid.empty() || own_device(d.uid)) continue;
    d.channels = channel_count(dev, kAudioObjectPropertyScopeInput);
    if (d.channels == 0) continue;
    d.name = get_string(dev, kAudioObjectPropertyName);
    if (d.name.empty()) d.name = d.uid;
    r.push_back(d);
  }
  return r;
}

AudioObjectID default_output_id() {
  AudioObjectID d = kAudioObjectUnknown;
  if (!get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, &d)) return kAudioObjectUnknown;
  return d;
}

AudioObjectID default_input_id() {
  AudioObjectID d = kAudioObjectUnknown;
  if (!get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice, &d)) return kAudioObjectUnknown;
  return d;
}

// "default" / "" = the system's default input; else a device UID.
bool find_input(const std::string& device, OutDev* out, std::string* error) {
  const bool def = device.empty() || device == "default";
  const AudioObjectID want = def ? default_input_id() : kAudioObjectUnknown;
  for (const OutDev& d : list_inputs())
    if (def ? d.obj == want : d.uid == device) {
      *out = d;
      return true;
    }
  if (error) *error = def ? "There is no input device" : "The device is not connected";
  return false;
}

// Every stream of the device in that direction takes 32-bit float (the
// IOProc's buffers are in the streams' virtual formats).
bool float_streams(AudioObjectID dev, AudioObjectPropertyScope scope) {
  for (AudioObjectID st : get_ids(dev, kAudioDevicePropertyStreams, scope)) {
    AudioStreamBasicDescription f;
    std::memset(&f, 0, sizeof f);
    if (!get(st, kAudioStreamPropertyVirtualFormat, &f)) continue;
    if (f.mFormatID != kAudioFormatLinearPCM || !(f.mFormatFlags & kAudioFormatFlagIsFloat) || f.mBitsPerChannel != 32)
      return false;
  }
  return true;
}

// Asks for a small I/O buffer for this process's IOProcs on the device (the
// HAL keeps other apps' own sizes; within the device's range only). Returns
// the size in effect afterwards (0 = unknown).
UInt32 request_buffer_frames(AudioObjectID dev, UInt32 want) {
  UInt32 cur = 0;
  get(dev, kAudioDevicePropertyBufferFrameSize, &cur);
  AudioValueRange range = {0, 0};
  if (get(dev, kAudioDevicePropertyBufferFrameSizeRange, &range) && range.mMaximum >= range.mMinimum) {
    want = UInt32(std::max<Float64>(range.mMinimum, std::min<Float64>(range.mMaximum, want)));
  }
  if (cur == 0 || cur > want) {
    const AudioObjectPropertyAddress a = addr(kAudioDevicePropertyBufferFrameSize);
    Boolean settable = false;
    if (AudioObjectIsPropertySettable(dev, &a, &settable) == noErr && settable)
      AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof want, &want);
    get(dev, kAudioDevicePropertyBufferFrameSize, &cur);
  }
  return cur;
}

// ---- microphone permission (TCC), through AVFoundation loaded at run time ----------------

enum MicStatus {  // AVAuthorizationStatus
  kMicStatusUnknown = -1,
  kMicStatusNotDetermined = 0,
  kMicStatusRestricted = 1,
  kMicStatusDenied = 2,
  kMicStatusAuthorized = 3,
};

Class mic_class() {
  static Class cls = [] {
    dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_LAZY | RTLD_LOCAL);
    Class c = NSClassFromString(@"AVCaptureDevice");
    return c && [c respondsToSelector:@selector(authorizationStatusForMediaType:)] ? c : Nil;
  }();
  return cls;
}

MicStatus mic_status() {
  Class cls = mic_class();
  if (!cls) return kMicStatusUnknown;
  // AVMediaTypeAudio is @"soun".
  const NSInteger st = [(Class<VirgilCaptureAuthorization>)cls authorizationStatusForMediaType:@"soun"];
  return st >= 0 && st <= 3 ? MicStatus(st) : kMicStatusUnknown;
}

// The app's Info.plist says why (without that, asking would end the process).
bool can_ask_mic() {
  @autoreleasepool {
    return [[NSBundle mainBundle] objectForInfoDictionaryKey:@"NSMicrophoneUsageDescription"] != nil;
  }
}

// Shows the system's question once (from the main thread); `answered` runs when it is answered.
void ask_mic(void (^answered)(void)) {
  static std::atomic<bool> asked{false};
  if (asked.exchange(true)) return;
  Class cls = mic_class();
  if (!cls || ![cls respondsToSelector:@selector(requestAccessForMediaType:completionHandler:)]) return;
  dispatch_async(dispatch_get_main_queue(), ^{
    [(Class<VirgilCaptureAuthorization>)cls requestAccessForMediaType:@"soun"
                                                    completionHandler:^(BOOL granted) {
                                                      (void)granted;
                                                      if (answered) answered();
                                                    }];
  });
}

// "default" / "" = the system's default output; else a device UID.
bool find_output(const std::string& device, OutDev* out, std::string* error) {
  const bool def = device.empty() || device == "default";
  const AudioObjectID want = def ? default_output_id() : kAudioObjectUnknown;
  for (const OutDev& d : list_outputs())
    if (def ? d.obj == want : d.uid == device) {
      *out = d;
      return true;
    }
  if (error) *error = def ? "There is no output device" : "The device is not connected";
  return false;
}

unsigned nominal_rate(AudioObjectID dev) {
  Float64 r = 0;
  if (!get(dev, kAudioDevicePropertyNominalSampleRate, &r) || !(r >= 8000 && r <= 768000)) return 0;
  return unsigned(r + 0.5);
}

uint64_t fnv(uint64_t h, const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

// ---- processes (macOS 14.2+) ----------------------------------------------------------------

struct Proc {
  AudioObjectID obj = kAudioObjectUnknown;
  pid_t pid = -1;
  std::string bundle;  // the HAL's bundle ID of the process (may be a helper's)
  bool output = false;  // playing right now
  std::vector<AudioObjectID> devices;  // output devices it uses (when asked for)
};

// Who a process belongs to: the app responsible for it (a browser for its
// audio helper), by bundle ID, else its own bundle ID or program name.
struct Ident {
  std::string key, name, exe;
};

using ResponsibleFn = pid_t (*)(pid_t);
ResponsibleFn responsible_fn() {
  // Not public API, so looked up at run time; without it helpers are listed
  // under their own names.
  static const ResponsibleFn fn =
      reinterpret_cast<ResponsibleFn>(dlsym(RTLD_DEFAULT, "responsibility_get_pid_responsible_for_pid"));
  return fn;
}

std::string exe_name(pid_t pid) {
  char buf[256] = {};
  if (proc_name(pid, buf, sizeof buf) <= 0) return std::string();
  return buf;
}

Ident identify(pid_t pid, const std::string& hal_bundle) {
  Ident who;
  who.exe = exe_name(pid);
  @autoreleasepool {
    pid_t owner = pid;
    if (ResponsibleFn fn = responsible_fn()) {
      const pid_t r = fn(pid);
      if (r > 0) owner = r;
    }
    NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:owner];
    if ((!app || !app.bundleIdentifier) && owner != pid)
      app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
    if (app && app.bundleIdentifier) {
      who.key = app.bundleIdentifier.UTF8String ?: "";
      who.name = app.localizedName.UTF8String ?: "";
    }
  }
  if (who.key.empty()) who.key = !hal_bundle.empty() ? hal_bundle : who.exe;
  if (who.name.empty()) who.name = !who.exe.empty() ? who.exe : who.key;
  return who;
}

#if VIRGIL_HAVE_TAPS
API_AVAILABLE(macos(14.2))
std::vector<Proc> list_processes(bool with_devices) {
  std::vector<Proc> r;
  for (AudioObjectID obj : get_ids(kAudioObjectSystemObject, kAudioHardwarePropertyProcessObjectList)) {
    Proc p;
    p.obj = obj;
    if (!get(obj, kAudioProcessPropertyPID, &p.pid)) continue;
    p.bundle = get_string(obj, kAudioProcessPropertyBundleID);
    UInt32 running = 0;
    if (get(obj, kAudioProcessPropertyIsRunningOutput, &running)) p.output = running != 0;
    if (with_devices) p.devices = get_ids(obj, kAudioProcessPropertyDevices, kAudioObjectPropertyScopeOutput);
    r.push_back(std::move(p));
  }
  return r;
}

API_AVAILABLE(macos(14.2))
AudioObjectID process_object(pid_t pid) {
  AudioObjectID obj = kAudioObjectUnknown;
  if (!get(kAudioObjectSystemObject, kAudioHardwarePropertyTranslatePIDToProcessObject, &obj,
           kAudioObjectPropertyScopeGlobal, sizeof(pid), &pid))
    return kAudioObjectUnknown;
  return obj;
}
#endif

// ---- streams --------------------------------------------------------------------------------

class CaStream : public Stream {
 public:
  ~CaStream() override { close_io(); }

  bool start(std::string* error) override {
    if (io_dev_ == kAudioObjectUnknown || !proc_) {
      if (error) *error = "Cannot start the stream";
      return false;
    }
    started_ns_.store(virgil::mono_ns(), std::memory_order_relaxed);
    last_cb_ns_.store(0, std::memory_order_relaxed);
    const OSStatus e = AudioDeviceStart(io_dev_, proc_);
    if (e != noErr) {
      if (error) *error = start_error(e);
      return false;
    }
    started_.store(true, std::memory_order_release);
    return true;
  }

  bool failed(std::string* why) const override {
    check();
    if (!failed_.load(std::memory_order_acquire)) return false;
    if (why) {
      std::lock_guard<std::mutex> l(mu_);
      *why = why_;
    }
    return true;
  }
  unsigned rate() const override { return rate_; }
  std::string device() const override { return device_uid_; }
  double period_s() const override { return period_frames_ && rate_ ? double(period_frames_) / rate_ : 0; }

 protected:
  virtual std::string start_error(OSStatus e) const { return "The device does not start (error " + os_text(e) + ")"; }

  // The IOProc is not called once this returns (AudioDeviceStop is
  // synchronous outside the I/O thread; the counter is for good measure).
  void close_io() {
    stopping_.store(true, std::memory_order_seq_cst);
    if (io_dev_ != kAudioObjectUnknown && proc_) {
      if (started_.load(std::memory_order_acquire)) AudioDeviceStop(io_dev_, proc_);
      AudioDeviceDestroyIOProcID(io_dev_, proc_);
    }
    proc_ = nullptr;
    started_.store(false, std::memory_order_release);
    while (in_cb_.load(std::memory_order_seq_cst) > 0) std::this_thread::yield();
  }

  void fail(const std::string& why) const {
    {
      std::lock_guard<std::mutex> l(mu_);
      if (why_.empty()) why_ = why;
    }
    failed_.store(true, std::memory_order_release);
  }

  // The engine polls failed() every 50 ms; the device is asked 5 times a second.
  void check() const {
    if (failed_.load(std::memory_order_acquire)) return;
    const int64_t now = virgil::mono_ns();
    if (now < next_check_.load(std::memory_order_relaxed)) return;
    next_check_.store(now + 200000000LL, std::memory_order_relaxed);
    UInt32 alive = 0;
    if (watch_dev_ != kAudioObjectUnknown && (!get(watch_dev_, kAudioDevicePropertyDeviceIsAlive, &alive) || !alive))
      return fail("The device went away");
    const unsigned r = nominal_rate(io_dev_);
    if (r && r != rate_) return fail("The device's sample rate changed");
    if (started_.load(std::memory_order_acquire)) {
      const int64_t last = last_cb_ns_.load(std::memory_order_relaxed);
      // Some devices (Bluetooth, AirPlay) take a few seconds to start.
      if (!last && now - started_ns_.load(std::memory_order_relaxed) > 8000000000LL)
        return fail("The device does not start");
      if (last && now - last > 3000000000LL) return fail("The device stopped");
    }
  }

  // Set up by the subclass before start().
  AudioObjectID io_dev_ = kAudioObjectUnknown;     // the IOProc's device
  AudioObjectID watch_dev_ = kAudioObjectUnknown;  // the real device behind it
  AudioDeviceIOProcID proc_ = nullptr;
  unsigned rate_ = 48000, ch_ = 2;
  unsigned first_ = 0;           // interface inputs / outputs: the device's first channel used (0-based)
  UInt32 period_frames_ = 0;     // the I/O buffer, when it was set
  std::string device_uid_;
  std::vector<float> buf_;

  std::atomic<bool> started_{false}, stopping_{false};
  std::atomic<int> in_cb_{0};
  std::atomic<int64_t> started_ns_{0}, last_cb_ns_{0};

 private:
  mutable std::atomic<bool> failed_{false};
  mutable std::atomic<int64_t> next_check_{0};
  mutable std::mutex mu_;
  mutable std::string why_;
};

// ---- render: receive channels -> an output device ---------------------------------------------

class RenderStream : public CaStream {
 public:
  explicit RenderStream(AudioSource* src) : src_(src) {}
  ~RenderStream() override { close_io(); }

  // first_channel = 0: a pair (or one on both sides) on the device's first
  // two channels; >= 1: `channels` channels to the outputs first_channel..
  bool open(const OutDev& d, unsigned first_channel, unsigned channels, std::string* error) {
    iface_ = first_channel > 0;
    ch_ = iface_ ? std::max(1u, std::min(vc::kMaxInterfaceInputs, channels)) : std::max(1u, std::min(2u, channels));
    first_ = iface_ ? first_channel - 1 : 0;
    device_uid_ = d.uid;
    io_dev_ = watch_dev_ = d.obj;
    if (iface_ && first_ + ch_ > d.channels) {
      if (error) *error = "The device has " + std::to_string(d.channels) + " outputs: choose others";
      return false;
    }
    if (!float_streams(d.obj, kAudioObjectPropertyScopeOutput)) {
      if (error) *error = "The device does not take 32-bit float audio";
      return false;
    }
    rate_ = nominal_rate(d.obj);
    if (!rate_) {
      if (error) *error = "The device has no sample rate";
      return false;
    }
    if (iface_) period_frames_ = request_buffer_frames(d.obj, kLowLatencyFrames);
    buf_.assign(kChunk * ch_, 0.f);
    const OSStatus e = AudioDeviceCreateIOProcID(d.obj, &RenderStream::io, this, &proc_);
    if (e != noErr || !proc_) {
      proc_ = nullptr;
      if (error) *error = "Cannot play on the device (error " + os_text(e) + ")";
      return false;
    }
    return true;
  }

 private:
  static OSStatus io(AudioObjectID, const AudioTimeStamp*, const AudioBufferList*, const AudioTimeStamp*,
                     AudioBufferList* out, const AudioTimeStamp*, void* user) {
    auto* self = static_cast<RenderStream*>(user);
    self->in_cb_.fetch_add(1, std::memory_order_seq_cst);
    if (!self->stopping_.load(std::memory_order_seq_cst) && out) self->play(out);
    self->in_cb_.fetch_sub(1, std::memory_order_seq_cst);
    return noErr;
  }

  void play(AudioBufferList* out) {
    const int64_t now = virgil::mono_ns();
    last_cb_ns_.store(now, std::memory_order_relaxed);
    AudioBuffer* b = out->mBuffers;
    size_t frames = SIZE_MAX;
    unsigned total = 0;
    for (UInt32 i = 0; i < out->mNumberBuffers; ++i) {
      if (b[i].mData) std::memset(b[i].mData, 0, b[i].mDataByteSize);
      if (!b[i].mNumberChannels || !b[i].mData) continue;
      frames = std::min<size_t>(frames, b[i].mDataByteSize / (sizeof(float) * b[i].mNumberChannels));
      total += b[i].mNumberChannels;
    }
    if (frames == SIZE_MAX || frames == 0 || total == 0) return;
    for (size_t done = 0; done < frames;) {
      const size_t n = std::min(frames - done, kChunk);
      float* in = buf_.data();
      src_->render(in, n, now);
      if (iface_) {
        // Engine channel j -> device channel first_ + j, wherever it is
        // (interleaved in one buffer or one buffer per channel); the rest
        // stays silent.
        unsigned k = 0;  // device channel index of the buffer's first channel
        for (UInt32 i = 0; i < out->mNumberBuffers; ++i) {
          const unsigned nch = b[i].mNumberChannels;
          float* d = static_cast<float*>(b[i].mData);
          if (d && nch)
            for (unsigned c = 0; c < nch; ++c) {
              const unsigned dc = k + c;
              if (dc < first_ || dc >= first_ + ch_) continue;
              const unsigned j = dc - first_;
              for (size_t f = 0; f < n; ++f) d[(done + f) * nch + c] = in[f * ch_ + j];
            }
          k += nch;
        }
        done += n;
        continue;
      }
      // The engine's first (or only) two channels into the device's first two.
      unsigned k = 0;  // device channel index across the buffers
      for (UInt32 i = 0; i < out->mNumberBuffers && k < 2; ++i) {
        const unsigned nch = b[i].mNumberChannels;
        float* d = static_cast<float*>(b[i].mData);
        if (d && nch) {
          for (unsigned c = 0; c < nch && k + c < 2; ++c) {
            const unsigned from = std::min(k + c, ch_ - 1);
            for (size_t f = 0; f < n; ++f) {
              const float v = total == 1 && ch_ == 2 ? 0.5f * (in[2 * f] + in[2 * f + 1]) : in[f * ch_ + from];
              d[(done + f) * nch + c] = v;
            }
          }
        }
        k += nch;
      }
      done += n;
    }
  }

  AudioSource* const src_;
  bool iface_ = false;
};

// ---- capture: an audio interface's inputs ------------------------------------------------------

class InputStream : public CaStream {
 public:
  explicit InputStream(AudioSink* sink) : sink_(sink) {}
  ~InputStream() override { close_io(); }

  bool open(const OutDev& d, unsigned first_input, unsigned count, std::string* error) {
    ch_ = std::max(1u, std::min(vc::kMaxInterfaceInputs, count));
    first_ = first_input > 0 ? first_input - 1 : 0;
    device_uid_ = d.uid;
    io_dev_ = watch_dev_ = d.obj;
    if (first_ + ch_ > d.channels) {
      if (error) *error = "The device has " + std::to_string(d.channels) + " inputs: choose others";
      return false;
    }
    if (!float_streams(d.obj, kAudioObjectPropertyScopeInput)) {
      if (error) *error = "The device does not give 32-bit float audio";
      return false;
    }
    rate_ = nominal_rate(d.obj);
    if (!rate_) {
      if (error) *error = "The device has no sample rate";
      return false;
    }
    period_frames_ = request_buffer_frames(d.obj, kLowLatencyFrames);
    buf_.assign(kChunk * ch_, 0.f);
    const OSStatus e = AudioDeviceCreateIOProcID(d.obj, &InputStream::io, this, &proc_);
    if (e != noErr || !proc_) {
      proc_ = nullptr;
      if (error) *error = "Cannot record from the device (error " + os_text(e) + ")";
      return false;
    }
    return true;
  }

  // Without the permission macOS delivers silence, not an error.
  std::string note() const override {
    if (!started_.load(std::memory_order_acquire)) return std::string();
    const int64_t now = virgil::mono_ns();
    if (now >= next_note_) {
      next_note_ = now + 1000000000LL;
      const MicStatus st = mic_status();
      const int64_t sound = last_sound_ns_.load(std::memory_order_relaxed);
      const int64_t since = sound ? sound : started_ns_.load(std::memory_order_relaxed);
      note_ = st == kMicStatusDenied || st == kMicStatusRestricted ? kMicDenied
              : st != kMicStatusAuthorized && last_cb_ns_.load(std::memory_order_relaxed) && now - since > 3000000000LL
                  ? kMicSilent
                  : "";
    }
    return note_;
  }

 private:
  std::string start_error(OSStatus e) const override {
    return "Cannot start recording from the device (error " + os_text(e) + ")";
  }

  static OSStatus io(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp* in_time,
                     AudioBufferList* out, const AudioTimeStamp*, void* user) {
    auto* self = static_cast<InputStream*>(user);
    self->in_cb_.fetch_add(1, std::memory_order_seq_cst);
    if (out)  // a duplex device's outputs: nothing to play from here
      for (UInt32 i = 0; i < out->mNumberBuffers; ++i)
        if (out->mBuffers[i].mData) std::memset(out->mBuffers[i].mData, 0, out->mBuffers[i].mDataByteSize);
    if (!self->stopping_.load(std::memory_order_seq_cst) && in) self->capture(in, in_time);
    self->in_cb_.fetch_sub(1, std::memory_order_seq_cst);
    return noErr;
  }

  void capture(const AudioBufferList* in, const AudioTimeStamp* t) {
    const int64_t now = virgil::mono_ns();
    last_cb_ns_.store(now, std::memory_order_relaxed);
    const UInt32 nb = in->mNumberBuffers;
    const AudioBuffer* b = in->mBuffers;
    // Where each wanted input is: the device's channels in buffer order.
    const float* src[vc::kMaxInterfaceInputs];
    size_t stride[vc::kMaxInterfaceInputs];
    unsigned got = 0, k = 0;  // k: device channel index of the buffer's first channel
    size_t frames = SIZE_MAX;
    bool gap = false;
    for (UInt32 i = 0; i < nb && got < ch_; ++i) {
      const unsigned nch = b[i].mNumberChannels;
      if (!nch) continue;
      if (k + nch > first_) {
        frames = std::min<size_t>(frames, b[i].mDataByteSize / (sizeof(float) * nch));
        if (!b[i].mData) gap = true;
        for (unsigned c = k < first_ ? first_ - k : 0; c < nch && got < ch_; ++c, ++got) {
          src[got] = b[i].mData ? static_cast<const float*>(b[i].mData) + c : nullptr;
          stride[got] = nch;
        }
      }
      k += nch;
    }
    if (got < ch_) return fail("The device's inputs changed");  // fewer than asked for: reopened
    if (frames == SIZE_MAX || frames == 0) return;
    int64_t end = now;
    if (t && (t->mFlags & kAudioTimeStampHostTimeValid)) {
      // The input time is the first frame's.
      const int64_t e = int64_t(AudioConvertHostTimeToNanos(t->mHostTime)) + int64_t(double(frames) * 1e9 / rate_);
      if (e <= now + 20000000LL && e >= now - 500000000LL) end = e;
    }
    if (gap) {
      sink_->audio(nullptr, frames, end);
      return;
    }
    bool sound = false;
    const unsigned C = ch_;
    for (size_t done = 0; done < frames;) {
      const size_t n = std::min(frames - done, kChunk);
      float* o = buf_.data();
      for (size_t f = 0; f < n; ++f)
        for (unsigned j = 0; j < C; ++j) {
          const float v = src[j][(done + f) * stride[j]];
          if (v != 0.f) sound = true;
          o[f * C + j] = v;
        }
      done += n;
      sink_->audio(o, n, end - int64_t(double(frames - done) * 1e9 / rate_));
    }
    if (sound) last_sound_ns_.store(now, std::memory_order_relaxed);
  }

  AudioSink* const sink_;
  std::atomic<int64_t> last_sound_ns_{0};
  // note(): the engine's source thread only.
  mutable int64_t next_note_ = 0;
  mutable std::string note_;
};

// ---- capture: a process tap read through a private aggregate device ------------------------------

enum TapKind { kDeviceTap, kAppsTap, kAllTap };

#if VIRGIL_HAVE_TAPS
API_AVAILABLE(macos(14.2))
OSStatus create_tap(TapKind kind, const std::vector<AudioObjectID>& procs, const std::string& device_uid,
                    AudioObjectID* tap, std::string* tap_uid) {
  @autoreleasepool {
    NSMutableArray<NSNumber*>* list = [NSMutableArray array];
    for (AudioObjectID p : procs) [list addObject:@(p)];
    CATapDescription* d = nil;
    if (kind == kDeviceTap) d = [[CATapDescription alloc] initExcludingProcesses:list andDeviceUID:ns(device_uid) withStream:0];
    else if (kind == kAppsTap) d = [[CATapDescription alloc] initStereoMixdownOfProcesses:list];
    else d = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:list];
    if (!d) return kAudioHardwareUnspecifiedError;
    d.name = @"Virgil desktop audio";
    // Only this process sees it (property `privateTap`, Swift `isPrivate`).
    if ([d respondsToSelector:@selector(setPrivateTap:)]) [d setValue:@YES forKey:@"privateTap"];
    d.muteBehavior = CATapUnmuted;  // the user still hears it
    AudioObjectID tap_id = kAudioObjectUnknown;
    const OSStatus e = AudioHardwareCreateProcessTap(d, &tap_id);
    if (e != noErr) return e;
    if (tap_id == kAudioObjectUnknown) return kAudioHardwareUnspecifiedError;
    *tap = tap_id;
    *tap_uid = d.UUID.UUIDString.UTF8String ?: "";
    return noErr;
  }
}
#endif

class TapStream : public CaStream {
 public:
  TapStream(AudioSink* sink, TapKind kind, std::vector<AudioObjectID> procs)
      : sink_(sink), kind_(kind), procs_(std::move(procs)) {}
  ~TapStream() override {
    close_io();
#if VIRGIL_HAVE_TAPS
    if (@available(macOS 14.2, *)) {
      if (agg_ != kAudioObjectUnknown) AudioHardwareDestroyAggregateDevice(agg_);
      if (tap_ != kAudioObjectUnknown) AudioHardwareDestroyProcessTap(tap_);
    }
#endif
  }

  // main: the output device that clocks the aggregate (and, for a device
  // tap, the one tapped).
  bool open(const OutDev& main, unsigned channels, std::string* error) {
#if VIRGIL_HAVE_TAPS
    if (@available(macOS 14.2, *)) {
      ch_ = std::max(1u, std::min(2u, channels));
      device_uid_ = main.uid;
      watch_dev_ = main.obj;
      main_id_ = main.obj;
      std::string tap_uid;
      OSStatus e = create_tap(kind_, procs_, main.uid, &tap_, &tap_uid);
      if (e != noErr && kind_ == kDeviceTap) {
        // Not this device's stream: everything every app plays instead.
        tap_ = kAudioObjectUnknown;
        e = create_tap(kAllTap, {}, std::string(), &tap_, &tap_uid);
      }
      if (e != noErr) {
        tap_ = kAudioObjectUnknown;
        if (error) *error = "Cannot capture (Core Audio error " + os_text(e) + "). " + kPermission;
        return false;
      }
      @autoreleasepool {
        NSString* main_uid = ns(main.uid);
        NSString* uid = [NSString stringWithFormat:@"%s%@", kOwnPrefix, [[NSUUID UUID] UUIDString]];
        NSDictionary* desc = @{
          @kAudioAggregateDeviceNameKey : @"Virgil desktop audio",
          @kAudioAggregateDeviceUIDKey : uid,
          @kAudioAggregateDeviceMainSubDeviceKey : main_uid,
          @kAudioAggregateDeviceIsPrivateKey : @YES,
          @kAudioAggregateDeviceIsStackedKey : @NO,
          @kAudioAggregateDeviceTapAutoStartKey : @YES,
          @kAudioAggregateDeviceSubDeviceListKey : @[ @{@kAudioSubDeviceUIDKey : main_uid} ],
          @kAudioAggregateDeviceTapListKey : @[ @{
            @kAudioSubTapUIDKey : ns(tap_uid),
            @kAudioSubTapDriftCompensationKey : @YES,
          } ],
        };
        AudioObjectID agg = kAudioObjectUnknown;
        e = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)desc, &agg);
        if (e != noErr || agg == kAudioObjectUnknown) {
          if (error) *error = "Cannot capture: no aggregate device (Core Audio error " + os_text(e) + ")";
          return false;
        }
        agg_ = io_dev_ = agg;
      }
      rate_ = nominal_rate(agg_);
      if (!rate_) rate_ = nominal_rate(main.obj);
      if (!rate_) {
        if (error) *error = "The device has no sample rate";
        return false;
      }
      // The aggregate's inputs: the sub-device's own (if it has any), then the tap's.
      channel_count(main.obj, kAudioObjectPropertyScopeInput, &skip_);
      buf_.assign(kChunk * ch_, 0.f);
      e = AudioDeviceCreateIOProcID(agg_, &TapStream::io, this, &proc_);
      if (e != noErr || !proc_) {
        proc_ = nullptr;
        if (error) *error = "Cannot capture (Core Audio error " + os_text(e) + ")";
        return false;
      }
      return true;
    }
#endif
    if (error) *error = kind_ == kDeviceTap ? kNeedsTapsDevice : kNeedsTaps;
    return false;
  }

  // Silence while the tapped apps play: most likely the permission.
  std::string note() const override {
    if (!started_.load(std::memory_order_acquire)) return std::string();
    const int64_t now = virgil::mono_ns();
    const int64_t sound = last_sound_ns_.load(std::memory_order_relaxed);
    const int64_t since = sound ? sound : started_ns_.load(std::memory_order_relaxed);
    if (now - since < 3000000000LL || !last_cb_ns_.load(std::memory_order_relaxed)) {
      note_on_ = false;
      return std::string();
    }
    if (now >= next_note_) {
      next_note_ = now + 1000000000LL;
      note_on_ = someone_plays();
    }
    return note_on_ ? kSilentNote : std::string();
  }

 private:
  std::string start_error(OSStatus e) const override {
    return "Cannot start capturing (Core Audio error " + os_text(e) + "). " + kPermission;
  }

  bool someone_plays() const {
#if VIRGIL_HAVE_TAPS
    if (@available(macOS 14.2, *)) {
      const pid_t self = getpid();
      for (const Proc& p : list_processes(kind_ == kDeviceTap)) {
        if (!p.output || p.pid == self) continue;
        const bool listed = std::find(procs_.begin(), procs_.end(), p.obj) != procs_.end();
        if (kind_ == kAppsTap && listed) return true;
        if (kind_ == kAllTap && !listed) return true;
        if (kind_ == kDeviceTap && std::find(p.devices.begin(), p.devices.end(), main_id_) != p.devices.end())
          return true;
      }
    }
#endif
    return false;
  }

  static OSStatus io(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp* in_time,
                     AudioBufferList* out, const AudioTimeStamp*, void* user) {
    auto* self = static_cast<TapStream*>(user);
    self->in_cb_.fetch_add(1, std::memory_order_seq_cst);
    if (out)  // the sub-device's outputs: nothing to play
      for (UInt32 i = 0; i < out->mNumberBuffers; ++i)
        if (out->mBuffers[i].mData) std::memset(out->mBuffers[i].mData, 0, out->mBuffers[i].mDataByteSize);
    if (!self->stopping_.load(std::memory_order_seq_cst) && in) self->capture(in, in_time);
    self->in_cb_.fetch_sub(1, std::memory_order_seq_cst);
    return noErr;
  }

  void capture(const AudioBufferList* in, const AudioTimeStamp* t) {
    const int64_t now = virgil::mono_ns();
    last_cb_ns_.store(now, std::memory_order_relaxed);
    const UInt32 nb = in->mNumberBuffers;
    if (nb == 0) return;
    const AudioBuffer* b = in->mBuffers;
    // Up to two channels, in order, from the tap's buffers.
    const float* src[2] = {nullptr, nullptr};
    size_t stride[2] = {1, 1};
    unsigned got = 0;
    size_t frames = SIZE_MAX;
    bool gap = false;
    for (UInt32 i = skip_ < nb ? skip_ : nb - 1; i < nb && got < 2; ++i) {
      const unsigned nch = b[i].mNumberChannels;
      if (!nch) continue;
      frames = std::min<size_t>(frames, b[i].mDataByteSize / (sizeof(float) * nch));
      if (!b[i].mData) gap = true;
      for (unsigned c = 0; c < nch && got < 2; ++c, ++got) {
        src[got] = b[i].mData ? static_cast<const float*>(b[i].mData) + c : nullptr;
        stride[got] = nch;
      }
    }
    if (!got || frames == SIZE_MAX || frames == 0) return;
    // When the last frame was recorded (the input time is the first one's).
    int64_t end = now;
    if (t && (t->mFlags & kAudioTimeStampHostTimeValid)) {
      const int64_t e = int64_t(AudioConvertHostTimeToNanos(t->mHostTime)) + int64_t(double(frames) * 1e9 / rate_);
      if (e <= now + 20000000LL && e >= now - 500000000LL) end = e;
    }
    if (gap) {
      sink_->audio(nullptr, frames, end);
      return;
    }
    bool sound = false;
    for (size_t done = 0; done < frames;) {
      const size_t n = std::min(frames - done, kChunk);
      float* o = buf_.data();
      for (size_t f = 0; f < n; ++f) {
        const size_t k = done + f;
        const float l = src[0][k * stride[0]];
        const float r = got > 1 ? src[1][k * stride[1]] : l;
        if (l != 0.f || r != 0.f) sound = true;
        if (ch_ == 1) {
          o[f] = 0.5f * (l + r);
        } else {
          o[2 * f] = l;
          o[2 * f + 1] = r;
        }
      }
      done += n;
      sink_->audio(o, n, end - int64_t(double(frames - done) * 1e9 / rate_));
    }
    if (sound) last_sound_ns_.store(now, std::memory_order_relaxed);
  }

  AudioSink* const sink_;
  const TapKind kind_;
  const std::vector<AudioObjectID> procs_;  // tapped (apps) or excluded (all apps)
  AudioObjectID tap_ = kAudioObjectUnknown, agg_ = kAudioObjectUnknown, main_id_ = kAudioObjectUnknown;
  unsigned skip_ = 0;  // input buffers of the sub-device before the tap's
  std::atomic<int64_t> last_sound_ns_{0};
  // note(): the engine's source thread only.
  mutable int64_t next_note_ = 0;
  mutable bool note_on_ = false;
};

// ---- the backend --------------------------------------------------------------------------------

class CoreAudioBackend : public Backend {
 public:
  CoreAudioBackend() : self_pid_(getpid()) {
    @autoreleasepool {
      NSString* b = [[NSBundle mainBundle] bundleIdentifier];
      self_bundle_ = b ? lower(b.UTF8String ?: "") : std::string();
    }
  }
  ~CoreAudioBackend() override {
    std::lock_guard<std::mutex> l(listen_mu_);
    for (const AudioObjectPropertyAddress& a : listening_)
      AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &a, &CoreAudioBackend::on_change, this);
  }

  std::string name() const override { return "Core Audio"; }

  bool connect(std::string*) override {
    std::lock_guard<std::mutex> l(listen_mu_);
    if (listening_.empty()) {
      {
        std::lock_guard<std::mutex> g(sig_mu_);
        device_sig_ = devices_signature();
      }
      listen(kAudioHardwarePropertyDevices);
      listen(kAudioHardwarePropertyDefaultOutputDevice);
      listen(kAudioHardwarePropertyDefaultInputDevice);
#if VIRGIL_HAVE_TAPS
      if (@available(macOS 14.2, *)) listen(kAudioHardwarePropertyProcessObjectList);
#endif
    }
    return true;  // Core Audio is always there
  }

  Capabilities capabilities() override {
    Capabilities c;
#if VIRGIL_HAVE_TAPS
    if (@available(macOS 14.2, *)) return c;
#endif
    c.app_capture = c.all_apps = false;
    c.app_hint = kNeedsTaps;
    return c;
  }

  std::vector<Device> outputs() override {
    const AudioObjectID def = default_output_id();
    std::vector<Device> r;
    for (const OutDev& d : list_outputs()) {
      Device e;
      e.id = d.uid;
      e.name = d.name;
      e.is_default = d.obj == def;
      e.channels = d.channels;
      r.push_back(e);
    }
    std::sort(r.begin(), r.end(), [](const Device& a, const Device& b) { return lower(a.name) < lower(b.name); });
    return r;
  }

  std::string default_output() override {
    const AudioObjectID def = default_output_id();
    if (def == kAudioObjectUnknown) return std::string();
    const std::string uid = get_string(def, kAudioDevicePropertyDeviceUID);
    return own_device(uid) ? std::string() : uid;
  }

  std::vector<Device> inputs() override {
    const AudioObjectID def = default_input_id();
    std::vector<Device> r;
    for (const OutDev& d : list_inputs()) {
      Device e;
      e.id = d.uid;
      e.name = d.name;
      e.is_default = d.obj == def;
      e.channels = d.channels;
      r.push_back(e);
    }
    std::sort(r.begin(), r.end(), [](const Device& a, const Device& b) { return lower(a.name) < lower(b.name); });
    return r;
  }

  std::string default_input() override {
    const AudioObjectID def = default_input_id();
    if (def == kAudioObjectUnknown) return std::string();
    const std::string uid = get_string(def, kAudioDevicePropertyDeviceUID);
    return own_device(uid) ? std::string() : uid;
  }

  std::vector<App> apps() override {
    std::vector<App> r;
#if VIRGIL_HAVE_TAPS
    if (@available(macOS 14.2, *)) {
      std::map<std::string, App> by_key;
      for (const Proc& p : list_processes(false)) {
        if (!p.output || p.pid == self_pid_) continue;
        const Ident who = identify(p.pid, p.bundle);
        if (who.key.empty() || is_self(who)) continue;
        App& a = by_key[lower(who.key)];
        if (a.id.empty()) {
          a.id = who.key;
          a.name = who.name;
        }
        a.playing = true;
      }
      for (auto& kv : by_key) r.push_back(kv.second);
      std::sort(r.begin(), r.end(), [](const App& a, const App& b) { return lower(a.name) < lower(b.name); });
    }
#endif
    return r;
  }

  std::vector<AppStream> app_streams(const std::string& app, bool all) override {
    std::vector<AppStream> r;
#if VIRGIL_HAVE_TAPS
    if (@available(macOS 14.2, *)) {
      std::string want = lower(app);
      while (!want.empty() && want.back() == ' ') want.pop_back();
      const size_t slash = want.find_last_of('/');
      if (slash != std::string::npos) want = want.substr(slash + 1);  // a path typed in
      if (want.size() > 4 && want.compare(want.size() - 4, 4, ".app") == 0) want.resize(want.size() - 4);
      std::vector<AudioObjectID> objs;
      std::string key;
      pid_t pid = 0;
      if (all) {
        // Everything but this process and what plays on a Virgil device (it
        // would go back into Dante).
        std::vector<AudioObjectID> virgil;
        for (const OutDev& d : list_outputs())
          if (lower(d.name).find("virgil") != std::string::npos || lower(d.uid).find("virgil") != std::string::npos)
            virgil.push_back(d.obj);
        const AudioObjectID me = process_object(self_pid_);
        if (me != kAudioObjectUnknown) objs.push_back(me);
        for (const Proc& p : list_processes(!virgil.empty())) {
          bool skip = p.pid == self_pid_;
          for (AudioObjectID d : p.devices) skip = skip || std::find(virgil.begin(), virgil.end(), d) != virgil.end();
          if (skip && std::find(objs.begin(), objs.end(), p.obj) == objs.end()) objs.push_back(p.obj);
        }
        key = "*all*";
      } else {
        if (want.empty()) return r;
        for (const Proc& p : list_processes(false)) {
          if (p.pid == self_pid_) continue;
          const Ident who = identify(p.pid, p.bundle);
          if (is_self(who)) continue;
          if (lower(who.key) != want && lower(who.name) != want && lower(who.exe) != want && lower(p.bundle) != want)
            continue;
          objs.push_back(p.obj);
          if (key.empty()) {
            key = who.key;
            pid = p.pid;
          }
        }
        if (objs.empty()) return r;
      }
      std::sort(objs.begin(), objs.end());
      // The id changes when the set of processes does: the engine then
      // opens a new tap (and closes the old one).
      uint64_t h = fnv(1469598103934665603ull, key.data(), key.size());
      h = fnv(h, objs.data(), objs.size() * sizeof(AudioObjectID));
      AppStream s;
      s.id = h;
      s.app = all ? std::string() : key;
      s.pid = uint32_t(pid);
      r.push_back(s);
      std::lock_guard<std::mutex> l(plans_mu_);
      if (plans_.size() > 256) plans_.clear();
      Plan& p = plans_[h];
      p.all = all;
      p.procs = objs;
    }
#endif
    return r;
  }

  unsigned changes() override { return changes_.load(std::memory_order_acquire); }
  unsigned default_changes() override { return defaults_.load(std::memory_order_acquire); }

  std::unique_ptr<Stream> open_device_capture(const std::string& device, unsigned channels, AudioSink* sink,
                                              std::string* error) override {
    connect(nullptr);
    if (!capabilities().app_capture) {
      if (error) *error = kNeedsTapsDevice;
      return nullptr;
    }
    OutDev d;
    if (!find_output(device, &d, error)) return nullptr;
    std::unique_ptr<TapStream> s(new TapStream(sink, kDeviceTap, {}));
    if (!s->open(d, channels, error)) return nullptr;
    return std::unique_ptr<Stream>(s.release());
  }

  std::unique_ptr<Stream> open_app_capture(const AppStream& as, unsigned channels, AudioSink* sink,
                                           std::string* error) override {
    connect(nullptr);
    Plan plan;
    {
      std::lock_guard<std::mutex> l(plans_mu_);
      auto it = plans_.find(as.id);
      if (it == plans_.end()) {
        if (error) *error = "The app stopped playing";
        return nullptr;
      }
      plan = it->second;
    }
    OutDev d;
    if (!find_output("default", &d, error)) return nullptr;
    std::unique_ptr<TapStream> s(new TapStream(sink, plan.all ? kAllTap : kAppsTap, plan.procs));
    if (!s->open(d, channels, error)) return nullptr;
    return std::unique_ptr<Stream>(s.release());
  }

  std::unique_ptr<Stream> open_input_capture(const std::string& device, unsigned first_input, unsigned count,
                                             AudioSink* sink, std::string* error) override {
    connect(nullptr);
    OutDev d;
    if (!find_input(device, &d, error)) return nullptr;
    // The microphone permission covers every audio input.
    const MicStatus st = mic_status();
    if (st == kMicStatusDenied || st == kMicStatusRestricted) {
      if (error) *error = kMicDenied;
      return nullptr;
    }
    if (st == kMicStatusNotDetermined && can_ask_mic()) {
      std::atomic<unsigned>* changes = &changes_;
      ask_mic(^{
        changes->fetch_add(1, std::memory_order_acq_rel);  // the engine tries again right away
      });
      if (error) *error = kMicAsking;
      return nullptr;
    }
    std::unique_ptr<InputStream> s(new InputStream(sink));
    if (!s->open(d, first_input, count, error)) return nullptr;
    return std::unique_ptr<Stream>(s.release());
  }

  std::unique_ptr<Stream> open_render(const std::string& device, unsigned first_channel, unsigned channels, double,
                                      AudioSource* source, std::string* error) override {
    connect(nullptr);
    OutDev d;
    if (!find_output(device, &d, error)) return nullptr;
    std::unique_ptr<RenderStream> s(new RenderStream(source));
    if (!s->open(d, first_channel, channels, error)) return nullptr;
    return std::unique_ptr<Stream>(s.release());
  }

 private:
  struct Plan {
    bool all = false;
    std::vector<AudioObjectID> procs;
  };

  bool is_self(const Ident& who) const { return !self_bundle_.empty() && lower(who.key) == self_bundle_; }

  // Under listen_mu_.
  void listen(AudioObjectPropertySelector sel) {
    const AudioObjectPropertyAddress a = addr(sel);
    if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &a, &CoreAudioBackend::on_change, this) == noErr)
      listening_.push_back(a);
  }

  static std::string devices_signature() {
    std::string s;
    for (const OutDev& d : list_outputs()) s += d.uid + ' ' + std::to_string(d.channels) + '\n';
    s += "in\n";
    for (const OutDev& d : list_inputs()) s += d.uid + ' ' + std::to_string(d.channels) + '\n';
    return s;
  }

  // Core Audio's notification thread.
  static OSStatus on_change(AudioObjectID, UInt32 n, const AudioObjectPropertyAddress* a, void* user) {
    auto* self = static_cast<CoreAudioBackend*>(user);
    for (UInt32 i = 0; i < n; ++i) {
      if (a[i].mSelector == kAudioHardwarePropertyDefaultOutputDevice ||
          a[i].mSelector == kAudioHardwarePropertyDefaultInputDevice) {
        self->defaults_.fetch_add(1, std::memory_order_acq_rel);
      } else if (a[i].mSelector == kAudioHardwarePropertyDevices) {
        // Our own aggregates come and go with every capture: not a change.
        const std::string sig = devices_signature();
        std::lock_guard<std::mutex> l(self->sig_mu_);
        if (sig != self->device_sig_) {
          self->device_sig_ = sig;
          self->changes_.fetch_add(1, std::memory_order_acq_rel);
        }
      } else {
        self->changes_.fetch_add(1, std::memory_order_acq_rel);  // processes
      }
    }
    return noErr;
  }

  const pid_t self_pid_;
  std::string self_bundle_;
  std::mutex listen_mu_;
  std::vector<AudioObjectPropertyAddress> listening_;
  std::mutex sig_mu_;
  std::string device_sig_;
  std::mutex plans_mu_;
  std::map<uint64_t, Plan> plans_;
  std::atomic<unsigned> changes_{0}, defaults_{0};
};

}  // namespace

std::unique_ptr<Backend> make_system_backend() { return std::unique_ptr<Backend>(new CoreAudioBackend()); }

}  // namespace desk
