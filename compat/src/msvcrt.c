// MSVC C runtime functions that don't exist on macOS.
#include "shogo_platform.h"

char *shogo_strupr(char *s)
{
    for (char *p = s; p && *p; ++p) *p = (char)toupper((unsigned char)*p);
    return s;
}

char *shogo_strlwr(char *s)
{
    for (char *p = s; p && *p; ++p) *p = (char)tolower((unsigned char)*p);
    return s;
}

static char *format_radix(unsigned long v, int negative, char *buf, int radix)
{
    char tmp[66];
    int n = 0;
    if (radix < 2 || radix > 36) { buf[0] = '\0'; return buf; }
    do { int d = (int)(v % (unsigned long)radix); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (unsigned long)radix; } while (v);
    char *p = buf;
    if (negative) *p++ = '-';
    while (n) *p++ = tmp[--n];
    *p = '\0';
    return buf;
}

char *shogo_ltoa(long value, char *buf, int radix)
{
    int negative = (radix == 10 && value < 0);
    unsigned long v = negative ? (unsigned long)(-(value + 1)) + 1 : (unsigned long)value;
    return format_radix(v, negative, buf, radix);
}

char *shogo_itoa(int value, char *buf, int radix)
{
    if (radix != 10) return format_radix((unsigned int)value, 0, buf, radix);
    return shogo_ltoa(value, buf, radix);
}
