#pragma once
/* strlcpy for the firmware's sources: glibc has it only from 2.38 on, the SS1's toolchain (glibc 2.32) doesn't.
   No libc header here: the sources define _GNU_SOURCE before including theirs. */
#include <stddef.h>
#define strlcpy smc_strlcpy
static inline size_t smc_strlcpy(char *d, const char *s, size_t n)
{
    size_t l = __builtin_strlen(s);
    if (n) {
        size_t c = l < n - 1 ? l : n - 1;
        __builtin_memcpy(d, s, c);
        d[c] = 0;
    }
    return l;
}
