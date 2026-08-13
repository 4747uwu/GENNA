/* gn_compat.h — the two libc extensions the existing tests assume.
 *
 * The tests were written against glibc. mingw-w64 has clock_gettime but no
 * memmem. This is a test-only shim: the engine itself uses neither.        */
#ifndef GN_COMPAT_H
#define GN_COMPAT_H

#include <string.h>
#include <stddef.h>

#if defined(_WIN32) || defined(GN_NEED_MEMMEM)
static inline void *gn_memmem(const void *hay, size_t hn, const void *ndl, size_t nn) {
    if (nn == 0) return (void*)hay;
    if (hn < nn) return NULL;
    const unsigned char *h = (const unsigned char*)hay;
    const unsigned char *n = (const unsigned char*)ndl;
    const unsigned char *end = h + (hn - nn);
    for (const unsigned char *p = h; p <= end; p++)
        if (p[0] == n[0] && memcmp(p, n, nn) == 0) return (void*)p;
    return NULL;
}
#define memmem gn_memmem
#endif

#endif /* GN_COMPAT_H */
