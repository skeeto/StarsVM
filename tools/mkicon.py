"""Extract an icon from the game's NE and write it as a Windows .ico.

An RT_GROUP_ICON directory is almost the ICO file format already: same header,
same 14-byte entries, except that the last field of a resource entry is a WORD
naming the RT_ICON resource that holds the image, where an ICO file wants a
DWORD byte offset to the image inside the file.  So the conversion is: widen
that field, lay the images out after the directory, and fill in the offsets.

Usage: mkicon.py <stars.exe> <out.ico> <out.rc> [GROUPNAME]

The .rc output is written either way: with an ICON directive when the icon was
produced, and with a comment when it could not be, so that a build without the
game present still compiles.
"""
import struct
import sys


def u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def resources(img):
    """Walk the NE resource table; return {type_id: [(name_id, off, len)]} and
    the resolved name strings."""
    base = u32(img, 0x3C) + u16(img, u32(img, 0x3C) + 0x24)
    shift = u16(img, base)
    p = base + 2
    table, names = {}, {}
    while True:
        tid = u16(img, p)
        if tid == 0:
            break
        count = u16(img, p + 2)
        entries = []
        for i in range(count):
            e = p + 8 + i * 12
            nid = u16(img, e + 6)
            if not (nid & 0x8000):
                n = img[base + nid]
                names[nid] = img[base + nid + 1:base + nid + 1 + n].decode('cp1252')
            entries.append((nid, u16(img, e) << shift, u16(img, e + 2) << shift))
        table[tid] = entries
        p += 8 + count * 12
    return table, names


def build_ico(img, group):
    table, names = resources(img)
    groups = table.get(0x800E, [])          # RT_GROUP_ICON
    icons = {nid & 0x7FFF: (off, ln) for nid, off, ln in table.get(0x8003, [])}

    chosen = None
    for nid, off, ln in groups:
        label = names.get(nid, '#%d' % (nid & 0x7FFF))
        if group is None or label.lower() == group.lower():
            chosen = (label, off, ln)
            break
    if chosen is None:
        raise SystemExit('mkicon: no RT_GROUP_ICON named %r' % group)

    label, off, ln = chosen
    d = img[off:off + ln]
    count = u16(d, 4)

    # ICONDIR, then one 16-byte ICONDIRENTRY per image, then the images.
    out = bytearray(struct.pack('<HHH', 0, 1, count))
    offset = 6 + count * 16
    images = []
    for i in range(count):
        e = 6 + i * 14
        width, height, colors, reserved = d[e], d[e + 1], d[e + 2], d[e + 3]
        planes, bits = u16(d, e + 4), u16(d, e + 6)
        size, rid = u32(d, e + 8), u16(d, e + 12)
        if rid not in icons:
            raise SystemExit('mkicon: group names RT_ICON %d, which is absent' % rid)
        ioff, ilen = icons[rid]
        data = img[ioff:ioff + min(size, ilen)]
        out += struct.pack('<BBBBHHII', width, height, colors, reserved,
                           planes, bits, len(data), offset)
        images.append(data)
        offset += len(data)
    for data in images:
        out += data
    return label, bytes(out)


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    exe, ico, rc = sys.argv[1], sys.argv[2], sys.argv[3]
    group = sys.argv[4] if len(sys.argv) > 4 else None

    try:
        with open(exe, 'rb') as f:
            img = f.read()
        label, blob = build_ico(img, group)
    except SystemExit:
        raise
    except OSError as err:
        with open(rc, 'w') as f:
            f.write('/* no icon: %s */\n' % err)
        print('mkicon: %s; building without an icon' % err)
        return

    with open(ico, 'wb') as f:
        f.write(blob)
    with open(rc, 'w') as f:
        f.write('1 ICON "%s"\n' % ico.replace('\\', '/'))
    print('mkicon: %s -> %s (%d bytes)' % (label, ico, len(blob)))


main()
