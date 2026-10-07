#include "virgil/platform.h"

#include <chrono>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <avrt.h>
#include <timeapi.h>
#elif defined(__APPLE__)
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#else
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

namespace virgil {

#if defined(_WIN32)

static int64_t qpc_freq() {
  static const int64_t f = [] {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return int64_t(li.QuadPart);
  }();
  return f;
}

int64_t mono_ns() {
  LARGE_INTEGER li;
  QueryPerformanceCounter(&li);
  const int64_t f = qpc_freq();
  const int64_t t = li.QuadPart;
  return (t / f) * 1000000000LL + (t % f) * 1000000000LL / f;
}

int64_t realtime_ns() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  return (int64_t(u.QuadPart) - 116444736000000000LL) * 100;  // 1601 -> 1970
}

void sleep_until_ns(int64_t deadline, int64_t spin_ns) {
  // A high-resolution waitable timer gets ~0.5 ms granularity on Win10 1803+;
  // the spin covers the rest.
  thread_local HANDLE timer = [] {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    if (!h) h = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    return h;
  }();
  int64_t now = mono_ns();
  int64_t sleep_ns = deadline - now - spin_ns;
  if (sleep_ns > 0 && timer) {
    LARGE_INTEGER due;
    due.QuadPart = -(sleep_ns / 100);  // relative, 100 ns units
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
      WaitForSingleObject(timer, INFINITE);
  }
  while (mono_ns() < deadline) YieldProcessor();
}

bool set_realtime_priority(int, int64_t) {
  timeBeginPeriod(1);
  DWORD task_index = 0;
  HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
  if (h) AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  return h != nullptr;
}

bool lock_memory() {
  // Working-set locking on Windows is per-region; the rings are touched every
  // tick and stay resident. Raise the minimum working set as a hint.
  SetProcessWorkingSetSize(GetCurrentProcess(), 64 << 20, 256 << 20);
  return true;
}

uint32_t process_id() { return GetCurrentProcessId(); }

bool process_alive(uint32_t pid) {
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
  DWORD r = WaitForSingleObject(h, 0);
  CloseHandle(h);
  return r == WAIT_TIMEOUT;
}

#else  // POSIX

#if defined(__APPLE__)
static const mach_timebase_info_data_t& timebase() {
  static mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t t;
    mach_timebase_info(&t);
    return t;
  }();
  return tb;
}

int64_t mono_ns() {
  const auto& tb = timebase();
  return int64_t((__uint128_t)mach_absolute_time() * tb.numer / tb.denom);
}

static uint64_t ns_to_mach(int64_t ns) {
  const auto& tb = timebase();
  return uint64_t((__uint128_t)ns * tb.denom / tb.numer);
}
#else
int64_t mono_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
#endif

int64_t realtime_ns() {
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

static inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}

void sleep_until_ns(int64_t deadline, int64_t spin_ns) {
  int64_t wake = deadline - spin_ns;
#if defined(__APPLE__)
  if (wake > mono_ns()) mach_wait_until(ns_to_mach(wake));
#else
  if (wake > mono_ns()) {
    timespec ts;
    ts.tv_sec = time_t(wake / 1000000000LL);
    ts.tv_nsec = long(wake % 1000000000LL);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
  }
#endif
  while (mono_ns() < deadline) cpu_relax();
}

bool set_realtime_priority(int priority, int64_t period_ns) {
#if defined(__APPLE__)
  const auto& tb = timebase();
  auto to_abs = [&](int64_t ns) { return uint32_t(double(ns) * tb.denom / tb.numer); };
  thread_time_constraint_policy_data_t pol;
  pol.period = to_abs(period_ns);
  pol.computation = to_abs(period_ns / 4);
  pol.constraint = to_abs(period_ns / 2);
  pol.preemptible = 1;
  (void)priority;
  return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                           reinterpret_cast<thread_policy_t>(&pol),
                           THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
#else
  (void)period_ns;
  sched_param sp{};
  sp.sched_priority = priority;
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
#endif
}

bool lock_memory() { return mlockall(MCL_CURRENT | MCL_FUTURE) == 0; }

uint32_t process_id() { return uint32_t(getpid()); }

bool process_alive(uint32_t pid) { return pid != 0 && (kill(pid_t(pid), 0) == 0 || errno == EPERM); }

#endif

}  // namespace virgil
