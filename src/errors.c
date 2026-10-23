#include <stdarg.h>
#include <stdio.h>

#include "errors.h"

void print_error(const char *format, ...) {
    va_list values;
    va_start(values, format);
    fputs("tunnel: ", stderr);
    vfprintf(stderr, format, values);
    fputc('\n', stderr);
    va_end(values);
}
