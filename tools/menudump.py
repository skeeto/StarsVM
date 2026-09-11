"""Dump the RT_MENU resources out of an NE file."""
import struct, sys
sys.path.insert(0, 'tools')
from dlgdump import resources

def walk(b, o, depth, out):
    while o < len(b):
        flags = struct.unpack_from('<H', b, o)[0]; o += 2
        if flags & 0x0010:                       # MF_POPUP
            j = o
            while b[j]: j += 1
            txt = b[o:j].decode('cp1252','replace'); o = j + 1
            out.append('%s popup  flags=%04X %r' % ('  '*depth, flags, txt))
            o = walk(b, o, depth+1, out)
        else:
            mid = struct.unpack_from('<H', b, o)[0]; o += 2
            j = o
            while b[j]: j += 1
            txt = b[o:j].decode('cp1252','replace'); o = j + 1
            out.append('%s item   flags=%04X id=%-5d %r' % ('  '*depth, flags, mid, txt))
        if flags & 0x0080:                       # MF_END
            return o
    return o

img = open(sys.argv[1],'rb').read()
res = resources(img)
for nid, off, ln, fl in res.get(0x8004, []):
    b = img[off:off+ln]
    ver, hdr = struct.unpack_from('<HH', b, 0)
    out = []
    walk(b, 4 + hdr, 1, out)
    print('MENU %d (len %d) version=%d headersize=%d' % (nid & 0x7FFF, ln, ver, hdr))
    print('\n'.join(out))
