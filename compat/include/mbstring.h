// Stand-in for MSVC <mbstring.h>. Shogo's text is single-byte, so the
// multibyte-string functions reduce to their plain byte-string versions.
#ifndef SHOGO_COMPAT_MBSTRING_H
#define SHOGO_COMPAT_MBSTRING_H

#include "shogo_platform.h"

#ifdef __cplusplus
#define SHOGO_MBS_INLINE static inline
#else
#define SHOGO_MBS_INLINE static inline
#endif

SHOGO_MBS_INLINE unsigned char *_mbsinc(const unsigned char *p) { return (unsigned char *)(p + 1); }
SHOGO_MBS_INLINE unsigned char *_mbsdec(const unsigned char *start, const unsigned char *p)
{
    return p > start ? (unsigned char *)(p - 1) : NULL;
}
SHOGO_MBS_INLINE size_t _mbstrlen(const char *s) { return strlen(s); }
SHOGO_MBS_INLINE size_t _mbsnbcnt(const unsigned char *s, size_t n)
{
    size_t len = strlen((const char *)s);
    return n < len ? n : len;
}
SHOGO_MBS_INLINE int _mbscmp(const unsigned char *a, const unsigned char *b)
{
    return strcmp((const char *)a, (const char *)b);
}
SHOGO_MBS_INLINE int _mbsnbcmp(const unsigned char *a, const unsigned char *b, size_t n)
{
    return strncmp((const char *)a, (const char *)b, n);
}
SHOGO_MBS_INLINE unsigned char *_mbsncpy(unsigned char *d, const unsigned char *s, size_t n)
{
    return (unsigned char *)strncpy((char *)d, (const char *)s, n);
}
SHOGO_MBS_INLINE unsigned char *_mbsrchr(const unsigned char *s, unsigned int c)
{
    return (unsigned char *)strrchr((const char *)s, (int)c);
}
SHOGO_MBS_INLINE unsigned char *_mbsspnp(const unsigned char *s, const unsigned char *set)
{
    s += strspn((const char *)s, (const char *)set);
    return *s ? (unsigned char *)s : NULL;
}

#endif
