# ICAP reference apps

Each app is its own project in a subdirectory, with **its own `LICENSE`**.

The SDK header [`../icap.h`](../icap.h) is MIT, so an app may use any license,
including GPL. Build each app against `../icap.h` and pack it into a `*.APP`.

| App  | What                                   | License (planned) |
|------|----------------------------------------|-------------------|
| q65  | Q65 decoder / beacon (WSJT-X-derived)  | GPL-3.0           |
| jt4g | JT4G beacon (WSJT-X-derived)           | GPL-3.0           |
| pi4  | PI4 beacon                             | per upstream      |

> Apps that reuse or derive from GPL-licensed code (e.g. WSJT-X) **must** be
> distributed under the GPL and carry the GPL text in their own directory. MIT on
> the SDK header is compatible with that.

Apps land here as the SDK foundation (the audio / TX / clock services behind
`icap_audio_read`, `icap_tx_*`, `icap_clock_now`) is completed.
