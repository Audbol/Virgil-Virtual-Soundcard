#include "virgil/platform.h"

#include <algorithm>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <mutex>
#include <string>
#endif

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

namespace {
HANDLE high_resolution_timer() {
  return CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
}
}  // namespace

bool precise_sleep_available() {
  static const bool ok = [] {
    HANDLE h = high_resolution_timer();
    if (h) CloseHandle(h);
    return h != nullptr;
  }();
  return ok;
}

void sleep_until_ns(int64_t deadline, int64_t spin_ns) {
  // A high-resolution waitable timer (Win10 1803+) wakes within a fraction
  // of a millisecond, so it also suits a 0.5 ms tick; the spin covers the
  // rest. Without it (older Windows) sleeps end on the 1 ms timer interrupt.
  // Closed when the thread ends (each engine reload and each ASIO start
  // runs a new thread: the handles piled up).
  struct Timer {
    HANDLE h;
    ~Timer() {
      if (h) CloseHandle(h);
    }
  };
  thread_local Timer t{[] {
    HANDLE h = high_resolution_timer();
    if (!h) h = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    return h;
  }()};
  const HANDLE timer = t.h;
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

namespace {
// What set_realtime_priority could not do, reported once by
// take_scheduling_warnings() (this library has no logger).
std::mutex g_sched_warn_mutex;
std::string g_sched_warn;
bool g_sched_warned_throttle = false, g_sched_warned_mmcss = false, g_sched_warned_prio = false;

void sched_warn(bool* once, const std::string& msg) {
  std::lock_guard<std::mutex> l(g_sched_warn_mutex);
  if (*once) return;
  *once = true;
  if (!g_sched_warn.empty()) g_sched_warn += "; ";
  g_sched_warn += msg;
}

// THREAD_POWER_THROTTLING_STATE via SetThreadInformation (Windows 10 1709+;
// looked up dynamically for older systems and SDK headers): EcoQoS / power
// throttling otherwise runs a background thread on efficiency cores at low
// clock speeds, which wakes the audio threads late.
bool opt_out_of_thread_throttling() {
  struct Throttling {
    ULONG Version, ControlMask, StateMask;
  } st{1 /* THREAD_POWER_THROTTLING_CURRENT_VERSION */, 0x1 /* EXECUTION_SPEED */, 0 /* never throttle */};
  using Fn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
  static const Fn fn = reinterpret_cast<Fn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadInformation")));
  if (!fn) return true;  // older Windows: no throttling to opt out of
  return fn(GetCurrentThread(), 3 /* ThreadPowerThrottling */, &st, sizeof st) != 0;
}
}  // namespace

bool set_realtime_priority(int, int64_t) {
  // Undone when the thread ends: the MMCSS registration and the 1 ms timer
  // period were left behind by every audio thread (a new one per engine
  // reload and per ASIO start).
  struct RtState {
    HANDLE mmcss = nullptr;
    bool period = false;
    ~RtState() {
      if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
      if (period) timeEndPeriod(1);
    }
  };
  thread_local RtState rt;
  if (!rt.period) rt.period = timeBeginPeriod(1) == TIMERR_NOERROR;
  DWORD task_index = 0;
  HANDLE h = rt.mmcss ? rt.mmcss : AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
  rt.mmcss = h;
  if (h) AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL);
  else
    sched_warn(&g_sched_warned_mmcss, "MMCSS \"Pro Audio\" scheduling refused (error " +
                                          std::to_string(GetLastError()) + ")");
  if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
    sched_warn(&g_sched_warned_prio,
               "time-critical thread priority refused (error " + std::to_string(GetLastError()) + ")");
  if (!opt_out_of_thread_throttling())
    sched_warn(&g_sched_warned_throttle, "could not opt an audio thread out of power throttling (error " +
                                             std::to_string(GetLastError()) + ")");
  return h != nullptr;
}

std::string take_scheduling_warnings() {
  std::lock_guard<std::mutex> l(g_sched_warn_mutex);
  std::string r;
  r.swap(g_sched_warn);
  return r;
}

void tune_process_timers() {
  timeBeginPeriod(1);
  // PROCESS_POWER_THROTTLING_STATE via SetProcessInformation (Windows 8+;
  // the timer-resolution flag needs Windows 11). Looked up dynamically so
  // older systems and SDK headers are fine.
  struct Throttling {
    ULONG Version, ControlMask, StateMask;
  } st{1 /* CURRENT_VERSION */, 0x1 /* EXECUTION_SPEED */ | 0x4 /* IGNORE_TIMER_RESOLUTION */,
       0 /* i.e. do not throttle either */};
  using Fn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
  if (auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
          GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation")))) {
    if (!fn(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &st, sizeof st)) {
      st.ControlMask = 0x1;  // pre-Windows 11: execution speed only
      fn(GetCurrentProcess(), 4, &st, sizeof st);
    }
  }
}

bool lock_memory() {
  // Working-set locking on Windows is per-region; the rings are touched every
  // tick and stay resident. Raise the minimum working set as a hint.
  SetProcessWorkingSetSize(GetCurrentProcess(), 64 << 20, 256 << 20);
  return true;
}

uint32_t process_id() { return GetCurrentProcessId(); }
std::string host_name() {
  char buf[256];
  DWORD n = sizeof buf;
  if (!GetComputerNameExA(ComputerNameDnsHostname, buf, &n) && !GetComputerNameA(buf, &(n = sizeof buf)))
    return "";
  return std::string(buf, n);
}

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

bool precise_sleep_available() { return true; }

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
  // The kernel refuses a computation under its minimum real-time quantum
  // (50 us) or above the constraint. A quarter of a 0.5 ms tick (125 us) is
  // fine; shorter ticks get 100 us (within the 125 us minimum tick), and the
  // constraint is never below the computation.
  const int64_t computation = std::max<int64_t>(period_ns / 4, 100000);
  const int64_t constraint = std::max(period_ns / 2, computation);
  thread_time_constraint_policy_data_t pol;
  pol.period = to_abs(period_ns);
  pol.computation = to_abs(computation);
  pol.constraint = to_abs(constraint);
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

void tune_process_timers() {}

std::string take_scheduling_warnings() { return std::string(); }

uint32_t process_id() { return uint32_t(getpid()); }
std::string host_name() {
  char buf[256] = {};
  if (gethostname(buf, sizeof buf - 1) != 0) return "";
  std::string h(buf);
  return h.substr(0, h.find('.'));
}

bool process_alive(uint32_t pid) { return pid != 0 && (kill(pid_t(pid), 0) == 0 || errno == EPERM); }

#endif

}  // namespace virgil
