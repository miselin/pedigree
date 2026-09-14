/* Copyright (c) 2026, Pedigree Developers. */
#ifndef FIXTURE_VERSION
#define FIXTURE_VERSION 1
#endif

__attribute__((aligned(4096)))
const volatile unsigned char executable_fixture_data[12288] = {[8192] = FIXTURE_VERSION};

__attribute__((noinline, aligned(4096))) int executable_fixture_warm(void) {
  return FIXTURE_VERSION;
}

__attribute__((noinline, aligned(4096))) int executable_fixture_cold(void) {
  return executable_fixture_data[8192];
}
