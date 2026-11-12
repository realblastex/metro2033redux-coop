"""Find strings in metro exe and RIP-relative code references to them. usage: xref.py exe needle [needle...]"""
import sys, struct

exe = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', exe, 0x3C)[0]
nsec = struct.unpack_from('<H', exe, pe + 6)[0]
optsz = struct.unpack_from('<H', exe, pe + 20)[0]
base = struct.unpack_from('<Q', exe, pe + 24 + 24)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = exe[o:o + 8].rstrip(b'\0').decode()
    vsz, va, rsz, raw = struct.unpack_from('<IIII', exe, o + 8)
    secs.append((name, va, vsz, raw, rsz))

def off2rva(off):
    for n, va, vsz, raw, rsz in secs:
        if raw <= off < raw + rsz:
            return va + off - raw

def rva2off(rva):
    for n, va, vsz, raw, rsz in secs:
        if va <= rva < va + rsz:
            return raw + rva - va

text = next(s for s in secs if s[0] == '.text')
_, tva, tvsz, traw, trsz = text
code = exe[traw:traw + trsz]

def refs_to(target_rva):
    # scan for disp32 such that next-insn-rva + disp == target; check common encodings (lea/mov r, [rip+d])
    out = []
    for i in range(len(code) - 4):
        d = struct.unpack_from('<i', code, i)[0]
        # assume insn ends right after disp32 (lea/mov reg,[rip+x])
        if tva + i + 4 + d == target_rva:
            out.append(tva + i)
    return out

def refs_many(targets):
    """{target_rva: [disp rva...]} in one pass over .text."""
    import array
    want = set(targets); out = {t: [] for t in targets}
    n = len(code) - 4
    for i in range(n):
        t = tva + i + 4 + int.from_bytes(code[i:i + 4], 'little', signed=True)
        if t in want:
            out[t].append(tva + i)
    return out

if __name__ == '__main__':
    for nd in sys.argv[2:]:
        for enc in (nd.encode(), nd.encode('utf-16-le')):
            start = 0
            while (p := exe.find(enc, start)) >= 0:
                rva = off2rva(p)
                print(f'{nd!r} enc={"w" if len(enc) > len(nd) else "a"} off={p:#x} rva={rva:#x} va={base + rva:#x}')
                for r in refs_to(rva):
                    print(f'   ref disp@rva {r:#x}')
                start = p + 1
