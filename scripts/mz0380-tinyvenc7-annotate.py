#!/usr/bin/env python3
# pattern-check: skip RE tooling, linear dump-and-annotate script
"""Annotate a tinyvenc ARM disassembly with resolved literal-pool values.

llvm-objdump prints `ldr rN, [pc, #imm]` without saying what the pool word
holds, and in these binaries every reference to a global or a string is such a
load - so an unannotated dump hides exactly what we want to read. This reads
the pool word out of the file, then maps it to a symbol or a C string.

    scripts/mz0380-tinyvenc7-annotate.py [binary] > tinyvenc7.annot.txt

Defaults to re-dump/fw/yuan_demo_sdi/tinyvenc7. Needs llvm-objdump (the system
objdump has no ARM support), readelf and nm. Touches no hardware.
"""
import os
import re,struct,subprocess,sys,bisect

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.join(HERE, '..', 're-dump', 'fw', 'yuan_demo_sdi', 'tinyvenc7')
P = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
b=open(P,'rb').read()
# sections: (name, addr, off, size)
SEC=[]
out=subprocess.check_output(['readelf','-S','-W',P]).decode()
for m in re.finditer(r'\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]{8})\s+([0-9a-f]{6})\s+([0-9a-f]{6})',out):
    SEC.append((m.group(1),int(m.group(2),16),int(m.group(3),16),int(m.group(4),16)))
def rd(addr,n=4):
    for nm,a,o,sz in SEC:
        if nm=='.bss': continue
        if a<=addr<a+sz and a!=0:
            return b[o+addr-a:o+addr-a+n]
    return None
def word(addr):
    d=rd(addr,4)
    return struct.unpack('<I',d)[0] if d and len(d)==4 else None
# symbols
syms=[]
for line in subprocess.check_output(['nm','-C',P]).decode().splitlines():
    pr=line.split(' ',2)
    if len(pr)==3 and pr[0].strip() and pr[1] in 'tTbBdDrRvVwW':
        if pr[2].startswith('$'): continue
        syms.append((int(pr[0],16),pr[2]))
syms.sort()
addrs=[s[0] for s in syms]
def sym(a):
    i=bisect.bisect_right(addrs,a)-1
    if i<0: return None
    base,nm=syms[i]
    if a-base>0x2000: return None
    return nm if a==base else f'{nm}+0x{a-base:x}'
def cstr(a):
    d=rd(a,200)
    if not d: return None
    e=d.find(b'\0')
    if e<1: return None
    s=d[:e]
    try: t=s.decode('ascii')
    except: return None
    return t if all(32<=c<127 or c in (9,10) for c in s) else None
dis=subprocess.check_output(['llvm-objdump','-d','-C','--triple=armv7-none-linux-gnueabi',P]).decode()
pat=re.compile(r'^\s*([0-9a-f]+):\s+\S+\s+(ldr\s+\w+, \[pc.*?@ (0x[0-9a-f]+))')
for line in dis.splitlines():
    m=pat.match(line)
    ann=''
    if m:
        v=word(int(m.group(3),16))
        if v is not None:
            s=sym(v); st=cstr(v)
            if st: ann=f'   ;; = 0x{v:x} "{st[:70]}"'
            elif s: ann=f'   ;; = 0x{v:x} <{s}>'
            else: ann=f'   ;; = 0x{v:x}'
    print(line+ann)
