// The few PulseAudio client API declarations desktop audio uses, written
// from the documented (stable since PulseAudio 1.0) ABI, so building needs no
// libpulse-dev and running does not need libpulse at all: the library is
// loaded with dlopen("libpulse.so.0") when desktop audio is first used.
// PipeWire's pipewire-pulse serves the same protocol.
//
// Structs are declared only up to the last member read here: libpulse owns
// them and hands out pointers, so a prefix with the same layout is enough.
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

struct pa_threaded_mainloop;
struct pa_mainloop_api;
struct pa_context;
struct pa_stream;
struct pa_operation;
struct pa_proplist;
struct pa_spawn_api;

typedef uint64_t pa_usec_t;
typedef uint32_t pa_volume_t;

enum { PA_CHANNELS_MAX_ = 32 };
constexpr uint32_t PA_INVALID_INDEX_ = 0xFFFFFFFFu;

enum pa_sample_format_t { PA_SAMPLE_FLOAT32LE_ = 5 };
struct pa_sample_spec {
  int format;  // pa_sample_format_t
  uint32_t rate;
  uint8_t channels;
};
struct pa_channel_map {
  uint8_t channels;
  int map[PA_CHANNELS_MAX_];
};
struct pa_cvolume {
  uint8_t channels;
  pa_volume_t values[PA_CHANNELS_MAX_];
};
struct pa_buffer_attr {
  uint32_t maxlength, tlength, prebuf, minreq, fragsize;
};

enum {  // pa_context_state_t
  PA_CONTEXT_UNCONNECTED_ = 0,
  PA_CONTEXT_CONNECTING_ = 1,
  PA_CONTEXT_AUTHORIZING_ = 2,
  PA_CONTEXT_SETTING_NAME_ = 3,
  PA_CONTEXT_READY_ = 4,
  PA_CONTEXT_FAILED_ = 5,
  PA_CONTEXT_TERMINATED_ = 6,
};
enum {  // pa_stream_state_t
  PA_STREAM_UNCONNECTED_ = 0,
  PA_STREAM_CREATING_ = 1,
  PA_STREAM_READY_ = 2,
  PA_STREAM_FAILED_ = 3,
  PA_STREAM_TERMINATED_ = 4,
};
enum {  // pa_context_flags_t
  PA_CONTEXT_NOFLAGS_ = 0,
  PA_CONTEXT_NOAUTOSPAWN_ = 1,
};
enum {  // pa_stream_flags_t
  PA_STREAM_INTERPOLATE_TIMING_ = 0x0002,
  PA_STREAM_NO_REMAP_CHANNELS_ = 0x0010,  // channels by index, not by position
  PA_STREAM_NO_REMIX_CHANNELS_ = 0x0020,
  PA_STREAM_AUTO_TIMING_UPDATE_ = 0x0008,
  PA_STREAM_DONT_MOVE_ = 0x0200,
  PA_STREAM_ADJUST_LATENCY_ = 0x2000,
  PA_STREAM_DONT_INHIBIT_AUTO_SUSPEND_ = 0x8000,
};
enum {  // pa_subscription_mask_t
  PA_SUBSCRIPTION_MASK_SINK_ = 0x0001,
  PA_SUBSCRIPTION_MASK_SOURCE_ = 0x0002,
  PA_SUBSCRIPTION_MASK_SINK_INPUT_ = 0x0004,
  PA_SUBSCRIPTION_MASK_SERVER_ = 0x0080,
};
enum {  // pa_subscription_event_type_t
  PA_SUBSCRIPTION_EVENT_SINK_ = 0x0000,
  PA_SUBSCRIPTION_EVENT_SOURCE_ = 0x0001,
  PA_SUBSCRIPTION_EVENT_SINK_INPUT_ = 0x0002,
  PA_SUBSCRIPTION_EVENT_SERVER_ = 0x0007,
  PA_SUBSCRIPTION_EVENT_FACILITY_MASK_ = 0x000F,
  PA_SUBSCRIPTION_EVENT_NEW_ = 0x0000,
  PA_SUBSCRIPTION_EVENT_CHANGE_ = 0x0010,
  PA_SUBSCRIPTION_EVENT_REMOVE_ = 0x0020,
  PA_SUBSCRIPTION_EVENT_TYPE_MASK_ = 0x0030,
};
enum { PA_SEEK_RELATIVE_ = 0 };

struct pa_server_info {  // prefix
  const char* user_name;
  const char* host_name;
  const char* server_version;
  const char* server_name;
  pa_sample_spec sample_spec;
  const char* default_sink_name;
  const char* default_source_name;
};
struct pa_sink_info {  // prefix
  const char* name;
  uint32_t index;
  const char* description;
  pa_sample_spec sample_spec;
  pa_channel_map channel_map;
  uint32_t owner_module;
  pa_cvolume volume;
  int mute;
  uint32_t monitor_source;
  const char* monitor_source_name;
  pa_usec_t latency;
  const char* driver;
  int flags;  // pa_sink_flags_t
  pa_proplist* proplist;
};
struct pa_source_info {  // prefix
  const char* name;
  uint32_t index;
  const char* description;
  pa_sample_spec sample_spec;
  pa_channel_map channel_map;
  uint32_t owner_module;
  pa_cvolume volume;
  int mute;
  uint32_t monitor_of_sink;  // PA_INVALID_INDEX_ unless it is a sink's monitor
  const char* monitor_of_sink_name;
};
struct pa_sink_input_info {  // prefix
  uint32_t index;
  const char* name;
  uint32_t owner_module;
  uint32_t client;
  uint32_t sink;
  pa_sample_spec sample_spec;
  pa_channel_map channel_map;
  pa_cvolume volume;
  pa_usec_t buffer_usec;
  pa_usec_t sink_usec;
  const char* resample_method;
  const char* driver;
  int mute;
  pa_proplist* proplist;
  int corked;
};

typedef void (*pa_context_notify_cb_t)(pa_context*, void*);
typedef void (*pa_context_success_cb_t)(pa_context*, int success, void*);
typedef void (*pa_context_subscribe_cb_t)(pa_context*, int event, uint32_t idx, void*);
typedef void (*pa_server_info_cb_t)(pa_context*, const pa_server_info*, void*);
typedef void (*pa_sink_info_cb_t)(pa_context*, const pa_sink_info*, int eol, void*);
typedef void (*pa_source_info_cb_t)(pa_context*, const pa_source_info*, int eol, void*);
typedef void (*pa_sink_input_info_cb_t)(pa_context*, const pa_sink_input_info*, int eol, void*);
typedef void (*pa_stream_notify_cb_t)(pa_stream*, void*);
typedef void (*pa_stream_request_cb_t)(pa_stream*, size_t nbytes, void*);
typedef void (*pa_free_cb_t)(void*);

}  // extern "C"

// Function table, filled from the library by name.
struct PulseApi {
  pa_threaded_mainloop* (*threaded_mainloop_new)();
  void (*threaded_mainloop_free)(pa_threaded_mainloop*);
  int (*threaded_mainloop_start)(pa_threaded_mainloop*);
  void (*threaded_mainloop_stop)(pa_threaded_mainloop*);
  void (*threaded_mainloop_lock)(pa_threaded_mainloop*);
  void (*threaded_mainloop_unlock)(pa_threaded_mainloop*);
  pa_mainloop_api* (*threaded_mainloop_get_api)(pa_threaded_mainloop*);

  pa_proplist* (*proplist_new)();
  void (*proplist_free)(pa_proplist*);
  int (*proplist_sets)(pa_proplist*, const char*, const char*);
  const char* (*proplist_gets)(const pa_proplist*, const char*);

  pa_context* (*context_new_with_proplist)(pa_mainloop_api*, const char*, const pa_proplist*);
  void (*context_unref)(pa_context*);
  int (*context_connect)(pa_context*, const char*, int, const pa_spawn_api*);
  void (*context_disconnect)(pa_context*);
  int (*context_get_state)(const pa_context*);
  int (*context_errno)(const pa_context*);
  void (*context_set_state_callback)(pa_context*, pa_context_notify_cb_t, void*);
  void (*context_set_subscribe_callback)(pa_context*, pa_context_subscribe_cb_t, void*);
  pa_operation* (*context_subscribe)(pa_context*, int, pa_context_success_cb_t, void*);
  pa_operation* (*context_get_server_info)(pa_context*, pa_server_info_cb_t, void*);
  pa_operation* (*context_get_sink_info_list)(pa_context*, pa_sink_info_cb_t, void*);
  pa_operation* (*context_get_sink_info_by_index)(pa_context*, uint32_t, pa_sink_info_cb_t, void*);
  pa_operation* (*context_get_source_info_list)(pa_context*, pa_source_info_cb_t, void*);
  pa_operation* (*context_get_source_info_by_index)(pa_context*, uint32_t, pa_source_info_cb_t, void*);
  pa_operation* (*context_get_sink_input_info_list)(pa_context*, pa_sink_input_info_cb_t, void*);
  pa_operation* (*context_get_sink_input_info)(pa_context*, uint32_t, pa_sink_input_info_cb_t, void*);
  void (*operation_unref)(pa_operation*);
  const char* (*strerror)(int);
  const char* (*get_library_version)();

  pa_stream* (*stream_new_with_proplist)(pa_context*, const char*, const pa_sample_spec*, const pa_channel_map*,
                                         pa_proplist*);
  void (*stream_unref)(pa_stream*);
  int (*stream_connect_record)(pa_stream*, const char*, const pa_buffer_attr*, int);
  int (*stream_connect_playback)(pa_stream*, const char*, const pa_buffer_attr*, int, const pa_cvolume*,
                                 pa_stream*);
  int (*stream_disconnect)(pa_stream*);
  int (*stream_get_state)(const pa_stream*);
  void (*stream_set_state_callback)(pa_stream*, pa_stream_notify_cb_t, void*);
  void (*stream_set_read_callback)(pa_stream*, pa_stream_request_cb_t, void*);
  void (*stream_set_write_callback)(pa_stream*, pa_stream_request_cb_t, void*);
  int (*stream_peek)(pa_stream*, const void**, size_t*);
  int (*stream_drop)(pa_stream*);
  size_t (*stream_readable_size)(const pa_stream*);
  int (*stream_begin_write)(pa_stream*, void**, size_t*);
  int (*stream_write)(pa_stream*, const void*, size_t, pa_free_cb_t, int64_t, int);
  int (*stream_set_monitor_stream)(pa_stream*, uint32_t);
  int (*stream_get_latency)(pa_stream*, pa_usec_t*, int*);
  const pa_buffer_attr* (*stream_get_buffer_attr)(pa_stream*);
};
