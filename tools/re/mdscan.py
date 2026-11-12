"""Test every FindPatternInEXE(pattern, mask) in MetroDeveloper against several exes.
Prints patterns that match the old exe but not the new one (= what the update broke)."""
import re, sys, glob, os
src_dir, exes = sys.argv[1], sys.argv[2:]
call = re.compile(r'FindPatternInEXE\(\s*((?:"[^"]*"\s*)+),\s*"([x?]+)"', re.S)
datas = {os.path.basename(e): open(e, 'rb').read() for e in exes}

def to_rx(pat, mask):
    b = bytes(int(h, 16) for h in re.findall(r'\\x([0-9a-fA-F]{2})', pat))
    assert len(b) == len(mask), (pat, mask)
    return re.compile(b''.join(re.escape(b[i:i+1]) if m == 'x' else b'.' for i, m in enumerate(mask)), re.S)

rows = []
for f in sorted(glob.glob(os.path.join(src_dir, '*.cpp'))):
    text = open(f, encoding='latin-1').read()
    for m in call.finditer(text):
        pat = ''.join(re.findall(r'"([^"]*)"', m.group(1)))
        line = text.count('\n', 0, m.start()) + 1
        try:
            rx = to_rx(pat, m.group(2))
        except AssertionError:
            continue
        counts = [len(rx.findall(d)) for d in datas.values()]
        rows.append((os.path.basename(f), line, counts))

print('file:line'.ljust(28), *[k[:22].ljust(23) for k in datas])
for f, line, c in rows:
    if any(c):
        flag = '  <-- BROKEN by update' if c[0] and not c[1] else ''
        print(f'{f}:{line}'.ljust(28), *[str(x).ljust(23) for x in c], flag)
