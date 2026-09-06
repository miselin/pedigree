#define _GNU_SOURCE
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/stat.h>

static int ordinary(int kind) {
  int failed = 0;
  struct tf_file input = {.fd = -1}, output = {.fd = -1};
  const size_t length = 3 * TF_CHUNK + 37;
  struct stat st;
  CHECK(!tf_create(&input, "/tmp", length, 11) && !tf_create(&output, "/tmp", 0, 0));
  CHECK(tf_copy(kind, input.fd, NULL, output.fd, NULL, length + 99) == (ssize_t)length);
  CHECK(!tf_verify(output.fd, 0, length, 0, 11) && !tf_verify(input.fd, 0, length, 0, 11));
  CHECK(lseek(input.fd, 0, SEEK_CUR) == (off_t)length &&
        lseek(output.fd, 0, SEEK_CUR) == (off_t)length);
  CHECK(!fstat(output.fd, &st) && st.st_size == (off_t)length);
  CHECK(tf_copy(kind, input.fd, NULL, output.fd, NULL, 17) == 0);
  CHECK(tf_copy(kind, input.fd, NULL, output.fd, NULL, 0) == 0);
  CHECK(lseek(input.fd, 0, SEEK_CUR) == (off_t)length &&
        lseek(output.fd, 0, SEEK_CUR) == (off_t)length);
  CHECK(lseek(input.fd, length - 7, SEEK_SET) == (off_t)length - 7);
  CHECK(lseek(output.fd, 0, SEEK_SET) == 0);
  CHECK(tf_copy(kind, input.fd, NULL, output.fd, NULL, 0x80000000ULL) == 7);
  CHECK(!tf_verify(output.fd, 0, 7, length - 7, 11));
out:
  tf_destroy(&output);
  tf_destroy(&input);
  return failed;
}
static int access_and_seals(int kind) {
  int failed = 0, readonly = -1, writeonly = -1, sealed = -1;
  struct tf_file input = {.fd = -1}, output = {.fd = -1};
  CHECK(!tf_create(&input, "/tmp", 64, 13) && !tf_create(&output, "/tmp", 64, 29));
  CHECK((readonly = open(output.path, O_RDONLY)) >= 0 &&
        (writeonly = open(input.path, O_WRONLY)) >= 0);
  CHECK(tf_copy(kind, writeonly, NULL, output.fd, NULL, 1) == -1 && errno == EBADF);
  CHECK(tf_copy(kind, input.fd, NULL, readonly, NULL, 1) == -1 && errno == EBADF);
  CHECK(tf_copy(kind, -1, NULL, output.fd, NULL, 0) == -1 && errno == EBADF);
  CHECK(!fcntl(output.fd, F_SETFL, O_APPEND));
  CHECK(tf_copy(kind, input.fd, NULL, output.fd, NULL, 1) == -1 &&
        errno == (kind == TF_SENDFILE ? EINVAL : EBADF));
  CHECK(tf_copy(kind, input.fd, NULL, output.fd, NULL, 0) == -1 &&
        errno == (kind == TF_SENDFILE ? EINVAL : EBADF));
  CHECK(!tf_verify(output.fd, 0, 64, 0, 29));
  CHECK(lseek(input.fd, 0, SEEK_CUR) == 0 && lseek(output.fd, 0, SEEK_CUR) == 0);
  CHECK(!fcntl(output.fd, F_SETFL, 0) && !fcntl(input.fd, F_SETFL, O_APPEND));
  CHECK(tf_copy(kind, input.fd, NULL, output.fd, NULL, 4) == 4);
  CHECK(!tf_verify(output.fd, 0, 4, 0, 13));
  /* Both memfds share the private filesystem; CFR must reach write policy. */
  sealed = memfd_create("transfer-output", MFD_ALLOW_SEALING);
  CHECK(sealed >= 0 && !ftruncate(sealed, 64));
  int source = memfd_create("transfer-input", MFD_ALLOW_SEALING);
  CHECK(source >= 0);
  close(writeonly);
  writeonly = source;
  CHECK(write(source, "seal", 4) == 4 && lseek(source, 0, SEEK_SET) == 0);
  CHECK(!fcntl(sealed, F_ADD_SEALS, F_SEAL_WRITE));
  CHECK(tf_copy(kind, source, NULL, sealed, NULL, 4) == -1 && errno == EPERM);
  CHECK(lseek(source, 0, SEEK_CUR) == 0 && lseek(sealed, 0, SEEK_CUR) == 0);
  unsigned char byte = 1;
  CHECK(pread(sealed, &byte, 1, 0) == 1 && byte == 0);
out:
  if (sealed >= 0)
    close(sealed);
  if (writeonly >= 0)
    close(writeonly);
  if (readonly >= 0)
    close(readonly);
  tf_destroy(&output);
  tf_destroy(&input);
  return failed;
}
static int cross_filesystem(void) {
  int failed = 0;
  struct tf_file input = {.fd = -1}, output = {.fd = -1};
  struct stat left, right;
  CHECK(!tf_create(&input, "/tmp", 32, 3) && !tf_create(&output, "", 32, 7));
  CHECK(!fstat(input.fd, &left) && !fstat(output.fd, &right) && left.st_dev != right.st_dev);
  CHECK(copy_file_range(input.fd, NULL, output.fd, NULL, 4, 0) == -1 && errno == EXDEV);
  CHECK(!tf_verify(output.fd, 0, 32, 0, 7));
  CHECK(lseek(input.fd, 0, SEEK_CUR) == 0 && lseek(output.fd, 0, SEEK_CUR) == 0);
out:
  tf_destroy(&output);
  tf_destroy(&input);
  return failed;
}
static int partial_backend(int kind) {
  int failed = 0, input = -1, output = -1;
  unsigned char buffer[4096];
  struct stat st;
  CHECK((input = memfd_create("partial-input", MFD_ALLOW_SEALING)) >= 0);
  CHECK((output = memfd_create("partial-output", MFD_ALLOW_SEALING)) >= 0);
  for (size_t offset = 0; offset < TF_CHUNK + 17;) {
    size_t count = TF_CHUNK + 17 - offset;
    if (count > sizeof(buffer))
      count = sizeof(buffer);
    for (size_t n = 0; n < count; ++n)
      buffer[n] = tf_pattern(offset + n, 59);
    CHECK(pwrite(input, buffer, count, offset) == (ssize_t)count);
    offset += count;
  }
  CHECK(!ftruncate(output, TF_CHUNK) && !fcntl(output, F_ADD_SEALS, F_SEAL_GROW));
  /* The first complete bounce fits, while the next write cannot extend EOF. */
  CHECK(tf_copy(kind, input, NULL, output, NULL, TF_CHUNK + 17) == TF_CHUNK);
  CHECK(lseek(input, 0, SEEK_CUR) == TF_CHUNK && lseek(output, 0, SEEK_CUR) == TF_CHUNK);
  CHECK(!tf_verify(output, 0, TF_CHUNK, 0, 59));
  CHECK(!fstat(output, &st) && st.st_size == TF_CHUNK);
  CHECK(tf_copy(kind, input, NULL, output, NULL, 17) == -1 && errno == EPERM);
  CHECK(lseek(input, 0, SEEK_CUR) == TF_CHUNK && lseek(output, 0, SEEK_CUR) == TF_CHUNK);
out:
  if (output >= 0)
    close(output);
  if (input >= 0)
    close(input);
  return failed;
}
int transfer_file_copy(void) {
  return ordinary(TF_SENDFILE) || ordinary(TF_COPY_RANGE) || access_and_seals(TF_SENDFILE) ||
         access_and_seals(TF_COPY_RANGE) || cross_filesystem() || partial_backend(TF_SENDFILE) ||
         partial_backend(TF_COPY_RANGE);
}
