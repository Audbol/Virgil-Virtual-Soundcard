// CoreAudio AudioServerPlugIn exposing virgild as a system-wide audio device.
//
// Object tree:   PlugIn(1) -> Device(2) -> InputStream(3), OutputStream(4)
//
// Timing: the device clock *is* the virgild media clock. GetZeroTimeStamp maps
// the PTP-disciplined anchor published in shared memory onto mach host time,
// so the HAL's rate scalar tracks the Dante clock master and no sample
// rate conversion or drift correction is needed anywhere.
//
// Latency: input safety offset = virgild rx latency, output safety offset =
// virgild tx lead; both directions add one engine tick of device latency.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "virgil/client.h"
#include "virgil/platform.h"

namespace {

enum : AudioObjectID {
  kObjectID_PlugIn = kAudioObjectPlugInObject,
  kObjectID_Device = 2,
  kObjectID_Stream_Input = 3,
  kObjectID_Stream_Output = 4,
};

constexpr UInt32 kZeroTimeStampPeriod = 8192;
#define kDeviceUID "Virgil-Virtual-Soundcard"
#define kDeviceModelUID "Virgil-Virtual-Soundcard-Model"
#define kManufacturer "Virgil Project"

// ---- state ------------------------------------------------------------------

struct State {
  std::mutex mutex;  // guards everything except the IO path
  AudioServerPlugInHostRef host = nullptr;
  std::atomic<UInt32> ref_count{0};

  virgil::Client client;  // valid while io_count > 0 and the daemon is up
  UInt32 io_count = 0;
  bool input_active = true;
  bool output_active = true;

  // Device format, taken from virgild at load (defaults if it is not running).
  Float64 sample_rate = 48000;
  UInt32 in_channels = 8;
  UInt32 out_channels = 8;
  UInt32 period_frames = 48;
  UInt32 rx_latency_frames = 96;
  UInt32 tx_lead_frames = 96;

  // Clock: sample time 0 == media frame `base_frame`.
  uint64_t base_frame = 0;
  bool free_running = false;  // daemon absent: synthesize from host clock
  int64_t free_base_ns = 0;
  std::atomic<UInt64> seed{1};
  uint64_t last_steps = 0;
};

State g;

mach_timebase_info_data_t timebase() {
  static mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t t;
    mach_timebase_info(&t);
    return t;
  }();
  return tb;
}
UInt64 ns_to_host(int64_t ns) {
  auto tb = timebase();
  return UInt64((__uint128_t)ns * tb.denom / tb.numer);
}

void load_format_from_daemon() {
  virgil::Client c;
  if (!c.open()) return;
  g.sample_rate = c.sample_rate();
  g.in_channels = c.rx_channels() ? c.rx_channels() : 1;
  g.out_channels = c.tx_channels() ? c.tx_channels() : 1;
  g.period_frames = c.period_frames();
  g.rx_latency_frames = c.rx_latency_frames();
  g.tx_lead_frames = c.tx_lead_frames();
}

// ---- helpers ----------------------------------------------------------------

AudioStreamBasicDescription stream_format(bool input) {
  const UInt32 ch = input ? g.in_channels : g.out_channels;
  AudioStreamBasicDescription f{};
  f.mSampleRate = g.sample_rate;
  f.mFormatID = kAudioFormatLinearPCM;
  f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
  f.mBytesPerPacket = 4 * ch;
  f.mFramesPerPacket = 1;
  f.mBytesPerFrame = 4 * ch;
  f.mChannelsPerFrame = ch;
  f.mBitsPerChannel = 32;
  return f;
}

template <typename T>
OSStatus put(UInt32 inDataSize, UInt32* outDataSize, void* outData, const T& value) {
  if (inDataSize < sizeof(T)) return kAudioHardwareBadPropertySizeError;
  *static_cast<T*>(outData) = value;
  *outDataSize = sizeof(T);
  return kAudioHardwareNoError;
}

OSStatus put_ids(UInt32 inDataSize, UInt32* outDataSize, void* outData, const AudioObjectID* ids,
                 UInt32 n) {
  const UInt32 fit = std::min<UInt32>(n, inDataSize / sizeof(AudioObjectID));
  std::memcpy(outData, ids, fit * sizeof(AudioObjectID));
  *outDataSize = fit * sizeof(AudioObjectID);
  return kAudioHardwareNoError;
}

// Streams visible in a scope.
UInt32 scoped_streams(AudioObjectPropertyScope scope, AudioObjectID* out) {
  UInt32 n = 0;
  if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeInput)
    out[n++] = kObjectID_Stream_Input;
  if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput)
    out[n++] = kObjectID_Stream_Output;
  return n;
}

// ---- IUnknown ---------------------------------------------------------------

HRESULT QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
ULONG AddRef(void* inDriver);
ULONG Release(void* inDriver);
OSStatus Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef inHost);
OSStatus CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*) { return kAudioHardwareUnsupportedOperationError; }
OSStatus DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) { return kAudioHardwareUnsupportedOperationError; }
OSStatus AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) { return kAudioHardwareNoError; }
OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) { return kAudioHardwareNoError; }
OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) { return kAudioHardwareNoError; }
OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) { return kAudioHardwareNoError; }
Boolean HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*);
OSStatus IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean*);
OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32*);
OSStatus GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, UInt32*, void*);
OSStatus SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*);
OSStatus StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
OSStatus StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64*, UInt64*, UInt64*);
OSStatus WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean*, Boolean*);
OSStatus BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) { return kAudioHardwareNoError; }
OSStatus DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*, void*, void*);
OSStatus EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) { return kAudioHardwareNoError; }

AudioServerPlugInDriverInterface gInterface = {
    nullptr,
    QueryInterface,
    AddRef,
    Release,
    Initialize,
    CreateDevice,
    DestroyDevice,
    AddDeviceClient,
    RemoveDeviceClient,
    PerformDeviceConfigurationChange,
    AbortDeviceConfigurationChange,
    HasProperty,
    IsPropertySettable,
    GetPropertyDataSize,
    GetPropertyData,
    SetPropertyData,
    StartIO,
    StopIO,
    GetZeroTimeStamp,
    WillDoIOOperation,
    BeginIOOperation,
    DoIOOperation,
    EndIOOperation,
};
AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

HRESULT QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface) {
  if (inDriver != gDriverRef || !outInterface) return kAudioHardwareBadObjectError;
  CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(nullptr, inUUID);
  const bool ok = CFEqual(requested, IUnknownUUID) ||
                  CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);
  CFRelease(requested);
  if (!ok) return E_NOINTERFACE;
  ++g.ref_count;
  *outInterface = gDriverRef;
  return S_OK;
}

ULONG AddRef(void* inDriver) {
  if (inDriver != gDriverRef) return 0;
  return ++g.ref_count;
}

ULONG Release(void* inDriver) {
  if (inDriver != gDriverRef) return 0;
  UInt32 r = g.ref_count.load();
  if (r > 0) r = --g.ref_count;
  return r;
}

OSStatus Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
  if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
  std::lock_guard<std::mutex> l(g.mutex);
  g.host = inHost;
  load_format_from_daemon();
  return kAudioHardwareNoError;
}

// ---- properties -------------------------------------------------------------

Boolean HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID id, pid_t,
                    const AudioObjectPropertyAddress* a) {
  if (inDriver != gDriverRef || !a) return false;
  switch (id) {
    case kObjectID_PlugIn:
      switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
          return true;
      }
      return false;
    case kObjectID_Device:
      switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyStreams:
        case kAudioDevicePropertyClockIsStable:
          return true;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyPreferredChannelsForStereo:
          return a->mScope == kAudioObjectPropertyScopeInput ||
                 a->mScope == kAudioObjectPropertyScopeOutput;
      }
      return false;
    case kObjectID_Stream_Input:
    case kObjectID_Stream_Output:
      switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyName:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
          return true;
      }
      return false;
  }
  return false;
}

OSStatus IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID id, pid_t pid,
                            const AudioObjectPropertyAddress* a, Boolean* out) {
  if (inDriver != gDriverRef || !a || !out) return kAudioHardwareIllegalOperationError;
  if (!HasProperty(inDriver, id, pid, a)) return kAudioHardwareUnknownPropertyError;
  *out = false;
  if (id == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) *out = true;
  if ((id == kObjectID_Stream_Input || id == kObjectID_Stream_Output) &&
      (a->mSelector == kAudioStreamPropertyIsActive ||
       a->mSelector == kAudioStreamPropertyVirtualFormat ||
       a->mSelector == kAudioStreamPropertyPhysicalFormat))
    *out = true;
  return kAudioHardwareNoError;
}

OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID id, pid_t pid,
                             const AudioObjectPropertyAddress* a, UInt32, const void*,
                             UInt32* out) {
  if (inDriver != gDriverRef || !a || !out) return kAudioHardwareIllegalOperationError;
  if (!HasProperty(inDriver, id, pid, a)) return kAudioHardwareUnknownPropertyError;
  AudioObjectID tmp[2];
  switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
      *out = sizeof(AudioClassID);
      return kAudioHardwareNoError;
    case kAudioObjectPropertyOwner:
    case kAudioPlugInPropertyTranslateUIDToDevice:
      *out = sizeof(AudioObjectID);
      return kAudioHardwareNoError;
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:
    case kAudioPlugInPropertyResourceBundle:
      *out = sizeof(CFStringRef);
      return kAudioHardwareNoError;
    case kAudioObjectPropertyOwnedObjects:
      if (id == kObjectID_PlugIn) *out = sizeof(AudioObjectID);
      else if (id == kObjectID_Device) *out = scoped_streams(a->mScope, tmp) * sizeof(AudioObjectID);
      else *out = 0;
      return kAudioHardwareNoError;
    case kAudioPlugInPropertyDeviceList:
    case kAudioDevicePropertyRelatedDevices:
      *out = sizeof(AudioObjectID);
      return kAudioHardwareNoError;
    case kAudioDevicePropertyStreams:
      *out = scoped_streams(a->mScope, tmp) * sizeof(AudioObjectID);
      return kAudioHardwareNoError;
    case kAudioObjectPropertyControlList:
      *out = 0;
      return kAudioHardwareNoError;
    case kAudioDevicePropertyNominalSampleRate:
      *out = sizeof(Float64);
      return kAudioHardwareNoError;
    case kAudioDevicePropertyAvailableNominalSampleRates:
      *out = sizeof(AudioValueRange);
      return kAudioHardwareNoError;
    case kAudioDevicePropertyPreferredChannelsForStereo:
      *out = 2 * sizeof(UInt32);
      return kAudioHardwareNoError;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
      *out = sizeof(AudioStreamBasicDescription);
      return kAudioHardwareNoError;
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats:
      *out = sizeof(AudioStreamRangedDescription);
      return kAudioHardwareNoError;
    default:
      *out = sizeof(UInt32);  // all remaining properties are UInt32
      return kAudioHardwareNoError;
  }
}

OSStatus GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID id, pid_t pid,
                         const AudioObjectPropertyAddress* a, UInt32 qsize, const void* qdata,
                         UInt32 inDataSize, UInt32* outDataSize, void* outData) {
  if (inDriver != gDriverRef || !a || !outDataSize || !outData)
    return kAudioHardwareIllegalOperationError;
  if (!HasProperty(inDriver, id, pid, a)) return kAudioHardwareUnknownPropertyError;
  std::lock_guard<std::mutex> l(g.mutex);
  const bool input = id == kObjectID_Stream_Input;

  if (id == kObjectID_PlugIn) {
    switch (a->mSelector) {
      case kAudioObjectPropertyBaseClass:
        return put<AudioClassID>(inDataSize, outDataSize, outData, kAudioObjectClassID);
      case kAudioObjectPropertyClass:
        return put<AudioClassID>(inDataSize, outDataSize, outData, kAudioPlugInClassID);
      case kAudioObjectPropertyOwner:
        return put<AudioObjectID>(inDataSize, outDataSize, outData, kAudioObjectUnknown);
      case kAudioObjectPropertyManufacturer:
        return put<CFStringRef>(inDataSize, outDataSize, outData, CFSTR(kManufacturer));
      case kAudioObjectPropertyOwnedObjects:
      case kAudioPlugInPropertyDeviceList: {
        const AudioObjectID dev = kObjectID_Device;
        return put_ids(inDataSize, outDataSize, outData, &dev, 1);
      }
      case kAudioPlugInPropertyTranslateUIDToDevice: {
        if (qsize != sizeof(CFStringRef) || !qdata) return kAudioHardwareBadPropertySizeError;
        CFStringRef uid = *static_cast<const CFStringRef*>(qdata);
        const bool match = uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo;
        return put<AudioObjectID>(inDataSize, outDataSize, outData,
                                  match ? AudioObjectID(kObjectID_Device) : kAudioObjectUnknown);
      }
      case kAudioPlugInPropertyResourceBundle:
        return put<CFStringRef>(inDataSize, outDataSize, outData, CFSTR(""));
    }
  } else if (id == kObjectID_Device) {
    AudioObjectID ids[2];
    switch (a->mSelector) {
      case kAudioObjectPropertyBaseClass:
        return put<AudioClassID>(inDataSize, outDataSize, outData, kAudioObjectClassID);
      case kAudioObjectPropertyClass:
        return put<AudioClassID>(inDataSize, outDataSize, outData, kAudioDeviceClassID);
      case kAudioObjectPropertyOwner:
        return put<AudioObjectID>(inDataSize, outDataSize, outData, kObjectID_PlugIn);
      case kAudioObjectPropertyName: {
        virgil::Client c;
        CFStringRef name = (c.open() && c.header()->device_name[0])
                               ? CFStringCreateWithCString(nullptr, c.header()->device_name,
                                                           kCFStringEncodingUTF8)
                               : CFSTR("Virgil Virtual Soundcard");
        return put<CFStringRef>(inDataSize, outDataSize, outData, name);
      }
      case kAudioObjectPropertyManufacturer:
        return put<CFStringRef>(inDataSize, outDataSize, outData, CFSTR(kManufacturer));
      case kAudioDevicePropertyDeviceUID:
        return put<CFStringRef>(inDataSize, outDataSize, outData, CFSTR(kDeviceUID));
      case kAudioDevicePropertyModelUID:
        return put<CFStringRef>(inDataSize, outDataSize, outData, CFSTR(kDeviceModelUID));
      case kAudioObjectPropertyOwnedObjects:
      case kAudioDevicePropertyStreams:
        return put_ids(inDataSize, outDataSize, outData, ids, scoped_streams(a->mScope, ids));
      case kAudioDevicePropertyRelatedDevices: {
        const AudioObjectID dev = kObjectID_Device;
        return put_ids(inDataSize, outDataSize, outData, &dev, 1);
      }
      case kAudioObjectPropertyControlList:
        *outDataSize = 0;
        return kAudioHardwareNoError;
      case kAudioDevicePropertyTransportType:
        return put<UInt32>(inDataSize, outDataSize, outData, kAudioDeviceTransportTypeVirtual);
      case kAudioDevicePropertyClockDomain:
        return put<UInt32>(inDataSize, outDataSize, outData, 0);
      case kAudioDevicePropertyDeviceIsAlive:
        return put<UInt32>(inDataSize, outDataSize, outData, 1);
      case kAudioDevicePropertyDeviceIsRunning:
        return put<UInt32>(inDataSize, outDataSize, outData, g.io_count > 0);
      case kAudioDevicePropertyDeviceCanBeDefaultDevice:
      case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        return put<UInt32>(inDataSize, outDataSize, outData, 1);
      case kAudioDevicePropertyLatency:
        // One engine tick of buffering each way.
        return put<UInt32>(inDataSize, outDataSize, outData, g.period_frames);
      case kAudioDevicePropertySafetyOffset:
        return put<UInt32>(inDataSize, outDataSize, outData,
                           a->mScope == kAudioObjectPropertyScopeInput
                               ? g.rx_latency_frames
                               : g.tx_lead_frames + g.period_frames);
      case kAudioDevicePropertyNominalSampleRate:
        return put<Float64>(inDataSize, outDataSize, outData, g.sample_rate);
      case kAudioDevicePropertyAvailableNominalSampleRates: {
        AudioValueRange r{g.sample_rate, g.sample_rate};
        return put<AudioValueRange>(inDataSize, outDataSize, outData, r);
      }
      case kAudioDevicePropertyIsHidden:
        return put<UInt32>(inDataSize, outDataSize, outData, 0);
      case kAudioDevicePropertyClockIsStable:
        return put<UInt32>(inDataSize, outDataSize, outData, 1);
      case kAudioDevicePropertyPreferredChannelsForStereo: {
        if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        UInt32* ch = static_cast<UInt32*>(outData);
        ch[0] = 1;
        ch[1] = 2;
        *outDataSize = 2 * sizeof(UInt32);
        return kAudioHardwareNoError;
      }
      case kAudioDevicePropertyZeroTimeStampPeriod:
        return put<UInt32>(inDataSize, outDataSize, outData, kZeroTimeStampPeriod);
    }
  } else {  // streams
    switch (a->mSelector) {
      case kAudioObjectPropertyBaseClass:
        return put<AudioClassID>(inDataSize, outDataSize, outData, kAudioObjectClassID);
      case kAudioObjectPropertyClass:
        return put<AudioClassID>(inDataSize, outDataSize, outData, kAudioStreamClassID);
      case kAudioObjectPropertyOwner:
        return put<AudioObjectID>(inDataSize, outDataSize, outData, kObjectID_Device);
      case kAudioObjectPropertyOwnedObjects:
        *outDataSize = 0;
        return kAudioHardwareNoError;
      case kAudioObjectPropertyName:
        return put<CFStringRef>(inDataSize, outDataSize, outData,
                                input ? CFSTR("Virgil Network In") : CFSTR("Virgil Network Out"));
      case kAudioStreamPropertyIsActive:
        return put<UInt32>(inDataSize, outDataSize, outData,
                           input ? g.input_active : g.output_active);
      case kAudioStreamPropertyDirection:
        return put<UInt32>(inDataSize, outDataSize, outData, input ? 1 : 0);
      case kAudioStreamPropertyTerminalType:
        return put<UInt32>(inDataSize, outDataSize, outData, kAudioStreamTerminalTypeLine);
      case kAudioStreamPropertyStartingChannel:
        return put<UInt32>(inDataSize, outDataSize, outData, 1);
      case kAudioStreamPropertyLatency:
        return put<UInt32>(inDataSize, outDataSize, outData, 0);
      case kAudioStreamPropertyVirtualFormat:
      case kAudioStreamPropertyPhysicalFormat:
        return put<AudioStreamBasicDescription>(inDataSize, outDataSize, outData,
                                                stream_format(input));
      case kAudioStreamPropertyAvailableVirtualFormats:
      case kAudioStreamPropertyAvailablePhysicalFormats: {
        AudioStreamRangedDescription r{};
        r.mFormat = stream_format(input);
        r.mSampleRateRange.mMinimum = g.sample_rate;
        r.mSampleRateRange.mMaximum = g.sample_rate;
        return put<AudioStreamRangedDescription>(inDataSize, outDataSize, outData, r);
      }
    }
  }
  return kAudioHardwareUnknownPropertyError;
}

OSStatus SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID id, pid_t,
                         const AudioObjectPropertyAddress* a, UInt32, const void*,
                         UInt32 inDataSize, const void* inData) {
  if (inDriver != gDriverRef || !a || !inData) return kAudioHardwareIllegalOperationError;
  std::lock_guard<std::mutex> l(g.mutex);
  if (id == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
    if (inDataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
    // The rate belongs to the network; it is set in virgild's config.
    return *static_cast<const Float64*>(inData) == g.sample_rate
               ? kAudioHardwareNoError
               : kAudioDeviceUnsupportedFormatError;
  }
  if (id == kObjectID_Stream_Input || id == kObjectID_Stream_Output) {
    const bool input = id == kObjectID_Stream_Input;
    switch (a->mSelector) {
      case kAudioStreamPropertyIsActive:
        if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        (input ? g.input_active : g.output_active) = *static_cast<const UInt32*>(inData) != 0;
        return kAudioHardwareNoError;
      case kAudioStreamPropertyVirtualFormat:
      case kAudioStreamPropertyPhysicalFormat: {
        if (inDataSize < sizeof(AudioStreamBasicDescription))
          return kAudioHardwareBadPropertySizeError;
        const auto* f = static_cast<const AudioStreamBasicDescription*>(inData);
        const auto want = stream_format(input);
        return (f->mSampleRate == want.mSampleRate && f->mFormatID == want.mFormatID &&
                f->mChannelsPerFrame == want.mChannelsPerFrame &&
                f->mBitsPerChannel == want.mBitsPerChannel)
                   ? kAudioHardwareNoError
                   : kAudioDeviceUnsupportedFormatError;
      }
    }
  }
  return kAudioHardwareUnknownPropertyError;
}

// ---- IO ---------------------------------------------------------------------

bool attach_daemon_locked() {
  if (g.client.is_open() && g.client.daemon_alive()) return true;
  g.client.close();
  if (!g.client.open() || !g.client.daemon_alive()) return false;
  if (g.client.sample_rate() != UInt32(g.sample_rate)) {
    // virgild was restarted with another rate; refuse rather than play at the
    // wrong speed. Reloading the plug-in (killall coreaudiod) picks it up.
    g.client.close();
    return false;
  }
  if (!g.client.acquire_tx_slot("coreaudio")) {
    g.client.close();
    return false;
  }
  g.client.set_tx_active(true);
  g.last_steps = g.client.header()->clock_steps.load();
  return true;
}

OSStatus StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID dev, UInt32) {
  if (inDriver != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
  std::lock_guard<std::mutex> l(g.mutex);
  if (g.io_count++ > 0) return kAudioHardwareNoError;
  double now = 0;
  if (attach_daemon_locked() && g.client.frame_now(&now)) {
    g.free_running = false;
    g.base_frame = uint64_t(now);
  } else {
    g.free_running = true;
    g.free_base_ns = virgil::mono_ns();
  }
  ++g.seed;
  return kAudioHardwareNoError;
}

OSStatus StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID dev, UInt32) {
  if (inDriver != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
  std::lock_guard<std::mutex> l(g.mutex);
  if (g.io_count == 0) return kAudioHardwareIllegalOperationError;
  if (--g.io_count == 0) g.client.close();
  return kAudioHardwareNoError;
}

OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID dev, UInt32,
                          Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
  if (inDriver != gDriverRef || dev != kObjectID_Device) return kAudioHardwareBadObjectError;
  const int64_t now_ns = virgil::mono_ns();
  virgil::ClockAnchor a;
  if (!g.free_running && g.client.is_open() && g.client.anchor(&a)) {
    // Clock steps (PTP relock) move the media timeline: start a new one.
    const uint64_t steps = g.client.header()->clock_steps.load(std::memory_order_relaxed);
    if (steps != g.last_steps) {
      g.last_steps = steps;
      double f = a.frame_at(now_ns);
      g.base_frame = uint64_t(f);
      ++g.seed;
    }
    const double rel = a.frame_at(now_ns) - double(g.base_frame);
    const double zero = std::floor(rel / kZeroTimeStampPeriod) * kZeroTimeStampPeriod;
    *outSampleTime = zero;
    *outHostTime = ns_to_host(a.host_ns_at(double(g.base_frame) + zero));
  } else {
    const double rel = double(now_ns - g.free_base_ns) * g.sample_rate * 1e-9;
    const double zero = std::floor(rel / kZeroTimeStampPeriod) * kZeroTimeStampPeriod;
    *outSampleTime = zero;
    *outHostTime = ns_to_host(g.free_base_ns + int64_t(zero * 1e9 / g.sample_rate));
  }
  *outSeed = g.seed.load(std::memory_order_relaxed);
  return kAudioHardwareNoError;
}

OSStatus WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID, UInt32,
                           UInt32 op, Boolean* outWillDo, Boolean* outWillDoInPlace) {
  if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
  const bool will = op == kAudioServerPlugInIOOperationReadInput ||
                    op == kAudioServerPlugInIOOperationWriteMix;
  if (outWillDo) *outWillDo = will;
  if (outWillDoInPlace) *outWillDoInPlace = true;
  return kAudioHardwareNoError;
}

OSStatus DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID, AudioObjectID stream,
                       UInt32, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo* cycle,
                       void* ioMainBuffer, void*) {
  if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
  float* buf = static_cast<float*>(ioMainBuffer);
  const bool live = !g.free_running && g.client.is_open();

  if (op == kAudioServerPlugInIOOperationReadInput && stream == kObjectID_Stream_Input) {
    const UInt32 ch = g.in_channels;
    if (!live) {
      std::memset(buf, 0, size_t(frames) * ch * sizeof(float));
      return kAudioHardwareNoError;
    }
    const uint64_t start = g.base_frame + uint64_t(std::llround(cycle->mInputTime.mSampleTime));
    g.client.read_rx(start, buf, frames, ch);
  } else if (op == kAudioServerPlugInIOOperationWriteMix && stream == kObjectID_Stream_Output) {
    if (!live) return kAudioHardwareNoError;
    const uint64_t start = g.base_frame + uint64_t(std::llround(cycle->mOutputTime.mSampleTime));
    g.client.write_tx(start, buf, frames, g.out_channels);
    g.client.touch();
  }
  return kAudioHardwareNoError;
}

}  // namespace

// ---- factory ------------------------------------------------------------------

extern "C" __attribute__((visibility("default"))) void* VIRGIL_Create(CFAllocatorRef,
                                                                   CFUUIDRef inRequestedTypeUUID) {
  if (!CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return nullptr;
  return gDriverRef;
}
