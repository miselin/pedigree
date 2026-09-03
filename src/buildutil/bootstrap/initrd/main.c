/*
 * Copyright (c) 2026 Pedigree Developers
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

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define TAR_BLOCK_SIZE 512U
#define TAR_RECORD_SIZE 10240U
#define COPY_BUFFER_SIZE 16384U

struct Entry {
  const char* name;
  size_t nameLength;
  const char* path;
};

struct Sha256 {
  uint32_t state[8];
  uint64_t bytes;
  unsigned char block[64];
  size_t blockLength;
};

static const uint32_t g_Sha256Constants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

static uint32_t rotateRight(uint32_t value, unsigned int count) {
  return (value >> count) | (value << (32U - count));
}

static uint32_t loadBigEndian32(const unsigned char* data) {
  return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) | ((uint32_t)data[2] << 8U) |
         (uint32_t)data[3];
}

static void sha256Transform(struct Sha256* sha, const unsigned char* block) {
  uint32_t words[64];
  uint32_t a, b, c, d, e, f, g, h;
  size_t i;

  for (i = 0; i < 16; ++i)
    words[i] = loadBigEndian32(block + (i * 4U));
  for (; i < 64; ++i) {
    const uint32_t s0 =
        rotateRight(words[i - 15], 7) ^ rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3U);
    const uint32_t s1 =
        rotateRight(words[i - 2], 17) ^ rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10U);
    words[i] = words[i - 16] + s0 + words[i - 7] + s1;
  }

  a = sha->state[0];
  b = sha->state[1];
  c = sha->state[2];
  d = sha->state[3];
  e = sha->state[4];
  f = sha->state[5];
  g = sha->state[6];
  h = sha->state[7];

  for (i = 0; i < 64; ++i) {
    const uint32_t sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
    const uint32_t choice = (e & f) ^ ((~e) & g);
    const uint32_t temporary1 = h + sum1 + choice + g_Sha256Constants[i] + words[i];
    const uint32_t sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temporary2 = sum0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }

  sha->state[0] += a;
  sha->state[1] += b;
  sha->state[2] += c;
  sha->state[3] += d;
  sha->state[4] += e;
  sha->state[5] += f;
  sha->state[6] += g;
  sha->state[7] += h;
}

static void sha256Start(struct Sha256* sha) {
  static const uint32_t initialState[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                           0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

  memcpy(sha->state, initialState, sizeof(initialState));
  sha->bytes = 0;
  sha->blockLength = 0;
}

static void sha256Update(struct Sha256* sha, const unsigned char* data, size_t length) {
  sha->bytes += length;
  while (length) {
    size_t available = sizeof(sha->block) - sha->blockLength;
    size_t amount = length < available ? length : available;
    memcpy(sha->block + sha->blockLength, data, amount);
    sha->blockLength += amount;
    data += amount;
    length -= amount;
    if (sha->blockLength == sizeof(sha->block)) {
      sha256Transform(sha, sha->block);
      sha->blockLength = 0;
    }
  }
}

static void sha256Finish(struct Sha256* sha, unsigned char digest[32]) {
  const uint64_t bitLength = sha->bytes * 8U;
  size_t i;

  sha->block[sha->blockLength++] = 0x80;
  if (sha->blockLength > 56) {
    memset(sha->block + sha->blockLength, 0, 64 - sha->blockLength);
    sha256Transform(sha, sha->block);
    sha->blockLength = 0;
  }
  memset(sha->block + sha->blockLength, 0, 56 - sha->blockLength);
  for (i = 0; i < 8; ++i)
    sha->block[63 - i] = (unsigned char)(bitLength >> (i * 8U));
  sha256Transform(sha, sha->block);

  for (i = 0; i < 8; ++i) {
    digest[(i * 4) + 0] = (unsigned char)(sha->state[i] >> 24U);
    digest[(i * 4) + 1] = (unsigned char)(sha->state[i] >> 16U);
    digest[(i * 4) + 2] = (unsigned char)(sha->state[i] >> 8U);
    digest[(i * 4) + 3] = (unsigned char)sha->state[i];
  }
}

static int writeAll(FILE* output, const void* data, size_t length) {
  return fwrite(data, 1, length, output) == length;
}

static int writeZeros(FILE* output, size_t length) {
  static const unsigned char zeros[TAR_BLOCK_SIZE] = {0};
  while (length) {
    size_t amount = length < sizeof(zeros) ? length : sizeof(zeros);
    if (!writeAll(output, zeros, amount))
      return 0;
    length -= amount;
  }
  return 1;
}

static int formatOctal(unsigned char* field, size_t width, uint64_t value, const char* label) {
  size_t i = width - 1;
  field[i] = '\0';
  while (i) {
    field[--i] = (unsigned char)('0' + (value & 7U));
    value >>= 3U;
  }
  if (value) {
    fprintf(stderr, "initrd: %s is too large for a ustar header\n", label);
    return 0;
  }
  return 1;
}

static int makeTarHeader(unsigned char header[TAR_BLOCK_SIZE], const struct Entry* entry,
                         uint64_t size) {
  unsigned int checksum = 0;
  size_t i;

  memset(header, 0, TAR_BLOCK_SIZE);
  memcpy(header, entry->name, entry->nameLength);
  if (!formatOctal(header + 100, 8, 0755, "mode") || !formatOctal(header + 108, 8, 0, "uid") ||
      !formatOctal(header + 116, 8, 0, "gid") ||
      !formatOctal(header + 124, 12, size, entry->path) ||
      !formatOctal(header + 136, 12, 0, "mtime"))
    return 0;

  memset(header + 148, ' ', 8);
  header[156] = '0';
  memcpy(header + 257, "ustar", 5);
  memcpy(header + 263, "00", 2);

  for (i = 0; i < TAR_BLOCK_SIZE; ++i)
    checksum += header[i];
  if (!formatOctal(header + 148, 7, checksum, "header checksum"))
    return 0;
  header[155] = ' ';
  return 1;
}

static int getFileSize(FILE* input, const char* path, uint64_t* size) {
  long position;

  if (fseek(input, 0, SEEK_END) != 0 || (position = ftell(input)) < 0 ||
      fseek(input, 0, SEEK_SET) != 0) {
    fprintf(stderr, "initrd: cannot determine size of %s: %s\n", path, strerror(errno));
    return 0;
  }
  *size = (uint64_t)position;
  return 1;
}

static int appendEntry(FILE* archive, FILE* manifest, const struct Entry* entry,
                       uint64_t* archiveLength) {
  static const char hex[] = "0123456789abcdef";
  unsigned char buffer[16384];
  unsigned char digest[32];
  unsigned char header[TAR_BLOCK_SIZE];
  struct Sha256 sha;
  uint64_t size;
  uint64_t remaining;
  size_t padding;
  size_t amount;
  size_t i;
  FILE* input = fopen(entry->path, "rb");

  if (!input) {
    fprintf(stderr, "initrd: cannot open %s: %s\n", entry->path, strerror(errno));
    return 0;
  }
  if (!getFileSize(input, entry->path, &size) || !makeTarHeader(header, entry, size) ||
      !writeAll(archive, header, sizeof(header))) {
    if (ferror(archive))
      fprintf(stderr, "initrd: cannot write archive: %s\n", strerror(errno));
    fclose(input);
    return 0;
  }

  sha256Start(&sha);
  remaining = size;
  while (remaining) {
    amount = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
    if (fread(buffer, 1, amount, input) != amount) {
      fprintf(stderr, "initrd: cannot read %s: %s\n", entry->path,
              ferror(input) ? strerror(errno) : "unexpected end of file");
      fclose(input);
      return 0;
    }
    if (!writeAll(archive, buffer, amount)) {
      fprintf(stderr, "initrd: cannot write archive: %s\n", strerror(errno));
      fclose(input);
      return 0;
    }
    sha256Update(&sha, buffer, amount);
    remaining -= amount;
  }
  if (fclose(input) != 0) {
    fprintf(stderr, "initrd: cannot close %s: %s\n", entry->path, strerror(errno));
    return 0;
  }

  padding = (size_t)((TAR_BLOCK_SIZE - (size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE);
  if (!writeZeros(archive, padding)) {
    fprintf(stderr, "initrd: cannot write archive: %s\n", strerror(errno));
    return 0;
  }
  *archiveLength += TAR_BLOCK_SIZE + size + padding;

  sha256Finish(&sha, digest);
  for (i = 0; i < sizeof(digest); ++i) {
    if (fputc(hex[digest[i] >> 4U], manifest) == EOF ||
        fputc(hex[digest[i] & 0xfU], manifest) == EOF)
      return 0;
  }
  if (fprintf(manifest, "  %.*s\n", (int)entry->nameLength, entry->name) < 0)
    return 0;
  return 1;
}

static int writeGzip(const char* archivePath, const char* outputPath) {
  unsigned char inputBuffer[COPY_BUFFER_SIZE];
  unsigned char outputBuffer[COPY_BUFFER_SIZE];
  gz_header header;
  z_stream stream;
  size_t amount;
  size_t outputAmount;
  int flush;
  int zlibResult;
  int streamStarted = 0;
  FILE* input = fopen(archivePath, "rb");
  FILE* output = NULL;
  int ok = 0;

  if (!input) {
    fprintf(stderr, "initrd: cannot open %s: %s\n", archivePath, strerror(errno));
    return 0;
  }
  output = fopen(outputPath, "wb");
  if (!output) {
    fprintf(stderr, "initrd: cannot open %s: %s\n", outputPath, strerror(errno));
    fclose(input);
    return 0;
  }

  memset(&stream, 0, sizeof(stream));
  zlibResult =
      deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);
  if (zlibResult != Z_OK) {
    fprintf(stderr, "initrd: cannot initialize zlib: %s\n",
            stream.msg ? stream.msg : zError(zlibResult));
    goto done;
  }
  streamStarted = 1;

  memset(&header, 0, sizeof(header));
  header.time = 0;
  header.os = 255;
  zlibResult = deflateSetHeader(&stream, &header);
  if (zlibResult != Z_OK) {
    fprintf(stderr, "initrd: cannot configure deterministic gzip metadata: %s\n",
            stream.msg ? stream.msg : zError(zlibResult));
    goto done;
  }

  do {
    amount = fread(inputBuffer, 1, sizeof(inputBuffer), input);
    if (ferror(input)) {
      fprintf(stderr, "initrd: cannot read %s: %s\n", archivePath, strerror(errno));
      goto done;
    }

    flush = feof(input) ? Z_FINISH : Z_NO_FLUSH;
    stream.next_in = inputBuffer;
    stream.avail_in = (uInt)amount;

    do {
      stream.next_out = outputBuffer;
      stream.avail_out = (uInt)sizeof(outputBuffer);
      zlibResult = deflate(&stream, flush);
      if (zlibResult != Z_OK && zlibResult != Z_STREAM_END) {
        fprintf(stderr, "initrd: zlib compression failed: %s\n",
                stream.msg ? stream.msg : zError(zlibResult));
        goto done;
      }
      outputAmount = sizeof(outputBuffer) - stream.avail_out;
      if (!writeAll(output, outputBuffer, outputAmount)) {
        fprintf(stderr, "initrd: cannot write %s: %s\n", outputPath, strerror(errno));
        goto done;
      }
    } while (stream.avail_in != 0 || (flush == Z_FINISH && zlibResult != Z_STREAM_END));

    if (stream.avail_in != 0 || (flush == Z_FINISH && zlibResult != Z_STREAM_END)) {
      fprintf(stderr, "initrd: zlib did not finish the gzip stream\n");
      goto done;
    }
  } while (flush != Z_FINISH);

  ok = 1;

done:
  if (streamStarted && deflateEnd(&stream) != Z_OK)
    ok = 0;
  if (fclose(input) != 0)
    ok = 0;
  if (fclose(output) != 0)
    ok = 0;
  return ok;
}

static void usage(const char* program) {
  fprintf(stderr,
          "usage: %s --output PATH --uncompressed PATH --manifest PATH "
          "NAME=PATH ...\n",
          program);
}

static void removeOutputs(const char* outputPath, const char* archivePath,
                          const char* manifestPath) {
  remove(outputPath);
  remove(archivePath);
  remove(manifestPath);
}

static int parseEntries(int argc, char** argv, struct Entry* entries, size_t* entryCount,
                        const char** outputPath, const char** archivePath,
                        const char** manifestPath) {
  size_t count = 0;
  int i;

  for (i = 1; i < argc; ++i) {
    const char** option = NULL;
    const char* separator;
    size_t j;

    if (strcmp(argv[i], "--output") == 0)
      option = outputPath;
    else if (strcmp(argv[i], "--uncompressed") == 0)
      option = archivePath;
    else if (strcmp(argv[i], "--manifest") == 0)
      option = manifestPath;

    if (option) {
      if (++i == argc || *option)
        return 0;
      *option = argv[i];
      continue;
    }
    if (argv[i][0] == '-')
      return 0;

    separator = strchr(argv[i], '=');
    if (!separator || separator == argv[i] || !separator[1])
      return 0;
    entries[count].name = argv[i];
    entries[count].nameLength = (size_t)(separator - argv[i]);
    entries[count].path = separator + 1;
    if (entries[count].nameLength > 100 ||
        memchr(entries[count].name, '/', entries[count].nameLength) ||
        (entries[count].nameLength == 1 && entries[count].name[0] == '.') ||
        (entries[count].nameLength == 2 && entries[count].name[0] == '.' &&
         entries[count].name[1] == '.'))
      return 0;
    for (j = 0; j < count; ++j) {
      if (entries[j].nameLength == entries[count].nameLength &&
          memcmp(entries[j].name, entries[count].name, entries[count].nameLength) == 0)
        return 0;
    }
    ++count;
  }

  *entryCount = count;
  if (!*outputPath || !*archivePath || !*manifestPath || !count ||
      strcmp(*outputPath, *archivePath) == 0 || strcmp(*outputPath, *manifestPath) == 0 ||
      strcmp(*archivePath, *manifestPath) == 0)
    return 0;

  for (i = 0; i < (int)count; ++i) {
    if (strcmp(entries[i].path, *outputPath) == 0 || strcmp(entries[i].path, *archivePath) == 0 ||
        strcmp(entries[i].path, *manifestPath) == 0)
      return 0;
  }
  return 1;
}

int main(int argc, char** argv) {
  const char* outputPath = NULL;
  const char* archivePath = NULL;
  const char* manifestPath = NULL;
  struct Entry* entries;
  size_t entryCount;
  size_t i;
  uint64_t archiveLength = 0;
  size_t finalPadding;
  FILE* archive;
  FILE* manifest;
  int ok = 0;

  entries = (struct Entry*)calloc((size_t)argc, sizeof(*entries));
  if (!entries) {
    fprintf(stderr, "initrd: out of memory\n");
    return 1;
  }
  if (!parseEntries(argc, argv, entries, &entryCount, &outputPath, &archivePath, &manifestPath)) {
    usage(argv[0]);
    free(entries);
    return 2;
  }

  archive = fopen(archivePath, "wb");
  if (!archive) {
    fprintf(stderr, "initrd: cannot open %s: %s\n", archivePath, strerror(errno));
    removeOutputs(outputPath, archivePath, manifestPath);
    free(entries);
    return 1;
  }
  manifest = fopen(manifestPath, "wb");
  if (!manifest) {
    fprintf(stderr, "initrd: cannot open %s: %s\n", manifestPath, strerror(errno));
    fclose(archive);
    removeOutputs(outputPath, archivePath, manifestPath);
    free(entries);
    return 1;
  }

  for (i = 0; i < entryCount; ++i) {
    if (!appendEntry(archive, manifest, &entries[i], &archiveLength))
      goto done;
  }
  if (!writeZeros(archive, TAR_BLOCK_SIZE * 2U))
    goto done;
  archiveLength += TAR_BLOCK_SIZE * 2U;
  finalPadding = (size_t)((TAR_RECORD_SIZE - (archiveLength % TAR_RECORD_SIZE)) % TAR_RECORD_SIZE);
  if (!writeZeros(archive, finalPadding))
    goto done;
  ok = 1;

done:
  if (fclose(manifest) != 0)
    ok = 0;
  if (fclose(archive) != 0)
    ok = 0;
  if (ok)
    ok = writeGzip(archivePath, outputPath);
  if (!ok) {
    removeOutputs(outputPath, archivePath, manifestPath);
    fprintf(stderr, "initrd: failed to create archive\n");
  }
  free(entries);
  return ok ? 0 : 1;
}
