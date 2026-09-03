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

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"

struct buffer {
  char* data;
  size_t length;
  size_t capacity;
};

static int reserve(struct buffer* buffer, size_t extra) {
  size_t required;
  size_t capacity;
  char* data;

  if (extra > ((size_t)-1) - buffer->length - 1)
    return 0;

  required = buffer->length + extra + 1;
  if (required <= buffer->capacity)
    return 1;

  capacity = buffer->capacity ? buffer->capacity : 4096;
  while (capacity < required) {
    if (capacity > ((size_t)-1) / 2) {
      capacity = required;
      break;
    }
    capacity *= 2;
  }

  data = (char*)realloc(buffer->data, capacity);
  if (!data)
    return 0;

  buffer->data = data;
  buffer->capacity = capacity;
  return 1;
}

static int append(struct buffer* buffer, const char* data, size_t length) {
  if (!reserve(buffer, length))
    return 0;

  if (length)
    memcpy(buffer->data + buffer->length, data, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
  return 1;
}

static int read_schema(struct buffer* sql, const char* path) {
  char chunk[16384];
  FILE* file = fopen(path, "rb");

  if (!file) {
    fprintf(stderr, "pedigree-configdb: cannot open '%s': %s\n", path, strerror(errno));
    return 0;
  }

  while (!feof(file)) {
    size_t count = fread(chunk, 1, sizeof(chunk), file);
    if (count && !append(sql, chunk, count)) {
      fprintf(stderr, "pedigree-configdb: out of memory\n");
      fclose(file);
      return 0;
    }
    if (ferror(file)) {
      fprintf(stderr, "pedigree-configdb: cannot read '%s': %s\n", path, strerror(errno));
      fclose(file);
      return 0;
    }
  }

  if (fclose(file) != 0) {
    fprintf(stderr, "pedigree-configdb: cannot close '%s': %s\n", path, strerror(errno));
    return 0;
  }
  return 1;
}

static int starts_with_create_table(const char* sql, size_t length, size_t at) {
  static const char prefix[] = "create table ";
  size_t i;

  if (length - at < sizeof(prefix) - 1)
    return 0;

  for (i = 0; i < sizeof(prefix) - 1; ++i) {
    unsigned char c = (unsigned char)sql[at + i];
    if ((char)tolower(c) != prefix[i])
      return 0;
  }
  return 1;
}

static size_t find_statement_end(const char* sql, size_t length, size_t at, int require_line_end) {
  size_t i;

  for (i = at; i < length; ++i) {
    if (sql[i] != ';')
      continue;
    if (!require_line_end || i + 1 == length || sql[i + 1] == '\n')
      return i + 1;
  }
  return (size_t)-1;
}

static int collect_tables(const struct buffer* sql, struct buffer* tables) {
  size_t at;

  for (at = 0; at < sql->length; ++at) {
    size_t end;
    if (at && sql->data[at - 1] != '\n')
      continue;
    if (!starts_with_create_table(sql->data, sql->length, at))
      continue;

    end = find_statement_end(sql->data, sql->length, at, 1);
    if (end == (size_t)-1)
      continue;
    if (!append(tables, sql->data + at, end - at) || !append(tables, "\n", 1))
      return 0;
    at = end - 1;
  }
  return 1;
}

static int remove_tables(const struct buffer* sql, struct buffer* remainder) {
  size_t copied = 0;
  size_t at = 0;

  while (at < sql->length) {
    size_t end;
    if (at && sql->data[at - 1] != '\n') {
      ++at;
      continue;
    }
    if (!starts_with_create_table(sql->data, sql->length, at)) {
      ++at;
      continue;
    }

    end = find_statement_end(sql->data, sql->length, at, 0);
    if (end == (size_t)-1)
      break;
    if (!append(remainder, sql->data + copied, at - copied))
      return 0;
    copied = end;
    at = end;
  }

  return append(remainder, sql->data + copied, sql->length - copied);
}

static int create_database(const char* output, const struct buffer* tables,
                           const struct buffer* remainder) {
  struct buffer script = {0};
  sqlite3* database = NULL;
  char* error = NULL;
  int result;

  if (!append(&script, "begin;", 6) || !append(&script, tables->data, tables->length) ||
      !append(&script, remainder->data, remainder->length) || !append(&script, "commit;", 7)) {
    fprintf(stderr, "pedigree-configdb: out of memory\n");
    free(script.data);
    return 0;
  }

  if (remove(output) != 0 && errno != ENOENT) {
    fprintf(stderr, "pedigree-configdb: cannot replace '%s': %s\n", output, strerror(errno));
    free(script.data);
    return 0;
  }

  result = sqlite3_open(output, &database);
  if (result != SQLITE_OK) {
    fprintf(stderr, "pedigree-configdb: cannot create '%s': %s\n", output,
            database ? sqlite3_errmsg(database) : "SQLite initialization failed");
    if (database)
      sqlite3_close(database);
    free(script.data);
    remove(output);
    return 0;
  }

  result = sqlite3_exec(database, script.data, NULL, NULL, &error);
  if (result != SQLITE_OK) {
    fprintf(stderr, "pedigree-configdb: schema failed: %s\n",
            error ? error : sqlite3_errmsg(database));
    sqlite3_free(error);
    sqlite3_close(database);
    free(script.data);
    remove(output);
    return 0;
  }

  result = sqlite3_close(database);
  free(script.data);
  if (result != SQLITE_OK) {
    fprintf(stderr, "pedigree-configdb: cannot close '%s': %s\n", output, sqlite3_errstr(result));
    remove(output);
    return 0;
  }
  return 1;
}

int main(int argc, char** argv) {
  struct buffer sql = {0};
  struct buffer tables = {0};
  struct buffer remainder = {0};
  int i;
  int success = 0;

  if (argc < 3) {
    fprintf(stderr, "usage: pedigree-configdb OUTPUT SCHEMA [SCHEMA ...]\n");
    return 2;
  }

  for (i = 2; i < argc; ++i) {
    if (!read_schema(&sql, argv[i]))
      goto done;
  }

  if (!collect_tables(&sql, &tables) || !remove_tables(&sql, &remainder)) {
    fprintf(stderr, "pedigree-configdb: out of memory\n");
    goto done;
  }

  success = create_database(argv[1], &tables, &remainder);

done:
  if (!success)
    remove(argv[1]);
  free(remainder.data);
  free(tables.data);
  free(sql.data);
  return success ? 0 : 1;
}
