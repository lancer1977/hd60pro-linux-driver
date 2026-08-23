#!/usr/bin/env python3
"""Build a basic-block CFG for one ARM function from llvm-objdump output.

Written because hand-reading branch ranges produced a wrong "this code is
unreachable" claim (M150/M152): a branch OVER a range says nothing about
whether other branches jump INTO it. This computes reachability properly.
"""
import re, sys, collections

INSN = re.compile(r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s*(.*)$')
TARGET = re.compile(r'0x([0-9a-f]+)')

def load(path, lo, hi):
    out = {}
    for line in open(path):
        m = INSN.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        if not (lo <= addr < hi):
            continue
        out[addr] = (m.group(2), m.group(3))
    return out

def analyse(insns, lo, hi, entry):
    addrs = sorted(insns)
    edges = collections.defaultdict(set)
    leaders = {entry}
    for i, a in enumerate(addrs):
        mnem, ops = insns[a]
        nxt = addrs[i + 1] if i + 1 < len(addrs) else None
        # branch?
        if mnem.startswith('b') and not mnem.startswith('bic') and not mnem.startswith('bfi'):
            m = TARGET.search(ops)
            if m:
                t = int(m.group(1), 16)
                if lo <= t < hi:
                    edges[a].add(t)
                    leaders.add(t)
                if nxt:
                    leaders.add(nxt)
                uncond = mnem in ('b', 'bl', 'bx') or mnem == 'b'
                # conditional branches also fall through
                if not (mnem == 'b' or mnem.startswith('bl')):
                    if nxt:
                        edges[a].add(nxt)
                elif mnem.startswith('bl'):
                    if nxt:
                        edges[a].add(nxt)   # call returns
                continue
        if 'pc' in ops and mnem in ('pop', 'ldm', 'mov'):
            continue                        # return
        if nxt:
            edges[a].add(nxt)
    return edges, leaders

def reachable(edges, entry, blocked_edge=None):
    seen, stack = set(), [entry]
    while stack:
        a = stack.pop()
        if a in seen:
            continue
        seen.add(a)
        for t in edges.get(a, ()):
            if blocked_edge and (a, t) == blocked_edge:
                continue
            stack.append(t)
    return seen

if __name__ == '__main__':
    path, lo, hi, entry = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), int(sys.argv[4], 16)
    insns = load(path, lo, hi)
    edges, _ = analyse(insns, lo, hi, entry)
    seen = reachable(edges, entry)
    print(f"instructions {len(insns)}  reachable {len(seen)}")
    for q in sys.argv[5:]:
        t = int(q, 16)
        print(f"  {q}: {'REACHABLE' if t in seen else 'unreachable'}")
    # who jumps to each queried address
    for q in sys.argv[5:]:
        t = int(q, 16)
        srcs = [hex(a) for a in edges if t in edges[a] and a + 4 != t]
        print(f"  {q} <- explicit branches from: {srcs}")
