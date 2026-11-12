"""Disassemble metro exe with import names. usage: disx.py exe start_rva_hex count"""
import sys, struct, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
sys.path.insert(0, __import__('os').path.dirname(__file__))
import xref

exe, base, pe = xref.exe, xref.base, struct.unpack_from('<I', xref.exe, 0x3C)[0]
imp_rva = struct.unpack_from('<I', exe, pe + 24 + 112 + 8)[0]  # data dir 1 (PE32+)
iat = {}
o = xref.rva2off(imp_rva)
while True:
    oft, _, _, name_rva, ft = struct.unpack_from('<IIIII', exe, o)
    if not name_rva: break
    dll = exe[xref.rva2off(name_rva):].split(b'\0')[0].decode()
    t = xref.rva2off(oft or ft); slot = ft
    while (e := struct.unpack_from('<Q', exe, t)[0]):
        nm = f'#{e & 0xffff}' if e >> 63 else exe[xref.rva2off(e & 0x7fffffff) + 2:].split(b'\0')[0].decode()
        iat[base + slot] = f'{dll}!{nm}'
        t += 8; slot += 8
    o += 20

def strat(va):
    off = xref.rva2off(va - base)
    if off is None: return ''
    b = exe[off:off + 80]
    if b[1:2] == b'\0' and b[0:1].isascii():
        s = b.decode('utf-16-le', 'ignore').split('\0')[0]
    else:
        s = b.split(b'\0')[0].decode('latin1')
    return repr(s[:60]) if len(s) >= 3 and s.isprintable() else ''

exc_rva, exc_sz = struct.unpack_from('<II', exe, pe + 24 + 112 + 3 * 8)
_pd = xref.rva2off(exc_rva)
funcs = sorted(struct.unpack_from('<II', exe, _pd + i * 12) for i in range(exc_sz // 12))

def func_of(rva):
    """(begin, end) of the .pdata function containing rva; follows chained unwind back to the root."""
    import bisect
    i = bisect.bisect_right(funcs, (rva, 1 << 40)) - 1
    b, e = funcs[i]
    while True:  # chained entries: find earliest contiguous chunk
        j = bisect.bisect_left(funcs, (b, 0)) - 1
        if j >= 0 and funcs[j][1] == b: b = funcs[j][0]
        else: break
    return b, e

if __name__ == '__main__':
    start = int(sys.argv[2], 16); n = int(sys.argv[3])
    if len(sys.argv) > 4 and sys.argv[4] == 'f':
        b, e = func_of(start); print(f'; function {b:#x}-{e:#x}'); start = b
    off = xref.rva2off(start)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    for i, ins in enumerate(md.disasm(exe[off:off + n * 15], base + start)):
        if i >= n: break
        note = ''
        m = re.search(r'\[rip \+ (0x[0-9a-f]+)\]', ins.op_str)
        if m:
            tgt = ins.address + ins.size + int(m.group(1), 16)
            note = iat.get(tgt) or strat(tgt) or f'{tgt - base:#x}'
        print(f'{ins.address - base:#08x}  {ins.mnemonic:6} {ins.op_str:40} {note}')
