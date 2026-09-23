/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/process/Rcu.h"

#include <pthread.h>

namespace {
// Native filesystem utilities have host threads rather than kernel CPUs.
// A shared lock supplies their grace period; nesting remains thread-local.
pthread_rwlock_t readers = PTHREAD_RWLOCK_INITIALIZER;
thread_local RcuReadState state;
}  // namespace

RcuReadGuard::RcuReadGuard() : m_State(&state), m_Interrupts(false) {
  if (!state.active() && pthread_rwlock_rdlock(&readers)) {
    panic("Native RCU reader lock failed.");
  }
  state.enter();
}

RcuReadGuard::~RcuReadGuard() {
  if (m_State != &state) {
    panic("Native RCU reader changed threads.");
  }
  state.leave();
  if (!state.active() && pthread_rwlock_unlock(&readers)) {
    panic("Native RCU reader unlock failed.");
  }
}

void Rcu::synchronize() {
  if (state.active() || pthread_rwlock_wrlock(&readers)) {
    panic("Native RCU grace period requires no local reader.");
  }
  if (pthread_rwlock_unlock(&readers)) {
    panic("Native RCU grace period unlock failed.");
  }
}
