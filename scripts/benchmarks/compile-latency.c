#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static int serial_fd = -1;
extern char** environ;

static void fail(const char* operation) {
  dprintf(STDOUT_FILENO, "COMPILEBENCH FAIL operation=%s errno=%d\n", operation, errno);
  exit(1);
}

static uint64_t now_ns(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    fail("clock");
  return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static uint64_t timeval_us(struct timeval t) {
  return (uint64_t)t.tv_sec * 1000000ULL + (uint64_t)t.tv_usec;
}

#define SYSCALL_LATENCY_BUCKET_COUNT 16
#define ACTIVITY_DURATION_BUCKET_COUNT 16
#define ACTIVITY_INTERRUPT_VECTOR_COUNT 256
#define ACTIVITY_USER_RETURN_STAGE_COUNT 12
#define ACTIVITY_USER_RETURN_SAMPLE_PERIOD 64
#define ACTIVITY_USER_ENTRY_SAMPLE_PERIOD 256
#define BENCHMARK_ABLATE_INTERRUPT_RETURN 1
#define BENCHMARK_ABLATE_SYSCALL_RETURN 2
#define BENCHMARK_ABLATE_VM_VECTOR_VALIDATION 1
#define BENCHMARK_ABLATE_VM_GUARD_TERMINATION 2
#define BENCHMARK_ABLATE_VM_GUARD_RECURSIVE_EVENTS 4
#define BENCHMARK_ABLATE_VM_TABLE_RETIREMENT 8
#define BENCHMARK_ABLATE_VM_REVERSE_FAULT_LOOKUP 16
#define SYSCALL_TIMING_RAW_SLOT_COUNT 512
#define SYSCALL_TIMING_SLOT_COUNT (SYSCALL_TIMING_RAW_SLOT_COUNT + 1)
#define VM_DIAGNOSTIC_COUNTER_COUNT 61

static unsigned benchmark_user_return_ablation;
static int benchmark_syscall_timing;
static int benchmark_vm_diagnostics;
static int benchmark_vm_ablation_api;
static unsigned benchmark_vm_ablation;

static const char* vm_diagnostic_names[VM_DIAGNOSTIC_COUNTER_COUNT] = {
    "mmap_calls",
    "mmap_anon_calls",
    "mmap_file_calls",
    "mmap_pages",
    "mmap_len_1",
    "mmap_len_2_3",
    "mmap_len_4_15",
    "mmap_len_16_63",
    "mmap_len_64_255",
    "mmap_len_256_plus",
    "publish_calls",
    "publish_object_count",
    "publish_overlap_probe_visits",
    "publish_overlap_hits",
    "publish_commit_retries",
    "reservation_snapshots",
    "reservation_extents",
    "reservation_scratch_allocations",
    "munmap_calls",
    "munmap_pages",
    "munmap_len_1",
    "munmap_len_2_3",
    "munmap_len_4_15",
    "munmap_len_16_63",
    "munmap_len_64_255",
    "munmap_len_256_plus",
    "remove_calls",
    "remove_object_count",
    "remove_object_visits",
    "remove_slice_calls",
    "remove_affected_objects",
    "allows_calls",
    "allows_object_count",
    "allows_object_visits",
    "fault_in_range_calls",
    "fault_in_range_pages",
    "fault_in_object_visits",
    "fault_in_present",
    "fault_in_copy_on_write",
    "fault_in_trap",
    "fault_calls",
    "fault_object_count",
    "fault_object_visits",
    "fault_resolved",
    "fault_backing",
    "fault_unhandled",
    "guard_entries",
    "guard_recursive_entries",
    "discard_tracked_pages",
    "discard_mapped_pages",
    "table_retirement_scans",
    "detach_pte_entries",
    "detach_pde_entries",
    "detach_pdpt_entries",
    "detach_tables",
    "invalidation_active",
    "invalidation_inactive",
    "ablation_vector_payload_checks_skipped",
    "ablation_guard_termination_scopes_skipped",
    "ablation_guard_recursive_event_scopes_skipped",
    "ablation_table_retirement_scans_skipped",
};

static const char* activity_user_return_stage_names[] = {
    "interrupt_tail",     "syscall_tail",     "interrupt_work",       "syscall_work",
    "checkpoint",         "process_stop",     "deferred_fault",       "event",
    "interrupt_affinity", "syscall_affinity", "interrupt_accounting", "syscall_accounting"};

_Static_assert(sizeof(activity_user_return_stage_names) /
                       sizeof(activity_user_return_stage_names[0]) ==
                   ACTIVITY_USER_RETURN_STAGE_COUNT,
               "user-return stage names must match the kernel ABI");

struct activity_snapshot {
  uint64_t interrupt_count;
  uint64_t exception_count;
  uint64_t hardware_interrupt_count;
  uint64_t other_interrupt_count;
  uint64_t interrupt_vector_counts[ACTIVITY_INTERRUPT_VECTOR_COUNT];
  uint64_t interrupt_duration_buckets[ACTIVITY_DURATION_BUCKET_COUNT];
  uint64_t page_fault_duration_buckets[ACTIVITY_DURATION_BUCKET_COUNT];
  uint64_t scheduler_timer_duration_buckets[ACTIVITY_DURATION_BUCKET_COUNT];
  uint64_t hard_dispatch_count;
  uint64_t hard_duration_buckets[ACTIVITY_DURATION_BUCKET_COUNT];
  uint64_t threaded_dispatch_count;
  uint64_t threaded_duration_buckets[ACTIVITY_DURATION_BUCKET_COUNT];
  uint64_t scheduler_timer_ticks;
  uint64_t schedule_calls;
  uint64_t same_thread_selections;
  uint64_t context_switches;
  uint64_t idle_selections;
  uint64_t scheduler_idle_fallbacks;
  uint64_t scheduler_idle_fallback_current_ready;
  uint64_t scheduler_idle_fallback_current_pending;
  uint64_t scheduler_no_eligible_selections;
  uint64_t ready_queue_scan_entries;
  uint64_t ready_queue_candidate_visits;
  uint64_t ready_queue_predicate_rejects;
  uint64_t ready_queue_selection_samples;
  uint64_t ready_queue_selection_duration_buckets[ACTIVITY_DURATION_BUCKET_COUNT];
  uint64_t time_accounting_samples;
  uint64_t time_accounting_duration_buckets[ACTIVITY_DURATION_BUCKET_COUNT];
  uint64_t idle_halt_entries;
  uint64_t framebuffer_flips;
  uint64_t framebuffer_cells;
  uint64_t framebuffer_duration_buckets[ACTIVITY_DURATION_BUCKET_COUNT];
  uint64_t user_return_stage_samples[ACTIVITY_USER_RETURN_STAGE_COUNT];
  uint64_t user_return_stage_total_nanoseconds[ACTIVITY_USER_RETURN_STAGE_COUNT];
  uint64_t user_return_stage_duration_buckets[ACTIVITY_USER_RETURN_STAGE_COUNT]
                                             [ACTIVITY_DURATION_BUCKET_COUNT];
  uint64_t user_return_fault_handled_samples;
  uint64_t user_return_fault_fallback_samples;
  uint64_t user_return_interrupt_affinity_waited_samples;
  uint64_t user_return_syscall_affinity_waited_samples;
  uint64_t user_return_interrupt_ablation_eligible;
  uint64_t user_return_interrupt_ablation_fast;
  uint64_t user_return_interrupt_ablation_fallback;
  uint64_t user_return_syscall_ablation_eligible;
  uint64_t user_return_syscall_ablation_fast;
  uint64_t user_return_syscall_ablation_fallback;
  uint64_t user_entry_capture_calls;
  uint64_t user_entry_restore_calls;
  uint64_t user_entry_capture_samples;
  uint64_t user_entry_restore_samples;
  uint64_t user_entry_capture_tsc_total;
  uint64_t user_entry_restore_tsc_total;
  uint64_t user_entry_empty_tsc_samples;
  uint64_t user_entry_empty_tsc_total;
  uint64_t user_entry_capture_tsc_buckets[ACTIVITY_DURATION_BUCKET_COUNT];
  uint64_t user_entry_restore_tsc_buckets[ACTIVITY_DURATION_BUCKET_COUNT];
  uint64_t user_entry_empty_tsc_buckets[ACTIVITY_DURATION_BUCKET_COUNT];
};

_Static_assert(sizeof(struct activity_snapshot) == 689 * sizeof(uint64_t),
               "activity snapshot layout must match the kernel ABI");

struct syscall_timing_entry {
  uint64_t calls;
  uint64_t kernel_nanoseconds;
};

struct syscall_timing_snapshot {
  struct syscall_timing_entry slots[SYSCALL_TIMING_SLOT_COUNT];
};

_Static_assert(sizeof(struct syscall_timing_snapshot) == 1026 * sizeof(uint64_t),
               "syscall timing snapshot layout must match the kernel ABI");

struct vm_diagnostic_snapshot {
  uint64_t counters[VM_DIAGNOSTIC_COUNTER_COUNT];
};

_Static_assert(sizeof(struct vm_diagnostic_snapshot) == 61 * sizeof(uint64_t),
               "VM diagnostic snapshot layout must match the kernel ABI");

static int reaped_child_syscall_count(uint64_t* count) {
  uint64_t result = 0;
  long status = syscall(SYS_syslog, 11, &result, sizeof(result));
  if (status != (long)sizeof(result))
    return 0;
  *count = result;
  return 1;
}

static int reaped_child_syscall_latency(uint64_t* buckets) {
  uint64_t result[SYSCALL_LATENCY_BUCKET_COUNT] = {0};
  long status = syscall(SYS_syslog, 12, result, sizeof(result));
  if (status != (long)sizeof(result))
    return 0;
  memcpy(buckets, result, sizeof(result));
  return 1;
}

static int activity_snapshot(struct activity_snapshot* result) {
  long status = syscall(SYS_syslog, 13, result, sizeof(*result));
  return status == (long)sizeof(*result);
}

static int activity_delta(const struct activity_snapshot* before,
                          const struct activity_snapshot* after,
                          struct activity_snapshot* result) {
  const uint64_t* before_words = (const uint64_t*)before;
  const uint64_t* after_words = (const uint64_t*)after;
  uint64_t* result_words = (uint64_t*)result;
  size_t count = sizeof(*result) / sizeof(uint64_t);
  for (size_t i = 0; i < count; ++i) {
    if (after_words[i] < before_words[i])
      return 0;
    result_words[i] = after_words[i] - before_words[i];
  }
  return 1;
}

static int syscall_timing_snapshot(struct syscall_timing_snapshot* result) {
  long status = syscall(SYS_syslog, 16, result, sizeof(*result));
  return status == (long)sizeof(*result);
}

static int syscall_timing_delta(const struct syscall_timing_snapshot* before,
                                const struct syscall_timing_snapshot* after,
                                struct syscall_timing_snapshot* result) {
  for (size_t i = 0; i < SYSCALL_TIMING_SLOT_COUNT; ++i) {
    if (after->slots[i].calls < before->slots[i].calls ||
        after->slots[i].kernel_nanoseconds < before->slots[i].kernel_nanoseconds) {
      return 0;
    }
    result->slots[i].calls = after->slots[i].calls - before->slots[i].calls;
    result->slots[i].kernel_nanoseconds =
        after->slots[i].kernel_nanoseconds - before->slots[i].kernel_nanoseconds;
  }
  return 1;
}

static int vm_diagnostic_snapshot(struct vm_diagnostic_snapshot* result) {
  long status = syscall(SYS_syslog, 18, result, sizeof(*result));
  return status == (long)sizeof(*result);
}

static int vm_diagnostic_delta(const struct vm_diagnostic_snapshot* before,
                               const struct vm_diagnostic_snapshot* after,
                               struct vm_diagnostic_snapshot* result) {
  for (size_t i = 0; i < VM_DIAGNOSTIC_COUNTER_COUNT; ++i) {
    if (after->counters[i] < before->counters[i]) {
      return 0;
    }
    result->counters[i] = after->counters[i] - before->counters[i];
  }
  return 1;
}

static void gate(const char* phase) {
  printf("COMPILEBENCH READY phase=%s\n", phase);
  for (;;) {
    char c;
    ssize_t n = read(serial_fd, &c, 1);
    if (n == 1 && c == 'g') {
      printf("COMPILEBENCH ACK phase=%s\n", phase);
      return;
    }
    if (n == 1 || (n < 0 && errno == EINTR))
      continue;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      fail("gate-read");
    struct pollfd p = {serial_fd, POLLIN, 0};
    int rc;
    do {
      // Pedigree's x86 serial device is polling-only and does not publish a
      // readiness edge, so an infinite poll can strand the benchmark gate.
      rc = poll(&p, 1, 10);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0 || (p.revents & (POLLERR | POLLNVAL | POLLHUP)))
      fail("gate-poll");
  }
}

static void metric(const char* phase, uint64_t start, uint64_t end, int rc,
                   const struct rusage* usage, uint64_t checksum, int have_syscalls,
                   uint64_t syscalls, int have_syscall_latency, const uint64_t* syscall_latency,
                   int have_syscall_timing, const struct syscall_timing_snapshot* syscall_timing,
                   int have_vm_diagnostics, const struct vm_diagnostic_snapshot* vm_diagnostics,
                   int have_activity, const struct activity_snapshot* activity) {
  printf(
      "COMPILEBENCH metric phase=%s total_us=%llu rc=%d user_us=%llu system_us=%llu "
      "minor_faults=%ld major_faults=%ld in_blocks=%ld out_blocks=%ld "
      "voluntary_switches=%ld involuntary_switches=%ld checksum=%llu "
      "benchmark_user_return_ablation=%u benchmark_syscall_timing=%d "
      "benchmark_vm_diagnostics=%d benchmark_vm_ablation_api=%d "
      "benchmark_vm_ablation=%u",
      phase, (unsigned long long)((end - start) / 1000), rc,
      (unsigned long long)timeval_us(usage->ru_utime),
      (unsigned long long)timeval_us(usage->ru_stime), usage->ru_minflt, usage->ru_majflt,
      usage->ru_inblock, usage->ru_oublock, usage->ru_nvcsw, usage->ru_nivcsw,
      (unsigned long long)checksum, benchmark_user_return_ablation, benchmark_syscall_timing,
      benchmark_vm_diagnostics, benchmark_vm_ablation_api, benchmark_vm_ablation);
  if (have_syscalls)
    printf(" syscalls=%llu", (unsigned long long)syscalls);
  if (have_syscall_latency) {
    for (unsigned i = 0; i < SYSCALL_LATENCY_BUCKET_COUNT; ++i)
      printf(" syscall_h%u=%llu", i, (unsigned long long)syscall_latency[i]);
  }
  if (have_syscall_timing) {
    uint64_t calls = 0;
    uint64_t kernel_nanoseconds = 0;
    for (unsigned i = 0; i < SYSCALL_TIMING_SLOT_COUNT; ++i) {
      calls += syscall_timing->slots[i].calls;
      kernel_nanoseconds += syscall_timing->slots[i].kernel_nanoseconds;
      if (syscall_timing->slots[i].calls || syscall_timing->slots[i].kernel_nanoseconds) {
        printf(" sc%u_calls=%llu sc%u_kernel_ns=%llu", i,
               (unsigned long long)syscall_timing->slots[i].calls, i,
               (unsigned long long)syscall_timing->slots[i].kernel_nanoseconds);
      }
    }
    printf(" syscall_timing_calls=%llu syscall_timing_kernel_ns=%llu", (unsigned long long)calls,
           (unsigned long long)kernel_nanoseconds);
  }
  if (have_vm_diagnostics) {
    for (unsigned i = 0; i < VM_DIAGNOSTIC_COUNTER_COUNT; ++i) {
      if (vm_diagnostics->counters[i]) {
        printf(" vm_%s=%llu", vm_diagnostic_names[i],
               (unsigned long long)vm_diagnostics->counters[i]);
      }
    }
  }
  if (have_activity) {
    printf(
        " activity_interrupts=%llu activity_exceptions=%llu activity_hardware_interrupts=%llu"
        " activity_other_interrupts=%llu activity_hard_dispatches=%llu"
        " activity_threaded_dispatches=%llu activity_scheduler_timer_ticks=%llu"
        " activity_schedule_calls=%llu activity_same_thread=%llu"
        " activity_context_switches=%llu activity_idle_selections=%llu"
        " activity_idle_fallbacks=%llu activity_idle_fallback_ready=%llu"
        " activity_idle_fallback_pending=%llu activity_no_eligible=%llu"
        " activity_ready_scans=%llu activity_ready_visits=%llu"
        " activity_ready_predicate_rejects=%llu activity_ready_selection_samples=%llu"
        " activity_time_accounting_samples=%llu activity_idle_halts=%llu"
        " activity_framebuffer_flips=%llu activity_framebuffer_cells=%llu"
        " activity_ur_sample_period=%u activity_ue_sample_period=%u"
        " activity_ur_fault_handled_samples=%llu"
        " activity_ur_fault_fallback_samples=%llu"
        " activity_ur_interrupt_affinity_waited_samples=%llu"
        " activity_ur_syscall_affinity_waited_samples=%llu"
        " activity_ur_interrupt_ablation_eligible=%llu"
        " activity_ur_interrupt_ablation_fast=%llu"
        " activity_ur_interrupt_ablation_fallback=%llu"
        " activity_ur_syscall_ablation_eligible=%llu"
        " activity_ur_syscall_ablation_fast=%llu"
        " activity_ur_syscall_ablation_fallback=%llu"
        " activity_ue_capture_calls=%llu activity_ue_restore_calls=%llu"
        " activity_ue_capture_samples=%llu activity_ue_restore_samples=%llu"
        " activity_ue_capture_tsc_total=%llu activity_ue_restore_tsc_total=%llu"
        " activity_ue_empty_tsc_samples=%llu activity_ue_empty_tsc_total=%llu",
        (unsigned long long)activity->interrupt_count,
        (unsigned long long)activity->exception_count,
        (unsigned long long)activity->hardware_interrupt_count,
        (unsigned long long)activity->other_interrupt_count,
        (unsigned long long)activity->hard_dispatch_count,
        (unsigned long long)activity->threaded_dispatch_count,
        (unsigned long long)activity->scheduler_timer_ticks,
        (unsigned long long)activity->schedule_calls,
        (unsigned long long)activity->same_thread_selections,
        (unsigned long long)activity->context_switches,
        (unsigned long long)activity->idle_selections,
        (unsigned long long)activity->scheduler_idle_fallbacks,
        (unsigned long long)activity->scheduler_idle_fallback_current_ready,
        (unsigned long long)activity->scheduler_idle_fallback_current_pending,
        (unsigned long long)activity->scheduler_no_eligible_selections,
        (unsigned long long)activity->ready_queue_scan_entries,
        (unsigned long long)activity->ready_queue_candidate_visits,
        (unsigned long long)activity->ready_queue_predicate_rejects,
        (unsigned long long)activity->ready_queue_selection_samples,
        (unsigned long long)activity->time_accounting_samples,
        (unsigned long long)activity->idle_halt_entries,
        (unsigned long long)activity->framebuffer_flips,
        (unsigned long long)activity->framebuffer_cells, ACTIVITY_USER_RETURN_SAMPLE_PERIOD,
        ACTIVITY_USER_ENTRY_SAMPLE_PERIOD,
        (unsigned long long)activity->user_return_fault_handled_samples,
        (unsigned long long)activity->user_return_fault_fallback_samples,
        (unsigned long long)activity->user_return_interrupt_affinity_waited_samples,
        (unsigned long long)activity->user_return_syscall_affinity_waited_samples,
        (unsigned long long)activity->user_return_interrupt_ablation_eligible,
        (unsigned long long)activity->user_return_interrupt_ablation_fast,
        (unsigned long long)activity->user_return_interrupt_ablation_fallback,
        (unsigned long long)activity->user_return_syscall_ablation_eligible,
        (unsigned long long)activity->user_return_syscall_ablation_fast,
        (unsigned long long)activity->user_return_syscall_ablation_fallback,
        (unsigned long long)activity->user_entry_capture_calls,
        (unsigned long long)activity->user_entry_restore_calls,
        (unsigned long long)activity->user_entry_capture_samples,
        (unsigned long long)activity->user_entry_restore_samples,
        (unsigned long long)activity->user_entry_capture_tsc_total,
        (unsigned long long)activity->user_entry_restore_tsc_total,
        (unsigned long long)activity->user_entry_empty_tsc_samples,
        (unsigned long long)activity->user_entry_empty_tsc_total);
    for (unsigned stage = 0; stage < ACTIVITY_USER_RETURN_STAGE_COUNT; ++stage) {
      printf(" activity_ur_%s_samples=%llu activity_ur_%s_total_ns=%llu",
             activity_user_return_stage_names[stage],
             (unsigned long long)activity->user_return_stage_samples[stage],
             activity_user_return_stage_names[stage],
             (unsigned long long)activity->user_return_stage_total_nanoseconds[stage]);
      for (unsigned i = 0; i < ACTIVITY_DURATION_BUCKET_COUNT; ++i) {
        printf(" activity_ur_%s_h%u=%llu", activity_user_return_stage_names[stage], i,
               (unsigned long long)activity->user_return_stage_duration_buckets[stage][i]);
      }
    }
    for (unsigned i = 0; i < ACTIVITY_DURATION_BUCKET_COUNT; ++i) {
      printf(
          " activity_irq_h%u=%llu activity_pf_h%u=%llu activity_timer_h%u=%llu"
          " activity_hard_h%u=%llu activity_threaded_h%u=%llu"
          " activity_ready_selection_h%u=%llu activity_time_accounting_h%u=%llu"
          " activity_framebuffer_h%u=%llu activity_ue_capture_tsc_h%u=%llu"
          " activity_ue_restore_tsc_h%u=%llu activity_ue_empty_tsc_h%u=%llu",
          i, (unsigned long long)activity->interrupt_duration_buckets[i], i,
          (unsigned long long)activity->page_fault_duration_buckets[i], i,
          (unsigned long long)activity->scheduler_timer_duration_buckets[i], i,
          (unsigned long long)activity->hard_duration_buckets[i], i,
          (unsigned long long)activity->threaded_duration_buckets[i], i,
          (unsigned long long)activity->ready_queue_selection_duration_buckets[i], i,
          (unsigned long long)activity->time_accounting_duration_buckets[i], i,
          (unsigned long long)activity->framebuffer_duration_buckets[i], i,
          (unsigned long long)activity->user_entry_capture_tsc_buckets[i], i,
          (unsigned long long)activity->user_entry_restore_tsc_buckets[i], i,
          (unsigned long long)activity->user_entry_empty_tsc_buckets[i]);
    }
    for (unsigned i = 0; i < ACTIVITY_INTERRUPT_VECTOR_COUNT; ++i) {
      if (activity->interrupt_vector_counts[i])
        printf(" activity_v%u=%llu", i,
               (unsigned long long)activity->interrupt_vector_counts[i]);
    }
  }
  printf("\n");
  printf("COMPILEBENCH DONE phase=%s\n", phase);
}

static void own_metric(const char* phase, uint64_t start, uint64_t end, const struct rusage* before,
                       uint64_t checksum, const struct activity_snapshot* before_activity) {
  struct rusage after;
  if (getrusage(RUSAGE_SELF, &after))
    fail("getrusage");
  uint64_t user = timeval_us(after.ru_utime) - timeval_us(before->ru_utime);
  uint64_t system = timeval_us(after.ru_stime) - timeval_us(before->ru_stime);
  after.ru_utime.tv_sec = user / 1000000;
  after.ru_utime.tv_usec = user % 1000000;
  after.ru_stime.tv_sec = system / 1000000;
  after.ru_stime.tv_usec = system % 1000000;
  after.ru_minflt -= before->ru_minflt;
  after.ru_majflt -= before->ru_majflt;
  after.ru_inblock -= before->ru_inblock;
  after.ru_oublock -= before->ru_oublock;
  after.ru_nvcsw -= before->ru_nvcsw;
  after.ru_nivcsw -= before->ru_nivcsw;
  struct activity_snapshot after_activity = {0}, activity = {0};
  int have_activity = before_activity && activity_snapshot(&after_activity) &&
                      activity_delta(before_activity, &after_activity, &activity);
  metric(phase, start, end, 0, &after, checksum, 0, 0, 0, NULL, 0, NULL, 0, NULL, have_activity,
         &activity);
}

static int command(const char* phase, char* const args[], int permit_failure) {
  printf("COMPILEBENCH command phase=%s argv=", phase);
  for (unsigned i = 0; args[i]; ++i)
    printf("%s%s", i ? " " : "", args[i]);
  printf("\n");
  gate(phase);
  uint64_t before_syscalls = 0;
  int have_before_syscalls = reaped_child_syscall_count(&before_syscalls);
  uint64_t before_latency[SYSCALL_LATENCY_BUCKET_COUNT] = {0};
  int have_before_latency = reaped_child_syscall_latency(before_latency);
  struct activity_snapshot before_activity = {0};
  int have_before_activity = activity_snapshot(&before_activity);
  struct syscall_timing_snapshot before_syscall_timing = {0};
  int have_before_syscall_timing = syscall_timing_snapshot(&before_syscall_timing);
  struct vm_diagnostic_snapshot before_vm_diagnostics = {0};
  int have_before_vm_diagnostics = vm_diagnostic_snapshot(&before_vm_diagnostics);
  uint64_t start = now_ns();
  pid_t child = fork();
  if (child < 0)
    fail("fork");
  if (!child) {
    if (benchmark_user_return_ablation &&
        syscall(SYS_syslog, 14, NULL, benchmark_user_return_ablation)) {
      dprintf(STDERR_FILENO, "COMPILEBENCH child ablation setup failed errno=%d\n", errno);
      _exit(124);
    }
    if (benchmark_vm_ablation_api &&
        syscall(SYS_syslog, 19, NULL, benchmark_vm_ablation)) {
      dprintf(STDERR_FILENO, "COMPILEBENCH child VM ablation setup failed errno=%d\n", errno);
      _exit(121);
    }
    if (benchmark_syscall_timing && syscall(SYS_syslog, 15, NULL, 1)) {
      dprintf(STDERR_FILENO, "COMPILEBENCH child syscall timing setup failed errno=%d\n", errno);
      _exit(123);
    }
    if (benchmark_vm_diagnostics && syscall(SYS_syslog, 17, NULL, 1)) {
      dprintf(STDERR_FILENO, "COMPILEBENCH child VM diagnostic setup failed errno=%d\n", errno);
      _exit(122);
    }
    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0)
      _exit(125);
    if (null_fd > STDERR_FILENO)
      close(null_fd);
    if (serial_fd > STDERR_FILENO)
      close(serial_fd);
    char* environment[] = {
        "PATH=/usr/bin:/bin",
        "LC_ALL=C",
        "HOME=/root",
        "TMPDIR=/tmp",
        "CPLUS_INCLUDE_PATH=/usr/include/c++/15.3.0:/usr/include/c++/15.3.0/x86_64-pedigree",
        NULL};
    environ = environment;
    execvp(args[0], args);
    dprintf(STDERR_FILENO, "exec %s failed: errno=%d\n", args[0], errno);
    _exit(127);
  }
  int status;
  struct rusage usage = {0};
  pid_t waited;
  do {
    waited = wait4(child, &status, 0, &usage);
  } while (waited < 0 && errno == EINTR);
  uint64_t end = now_ns();
  if (waited != child)
    fail("wait4");
  int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  uint64_t after_syscalls = 0;
  int have_after_syscalls = reaped_child_syscall_count(&after_syscalls);
  int have_syscalls = have_before_syscalls && have_after_syscalls &&
                      after_syscalls >= before_syscalls;
  uint64_t syscalls = have_syscalls ? after_syscalls - before_syscalls : 0;
  uint64_t after_latency[SYSCALL_LATENCY_BUCKET_COUNT] = {0};
  int have_after_latency = reaped_child_syscall_latency(after_latency);
  int have_latency = have_before_latency && have_after_latency;
  uint64_t latency[SYSCALL_LATENCY_BUCKET_COUNT] = {0};
  if (have_latency) {
    for (unsigned i = 0; i < SYSCALL_LATENCY_BUCKET_COUNT; ++i) {
      if (after_latency[i] < before_latency[i]) {
        have_latency = 0;
        break;
      }
      latency[i] = after_latency[i] - before_latency[i];
    }
  }
  struct activity_snapshot after_activity = {0}, activity = {0};
  int have_after_activity = activity_snapshot(&after_activity);
  int have_activity = have_before_activity && have_after_activity &&
                      activity_delta(&before_activity, &after_activity, &activity);
  struct syscall_timing_snapshot after_syscall_timing = {0}, syscall_timing = {0};
  int have_after_syscall_timing = syscall_timing_snapshot(&after_syscall_timing);
  int have_syscall_timing =
      have_before_syscall_timing && have_after_syscall_timing &&
      syscall_timing_delta(&before_syscall_timing, &after_syscall_timing, &syscall_timing);
  struct vm_diagnostic_snapshot after_vm_diagnostics = {0}, vm_diagnostics = {0};
  int have_after_vm_diagnostics = vm_diagnostic_snapshot(&after_vm_diagnostics);
  int have_vm_diagnostics =
      have_before_vm_diagnostics && have_after_vm_diagnostics &&
      vm_diagnostic_delta(&before_vm_diagnostics, &after_vm_diagnostics, &vm_diagnostics);
  metric(phase, start, end, rc, &usage, 0, have_syscalls, syscalls, have_latency, latency,
         have_syscall_timing, &syscall_timing, have_vm_diagnostics, &vm_diagnostics, have_activity,
         &activity);
  if (rc && !permit_failure)
    fail(phase);
  return rc;
}

static void require_file(const char* path) {
  struct stat st;
  if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0)
    fail(path);
  printf("COMPILEBENCH file path=%s bytes=%lld\n", path, (long long)st.st_size);
}

static void controls(void) {
  struct rusage before;
  gate("idle");
  if (getrusage(RUSAGE_SELF, &before))
    fail("getrusage");
  struct activity_snapshot before_activity = {0};
  int have_before_activity = activity_snapshot(&before_activity);
  uint64_t start = now_ns();
  struct timespec delay = {5, 0};
  while (nanosleep(&delay, &delay)) {
    if (errno != EINTR)
      fail("nanosleep");
  }
  own_metric("idle", start, now_ns(), &before, 0,
             have_before_activity ? &before_activity : NULL);
  gate("cpu");
  if (getrusage(RUSAGE_SELF, &before))
    fail("getrusage");
  have_before_activity = activity_snapshot(&before_activity);
  start = now_ns();
  uint64_t value = 0x123456789abcdefULL;
  for (uint64_t i = 0; i < 100000000; ++i) {
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
  }
  own_metric("cpu", start, now_ns(), &before, value,
             have_before_activity ? &before_activity : NULL);
}

static void anonymous_faults(unsigned mib) {
  size_t size = (size_t)mib * 1024 * 1024;
  unsigned char* mapping =
      mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED)
    fail("mmap");
  char phase[64];
  snprintf(phase, sizeof(phase), "anon-%umib", mib);
  gate(phase);
  struct rusage before;
  if (getrusage(RUSAGE_SELF, &before))
    fail("getrusage");
  uint64_t start = now_ns();
  volatile unsigned char* touched = mapping;
  for (size_t i = 0; i < size; i += 4096)
    touched[i] = (unsigned char)((i / 4096) * 37U + 0x53U);
  uint64_t end = now_ns(), checksum = 0;
  for (size_t i = 0; i < size; i += 4096) {
    if (mapping[i] != (unsigned char)((i / 4096) * 37U + 0x53U))
      fail("anon-pattern");
    checksum += mapping[i];
  }
  own_metric(phase, start, end, &before, checksum, NULL);
  if (munmap(mapping, size))
    fail("munmap");
}

static uint64_t persisted_output(int verify) {
  int fd = open("which", O_RDONLY);
  if (fd < 0)
    fail("persist-open-output");
  uint64_t hash = 14695981039346656037ULL, bytes = 0;
  unsigned char buffer[4096];
  for (;;) {
    ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      fail("persist-read-output");
    if (!n)
      break;
    bytes += n;
    for (ssize_t i = 0; i < n; ++i) {
      hash ^= buffer[i];
      hash *= 1099511628211ULL;
    }
  }
  close(fd);
  if (!bytes)
    fail("persist-empty-output");
  if (verify) {
    FILE* sentinel = fopen("persisted-output", "r");
    unsigned long long expected_bytes, expected_hash;
    char trailing;
    if (!sentinel ||
        fscanf(sentinel, "COMPILEBENCH-V1 %llu %llx %c", &expected_bytes, &expected_hash,
               &trailing) != 2 ||
        expected_bytes != bytes || expected_hash != hash)
      fail("persist-checksum");
    fclose(sentinel);
  } else {
    // The next boot consumes this sentinel without rewriting it or compiling again.
    fd = open("persisted-output", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 ||
        dprintf(fd, "COMPILEBENCH-V1 %llu %016llx\n", (unsigned long long)bytes,
                (unsigned long long)hash) <= 0 ||
        fsync(fd))
      fail("persist-write-sentinel");
    close(fd);
  }
  printf("COMPILEBENCH persisted action=%s bytes=%llu fnv1a64=%016llx\n",
         verify ? "verified" : "stored", (unsigned long long)bytes, (unsigned long long)hash);
  return hash;
}

int main(void) {
  serial_fd = open("/dev/ttyS0", O_RDWR | O_NONBLOCK);
  if (serial_fd < 0 || dup2(serial_fd, STDOUT_FILENO) < 0 || dup2(serial_fd, STDERR_FILENO) < 0)
    fail("serial-open");
  setvbuf(stdout, NULL, _IONBF, 0);
  struct termios t;
  if (!tcgetattr(serial_fd, &t)) {
    t.c_iflag = 0;
    t.c_oflag = 0;
    t.c_lflag = 0;
    t.c_cflag = (t.c_cflag & ~(CSIZE | PARENB)) | CS8;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(serial_fd, TCSANOW, &t))
      fail("serial-termios");
  } else if (errno != ENOTTY) {
    fail("serial-termios");
  }
  if (chdir("/root/compile-bench"))
    fail("setup");
  if (!access("ablate-interrupt-return", F_OK))
    benchmark_user_return_ablation |= BENCHMARK_ABLATE_INTERRUPT_RETURN;
  if (!access("ablate-syscall-return", F_OK))
    benchmark_user_return_ablation |= BENCHMARK_ABLATE_SYSCALL_RETURN;
  benchmark_syscall_timing = !access("time-syscalls", F_OK);
  benchmark_vm_diagnostics = !access("trace-vm", F_OK);
  benchmark_vm_ablation_api = !access("vm-ablation", F_OK);
  if (!access("ablate-vm-vector-validation", F_OK))
    benchmark_vm_ablation |= BENCHMARK_ABLATE_VM_VECTOR_VALIDATION;
  if (!access("ablate-vm-guard-termination", F_OK))
    benchmark_vm_ablation |= BENCHMARK_ABLATE_VM_GUARD_TERMINATION;
  if (!access("ablate-vm-guard-recursive-events", F_OK))
    benchmark_vm_ablation |= BENCHMARK_ABLATE_VM_GUARD_RECURSIVE_EVENTS;
  if (!access("ablate-vm-guards", F_OK))
    benchmark_vm_ablation |= BENCHMARK_ABLATE_VM_GUARD_TERMINATION |
                             BENCHMARK_ABLATE_VM_GUARD_RECURSIVE_EVENTS;
  if (!access("ablate-vm-table-retirement", F_OK))
    benchmark_vm_ablation |= BENCHMARK_ABLATE_VM_TABLE_RETIREMENT;
  if (!access("ablate-vm-reverse-fault-lookup", F_OK))
    benchmark_vm_ablation |= BENCHMARK_ABLATE_VM_REVERSE_FAULT_LOOKUP;
  if (benchmark_vm_ablation && !benchmark_vm_ablation_api) {
    errno = EINVAL;
    fail("vm-ablation-marker");
  }
  printf("COMPILEBENCH BEGIN\n");
  printf(
      "COMPILEBENCH configuration benchmark_user_return_ablation=%u "
      "benchmark_syscall_timing=%d benchmark_vm_diagnostics=%d "
      "benchmark_vm_ablation_api=%d benchmark_vm_ablation=%u\n",
      benchmark_user_return_ablation, benchmark_syscall_timing, benchmark_vm_diagnostics,
      benchmark_vm_ablation_api, benchmark_vm_ablation);
  if (!access("synthetic-vm", F_OK)) {
    char* synthetic[] = {"./vm-syscall-latency", "1", NULL};
    command("vm-synthetic", synthetic, 0);
    printf("COMPILEBENCH PASS END\n");
    return 0;
  }
  static char kernel_log[256 * 1024];
  long log_size = syscall(SYS_syslog, 3, kernel_log, sizeof(kernel_log) - 1);
  if (log_size > 0) {
    kernel_log[log_size] = 0;
    char* line = strtok(kernel_log, "\n");
    while (line) {
      char* module = strstr(line, "KERNELELF: Preloaded module ");
      if (module)
        printf("COMPILEBENCH %s\n", module);
      line = strtok(NULL, "\n");
    }
  }
  int quick = !access("quick-run", F_OK);
  int skip_sync = !access("no-sync", F_OK);
  int persist = !skip_sync && !access("persist-check", F_OK);
  char* run[] = {"./which", "gcc", NULL};
  if (persist && !access("persisted-output", F_OK)) {
    gate("verify-persisted");
    struct rusage before;
    if (getrusage(RUSAGE_SELF, &before))
      fail("getrusage");
    uint64_t start = now_ns();
    uint64_t hash = persisted_output(1);
    own_metric("verify-persisted", start, now_ns(), &before, hash, NULL);
    command("run-persisted", run, 0);
    printf("COMPILEBENCH PASS END\n");
    return 0;
  }
  if (quick && access("link-cxx", F_OK))
    fail("quick-requires-link-cxx");
  require_file("which.cc");
  controls();
  char* exact[] = {"gcc", "-o", "which", "which.cc", NULL};
  char* practical[] = {"gcc", "-o", "which", "which.cc", "-lstdc++", NULL};
  char** successful = exact;
  if (!access("link-cxx", F_OK)) {
    command("compile-cold", practical, 0);
    successful = practical;
  } else if (command("compile-exact", exact, 1)) {
    command("compile-practical", practical, 0);
    successful = practical;
  }
  require_file("which");
  command("compile-warm-1", successful, 0);
  if (!quick) {
    command("compile-warm-2", successful, 0);
    char* preprocess[] = {"gcc", "-E", "which.cc", "-o", "which.ii", NULL};
    char* codegen[] = {"gcc", "-ftime-report", "-S", "which.ii", "-o", "which.s", NULL};
    char* assemble[] = {"gcc", "-c", "which.s", "-o", "which.o", NULL};
    char* link[] = {"gcc", "-o", "which", "which.o", "-lstdc++", NULL};
    command("preprocess", preprocess, 0);
    require_file("which.ii");
    command("codegen", codegen, 0);
    require_file("which.s");
    command("assemble", assemble, 0);
    require_file("which.o");
    command("link", link, 0);
    require_file("which");
  }
  command("run", run, 0);
  if (skip_sync) {
    printf("COMPILEBENCH skipped phase=sync reason=writes-disabled-performance-only\n");
  } else {
    struct rusage before;
    gate("sync");
    if (getrusage(RUSAGE_SELF, &before))
      fail("getrusage");
    uint64_t start = now_ns();
    if (persist)
      persisted_output(0);
    int fd = open("which", O_RDONLY);
    if (fd < 0 || fsync(fd))
      fail("fsync");
    close(fd);
    sync();
    own_metric("sync", start, now_ns(), &before, 0, NULL);
  }
  require_file("which");
  if (!quick) {
    anonymous_faults(1);
    anonymous_faults(4);
    anonymous_faults(16);
    anonymous_faults(64);
  }
  char* contract[] = {"./anonymous-contract", "1", NULL};
  command("anon-contract", contract, 0);
  printf("COMPILEBENCH PASS END\n");
  return 0;
}
