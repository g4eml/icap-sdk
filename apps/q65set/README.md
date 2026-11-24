# Q65SET.APP - switch the Q65 period between 60 s and 30 s

A small helper for [Q65.APP](../q65/): it changes the T/R period in `Q65.CFG` on the radio, so the
SD card does not have to come out to edit the file.

## Use

1. Stop the app that is running, if any, with the **STOP** button in **MENU › SET › Apps**. The
   radio runs one app at a time: while Q65 (or any other app) runs, starting Q65SET just shows the
   running app's window.
2. Start **Q65SET** from **MENU › SET › Apps**. Every start switches the period:
   30 s → 60 s, anything else (60 s, 15 s, no setting, no `Q65.CFG`) → 30 s.
   The window shows the result, e.g. `Q65 period: 30 s (was 60 s)`, for 10 s (EXIT closes it).
3. Select **Q65** with the RTTY button; it starts with the new period.

The submode stays with the radio's own setting (DECODE › SET › SUBMODE). Q65SET only changes the
period.

On the IC-705 the Apps menu lists at most 8 apps, and the release bundle already has 8: remove one
you do not use (e.g. `CLOCK.APP`) or Q65SET does not appear (see the SDK
[`README.md`](../../README.md)).

## What it does to Q65.CFG

- `period=` becomes the first line (Q65.APP reads only the start of the file); every other line is
  kept as it was.
- A file in the old positional form (`fmin fmax [capture_s]`, starting with a number) is rewritten
  as `flo=` / `fhi=` / `cap=` lines, because Q65.APP reads no keys from that form. A capture length
  that suits only 60 s (52..57) is ignored by Q65.APP at 30 s, which then uses its default, 28 s.
- The previous file is saved as `Q65CFG.BAK`. The new file is read back and compared before success
  is reported; a failed write says so and points to the backup.
- A `Q65.CFG` larger than 2 KiB is left alone (an error is shown).

## Build

Needs the Arm GNU toolchain (`arm-none-eabi-gcc`) and Python 3.

```sh
sh build.sh          # -> build/Q65SET.APP (one file for the IC-705 and the IC-9700)
```

Uses only the public API in [`icap.h`](../../icap.h) (ABI 1: files and the app window).

## License

**GPL-3.0-or-later** ([`LICENSE`](LICENSE)), as Q65.APP.
