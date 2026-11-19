/* SPDX-License-Identifier: GPL-3.0-or-later
 * ft8enc.c - FT8 encoder, see ft8enc.h.  Tones from the vendored ft8_lib (MIT, ft8/ft8/encode.c,
 * crc.c, constants.c); 77-bit packing from msg77.c (GPL-3.0, after WSJT-X packjt77).
 */
#include "ft8enc.h"
#include "msg77.h"
#include "ft8/ft8/encode.h"

int ft8_encode_msg(const char *msg, uint8_t tones[FT8_NSYM])
{
    uint8_t b[10];
    int rc;
    if (!tones) return TXE_ARG;
    if ((rc = msg77_pack(msg, b)) != 0) return rc;
    ft8_encode(b, tones);
    return 0;
}

int ft8_tx_init(ft8_tx_t *t, const char *msg, double f0_hz, int level)
{
    int rc;
    if (!t) return TXE_ARG;
    if ((rc = ft8_encode_msg(msg, t->tones)) != 0) return rc;
    return txs_mfsk_init(&t->s, t->tones, FT8_NSYM, FT8_NSPS, 1u, f0_hz, 6.25, 8, 1, level, FT8_RAMP);
}
