/* HELLO - the smallest useful ICAP app: prints to its window, shows the radio model and the
 * clock, and exits cleanly when the owner presses STOP / EXIT.  MIT license (see ../../LICENSE). */
#include "icap.h"

static char *two(char *p, unsigned v) { *p++ = (char)('0' + v / 10 % 10); *p++ = (char)('0' + v % 10); return p; }

int app_main(const struct icap_svc *svc)
{
    icap_use(svc);                         /* required, once, first */
    icap_clear();
    icap_print("Hello from ICAP!");
    icap_set_line(2, icap_is_ic705() ? "IC-705" : "IC-9700");

    while (!icap_should_stop()) {          /* poll at least every 500 ms */
        struct icap_time t;
        if (icap_clock_now(&t) == 0) {
            char b[16], *p = b;
            p = two(p, t.hour); *p++ = ':'; p = two(p, t.min); *p++ = ':'; p = two(p, t.sec); *p = 0;
            icap_set_line(3, b);
        }
        icap_sleep_ms(200);
    }
    return 0;
}
