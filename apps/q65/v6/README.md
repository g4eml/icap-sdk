# q65_705.c as built into the 20261119v6 IC-9700 Q65.APP

The IC-9700 Q65.APP in the 20261119v8 app bundle was built from this older shim (the beacon TX
buffer is a static array instead of a part of the heap chunk). Build with this file in place of
`../q65_705.c` (and `../../digi-common/v6/beacon.h`) to get exactly that binary. Next releases use the
current `../q65_705.c`.
