// Thin OS layer: clocks, precise sleeping and real-time scheduling.
#pragma once

#include <cstdint>

namespace virgil {

// Monotonic clock in nanoseconds. On macOS this is mach_absolute_time()
// scaled to ns, so CoreAudio host time and virgil time share one timebase.
int64_t mono_ns();

// Wall clock, ns since the Unix epoch (UTC).
int64_t realtime_ns();

// Sleep until mono_ns() >= deadline. The final `spin_ns` are busy-waited to
// shave scheduler wake-up jitter off the deadline.
void sleep_until_ns(int64_t deadline_ns, int64_t spin_ns = 0);

// Put the calling thread in the OS's real-time / pro-audio class.
// `period_ns` is the expected wake-up period (used by macOS time-constraint
// policy). Returns false if the OS refused (e.g. missing RLIMIT_RTPRIO).
bool set_realtime_priority(int priority, int64_t period_ns);

// Lock all current and future pages so the audio path never page-faults.
bool lock_memory();
// Process-wide timer setup for a background audio daemon. On Windows: 1 ms
// timer resolution, and opt out of the power throttling with which Windows 11
// ignores that request (and slows timers) for processes without a visible
// window, e.g. services. No-op elsewhere.
void tune_process_timers();

uint32_t process_id();
bool process_alive(uint32_t pid);

}  // namespace virgil
