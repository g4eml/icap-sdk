/* SPDX-License-Identifier: MIT
 * ft8_port.c - the libc subset the vendored ft8_lib needs (see ft8_port.h).  Same code in every build.
 */
#include "ft8_port.h"

size_t ft8p_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
char *ft8p_strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++) != 0) {} return r; }
char *ft8p_strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
char *ft8p_strcat(char *d, const char *s) { ft8p_strcpy(d + ft8p_strlen(d), s); return d; }
int ft8p_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
int ft8p_strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (int)(unsigned char)*a - (int)(unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}
char *ft8p_strchr(const char *s, int c)
{
    for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; }
}
int ft8p_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return (int)*p - (int)*q;
    return 0;
}
void *ft8p_memcpy(void *d, const void *s, size_t n) { char *p = d; const char *q = s; while (n--) *p++ = *q++; return d; }
void *ft8p_memset(void *d, int c, size_t n) { char *p = d; while (n--) *p++ = (char)c; return d; }
int ft8p_atoi(const char *s)
{
    int v = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
