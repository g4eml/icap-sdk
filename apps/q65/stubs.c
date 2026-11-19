/* SPDX-License-Identifier: GPL-3.0-or-later
 * Freestanding support symbols the Rust staticlib / libgcc expect but
 * that are absent with -nostdlib.  panic=abort means unwinding never
 * runs, so the personality / unwind hooks are empty. */
#include <stddef.h>

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    while (n--) *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    if (a < b) { while (n--) *a++ = *b++; }
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *a = d;
    while (n--) *a++ = (unsigned char)c;
    return d;
}
int memcmp(const void *x, const void *y, size_t n) {
    const unsigned char *a = x, *b = y;
    while (n--) { if (*a != *b) return (int)*a - (int)*b; a++; b++; }
    return 0;
}
/* unwinding disabled (panic=abort) */
int rust_eh_personality(void) { return 0; }
void __aeabi_unwind_cpp_pr0(void) {}
void __aeabi_unwind_cpp_pr1(void) {}
void __aeabi_unwind_cpp_pr2(void) {}
