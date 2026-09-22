"""Build src/helpmap.inc: WinHelp context id -> page of the Player's Guide.

    uv run --with pypdf tools/helpmap.py Stars.hlp Stars.pdf src/helpmap.inc

Stars! answers every Help button with WinHelp(HELP_CONTEXT, n), and modern
Windows has no viewer for the .hlp those numbers address.  The numbers are not
arbitrary: the compiled help file carries the [MAP] section that produced them,
so each one can be turned back into the title of the topic it opened.  The
Player's Guide covers the same ground under headings that read almost the same,
which is what makes the join possible at all:

    |CTXOMAP   context id   -> topic offset     (the [MAP] section)
    |TTLBTREE  topic offset -> topic title
    the guide  title        -> heading -> page

"Almost" is the whole difficulty.  The scan's text layer is OCR, the help file
and the book were written and edited separately, and the book sets its headings
in spaced small caps that OCR reads as "T HE G UTS OF M INEFIELDS".  So titles
and lines are normalised down to letters and digits, which also undoes the
letter-spacing, and are then tried in three passes of falling confidence: whole
line, line containing the title, and a fuzzy ratio over lines that share enough
words to be worth comparing.

Not every hit is a heading.  A topic's own section beats a bold mention of it
somewhere else, so candidates are ranked: body pages over the contents and the
index, chapters over the glossary, then by type size.  A line that is neither
bold nor large is prose, and prose is a mention rather than the topic.

The result is checked against the book's own contents, which is an independent
route to the same answer - the guide numbers its pages per chapter, prints that
label in every running head, and lists every heading against one.  Where both
routes fire they agree on 100 topics out of 101.  The one exception is The Guts
of Minefields, where chapters 25 and 26 open on the same recovered offset and
the contents route is the one that is wrong.
"""
import difflib
import json
import re
import struct
import sys
from collections import Counter, defaultdict

# Where the body of the book starts and ends, as PDF page numbers.  Taken from
# the running-head labels: page 13 is 1-1, the glossary opens at 264 and the
# index at 270.
FRONT_END = 12
GLOSSARY_START = 264
INDEX_START = 270

# Topics that are the help file's own furniture - its contents, its "How To..."
# browse list - and answer to nothing in the book.
NAVIGATION = {1, 2, 3, 4, 6, 7, 8, 9, 10}

# Topics the matcher cannot reach, with the page settled by looking.
#
# The cluster below is one mismatch rather than seven.  The help file is a
# reference to the dialogs and names its topics after them; the book teaches
# tasks and names its headings after those.  So the game's setup dialogs ask
# for "New Game Setup (Basic)" and "Step 3: Victory Conditions" where the book
# says "Starting a Single Player Game" and "Setting and Viewing Winning
# Conditions" - the same material under words with nothing in common.  No
# amount of fuzzy matching bridges victory to winning, so these are placed by
# hand, against the headings named in each comment.
OVERRIDES = {
    # The Help menu's Introduction.  The help file calls the topic "Welcome to
    # Stars!" and the book has no heading of that name: page 11 is its
    # full-page INTRODUCTION divider, and chapter 1 proper opens on 13.
    4501: 11,

    # STARTING A SINGLE PLAYER GAME, which is the New Game dialog: picking the
    # universe size, the density and the opponents.  The advanced wizard is
    # reached from the same dialog and specifies the same things at length.
    1002: 20,           # New Game Setup (Basic)
    1011: 20,           # New Game Setup (Advanced)
    1012: 20,           # Step 1: Specifying the Universe
    1020: 20,           # Step 2: Specifying the Players

    # SETTING AND VIEWING WINNING CONDITIONS, which also describes the score
    # sheet by name and what it shows.
    1021: 21,           # Step 3: Victory Conditions
    1097: 21,           # Public Player Scores
    1109: 21,           # Score sheet
}

STOP = {'A', 'AN', 'AND', 'THE', 'OF', 'TO', 'IN', 'ON', 'FOR', 'YOUR', 'WITH',
        'OR', 'AT', 'IS', 'IT', 'BY', 'FROM', 'AS'}

LABEL = re.compile(
    r'(?:^|\s)([0-9]{1,2}|[A-Z]{1,3})\s?[-–—]\s?([0-9]{1,3})(?:\s|$)')


# ---- the help file --------------------------------------------------------

class Hlp:
    """A WinHelp 3.x .hlp, read far enough to recover its [MAP] section."""

    def __init__(self, path):
        self.d = open(path, 'rb').read()
        magic, dirstart = struct.unpack('<ii', self.d[:8])
        if magic != 0x00035F3F:
            raise SystemExit('%s: not a WinHelp 3.x file (magic %08X)'
                             % (path, magic))
        self.files = dict(self._btree(dirstart, 'z4'))

    def _filedata(self, off):
        _reserved, used, _flags = struct.unpack('<iiB', self.d[off:off + 9])
        return self.d[off + 9:off + 9 + used]

    def _btree(self, off, structure):
        """Walk a B+ tree internal file.  `structure` is its key and value
        types as the format names them: 'z' a string, 'L' a long key, '4' a
        long value."""
        b = self._filedata(off)
        magic, _flags, pagesize = struct.unpack('<HHH', b[:6])
        if magic != 0x293B:
            raise SystemExit('bad btree magic %04X' % magic)
        root, _neg1, _total, nlevels, _entries = struct.unpack('<hhhhi', b[26:38])
        pages = b[38:]

        def page(n):
            return pages[n * pagesize:(n + 1) * pagesize]

        cur = root                      # descend to the leftmost leaf...
        for _ in range(nlevels - 1):
            cur = struct.unpack('<h', page(cur)[4:6])[0]

        out = []
        while cur != -1:                # ...then follow the leaf chain
            p = page(cur)
            _unused, n, _prev, nxt = struct.unpack('<hhhh', p[:8])
            o = 8
            for _ in range(n):
                if structure[0] == 'z':
                    end = p.index(b'\0', o)
                    key, o = p[o:end].decode('cp1252', 'replace'), end + 1
                else:
                    key, o = struct.unpack('<i', p[o:o + 4])[0], o + 4
                if structure[1] == 'z':
                    end = p.index(b'\0', o)
                    val, o = p[o:end].decode('cp1252', 'replace'), end + 1
                else:
                    val, o = struct.unpack('<i', p[o:o + 4])[0], o + 4
                out.append((key, val))
            cur = nxt
        return out

    def ctxomap(self):
        """The [MAP] section: [(context id, topic offset)].  The count is a
        word, not a long: 2 + 419*8 is exactly the file's length."""
        off = self.files.get('|CTXOMAP')
        if off is None:
            return []
        b = self._filedata(off)
        n = struct.unpack('<H', b[:2])[0]
        return [struct.unpack('<ii', b[2 + i * 8:10 + i * 8]) for i in range(n)]

    def titles(self):
        """topic offset -> title."""
        off = self.files.get('|TTLBTREE')
        return dict(self._btree(off, 'Lz')) if off is not None else {}


# ---- the guide ------------------------------------------------------------

def read_pdf(path):
    """[[y, size, text, bold]] per page, runs grouped into lines."""
    import pypdf

    out = []
    for page in pypdf.PdfReader(path).pages:
        rows = defaultdict(list)

        def visit(text, cm, tm, font_dict, font_size, rows=rows):
            if not text.strip():
                return
            name = ''
            try:
                name = str(font_dict.get('/BaseFont', '')) if font_dict else ''
            except Exception:
                pass
            rows[round(tm[5], 0)].append(
                (tm[4], text, abs(font_size * (tm[3] or 1)),
                 'bold' in name.lower() or 'black' in name.lower()))

        page.extract_text(visitor_text=visit)
        lines = []
        for y in sorted(rows, reverse=True):
            runs = sorted(rows[y])
            lines.append([round(y, 1),
                          round(max(r[2] for r in runs), 1),
                          ' '.join(' '.join(r[1] for r in runs).split()),
                          any(r[3] for r in runs)])
        out.append(lines)
    return out


def norm(s):
    return re.sub(r'[^A-Z0-9]', '', s.upper())


def norm_head(s):
    """As norm(), less the chapter number a chapter opener carries: the book
    opens chapter 12 with "12 C OLONIZATION", and that title page would
    otherwise lose to any bold mention of "Colonization" elsewhere."""
    return re.sub(r'^[0-9]{1,2}(?=[A-Z])', '', norm(s))


def words(s):
    return [w for w in re.split(r'[^A-Za-z0-9]+', s.upper()) if w]


def variants(title):
    """A title, and the shorter forms the book is likely to print instead."""
    out = [title]
    t = re.sub(r'\s+dialog(\s+box)?\s*$', '', title, flags=re.I)
    if t != title:                                  # "Merge Fleets dialog"
        out.append(t)
    u = re.sub(r'\s*\([^)]*\)\s*$', '', t).strip()
    if u and u != t:                                # "... (Player Race)"
        out.append(u)
    v = re.sub(r'^Step\s+\d+\s*:\s*', '', u).strip()
    if v and v != u:                                # "Step 3: Lesser Traits"
        out.append(v)
    return out


def build_lines(pdf):
    lines = []
    for pno, page in enumerate(pdf, 1):
        for _y, size, text, bold in page:
            n = norm(text)
            if not n:
                continue
            lines.append({
                'page': pno, 'size': size, 'text': text, 'norm': n,
                'head_norm': norm_head(text), 'words': words(text),
                # Headings are set large; subheadings are bold at body size.
                # Weight is what separates one from the prose around it.
                'head': bold or size >= 11,
                'body': FRONT_END < pno < INDEX_START,
                'chapter': FRONT_END < pno < GLOSSARY_START,
            })
    return lines


def page_rank(line):
    """Higher is a better destination.  A glossary entry defines a term in a
    sentence where a chapter explains it, so chapters outrank the glossary."""
    return (line['body'], line['chapter'], line['size'], line['head'])


def find(title, lines, index):
    """(page, score, how, heading) for the best page, or None."""
    best = None
    for depth, v in enumerate(variants(title)):
        n = norm(v)
        if len(n) < 5:
            continue
        cands = [l for l in lines
                 if l['body'] and n in (l['norm'], l['head_norm'])]
        how = 'exact'
        if not cands:
            # A heading may carry a word or two more, but a sentence that
            # merely contains the title is a mention, not the topic.
            cands = [l for l in lines if l['body'] and n in l['norm']
                     and len(l['norm']) <= len(n) * 3 // 2 + 12]
            how = 'contained'
        if cands:
            cands.sort(key=page_rank, reverse=True)
            line = cands[0]
            if not line['head']:
                continue
            cand = (line['page'], (1.0 if how == 'exact' else 0.9) - 0.1 * depth,
                    how, line['text'])
        else:
            ws = [w for w in words(v) if w not in STOP and len(w) > 2]
            pool = Counter()
            for w in ws:
                pool.update(index.get(w, ()))
            need = max(2, len(ws) * 2 // 3)
            scored = []
            for i, c in pool.items():
                line = lines[i]
                if c < need or not (line['body'] and line['head']):
                    continue
                r = difflib.SequenceMatcher(None, n, line['norm']).ratio()
                if r >= 0.82:
                    scored.append((r, line))
            if not scored:
                continue
            scored.sort(key=lambda x: (page_rank(x[1]), x[0]), reverse=True)
            r, line = scored[0]
            cand = (line['page'], r - 0.1 * depth, 'fuzzy', line['text'])
        if best is None or cand[1] > best[1]:
            best = cand
        if best[1] >= 1.0:
            break
    return best


# ---- the cross-check ------------------------------------------------------

def chapter_offsets(pdf):
    """chapter -> PDF page of its page 1, from the running-head labels.

    A label read off a contents entry would shift a whole chapter, so the
    offset that the most pages agree on wins."""
    votes = defaultdict(Counter)
    for pno, page in enumerate(pdf, 1):
        edge = page[:3] + page[-3:]
        for _y, _size, text, _bold in edge:
            if not text or len(text) > 60:
                continue
            m = LABEL.search(text)
            if m:
                votes[m.group(1)][pno - int(m.group(2))] += 1
                break
    out = {}
    for ch, c in votes.items():
        off, n = c.most_common(1)[0]
        if n >= 3:                      # three pages agreeing is a real chapter
            out[ch] = off
    return out


def toc_pages(pdf, offsets):
    """normalised title -> PDF page, from the contents."""
    out = {}
    for page in pdf[:FRONT_END]:
        for _y, _size, text, _bold in page:
            m = LABEL.search(text)
            if not m:
                continue
            title = text[:m.start()].strip(' .…')
            if m.group(1) in offsets and len(norm(title)) >= 5:
                out.setdefault(norm(title), offsets[m.group(1)] + int(m.group(2)))
    return out


# ---- output ---------------------------------------------------------------

BANNER = """\
/* Generated by tools/helpmap.py - do not edit by hand.
 *
 * HELP(context, page) - a WinHelp context id, as the game passes it to
 * WinHelp(HELP_CONTEXT), and the page of the Player's Guide that answers it.
 * Built by joining the [MAP] section of stars!.hlp to the guide's headings;
 * the header of tools/helpmap.py explains how, and how far it can be trusted.
 *
 * Pages are into the scan at
 * https://archive.org/download/manual_Stars/Stars.pdf, which is what
 * api_user.c opens.  A different edition of the book would renumber all of
 * them - the CD's own MANUAL.PDF, for one, runs to 280 pages against this
 * one's 277.
 *
 * %d of the help file's %d context ids are here.  The rest are topics the
 * book has no heading for, and they open the guide at its cover.
 */
"""


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__.strip().splitlines()[2].strip())
    hlppath, pdfpath, outpath = sys.argv[1:4]

    hlp = Hlp(hlppath)
    pdf = read_pdf(pdfpath)
    lines = build_lines(pdf)
    index = defaultdict(list)
    for i, l in enumerate(lines):
        for w in set(l['words']):
            index[w].append(i)

    titles = hlp.titles()
    cmap = sorted(hlp.ctxomap())
    rows = []
    for cid, topic in cmap:
        title = titles.get(topic, '')
        if cid in OVERRIDES:
            rows.append((cid, title, OVERRIDES[cid], 'override'))
        elif cid not in NAVIGATION:
            hit = find(title, lines, index)
            if hit:
                rows.append((cid, title, hit[0], hit[2]))

    # Cross-check against the book's own contents.
    offsets = chapter_offsets(pdf)
    toc = toc_pages(pdf, offsets)
    agree = disagree = 0
    for cid, title, page, how in rows:
        t = next((toc[norm(v)] for v in variants(title) if norm(v) in toc), None)
        if t is None:
            continue
        if t == page:
            agree += 1
        else:
            disagree += 1
            print('  differs from the contents: %-38s p%d vs p%d'
                  % (title[:38], page, t), file=sys.stderr)

    with open(outpath, 'w', newline='\n') as f:
        f.write(BANNER % (len(rows), len(cmap)))
        for cid, title, page, how in rows:
            f.write('HELP(%5d, %3d)   /* %s */\n' % (cid, page, title))

    kinds = Counter(how for _, _, _, how in rows)
    print('%s: %d of %d context ids, over %d pages'
          % (outpath, len(rows), len(cmap), len(set(r[2] for r in rows))))
    print('  matched by: %s' % ', '.join('%s %d' % kv for kv in kinds.items()))
    print('  agrees with the book\'s contents on %d of %d'
          % (agree, agree + disagree))


if __name__ == '__main__':
    main()
