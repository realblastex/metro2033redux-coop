"""List printable ASCII strings in exe matching regex. usage: strs.py exe regex"""
import sys, re
sys.path.insert(0, __import__('os').path.dirname(__file__))
import xref
rx = re.compile(sys.argv[2].encode(), re.I)
for m in re.finditer(rb'[\x20-\x7e]{4,}', xref.exe):
    if rx.search(m.group()):
        rva = xref.off2rva(m.start())
        print(f'{rva:#x} {m.group().decode()[:120]}')
