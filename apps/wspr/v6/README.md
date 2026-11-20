# wsprbcn.c - the beacon-only WSPR.APP of the 20261119v6 / v8 app bundles

Up to the 20261119v8 app bundle WSPR.APP was a beacon-only app built from this file (with
`../../digi-common/v6/beacon.h`, `../wsprenc.c` and the shared `txsynth.c` / `msg77.c`). It is
superseded and no longer built: from firmware release 20261119v13 on, WSPR.APP is the decoder +
beacon app `../wsprapp.c`. Kept here as the source of those older binaries.
