/* SPDX-License-Identifier: MIT
 * ft8_port.h - force-included (-include) when compiling the vendored ft8_lib sources in ft8/ft8
 * for the freestanding ICAP app.  The vendored files stay unmodified:
 *   - libc string / math calls are renamed to the small helpers in ft8_port.c (identical code on the radio,
 *     on x86 and under qemu-arm, so the ARM and host builds decode bit-identically);
 *   - debug logging is compiled out.
 */
#ifndef FT8_PORT_H
#define FT8_PORT_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define LOG_PRINTF(...) ((void)0)

size_t ft8p_strlen(const char *s);
char  *ft8p_strcpy(char *d, const char *s);
char  *ft8p_strncpy(char *d, const char *s, size_t n);
char  *ft8p_strcat(char *d, const char *s);
int    ft8p_strcmp(const char *a, const char *b);
int    ft8p_strncmp(const char *a, const char *b, size_t n);
char  *ft8p_strchr(const char *s, int c);
int    ft8p_memcmp(const void *a, const void *b, size_t n);
void  *ft8p_memcpy(void *d, const void *s, size_t n);
void  *ft8p_memset(void *d, int c, size_t n);
int    ft8p_atoi(const char *s);

#define strlen  ft8p_strlen
#define strcpy  ft8p_strcpy
#define strncpy ft8p_strncpy
#define strcat  ft8p_strcat
#define strcmp  ft8p_strcmp
#define strncmp ft8p_strncmp
#define strchr  ft8p_strchr
#define memcmp  ft8p_memcmp
#define memcpy  ft8p_memcpy
#define memset  ft8p_memset
#define atoi    ft8p_atoi
#define sqrtf   __builtin_sqrtf    /* vsqrt.f32 / sqrtss with -fno-math-errno: correctly rounded on both */

#endif
