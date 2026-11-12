"""Find direct E8/E9 callers of the function containing each rva. usage: callers.py exe rva_hex..."""
import sys, struct
sys.path.insert(0, __import__('os').path.dirname(__file__))
import xref, disx
code, tva = xref.code, xref.tva
for a in sys.argv[2:]:
    b, e = disx.func_of(int(a, 16))
    hits = []
    for i in range(len(code) - 5):
        if code[i] in (0xE8, 0xE9) and tva + i + 5 + struct.unpack_from('<i', code, i + 1)[0] == b:
            src = tva + i
            hits.append(f'{src:#x}(in {disx.func_of(src)[0]:#x})')
    print(f'{a}: func {b:#x}-{e:#x} callers: {hits}')
