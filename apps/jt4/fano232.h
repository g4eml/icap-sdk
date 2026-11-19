/* SPDX-License-Identifier: GPL-3.0-or-later
 * fano232.h - K=32 r=1/2 convolutional encoder + Fano sequential decoder (WSJT-X port).
 */
#ifndef FANO232_H
#define FANO232_H
#include <stdint.h>

#define FANO232_MAXBITS 103
typedef struct {                       /* one tree node, 44 bytes */
    int32_t  gamma;                    /* path metric up to (not including) this node's branch */
    uint32_t st;                       /* encoder state with the branch currently taken */
    uint32_t sel;                      /* encoder symbol pair of the 0-branch | 0x100 second branch in use | 0x200 tail node */
    int32_t  bf[4];                    /* per symbol pair: 2 * metric of the better branch + (1 if that is the 1-branch) */
    int32_t  sec[4];                   /* per symbol pair: metric of the other branch */
} fano232_node_t;
typedef struct {                       /* ~9.7 KiB: keep it in the caller's work area, not on stack */
    fano232_node_t n[FANO232_MAXBITS + 2];   /* [0] = sentinel */
    uint8_t  e0[2048], e1[2048], e2[1024];   /* encoder output (2 bits) for state bits 0-10, 11-21, 22-31 */
} fano232_work_t;

/* fill the encoder tables of w: once, before the first fano232() with this work area */
void fano232_init(fano232_work_t *w);
/* data: nbits bits MSB first -> sym[2*nbits] (0/1) */
void conv232_encode(const uint8_t *data, int nbits, uint8_t *sym);
/* symbol: 2*nbits soft symbols, +127 = confident '1'. dat gets (nbits+7)/8 bytes.
 * returns 0 ok, -1 cycle limit, -2 aborted. */
int fano232(const int8_t *symbol, int nbits, int maxcycles, uint8_t *dat,
            fano232_work_t *w, int (*should_abort)(void), uint32_t *ncycles_out);
#endif
