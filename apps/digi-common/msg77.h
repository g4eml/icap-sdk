/* SPDX-License-Identifier: GPL-3.0-or-later
 * msg77.h - pack a WSJT-X 77-bit "standard" message (i3 = 1, or i3 = 2 with /P), the subset
 * needed by the FT8 and Q65 beacons:   TOKEN1 CALL2 [GRID4]
 *   TOKEN1 = DE | QRZ | CQ | standard call;  CALL2 = standard call;  calls may carry /P or /R.
 * Exactly what WSJT-X pack77_1 produces for these messages; anything WSJT-X would encode
 * differently (hashed <calls>, dropped DL/ prefixes, free text, other types) is refused
 * with TXE_CALL / TXE_GRID / TXE_MSG instead of silently changing the message.
 */
#ifndef MSG77_H
#define MSG77_H
#include <stdint.h>

/* msg: e.g. "DE SR6LEG JO81" (case-insensitive, blanks collapsed).
 * out: the 77 bits MSB first in out[0..9] (out[9] low 3 bits = 0).  Returns 0 or TXE_*. */
int msg77_pack(const char *msg, uint8_t out[10]);

/* Builds "<prefix> <call> <loc4>" (prefix "DE" for beacons) into out (>= 24 bytes).
 * Validates loc (4 chars, or 6 chars of which only the first 4 are used).  Returns 0 or TXE_*. */
int msg77_beacon_text(const char *prefix, const char *call, const char *loc, char *out);

#endif
