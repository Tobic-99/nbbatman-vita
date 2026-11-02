#!/usr/bin/env python3
"""v33_discover.py [--check]: find the V33 code in the game's main ROM (rom/main.bin, interleaved by
tools/mkroms.py). Recursive descent from the reset vector and the interrupt vector table (physical 0x00000, in
ROM on this board), following near/far calls and jumps. A function is keyed by (CS, IP): near branches stay in
the segment. Writes gen/v33_functions.txt and prints statistics. --check: compare lengths with capstone (x86-16)
for the standard opcodes.
"""
import os, sys, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import v33_isa as I

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = open(os.path.join(P, 'rom', 'main.bin'), 'rb').read()


def fetch(a):
    """Physical address -> byte for the ROM areas (0-0x9ffff, 0xc0000-0xcffff mirror of 0, 0xffff0 = 0x7fff0)."""
    if a < 0xA0000: return ROM[a]
    if 0xC0000 <= a < 0xD0000: return ROM[a - 0xC0000]
    if a >= 0xFFFF0: return ROM[a - 0xFFFF0 + 0x7FFF0]
    return None


def decode(cs, ip):
    return I.decode(fetch, cs, ip)


class Func:
    def __init__(s, cs, ip, why):
        s.cs, s.ip, s.why = cs, ip, why
        s.insns = {}          # ip -> Insn
        s.bad = []
        s.indirect = []       # ips of unresolved indirect jumps/calls


funcs = {}
pending = collections.deque()
EXTRA = {}   # (cs, ip) -> why: entry points found by hand / at run time


def add(cs, ip, why):
    k = (cs, ip)
    if fetch(((cs << 4) + ip) & 0xFFFFF) is None:
        return
    if k not in funcs:
        funcs[k] = Func(cs, ip, why)
        pending.append(k)


def walk(f):
    work = [f.ip]
    while work:
        ip = work.pop()
        while True:
            if ip in f.insns:
                break
            ins = decode(f.cs, ip)
            if ins is None:
                f.bad.append(ip)
                break
            f.insns[ip] = ins
            op = ins.op
            nxt = ins.next_ip
            if op == 'call':
                add(f.cs, ins.target, 'call')
            elif op == 'callf':
                add(ins.target[0], ins.target[1], 'callf')
            elif op in ('calli', 'callfi'):
                f.indirect.append(ip)
            if op in ('retn', 'retf', 'iret', 'hlt'):
                break
            if op == 'jmp':
                work.append(ins.target); break
            if op == 'jmpf':
                add(ins.target[0], ins.target[1], 'jmpf'); break
            if op in ('jmpi', 'jmpfi'):
                f.indirect.append(ip); break
            if op in ('jcc', 'loop', 'loopz', 'loopnz', 'jcxz'):
                work.append(ins.target)
            ip = nxt


def run():
    add(0xFFFF, 0x0000, 'reset')   # FFFF:0000 = physical 0xFFFF0
    for v in range(256):
        off, seg = struct.unpack_from('<HH', ROM, 4 * v)
        if (seg, off) != (0, 0) and (seg, off) != (0xFFFF, 0xFFFF):
            add(seg, off, 'int%02x' % v)
    for k, why in EXTRA.items():
        add(k[0], k[1], why)
    while pending:
        walk(funcs[pending.popleft()])


def main():
    run()
    n = sum(len(f.insns) for f in funcs.values())
    nbad = sum(len(f.bad) for f in funcs.values())
    nind = sum(len(f.indirect) for f in funcs.values())
    print('functions %d, instructions %d, bad decodes %d, indirect jumps/calls %d' % (len(funcs), n, nbad, nind))
    ops = collections.Counter(i.op for f in funcs.values() for i in f.insns.values())
    print('operations (%d):' % len(ops), ', '.join('%s %d' % kv for kv in ops.most_common()))
    segs = collections.Counter(cs for cs, ip in funcs)
    print('code segments:', ', '.join('%04x:%d' % kv for kv in segs.most_common()))
    with open(os.path.join(P, 'gen', 'v33_functions.txt'), 'w') as o:
        for k in sorted(funcs):
            f = funcs[k]
            o.write('%04x:%04x %5d insns ind %d bad %d %s\n' % (f.cs, f.ip, len(f.insns), len(f.indirect), len(f.bad), f.why))
    for f in funcs.values():
        for ip in f.bad[:1]:
            a = ((f.cs << 4) + ip) & 0xFFFFF
            print('  bad decode %04x:%04x (%05x): %s' % (f.cs, ip, a, bytes(fetch(a + i) or 0 for i in range(6)).hex()))
    if '--check' in sys.argv:
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
        diff = 0
        for f in funcs.values():
            for ip, ins in f.insns.items():
                if ins.raw[:1] in (b'\x0f', b'\x64', b'\x65') or b'\x0f' == ins.raw[1:2]:
                    continue
                c = next(md.disasm(ins.raw + b'\x90' * 8, ip, 1), None)
                if c is None or c.size != ins.size:
                    diff += 1
                    if diff <= 20:
                        print('  len diff %04x:%04x ours %d %r | capstone %s %s %s' % (f.cs, ip, ins.size, ins, c and c.size, c and c.mnemonic, c and c.op_str))
        print('check: %d length differences' % diff)


if __name__ == '__main__':
    main()
