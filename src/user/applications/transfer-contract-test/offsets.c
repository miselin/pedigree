#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/sendfile.h>

static int combinations(int kind) {
  int failed = 0, alias = -1;
  struct tf_file input = {.fd = -1}, output = {.fd = -1};
  CHECK(!tf_create(&input, "/tmp", 128, 31) && !tf_create(&output, "/tmp", 128, 73));
  CHECK((alias = dup(input.fd)) >= 0);
  for (int explicit_input = 0; explicit_input < 2; ++explicit_input) {
    for (int explicit_output = 0; explicit_output < (kind == TF_COPY_RANGE ? 2 : 1);
         ++explicit_output) {
      off_t from = 3, to = 11;
      const off_t source = explicit_input ? from : 9, destination = explicit_output ? to : 17;
      CHECK(lseek(input.fd, 9, SEEK_SET) == 9 && lseek(output.fd, 17, SEEK_SET) == 17);
      CHECK(tf_copy(kind, alias, explicit_input ? &from : NULL, output.fd,
                    explicit_output ? &to : NULL, 25) == 25);
      CHECK(from == (explicit_input ? 28 : 3) && to == (explicit_output ? 36 : 11));
      CHECK(lseek(input.fd, 0, SEEK_CUR) == (explicit_input ? 9 : 34));
      CHECK(lseek(output.fd, 0, SEEK_CUR) == (explicit_output ? 17 : 42));
      CHECK(!tf_verify(output.fd, destination, 25, source, 31));
    }
  }
out:
  if (alias >= 0)
    close(alias);
  tf_destroy(&output);
  tf_destroy(&input);
  return failed;
}
static int same_file(void) {
  int failed = 0, alias = -1, separate = -1;
  struct tf_file file = {.fd = -1};
  unsigned char before[64], after[64];
  CHECK(!tf_create(&file, "/tmp", 64, 43));
  CHECK((alias = dup(file.fd)) >= 0 && (separate = open(file.path, O_RDWR)) >= 0);
  CHECK(lseek(file.fd, 7, SEEK_SET) == 7);
  CHECK(sendfile(alias, file.fd, NULL, 9) == 9);
  CHECK(lseek(file.fd, 0, SEEK_CUR) == 16 && lseek(alias, 0, SEEK_CUR) == 16);
  CHECK(!tf_verify(file.fd, 0, 64, 0, 43));
  CHECK(copy_file_range(file.fd, NULL, alias, NULL, 9, 0) == -1 && errno == EINVAL);
  CHECK(lseek(alias, 0, SEEK_CUR) == 16);
  CHECK(copy_file_range(file.fd, NULL, alias, NULL, 0, 0) == 0);
  off_t from = 0, to = 32;
  CHECK(copy_file_range(file.fd, &from, separate, &to, 16, 0) == 16);
  CHECK(from == 16 && to == 48 && !tf_verify(file.fd, 32, 16, 0, 43));
  CHECK(lseek(file.fd, 0, SEEK_CUR) == 16 && lseek(separate, 0, SEEK_CUR) == 0);
  CHECK(pread(file.fd, before, sizeof(before), 0) == sizeof(before));
  from = 0;
  to = 8;
  CHECK(copy_file_range(file.fd, &from, separate, &to, 16, 0) == -1 && errno == EINVAL);
  CHECK(from == 0 && to == 8);
  CHECK(pread(file.fd, after, sizeof(after), 0) == sizeof(after) &&
        !memcmp(before, after, sizeof(before)));
  /* The available input is sixteen bytes, adjacent to the destination range. */
  from = 48;
  to = 32;
  CHECK(copy_file_range(file.fd, &from, alias, &to, 40, 0) == 16);
  CHECK(from == 64 && to == 48 && !tf_verify(file.fd, 32, 16, 48, 43));
  CHECK(lseek(file.fd, 0, SEEK_CUR) == 16);
  CHECK(lseek(file.fd, 64, SEEK_SET) == 64);
  CHECK(copy_file_range(file.fd, NULL, alias, NULL, 4, 0) == 0);
out:
  if (separate >= 0)
    close(separate);
  if (alias >= 0)
    close(alias);
  tf_destroy(&file);
  return failed;
}
static int invalid_ranges(void) {
  int failed = 0;
  struct tf_file input = {.fd = -1}, output = {.fd = -1};
  const size_t page = sysconf(_SC_PAGESIZE);
  void* denied = MAP_FAILED;
  CHECK(!tf_create(&input, "/tmp", 64, 19) && !tf_create(&output, "/tmp", 64, 61));
  denied = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(denied != MAP_FAILED);
  for (int kind = 0; kind < 2; ++kind) {
    off_t offset = -2;
    CHECK(tf_copy(kind, input.fd, &offset, output.fd, NULL, 1) == -1 && errno == EINVAL);
    CHECK(offset == -2);
    CHECK(tf_copy(kind, input.fd, denied, output.fd, NULL, 1) == -1 && errno == EFAULT);
  }
  off_t from = 0;
  CHECK(copy_file_range(input.fd, &from, output.fd, denied, 1, 0) == -1 && errno == EFAULT);
  CHECK(from == 0);
  off_t to = -2;
  CHECK(copy_file_range(input.fd, NULL, output.fd, &to, 1, 0) == -1 && errno == EINVAL);
  CHECK(to == -2);
  to = 1;
  CHECK(copy_file_range(input.fd, NULL, output.fd, &to, SIZE_MAX, 0) == -1 && errno == EOVERFLOW);
  CHECK(to == 1);
  from = 1;
  CHECK(copy_file_range(input.fd, &from, output.fd, NULL, SIZE_MAX, 0) == -1 && errno == EOVERFLOW);
  CHECK(from == 1);
  from = INT64_MAX - 7;
  CHECK(sendfile(output.fd, input.fd, &from, 16) == -1 && errno == EINVAL);
  CHECK(from == INT64_MAX - 7);
  CHECK(copy_file_range(input.fd, &from, output.fd, NULL, 16, 0) == 0);
  CHECK(from == INT64_MAX - 7);
  CHECK(copy_file_range(input.fd, NULL, output.fd, NULL, 1, 1) == -1 && errno == EINVAL);
  CHECK(copy_file_range(input.fd, NULL, output.fd, NULL, 0, 1) == -1 && errno == EINVAL);
  CHECK(!tf_verify(output.fd, 0, 64, 0, 61));
  CHECK(lseek(input.fd, 0, SEEK_CUR) == 0 && lseek(output.fd, 0, SEEK_CUR) == 0);
out:
  if (denied != MAP_FAILED)
    munmap(denied, page);
  tf_destroy(&output);
  tf_destroy(&input);
  return failed;
}
static int late_copyout(void) {
  int failed = 0;
  struct tf_file input = {.fd = -1}, output = {.fd = -1};
  const size_t page = sysconf(_SC_PAGESIZE);
  off_t* readonly = MAP_FAILED;
  CHECK(!tf_create(&input, "/tmp", 64, 17) && !tf_create(&output, "/tmp", 128, 91));
  readonly = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(readonly != MAP_FAILED);
  *readonly = 8;
  CHECK(!mprotect(readonly, page, PROT_READ));
  CHECK(lseek(input.fd, 13, SEEK_SET) == 13);
  CHECK(sendfile(output.fd, input.fd, readonly, 4) == -1 && errno == EFAULT);
  CHECK(*readonly == 8 && lseek(input.fd, 0, SEEK_CUR) == 13 && lseek(output.fd, 0, SEEK_CUR) == 4);
  CHECK(!tf_verify(output.fd, 0, 4, 8, 17));
  off_t to = 24;
  CHECK(copy_file_range(input.fd, readonly, output.fd, &to, 4, 0) == -1 && errno == EFAULT);
  CHECK(to == 28 && *readonly == 8 && !tf_verify(output.fd, 24, 4, 8, 17));
  CHECK(lseek(output.fd, 0, SEEK_CUR) == 4);
  off_t from = 3;
  CHECK(copy_file_range(input.fd, &from, output.fd, readonly, 4, 0) == -1 && errno == EFAULT);
  CHECK(from == 7 && *readonly == 8 && !tf_verify(output.fd, 8, 4, 3, 17));
  CHECK(copy_file_range(input.fd, readonly, output.fd, NULL, 4, 0) == -1 && errno == EFAULT);
  CHECK(lseek(output.fd, 0, SEEK_CUR) == 8 && !tf_verify(output.fd, 4, 4, 8, 17));
  CHECK(copy_file_range(input.fd, NULL, output.fd, readonly, 4, 0) == -1 && errno == EFAULT);
  CHECK(lseek(input.fd, 0, SEEK_CUR) == 17 && !tf_verify(output.fd, 8, 4, 13, 17));
  CHECK(copy_file_range(input.fd, readonly, output.fd, NULL, 0, 0) == 0);
  CHECK(sendfile(output.fd, input.fd, readonly, 0) == -1 && errno == EFAULT);
  CHECK(sendfile(-1, input.fd, readonly, 1) == -1 && errno == EFAULT);
  CHECK(copy_file_range(input.fd, readonly, output.fd, NULL, 0, 1) == -1 && errno == EINVAL);
  CHECK(!mprotect(readonly, page, PROT_READ | PROT_WRITE));
  *readonly = 64;
  CHECK(!mprotect(readonly, page, PROT_READ));
  CHECK(copy_file_range(input.fd, readonly, output.fd, NULL, 4, 0) == 0);
  CHECK(sendfile(output.fd, input.fd, readonly, 4) == -1 && errno == EFAULT);
  CHECK(*readonly == 64 && lseek(input.fd, 0, SEEK_CUR) == 17 &&
        lseek(output.fd, 0, SEEK_CUR) == 8);
out:
  if (readonly != MAP_FAILED)
    munmap(readonly, page);
  tf_destroy(&output);
  tf_destroy(&input);
  return failed;
}
int transfer_offsets(void) {
  return combinations(TF_SENDFILE) || combinations(TF_COPY_RANGE) || same_file() ||
         invalid_ranges() || late_copyout();
}
