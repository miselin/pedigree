#include "vterm_internal.h"

#include <stdint.h>
#include <stddef.h>

static void append_char(char *buffer, size_t size, size_t *length, char value) {
  if (*length + 1 < size)
    buffer[*length] = value;
  ++*length;
}

static void append_string(char *buffer, size_t size, size_t *length,
                          const char *value, size_t limit) {
  if (!value)
    value = "(null)";
  while (*value && limit--) {
    append_char(buffer, size, length, *value);
    ++value;
  }
}

static void append_unsigned(char *buffer, size_t size, size_t *length,
                            unsigned long long value, unsigned base) {
  static const char digits[] = "0123456789abcdef";
  char reversed[32];
  size_t count = 0;

  do {
    reversed[count++] = digits[value % base];
    value /= base;
  } while (value && count < sizeof(reversed));

  while (count)
    append_char(buffer, size, length, reversed[--count]);
}

static void append_signed(char *buffer, size_t size, size_t *length, long long value) {
  if (value < 0) {
    append_char(buffer, size, length, '-');
    append_unsigned(buffer, size, length, (unsigned long long)(-(value + 1)) + 1, 10);
  } else {
    append_unsigned(buffer, size, length, (unsigned long long)value, 10);
  }
}

int vterm_vsnprintf(char *buffer, size_t size, const char *format, va_list args) {
  size_t length = 0;

  while (*format) {
    if (*format != '%') {
      append_char(buffer, size, &length, *format++);
      continue;
    }

    ++format;
    if (*format == '%') {
      append_char(buffer, size, &length, *format++);
      continue;
    }

    size_t string_limit = (size_t)-1;
    if (*format == '.') {
      ++format;
      if (*format == '*') {
        string_limit = (size_t)va_arg(args, int);
        ++format;
      }
    }

    bool long_value = false;
    bool size_value = false;
    if (*format == 'l') {
      long_value = true;
      ++format;
    } else if (*format == 'z') {
      size_value = true;
      ++format;
    }

    switch (*format++) {
      case 'c':
        append_char(buffer, size, &length, (char)va_arg(args, int));
        break;
      case 'd':
        if (size_value)
          append_signed(buffer, size, &length, (long long)va_arg(args, ptrdiff_t));
        else if (long_value)
          append_signed(buffer, size, &length, (long long)va_arg(args, long));
        else
          append_signed(buffer, size, &length, (long long)va_arg(args, int));
        break;
      case 'u':
        if (size_value)
          append_unsigned(buffer, size, &length, (unsigned long long)va_arg(args, size_t), 10);
        else if (long_value)
          append_unsigned(buffer, size, &length, (unsigned long long)va_arg(args, unsigned long), 10);
        else
          append_unsigned(buffer, size, &length, (unsigned long long)va_arg(args, unsigned), 10);
        break;
      case 's':
        append_string(buffer, size, &length, va_arg(args, const char *), string_limit);
        break;
      default:
        append_char(buffer, size, &length, '?');
        break;
    }
  }

  if (size)
    buffer[length < size ? length : size - 1] = 0;
  return (int)length;
}

int vterm_snprintf(char *buffer, size_t size, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int result = vterm_vsnprintf(buffer, size, format, args);
  va_end(args);
  return result;
}

void vterm_fatal(void) {
  __builtin_trap();
}
