/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/mman.h>

extern void fail();

void test_mmap() {
  const long configured_page_size = sysconf(_SC_PAGESIZE);
  if (configured_page_size <= 0)
    fail();
  const size_t page_size = (size_t)configured_page_size;

  printf("Testing mmap(2) placement semantics...\n");
  unsigned char* original =
      mmap(0, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (original == MAP_FAILED)
    fail();
  *original = 0x5A;

  errno = 0;
  void* no_replace = mmap(original, page_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
  if (no_replace != MAP_FAILED || errno != EEXIST || *original != 0x5A)
    fail();

  errno = 0;
  if (munmap(original, SIZE_MAX) != -1 || errno != EINVAL || *original != 0x5A)
    fail();

  const size_t replacement_size = page_size * 3;
  void* replacement = mmap(original, replacement_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  if (replacement != original || *original != 0)
    fail();
  *original = 0x7C;

  void* follower = mmap(0, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (follower == MAP_FAILED || ((uintptr_t)follower >= (uintptr_t)original &&
                                 (uintptr_t)follower < (uintptr_t)original + replacement_size))
    fail();

  void* hinted =
      mmap(original + 17, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (hinted == MAP_FAILED || hinted == original || ((uintptr_t)hinted & (page_size - 1)) ||
      *original != 0x7C)
    fail();

  if (munmap(hinted, page_size) || munmap(follower, page_size) ||
      munmap(replacement, replacement_size))
    fail();

  void* reclaimed = mmap(replacement, replacement_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
  if (reclaimed != replacement || munmap(reclaimed, replacement_size))
    fail();

  unsigned char* split =
      mmap(0, replacement_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (split == MAP_FAILED || munmap(split + page_size, page_size))
    fail();

  void* middle = mmap(split + page_size, page_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
  if (middle != split + page_size)
    fail();

  errno = 0;
  void* prefix = mmap(split, page_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
  if (prefix != MAP_FAILED || errno != EEXIST || munmap(split, replacement_size))
    fail();

  void* split_reclaimed = mmap(split, replacement_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
  if (split_reclaimed != split || munmap(split_reclaimed, replacement_size))
    fail();
  printf("mmap(2) placement semantics were successful!\n");
}
