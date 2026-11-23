#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Regression run of Q65.APP on the PC harness (build.sh first): for each mode, a simulated 125 s with one
# transmission per period at -18 dB; every period the app could capture whole must decode, the decoder must
# not panic, and no more than 4 files may be open.  Exit status 0 = all passed.
cd "$(dirname "$0")"
H=build/harness; [ -x $H ] || { echo "run build.sh first"; exit 1; }
fail=0
# period sub(0..4) menu_letter start_s(UTC s of day) expected_decodes
while read P S L T N; do
  W=build/run; mkdir -p $W
  out=$(timeout 300 $H $W 125 $P $S -18 $L "period=$P
" $T 20)
  dec=$(echo "$out" | grep -c 'DECODE\]')
  pan=$(grep -ac rust_panic $W/Q65DBG.TXT)
  ref=$(echo "$out" | grep -c '5th open')
  if [ "$dec" -ge "$N" ] && [ "$pan" = 0 ] && [ "$ref" = 0 ]; then r=ok; else r=FAIL; fail=1; fi
  echo "Q65-$P$L start=$T: decoded $dec (want >= $N), panics $pan, refused opens $ref: $r"
done <<EOF
60 3 D 45230.4 1
30 2 C 45230.4 3
15 2 C 45230.4 7
30 0 A 45240.0 4
60 0 A 45240.0 2
15 1 B 45255.999 7
EOF
exit $fail
