"""Dump the RT_DIALOG resources out of an NE file, decoded.

Used to check the template converter against what the game actually ships
rather than against what the format documentation says.
"""
import struct, sys

def u16(b,o): return struct.unpack_from('<H', b, o)[0]
def u32(b,o): return struct.unpack_from('<I', b, o)[0]

def resources(img):
    e_lfanew = u32(img, 0x3C)
    rsrctab = u16(img, e_lfanew + 0x24)
    base = e_lfanew + rsrctab
    shift = u16(img, base)
    p = base + 2
    out = {}
    while True:
        tid = u16(img, p)
        if tid == 0: break
        cnt = u16(img, p + 2)
        ents = []
        for i in range(cnt):
            e = p + 8 + i*12
            off, ln, fl, nid = u16(img,e)<<shift, u16(img,e+2)<<shift, u16(img,e+4), u16(img,e+6)
            ents.append((nid, off, ln, fl))
        out[tid] = ents
        p += 8 + cnt*12
    return out

def pstr(b, o):
    """A NUL-terminated string, or an 0xFF-prefixed ordinal."""
    if b[o] == 0xFF:
        return ('#%d' % u16(b, o+1)), o+3
    j = o
    while b[j]: j += 1
    return b[o:j].decode('cp1252','replace'), j+1

CLS = {0x80:'BUTTON',0x81:'EDIT',0x82:'STATIC',0x83:'LISTBOX',
       0x84:'SCROLLBAR',0x85:'COMBOBOX'}

def dump_dialog(b, label):
    o = 0
    style = u32(b, 0); o = 4
    ndlg = b[o]; o += 1
    x,y,cx,cy = struct.unpack_from('<hhhh', b, o); o += 8
    menu, o = pstr(b, o)
    cls,  o = pstr(b, o)
    cap,  o = pstr(b, o)
    print('%s style=%08X items=%d rect=%d,%d %dx%d menu=%r class=%r caption=%r'
          % (label, style, ndlg, x,y,cx,cy, menu, cls, cap))
    if style & 0x00000040:            # DS_SETFONT
        pt = u16(b, o); o += 2
        face, o = pstr(b, o)
        print('    font %d %r' % (pt, face))
    for i in range(ndlg):
        ix,iy,icx,icy,iid = struct.unpack_from('<hhhhH', b, o); o += 10
        istyle = u32(b, o); o += 4
        if b[o] in CLS:
            icls = CLS[b[o]]; o += 1
        else:
            icls, o = pstr(b, o)
        itext, o = pstr(b, o)
        extra = b[o]; o += 1 + extra
        print('    %-9s id=%-5d %4d,%-4d %3dx%-3d style=%08X text=%r extra=%d'
              % (icls, iid, ix,iy,icx,icy, istyle, itext, extra))

def main():
    img = open(sys.argv[1],'rb').read()
    want = [int(a) for a in sys.argv[2:]]
    res = resources(img)
    for nid, off, ln, fl in res.get(0x8005, []):
        num = nid & 0x7FFF
        if want and num not in want: continue
        dump_dialog(img[off:off+ln], 'DIALOG %d (off %d len %d):' % (num, off, ln))

if __name__ == '__main__':
    main()
