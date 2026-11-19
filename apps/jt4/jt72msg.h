/* SPDX-License-Identifier: GPL-3.0-or-later
 * jt72msg.h - unpack the 72-bit JT4/JT9/JT65 message (port of WSJT-X packjt.f90).
 */
#ifndef JT72MSG_H
#define JT72MSG_H
#include <stdint.h>
/* b: 72 bits, MSB first. out: NUL-terminated, trailing blanks removed (<=22 chars).
 * returns 0, or -1 for the '000AAA' wrong-mode pattern WSJT-X rejects. */
int jt72_unpack(const uint8_t b[9], char out[23]);
#endif
