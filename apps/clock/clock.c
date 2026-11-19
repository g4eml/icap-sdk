/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Dawid SQ6EMM
 *
 * CLOCK - ICAP v1rc smoke test: radio clock (icap_clock_now), decode_out, service probing.
 * Built only against the public SDK header icap.h (no firmware addresses). */
#include "icap.h"

static char *dec(char *p, unsigned v, int width)
{
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n < width) t[n++] = '0';
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}

static void fmt_time(char *b, const struct icap_time *t)
{
    static const char *W[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    char *p = b;
    p = dec(p, t->year, 4); *p++ = '-'; p = dec(p, t->mon, 2); *p++ = '-'; p = dec(p, t->day, 2);
    *p++ = ' '; p = dec(p, t->hour, 2); *p++ = ':'; p = dec(p, t->min, 2); *p++ = ':'; p = dec(p, t->sec, 2);
    *p++ = ' '; const char *w = W[t->wday < 7 ? t->wday : 0]; while (*w) *p++ = *w++; *p = 0;
}

static const char *yn(int ok) { return ok ? "yes" : "no"; }

int app_main(const struct icap_svc *svc)
{
    icap_use(svc);
    icap_clear();
    char b[40], *p;

    p = b; const char *s = "CLOCK test, ABI "; while (*s) *p++ = *s++; dec(p, svc->abi_version, 1);
    icap_print(b);
    if (svc->abi_version < 3) { icap_print("needs v1rc firmware (ABI 3)"); icap_sleep_ms(3000); return 1; }

    /* which v1rc services does this firmware provide? */
    p = b; s = "clock "; while (*s) *p++ = *s++; s = yn(ICAP_HAS(clock_now)); while (*s) *p++ = *s++;
    s = " audio "; while (*s) *p++ = *s++; s = yn(ICAP_HAS(audio_read)); while (*s) *p++ = *s++;
    s = " tx "; while (*s) *p++ = *s++; s = yn(icap_tx_available()); while (*s) *p++ = *s++; *p = 0;
    icap_print(b);

    struct icap_time t;
    int last = -1;
    while (!icap_should_stop()) {
        int r = icap_clock_now(&t);
        if (r == ICAP_E_NOSYS) icap_set_line(3, "clock: not provided");
        else if (r < 0) icap_set_line(3, "clock: not set / not read yet");
        else if (t.sec != last) {
            last = t.sec;
            fmt_time(b, &t);
            icap_set_line(3, b);
            if (t.sec == 0) icap_decode_out(b);          /* once a minute: exercises decode_out */
        }
        icap_sleep_ms(200);
    }
    return 0;
}
