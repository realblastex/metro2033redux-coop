"""Find functions that write [reg+disp] (given size) and also reference a RIP-relative global in a range.
usage: memwrite.py exe <disp hex> <size bytes> [global_lo_rva hex] [global_hi_rva hex]
Prints function begin RVA, write sites, and whether the function references the global range."""
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
sys.path.insert(0, __import__('os').path.dirname(__file__))
import xref, disx

disp, size = int(sys.argv[2], 16), int(sys.argv[3])
glo = int(sys.argv[4], 16) if len(sys.argv) > 4 else None
ghi = int(sys.argv[5], 16) if len(sys.argv) > 5 else glo
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
exe, base = xref.exe, xref.base
for b, e in disx.funcs:
    off = xref.rva2off(b)
    if off is None or e - b > 0x20000: continue
    hits, gref = [], False
    for ins in md.disasm(exe[off:off + (e - b)], b):
        ops = ins.operands
        if not ops: continue
        for op in ops:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and glo is not None:
                t = ins.address + ins.size + op.mem.disp
                if glo <= t <= ghi: gref = True
        d = ops[0]
        if ins.mnemonic.startswith('mov') and d.type == X86_OP_MEM and d.mem.disp == disp and d.size == size \
                and d.mem.base != X86_REG_RIP and len(ops) == 2:
            hits.append(f'{ins.address:#x} {ins.mnemonic} {ins.op_str}')
    if hits and (glo is None or gref):
        print(f'func {b:#x}: ' + ' | '.join(hits))
