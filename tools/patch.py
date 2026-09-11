"""In-place text patcher used by the build scripts.

Two accommodations, both for the shell in between: patch text is written with
no backslashes at all - spell a C escape as @@N@@, @@T@@, @@Q@@ or @@B@@ - and
matching is tried with both line endings, so an edit does not depend on which
tool last wrote the file.
"""
import io

LF = chr(10)
CR = chr(13)
BS = chr(92)

def unmark(t):
    return (t.replace('@@N@@', BS + 'n')
             .replace('@@T@@', BS + 't')
             .replace('@@Q@@', BS + '"')
             .replace('@@B@@', BS + BS)
             .replace('@@0@@', BS + '0'))

def patch(path, old, new, count=1):
    s = io.open(path, encoding='utf-8', newline='').read()
    old, new = unmark(old), unmark(new)
    for crlf in (False, True):
        o = old.replace(LF, CR + LF) if crlf else old
        n = new.replace(LF, CR + LF) if crlf else new
        if s.count(o) == count:
            io.open(path, 'w', encoding='utf-8', newline='').write(s.replace(o, n))
            return True
    raise SystemExit('patch: no unique match in ' + path)

def append(path, text):
    s = io.open(path, encoding='utf-8', newline='').read()
    crlf = (CR + LF) in s
    text = unmark(text)
    if crlf:
        text = text.replace(LF, CR + LF)
    io.open(path, 'a', encoding='utf-8', newline='').write(text)
