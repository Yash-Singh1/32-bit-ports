#ifndef LP32_CRASH_OUTPUT_H
#define LP32_CRASH_OUTPUT_H

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

/* Signal-handler output must not reenter stdio, malloc, or locale locks.
   Only strings, characters, pointers and integer printf conversions are
   supported. Each record uses bounded stack storage and write(2). */
static inline size_t lp32_crash_vformat(char *out, size_t capacity,
                                      const char *format, va_list arguments)
{
    size_t used = 0;
#define CRASH_CHAR(c) do { if (used + 1 < capacity) out[used++] = (c); } while (0)
    while (*format && used + 1 < capacity) {
        if (*format != '%') { CRASH_CHAR(*format++); continue; }
        ++format;
        int left = 0, zero = 0;
        if (*format == '-') { left = 1; ++format; }
        if (*format == '0') { zero = 1; ++format; }
        unsigned width = 0;
        while (*format >= '0' && *format <= '9') {
            if (width < capacity) width = width * 10 + (unsigned)(*format - '0');
            ++format;
        }
        if (width > capacity) width = (unsigned)capacity;
        enum { NORMAL, LONG, LONG_LONG, SIZE } length = NORMAL;
        if (*format == 'l') {
            ++format; length = LONG;
            if (*format == 'l') { ++format; length = LONG_LONG; }
        } else if (*format == 'z') { ++format; length = SIZE; }
        char conversion = *format;
        if (!conversion) break;
        ++format;
        if (conversion == '%') { CRASH_CHAR('%'); continue; }
        if (conversion == 's' || conversion == 'c') {
            char character;
            const char *string;
            size_t count = 0;
            if (conversion == 'c') {
                character = (char)va_arg(arguments, int);
                string = &character; count = 1;
            } else {
                string = va_arg(arguments, const char *);
                if (!string) string = "(null)";
                while (count < capacity && string[count]) ++count;
            }
            if (!left) for (size_t i = count; i < width; ++i) CRASH_CHAR(' ');
            for (size_t i = 0; i < count; ++i) CRASH_CHAR(string[i]);
            if (left) for (size_t i = count; i < width; ++i) CRASH_CHAR(' ');
            continue;
        }
        if (conversion != 'd' && conversion != 'i' && conversion != 'u' &&
            conversion != 'x' && conversion != 'p') {
            CRASH_CHAR('?'); break;
        }
        uint64_t value;
        int negative = 0;
        if (conversion == 'p') {
            value = (uintptr_t)va_arg(arguments, void *);
            CRASH_CHAR('0'); CRASH_CHAR('x');
        } else if (conversion == 'd' || conversion == 'i') {
            int64_t signed_value = length == LONG_LONG ? va_arg(arguments, long long) :
                length == LONG ? va_arg(arguments, long) :
                length == SIZE ? va_arg(arguments, ssize_t) : va_arg(arguments, int);
            negative = signed_value < 0;
            value = negative ? 0 - (uint64_t)signed_value : (uint64_t)signed_value;
        } else {
            value = length == LONG_LONG ? va_arg(arguments, unsigned long long) :
                length == LONG ? va_arg(arguments, unsigned long) :
                length == SIZE ? va_arg(arguments, size_t) : va_arg(arguments, unsigned);
        }
        unsigned base = conversion == 'x' || conversion == 'p' ? 16 : 10;
        char digits[64];
        unsigned count = 0;
        do {
            digits[count++] = "0123456789abcdef"[value % base];
            value /= base;
        } while (value);
        unsigned total = count + negative;
        if (!left && !zero) for (unsigned i = total; i < width; ++i) CRASH_CHAR(' ');
        if (negative) CRASH_CHAR('-');
        if (!left && zero) for (unsigned i = total; i < width; ++i) CRASH_CHAR('0');
        while (count) CRASH_CHAR(digits[--count]);
        if (left) for (unsigned i = total; i < width; ++i) CRASH_CHAR(' ');
    }
    if (capacity) out[used] = '\0';
#undef CRASH_CHAR
    return used;
}

static inline void lp32_crash_format(char *out, size_t capacity,
                                    const char *format, ...)
    __attribute__((format(printf, 3, 4)));
static inline void lp32_crash_format(char *out, size_t capacity,
                                    const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    lp32_crash_vformat(out, capacity, format, arguments);
    va_end(arguments);
}

static inline void lp32_crash_writef(int fd, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
static inline void lp32_crash_writef(int fd, const char *format, ...)
{
    int saved_errno = errno;
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    size_t count = lp32_crash_vformat(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    size_t offset = 0;
    while (offset < count) {
        ssize_t written = write(fd, buffer + offset, count - offset);
        if (written > 0) offset += (size_t)written;
        else if (written < 0 && errno == EINTR) continue;
        else break;
    }
    errno = saved_errno;
}

#endif
