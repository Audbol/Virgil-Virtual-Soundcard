use thread_priority::{thread_native_id, Error};

#[cfg(target_family = "unix")]
pub fn set_current_thread_realtime(priority_hint: u8) -> Result<(), Error> {
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

#[cfg(not(target_family = "unix"))]
pub fn set_current_thread_realtime(_priority_hint: u8) -> Result<(), Error> {
  // Virgil: fixed for non-Unix targets (type and import were Unix-only), and
  // on Windows also join the MMCSS "Pro Audio" class like the audio engine,
  // so the scheduler does not starve or throttle the transmit thread.
  #[cfg(windows)]
  unsafe {
    let task: Vec<u16> = "Pro Audio".encode_utf16().chain(std::iter::once(0)).collect();
    let mut index = 0u32;
    let h = AvSetMmThreadCharacteristicsW(task.as_ptr(), &mut index);
    if h != 0 {
      AvSetMmThreadPriority(h, 2 /* AVRT_PRIORITY_HIGH */);
    }
  }
  thread_priority::set_current_thread_priority(thread_priority::ThreadPriority::Max)
}
