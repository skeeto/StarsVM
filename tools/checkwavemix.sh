#!/bin/sh
# Re-derive the WaveMix Pascal pop counts from wavemix.dll itself.
# Each export ends in RETF imm16, and that immediate is the pop count.
# Usage: tools/checkwavemix.sh [path-to-wavemix.dll]
set -e
DLL=${1:-../wavemix.dll}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python - "$DLL" "$TMP" <<'PY'
import struct, sys
dll, tmp = sys.argv[1], sys.argv[2]
d = open(dll, 'rb').read()
e = struct.unpack_from('<I', d, 0x3c)[0]
align  = struct.unpack_from('<H', d, e + 0x32)[0]
segtab = e + struct.unpack_from('<H', d, e + 0x22)[0]
enttab = e + struct.unpack_from('<H', d, e + 0x04)[0]
cbent  = struct.unpack_from('<H', d, e + 0x06)[0]
nres   = struct.unpack_from('<I', d, e + 0x2c)[0]
cbnres = struct.unpack_from('<H', d, e + 0x20)[0]

s, l, fl, ma = struct.unpack_from('<HHHH', d, segtab)
base = s << align
open(tmp + '/seg1.bin', 'wb').write(d[base:base + (l or 0x10000)])

names, p, first = {}, nres, True
while p < nres + cbnres and d[p]:
    n = d[p]
    nm = d[p+1:p+1+n].decode()
    o = struct.unpack_from('<H', d, p+1+n)[0]
    if not first:
        names[o] = nm
    first = False
    p += 1 + n + 2

p, end, ordv = enttab, enttab + cbent, 1
while p + 2 <= end:
    cnt, typ = d[p], d[p+1]
    if cnt == 0:
        break
    p += 2
    if typ == 0:
        ordv += cnt
        continue
    for _ in range(cnt):
        if typ == 0xff:
            sg, off = d[p+3], struct.unpack_from('<H', d, p+4)[0]
            p += 6
        else:
            sg, off = typ, struct.unpack_from('<H', d, p+1)[0]
            p += 3
        if sg == 1 and ordv in names:
            print('%d %s %d' % (ordv, names[ordv], off))
        ordv += 1
PY
} > "$TMP/ents.txt"

while read ord name off; do
    line=$(objdump -D -b binary -m i386 -M addr16,data16 \
             --start-address="$off" --stop-address=$((off + 0x900)) \
             "$TMP/seg1.bin" 2>/dev/null | grep -m1 -E '\blret\b')
    imm=$(echo "$line" | sed -n 's/.*lret[ \t]*\$0x\([0-9a-f]*\).*/\1/p')
    [ -z "$imm" ] && imm=0
    printf 'ord %2s  %-24s pop=%d\n' "$ord" "$name" "$((16#0$imm))"
done < "$TMP/ents.txt"
