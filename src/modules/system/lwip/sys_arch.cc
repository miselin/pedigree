/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <lwip/arch/sys_arch.h>
#include <lwip/err.h>
#include <lwip/errno.h>
#include <lwip/sys.h>
#include <pedigree/kernel/Log.h>
#include <pedigree/kernel/process/Mutex.h>
#include <pedigree/kernel/process/Semaphore.h>
#include <pedigree/kernel/process/Thread.h>
#include <pedigree/kernel/processor/Processor.h>
#include <pedigree/kernel/utilities/RingBuffer.h>
#include <pedigree/kernel/utilities/pocketknife.h>

#if UTILITY_LINUX
#include <errno.h>
#include <thread>
#include <time.h>

static Spinlock g_Protection(false);
#else
// errno for lwIP usage, this is not ideal as it'll be exposed to ALL modules.
int errno;
#endif

struct pedigree_mbox {
  pedigree_mbox() : buffer(64) {}

  using Buffer = RingBuffer<void*, 64>;
  Buffer buffer;
};

void sys_init() {}

u32_t sys_now() {
#if UTILITY_LINUX
  struct timespec spec;
  clock_gettime(CLOCK_REALTIME, &spec);

  return (spec.tv_sec * 1000) + (spec.tv_nsec / 1000000);
#else
  return Time::getTimeNanoseconds() / Time::Multiplier::Millisecond;
#endif
}

struct thread_meta {
  lwip_thread_fn thread;
  void* arg;
  char name[64];
};

static int thread_shim(void* arg) {
  struct thread_meta* meta = static_cast<struct thread_meta*>(arg);
  lwip_thread_fn thread = meta->thread;
  void* threadArg = meta->arg;
  delete meta;
  thread(threadArg);
  return 0;
}

sys_thread_t sys_thread_new(const char* name, lwip_thread_fn thread, void* arg, int stacksize,
                            int prio) {
  /// \todo stacksize might be important
  auto meta = new struct thread_meta;
  meta->thread = thread;
  meta->arg = arg;
  StringCopy(meta->name, name);
  return pocketknife::runConcurrentlyAttached(thread_shim, meta);
}

int sys_thread_join(sys_thread_t thread) {
  if (!thread) {
    return 1;
  }
#if UTILITY_LINUX
  pocketknife::attachTo(thread);
  return 1;
#else
  return pocketknife::attachToForCompletion(thread) ? 1 : 0;
#endif
}

int sys_thread_is_current(sys_thread_t thread) {
  if (!thread) {
    return 0;
  }
#if UTILITY_LINUX
  std::thread* candidate = reinterpret_cast<std::thread*>(thread);
  return candidate->get_id() == std::this_thread::get_id() ? 1 : 0;
#else
  return reinterpret_cast<Thread*>(thread) ==
                 Processor::information().getCurrentThread()
             ? 1
             : 0;
#endif
}

err_t sys_sem_new(sys_sem_t* sem, u8_t count) {
#if UTILITY_LINUX
  if (sem_init(sem, 0, count) != 0) {
    return ERR_ARG;
  } else {
    return ERR_OK;
  }
#else
  Semaphore* newsem = new Semaphore(count);
  *sem = reinterpret_cast<void*>(newsem);
  return ERR_OK;
#endif
}

void sys_sem_free(sys_sem_t* sem) {
#if UTILITY_LINUX
  sem_destroy(sem);
#else
  Semaphore* s = reinterpret_cast<Semaphore*>(*sem);
  delete s;
  *sem = nullptr;
#endif
}

int sys_sem_valid(sys_sem_t* sem) {
#if UTILITY_LINUX
  return 1;
#else
  if (*sem) {
    return 1;
  } else {
    return 0;
  }
#endif
}

void sys_sem_set_invalid(sys_sem_t* sem) {
#if !UTILITY_LINUX
  *sem = nullptr;
#endif
}

void sys_sem_signal(sys_sem_t* sem) {
#if UTILITY_LINUX
  sem_post(sem);
#else
  Semaphore* s = reinterpret_cast<Semaphore*>(*sem);
  s->release();
#endif
}

u32_t sys_arch_sem_wait(sys_sem_t* sem, u32_t timeout) {
#if UTILITY_LINUX
  struct timespec started = {};
  clock_gettime(CLOCK_MONOTONIC, &started);

  int result = 0;
  if (timeout) {
    struct timespec deadline = {};
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout / 1000;
    deadline.tv_nsec += (timeout % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
      ++deadline.tv_sec;
      deadline.tv_nsec -= 1000000000;
    }

#ifdef __APPLE__
    // TODO: this is very wrong, it's a HACK
    result = sem_wait(sem);
#else
    do {
      result = sem_timedwait(sem, &deadline);
    } while (result == -1 && errno == EINTR);
#endif
  } else {
    do {
      result = sem_wait(sem);
    } while (result == -1 && errno == EINTR);
  }

  if (result == 0) {
    struct timespec completed = {};
    clock_gettime(CLOCK_MONOTONIC, &completed);
    const uint64_t startedNs =
        (static_cast<uint64_t>(started.tv_sec) * 1000000000ULL) + started.tv_nsec;
    const uint64_t completedNs =
        (static_cast<uint64_t>(completed.tv_sec) * 1000000000ULL) + completed.tv_nsec;
    return (completedNs - startedNs) / 1000000ULL;
  }

  return SYS_ARCH_TIMEOUT;
#else
  const Time::Timestamp begin = Time::getTicks();

  Semaphore* s = reinterpret_cast<Semaphore*>(*sem);
  const size_t timeoutSecs = timeout / 1000;
  const size_t timeoutUsecs = (timeout % 1000) * 1000;

  // lwIP has no interrupted-semaphore result, and many callers keep a
  // stack-owned request alive until this wait completes. Defer a signal to
  // the surrounding syscall boundary instead of misreporting it as timeout
  // and abandoning that request.
  if (!s->acquireForCompletion(1, timeoutSecs, timeoutUsecs)) {
    return SYS_ARCH_TIMEOUT;
  }

  const Time::Timestamp end = Time::getTicks();
  return (end - begin) / Time::Multiplier::Millisecond;
#endif
}

err_t sys_mbox_new(sys_mbox_t* mbox, int size) {
  *mbox = new pedigree_mbox;
  return ERR_OK;
}

void sys_mbox_free(sys_mbox_t* mbox) {
  pedigree_mbox* mailbox = *mbox;
  if (!mailbox) {
    return;
  }

  mailbox->buffer.close();
  delete mailbox;
  *mbox = nullptr;
}

void sys_mbox_post(sys_mbox_t* mbox, void* msg) {
  if ((*mbox)->buffer.write(msg) != pedigree_mbox::Buffer::NoError) {
    FATAL("sys_mbox_post failed");
  }
}

err_t sys_mbox_post_and_close(sys_mbox_t* mbox, void* msg) {
  if (!mbox || !*mbox) {
    return ERR_ARG;
  }
  return (*mbox)->buffer.closeWritesWithFinal(msg) ? ERR_OK : ERR_CLSD;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t* mbox, void** msg) {
  if (!(*mbox)->buffer.dataReady()) {
    return SYS_MBOX_EMPTY;
  }

  void* value = nullptr;
  pedigree_mbox::Buffer::Error error = pedigree_mbox::Buffer::NoError;
  if (!(*mbox)->buffer.read(value, error)) {
    // TODO: what error?
    ERROR(
        "sys_arch_mbox_tryfetch: read() failed after dataReady() returned "
        "true");
    return SYS_MBOX_EMPTY;
  }

  *msg = value;
  return 0;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t* mbox, void** msg, u32_t timeout) {
  Time::Timestamp begin = Time::getTimeNanoseconds();

  Time::Timestamp timeoutMs = 0;
  if (timeout == 0) {
    timeoutMs = Time::Infinity;
  } else {
    timeoutMs = timeout * Time::Multiplier::Millisecond;
  }

  void* value = nullptr;
  pedigree_mbox::Buffer::Error error = pedigree_mbox::Buffer::NoError;
  if (!(*mbox)->buffer.read(value, timeoutMs, error)) {
    // TODO: check the specific error
    return SYS_ARCH_TIMEOUT;
  }

  *msg = value;

  Time::Timestamp end = Time::getTimeNanoseconds();
  return (end - begin) / Time::Multiplier::Millisecond;
}

err_t sys_mbox_trypost(sys_mbox_t* mbox, void* msg) {
  const pedigree_mbox::Buffer::Error error = (*mbox)->buffer.tryWrite(msg);
  if (error == pedigree_mbox::Buffer::WouldBlock) {
    return ERR_WOULDBLOCK;
  }

  return error == pedigree_mbox::Buffer::NoError ? ERR_OK : ERR_BUF;
}

int sys_mbox_valid(sys_mbox_t* mbox) {
  return *mbox != nullptr ? 1 : 0;
}

void sys_mbox_set_invalid(sys_mbox_t* mbox) {
  *mbox = nullptr;
}

err_t sys_mutex_new(sys_mutex_t* mutex) {
  Mutex* m = new Mutex;
  *mutex = m;
  return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t* mutex) {
  Mutex* m = reinterpret_cast<Mutex*>(*mutex);
  while (!m->acquire())
    ;
}

void sys_mutex_unlock(sys_mutex_t* mutex) {
  Mutex* m = reinterpret_cast<Mutex*>(*mutex);
  m->release();
}

void sys_mutex_free(sys_mutex_t* mutex) {
  Mutex* m = reinterpret_cast<Mutex*>(*mutex);
  delete m;
  *mutex = nullptr;
}

int sys_mutex_valid(sys_mutex_t* mutex) {
  return *mutex != nullptr ? 1 : 0;
}

void sys_mutex_set_invalid(sys_mutex_t* mutex) {
  *mutex = nullptr;
}

sys_prot_t sys_arch_protect() {
#if UTILITY_LINUX
  while (!g_Protection.acquire(true))
    ;

  return 0;
#else
  bool was = Processor::getInterrupts();
  Processor::setInterrupts(false);
  return was ? 1 : 0;
#endif
}

void sys_arch_unprotect(sys_prot_t pval) {
#if UTILITY_LINUX
  g_Protection.release();
#else
  Processor::setInterrupts(pval);
#endif
}
