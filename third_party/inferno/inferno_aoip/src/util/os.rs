use thread_priority::Error;

#[cfg(target_family = "unix")]
pub fn set_current_thread_realtime(priority_hint: u8) -> Result<(), Error> {
  use thread_priority::thread_native_id;
  use thread_priority::unix::set_thread_priority_and_policy;
  use thread_priority::{
    RealtimeThreadSchedulePolicy, ThreadPriority, ThreadPriorityValue, ThreadSchedulePolicy,
  };

  set_thread_priority_and_policy(
    thread_native_id(),
    ThreadPriority::Crossplatform(ThreadPriorityValue::try_from(priority_hint).unwrap()),
    ThreadSchedulePolicy::Realtime(RealtimeThreadSchedulePolicy::Fifo),
  )
}

#[cfg(windows)]
#[link(name = "avrt")]
extern "system" {
  fn AvSetMmThreadCharacteristicsW(task_name: *const u16, task_index: *mut u32) -> isize;
  fn AvSetMmThreadPriority(handle: isize, priority: i32) -> i32;
}

#[cfg(windows)]
#[link(name = "kernel32")]
extern "system" {
  fn GetCurrentThread() -> isize;
  fn SetThreadPriority(thread: isize, priority: i32) -> i32;
  fn GetLastError() -> u32;
}

/// Virgil patch. On Windows the thread joins the MMCSS "Pro Audio" class at
/// AVRT_PRIORITY_CRITICAL, like the audio engine, and its priority is then
/// left to MMCSS (a SetThreadPriority afterwards would replace the MMCSS
/// real-time boost with THREAD_PRIORITY_HIGHEST). Without MMCSS (service
/// stopped) it falls back to THREAD_PRIORITY_TIME_CRITICAL and returns an
/// error saying so, for the caller to log; the MMCSS outcome is logged here.
#[cfg(not(target_family = "unix"))]
pub fn set_current_thread_realtime(_priority_hint: u8) -> Result<(), Error> {
  #[cfg(windows)]
  unsafe {
    const AVRT_PRIORITY_CRITICAL: i32 = 2;
    const THREAD_PRIORITY_TIME_CRITICAL: i32 = 15;
    let task: Vec<u16> = "Pro Audio".encode_utf16().chain(std::iter::once(0)).collect();
    let mut index = 0u32;
    let h = AvSetMmThreadCharacteristicsW(task.as_ptr(), &mut index);
    if h != 0 {
      if AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL) == 0 {
        log::warn!(
          "thread {:?} joined MMCSS \"Pro Audio\" but could not set critical priority (error {})",
          std::thread::current().name(),
          GetLastError()
        );
      } else {
        log::info!(
          "thread {:?} runs in MMCSS \"Pro Audio\" at critical priority",
          std::thread::current().name()
        );
      }
      return Ok(());
    }
    let mmcss_error = GetLastError();
    if SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0 {
      log::debug!("AvSetMmThreadCharacteristicsW failed (error {mmcss_error})");
      return Err(Error::Ffi("MMCSS unavailable, using THREAD_PRIORITY_TIME_CRITICAL instead"));
    }
    return Err(Error::OS(GetLastError() as i32));
  }
  #[cfg(not(windows))]
  thread_priority::set_current_thread_priority(thread_priority::ThreadPriority::Max)
}
