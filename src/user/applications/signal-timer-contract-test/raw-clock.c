#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include "contract.h"
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/timex.h>

void vdso_init_from_sysinfo_ehdr(uintptr_t base);
void* vdso_sym(const char* version, const char* name);

static int64_t nanoseconds(struct timespec value) {
  if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000)
    return -1;
  return (int64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

static int64_t direct_clock(clockid_t clock) {
  struct timespec value;
  return syscall(SYS_clock_gettime, clock, &value) ? -1 : nanoseconds(value);
}

int signal_timer_test_raw_clock(void) {
  int failed = 0, changed = 0, fd = -1, have_timer = 0;
  timer_t timer;
  const int64_t saved_real = direct_clock(CLOCK_REALTIME);
  const int64_t saved_mono = direct_clock(CLOCK_MONOTONIC);
  const uintptr_t vdso_base = getauxval(AT_SYSINFO_EHDR);
  CHECK(vdso_base != 0 && saved_real >= 0 && saved_mono >= 0);
  vdso_init_from_sysinfo_ehdr(vdso_base);
  int (*vdso_clock)(clockid_t, struct timespec*) =
      (int (*)(clockid_t, struct timespec*))vdso_sym("LINUX_2.6", "__vdso_clock_gettime");
  CHECK(vdso_clock != NULL);

  int64_t previous_libc = -1, previous_vdso = -1, previous_direct = -1;
  for (unsigned n = 0; n < 16; ++n) {
    struct timespec libc_value, vdso_value;
    const int64_t before = direct_clock(CLOCK_MONOTONIC);
    CHECK(clock_gettime(CLOCK_MONOTONIC_RAW, &libc_value) == 0);
    CHECK(vdso_clock(CLOCK_MONOTONIC_RAW, &vdso_value) == 0);
    const int64_t raw = direct_clock(CLOCK_MONOTONIC_RAW);
    const int64_t after = direct_clock(CLOCK_MONOTONIC);
    const int64_t libc_raw = nanoseconds(libc_value), vdso_raw = nanoseconds(vdso_value);
    CHECK(before >= 0 && raw >= before && raw <= after && raw >= previous_direct);
    // The info block is a periodically refreshed snapshot, unlike the syscall.
    CHECK(libc_raw >= 0 && libc_raw >= before - 1000000000 && libc_raw <= after);
    CHECK(vdso_raw >= 0 && vdso_raw >= before - 1000000000 && vdso_raw <= after);
    CHECK(libc_raw >= previous_libc && vdso_raw >= previous_vdso);
    previous_libc = libc_raw;
    previous_vdso = vdso_raw;
    previous_direct = raw;
    st_pause(2);
  }
  puts("SIGNAL-TIMER-CONTRACT: PASS raw-clock-vdso-libc-syscall");

  struct timespec resolution = {-1, -1};
  CHECK(clock_getres(CLOCK_MONOTONIC_RAW, &resolution) == 0);
  CHECK(resolution.tv_sec == 0 && resolution.tv_nsec == 1);
  resolution = (struct timespec){-1, -1};
  CHECK(syscall(SYS_clock_getres, CLOCK_MONOTONIC_RAW, &resolution) == 0);
  CHECK(resolution.tv_sec == 0 && resolution.tv_nsec == 1);
  CHECK(clock_getres(CLOCK_MONOTONIC_RAW, NULL) == 0);
  CHECK(syscall(SYS_clock_getres, CLOCK_MONOTONIC_RAW, NULL) == 0);
  CHECK(vdso_clock(CLOCK_MONOTONIC_RAW, NULL) == -EFAULT);
  CHECK(syscall(SYS_clock_gettime, CLOCK_MONOTONIC_RAW, NULL) == -1 && errno == EFAULT);
  CHECK(syscall(SYS_clock_getres, CLOCK_MONOTONIC_RAW, (void*)1) == -1 && errno == EFAULT);
  CHECK(clock_gettime(-1, &resolution) == -1 && errno == EINVAL);
  CHECK(vdso_clock(-1, &resolution) == -EINVAL);
  CHECK(syscall(SYS_clock_gettime, -1, &resolution) == -1 && errno == EINVAL);

  const struct timespec zero = {0, 0};
  struct timespec remainder = {123, 456};
  CHECK(clock_nanosleep(CLOCK_MONOTONIC_RAW, 0, &zero, &remainder) == EINVAL);
  CHECK(remainder.tv_sec == 123 && remainder.tv_nsec == 456);
  CHECK(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC_RAW, TIMER_ABSTIME, &zero, NULL) == -1 &&
        errno == EINVAL);
  CHECK(clock_settime(CLOCK_MONOTONIC_RAW, &zero) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_clock_settime, CLOCK_MONOTONIC_RAW, &zero) == -1 && errno == EINVAL);
  have_timer = timer_create(CLOCK_MONOTONIC_RAW, NULL, &timer) == 0;
  CHECK(!have_timer && errno == EINVAL);
  fd = timerfd_create(CLOCK_MONOTONIC_RAW, TFD_NONBLOCK);
  CHECK(fd == -1 && errno == EINVAL);
  puts("SIGNAL-TIMER-CONTRACT: PASS raw-clock-read-only");

  if (geteuid() == 0) {
    for (unsigned route = 0; route < 2; ++route) {
      const int64_t real_before = direct_clock(CLOCK_REALTIME);
      const int64_t raw_before = direct_clock(CLOCK_MONOTONIC_RAW);
      const int64_t mono_before = direct_clock(CLOCK_MONOTONIC);
      if (!route) {
        struct timespec target = st_timespec(real_before + 10000000000);
        CHECK(clock_settime(CLOCK_REALTIME, &target) == 0);
      } else {
        struct timex adjustment = {.modes = ADJ_SETOFFSET, .time = {.tv_sec = -20}};
        CHECK(adjtimex(&adjustment) >= 0);
      }
      changed = 1;
      const int64_t raw_after = direct_clock(CLOCK_MONOTONIC_RAW);
      const int64_t mono_after = direct_clock(CLOCK_MONOTONIC);
      const int64_t real_after = direct_clock(CLOCK_REALTIME);
      CHECK(raw_before >= 0 && raw_after >= raw_before && mono_after >= mono_before);
      const int64_t drift = (raw_after - raw_before) - (mono_after - mono_before);
      CHECK(drift > -1000000000 && drift < 1000000000);
      CHECK(route ? real_after < real_before - 18000000000
                  : real_after >= real_before + 10000000000);
      struct timespec vdso_value, libc_value;
      CHECK(vdso_clock(CLOCK_MONOTONIC_RAW, &vdso_value) == 0);
      CHECK(clock_gettime(CLOCK_MONOTONIC_RAW, &libc_value) == 0);
      CHECK(nanoseconds(vdso_value) >= raw_before - 1000000000 &&
            nanoseconds(vdso_value) <= direct_clock(CLOCK_MONOTONIC));
      CHECK(nanoseconds(libc_value) >= raw_before - 1000000000 &&
            nanoseconds(libc_value) <= direct_clock(CLOCK_MONOTONIC));
    }
    puts("SIGNAL-TIMER-CONTRACT: PASS raw-clock-realtime-independent");
  } else {
    puts("SIGNAL-TIMER-CONTRACT: SKIP raw-clock-realtime-independent requires root");
  }
out:
  if (have_timer)
    timer_delete(timer);
  if (fd >= 0)
    close(fd);
  if (changed) {
    struct timespec restored = st_timespec(saved_real + direct_clock(CLOCK_MONOTONIC) - saved_mono);
    if (clock_settime(CLOCK_REALTIME, &restored))
      failed = 1;
  }
  return failed;
}
