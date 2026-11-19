# beacon.h as built into the 20261119v6 app binaries

The published app bundles of release 20261119v8 contain some apps built from this older
`beacon.h` (it lacks the "HHMMSS TX <mode> <call> <loc>" line on the DECODE screen):
WSPR.APP (both radios) and the IC-9700 FT8/JT4/PI4 apps. Build them with this file in place of
`../beacon.h` to get exactly those binaries. Everything else is the same source as in
`apps/`. Next releases use the current `../beacon.h`.
