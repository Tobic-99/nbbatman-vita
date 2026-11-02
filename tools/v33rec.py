#!/usr/bin/env python3
"""v33rec.py: static recompiler for the game's V33 code -> C++ (gen/v33/).

Entry points: the static discovery (tools/v33_discover.py: reset, interrupt vectors, direct calls/jumps) plus every
target the desktop interpreter recorded at run time (tools/v33_runtime_targets.txt). Each routine (CS:IP) becomes
    void r_PPPPP_CCCC(V33& c)          (PPPPP = physical address, CCCC = CS)
that runs until control leaves it, then sets CS:IP and returns to the dispatcher (host/rec.cpp), which also takes
interrupts and falls back to the interpreter for anything not translated. Branches inside the routine are gotos;
backward ones end the routine when the slice's cycles are used up.

Registers and flags live in the V33 struct; operand access, ALU, shifts and stack use the interpreter's own helpers
(host/v33_exec.h), so both share one definition of every instruction. Rare instructions (string ops, mul/div,
BCD, NEC extras, int/iret, enter/leave...) run through c.exec_one() - the interpreter for exactly that one.

    tools/v33rec.py   -> gen/v33/v33_rec_NN.cpp (one per 64 KB of physical address), v33_table.cpp
"""
import os, re, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import v33_isa as I
import v33_discover as D

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(P, 'gen', 'v33')
ALU = I.ALU
SHIFT = I.SHIFT
BASE_EXPR = ['c.r[V33::BX] + c.r[V33::SI]', 'c.r[V33::BX] + c.r[V33::DI]', 'c.r[V33::BP] + c.r[V33::SI]',
             'c.r[V33::BP] + c.r[V33::DI]', 'c.r[V33::SI]', 'c.r[V33::DI]', 'c.r[V33::BP]', 'c.r[V33::BX]']
NATIVE = set(ALU) | set(SHIFT) | {'mov', 'test', 'inc', 'dec', 'not', 'neg', 'xchg', 'lea', 'push', 'pop', 'cbw',
                                  'cwd', 'clc', 'stc', 'cmc', 'cli', 'sti', 'cld', 'std', 'lahf', 'sahf', 'pushf',
                                  'popf', 'nop', 'wait', 'jcc', 'jmp', 'jmpf', 'jmpi', 'jmpfi', 'call', 'callf',
                                  'calli', 'callfi', 'retn', 'retf', 'loop', 'loopz', 'loopnz', 'jcxz', 'in', 'out',
                                  'les', 'lds', 'hlt'}


class Unsupported(Exception):
    pass


def fname(f):
    return 'r_%05x_%04x' % (((f.cs << 4) + f.ip) & 0xFFFFF, f.cs)


def h(v):
    return '0x%xu' % v


def cycles(ins):
    """Rough V33 cycles, the interpreter's estimate: 2 + 4 for a memory operand."""
    return 2 + (4 if any(o[0] == 'mem' for o in ins.ops) else 0)


class Gen:
    def __init__(s, f):
        s.f = f
        s.out = []
        s.labels = set()
        s.n = 0

    def emit(s, line):
        s.out.append('    ' + line)

    # ---- operands ----
    def ea(s, o):
        base, disp = o[2], o[3]
        if base is None:
            return h(disp & 0xFFFF)
        return '(uint16_t)(%s%s)' % (BASE_EXPR[base], (' + %d' % disp) if disp else '')

    def prep(s, o):
        """Computes a memory operand's offset once; returns a handle for rd/wr."""
        if o[0] != 'mem':
            return o
        s.n += 1
        v = 'ea%d' % s.n
        s.emit('const uint16_t %s = %s;' % (v, s.ea(o)))
        return ('memv', o[1], v, o[4])

    def rd(s, o, sz):
        k = o[0]
        if k == 'reg':
            return 'c.r[%d]' % o[2] if sz == 2 else 'e.r8(%d)' % o[2]
        if k == 'sreg':
            return 'c.sreg[%d]' % o[1]
        if k == 'imm':
            return h(o[2] & (0xFF if sz == 1 else 0xFFFF))
        if k == 'memv':
            return 'e.r%s(%d, %s)' % ('w' if sz == 2 else 'b', o[3], o[2])
        raise Unsupported('operand %r' % (o,))

    def wr(s, o, sz, v):
        k = o[0]
        if k == 'reg':
            s.emit(('c.r[%d] = (uint16_t)(%s);' % (o[2], v)) if sz == 2 else ('e.w8(%d, (uint8_t)(%s));' % (o[2], v)))
        elif k == 'sreg':
            s.emit('c.sreg[%d] = (uint16_t)(%s);' % (o[1], v))
        elif k == 'memv':
            s.emit('e.w%s(%d, %s, (%s)(%s));' % ('w' if sz == 2 else 'b', o[3], o[2], 'uint16_t' if sz == 2 else 'uint8_t', v))
        else:
            raise Unsupported('write to %r' % (o,))

    # ---- control flow ----
    def exit(s, cs, ip):
        s.emit('EXIT(%s, %s);' % (cs, ip))

    def goto(s, ip, frm):
        s.emit('cyc += 6;')   # a taken branch, as the interpreter counts it
        if ip in s.f.insns:
            if ip <= frm:
                s.emit('if (c.cycles + cyc >= c.slice) EXIT(%s, %s);' % (h(s.f.cs), h(ip)))
            s.emit('goto L_%04x;' % ip)
            s.labels.add(ip)
        else:
            s.exit(h(s.f.cs), h(ip))

    def interp(s, ins):
        """Exactly this instruction through the interpreter; leave if it went elsewhere (int, div error...)."""
        nxt = ins.next_ip
        s.emit('c.ip = %s; c.sreg[V33::CS] = %s; cyc += c.exec_one();' % (h(ins.ip), h(s.f.cs)))
        s.emit('if (c.ip != %s || c.sreg[V33::CS] != %s || c.halted) EXIT(c.sreg[V33::CS], c.ip);' % (h(nxt), h(s.f.cs)))

    def insn(s, ins):
        op, sz, ops = ins.op, ins.osize, ins.ops
        cs, nxt = s.f.cs, ins.next_ip
        s.emit('cyc += %d;' % cycles(ins))
        if op not in NATIVE or ins.rep:
            s.interp(ins)
            return
        W = 8 if sz == 1 else 16
        if op == 'mov':
            d = s.prep(ops[0]); src = s.prep(ops[1])
            s.wr(d, sz, s.rd(src, sz)); return
        if op in ALU:
            k = ALU.index(op)
            d = s.prep(ops[0]); src = s.prep(ops[1])
            s.emit('{ uint32_t r_ = e.alu<%d>(%d, %s, %s);' % (W, k, s.rd(d, sz), s.rd(src, sz)))
            if k != 7:
                s.wr(d, sz, 'r_')
            s.emit('}'); return
        if op == 'test':
            a = s.prep(ops[0]); b = s.prep(ops[1])
            s.emit('e.logic<%d>(%s & %s);' % (W, s.rd(a, sz), s.rd(b, sz))); return
        if op in ('inc', 'dec'):
            d = s.prep(ops[0])
            s.wr(d, sz, 'e.%s<%d>(%s)' % (op, W, s.rd(d, sz))); return
        if op == 'not':
            d = s.prep(ops[0])
            s.wr(d, sz, '~%s' % s.rd(d, sz)); return
        if op == 'neg':
            d = s.prep(ops[0])
            s.emit('{ uint32_t v_ = %s; uint32_t r_ = e.sub<%d>(0, v_, false); c.CF = v_ != 0;' % (s.rd(d, sz), W))
            s.wr(d, sz, 'r_'); s.emit('}'); return
        if op == 'xchg':
            a = s.prep(ops[0]); b = s.prep(ops[1])
            s.emit('{ uint32_t t_ = %s;' % s.rd(a, sz))
            s.wr(a, sz, s.rd(b, sz)); s.wr(b, sz, 't_'); s.emit('}'); return
        if op == 'lea':
            s.emit('c.r[%d] = %s;' % (ops[0][2], s.ea(ops[1]))); return
        if op in ('les', 'lds'):
            m = s.prep(ops[1])
            s.emit('{ uint16_t o_ = e.rw(%d, %s), g_ = e.rw(%d, (uint16_t)(%s + 2));' % (m[3], m[2], m[3], m[2]))
            s.emit('c.r[%d] = o_; c.sreg[%d] = g_; }' % (ops[0][2], 0 if op == 'les' else 3)); return
        if op == 'push':
            o = ops[0]
            if o == ('reg', 2, 4):     # push sp: the value after the decrement (8086 / V30)
                s.emit('{ uint16_t sp_ = c.r[V33::SP] - 2; e.push(sp_); }'); return
            o = s.prep(o)
            s.emit('e.push(%s);' % s.rd(o, 2)); return
        if op == 'pop':
            d = s.prep(ops[0])
            s.emit('{ uint16_t v_ = e.pop();'); s.wr(d, 2, 'v_'); s.emit('}'); return
        if op in SHIFT:
            d = s.prep(ops[0])
            n = h(ops[1][2]) if ops[1][0] == 'imm' else 'e.r8(1)'
            s.wr(d, sz, 'e.shift<%d>(%d, %s, %s)' % (W, SHIFT.index(op), s.rd(d, sz), n)); return
        if op == 'cbw': s.emit('c.r[V33::AX] = (uint16_t)(int8_t)e.r8(0);'); return
        if op == 'cwd': s.emit('c.r[V33::DX] = (c.r[V33::AX] & 0x8000) ? 0xffff : 0;'); return
        flagops = {'clc': 'c.CF = false;', 'stc': 'c.CF = true;', 'cmc': 'c.CF = !c.CF;', 'cli': 'c.IF = false;',
                   'cld': 'c.DF = false;', 'std': 'c.DF = true;', 'nop': '', 'wait': '',
                   'lahf': 'e.w8(4, (uint8_t)c.flags());', 'pushf': 'e.push(c.flags());',
                   'sahf': '{ uint8_t ah_ = e.r8(4); c.set_flags((c.flags() & 0xff00) | ah_); }'}
        if op in flagops:
            s.emit(flagops[op]); return
        if op in ('sti', 'popf'):        # may enable interrupts: let the dispatcher look
            s.emit('c.IF = true;' if op == 'sti' else 'c.set_flags(e.pop());')
            s.exit(h(cs), h(nxt)); return
        if op == 'hlt':
            s.emit('c.halted = true;'); s.exit(h(cs), h(nxt)); return
        if op == 'in':
            port = h(ops[0][2]) if ops[0][0] == 'imm' else 'c.r[V33::DX]'
            if sz == 1: s.emit('e.w8(0, c.in8(%s));' % port)
            else: s.emit('{ uint16_t p_ = %s; c.r[V33::AX] = c.in8(p_) | (c.in8((uint16_t)(p_ + 1)) << 8); }' % port)
            return
        if op == 'out':
            port = h(ops[0][2]) if ops[0][0] == 'imm' else 'c.r[V33::DX]'
            if sz == 1: s.emit('c.out8(%s, e.r8(0));' % port)
            else: s.emit('{ uint16_t p_ = %s; c.out8(p_, (uint8_t)c.r[V33::AX]); c.out8((uint16_t)(p_ + 1), c.r[V33::AX] >> 8); }' % port)
            s.exit(h(cs), h(nxt))   # I/O can raise interrupts (PIC, DMA): back to the dispatcher
            return
        # ---- control flow ----
        if op == 'jcc':
            s.emit('if (e.cond(%d)) {' % ins.cc); s.goto(ins.target, ins.ip); s.emit('}'); return
        if op in ('loop', 'loopz', 'loopnz'):
            cond = {'loop': 'true', 'loopz': 'c.ZF', 'loopnz': '!c.ZF'}[op]
            s.emit('if (--c.r[V33::CX] != 0 && %s) {' % cond); s.goto(ins.target, ins.ip); s.emit('}'); return
        if op == 'jcxz':
            s.emit('if (!c.r[V33::CX]) {'); s.goto(ins.target, ins.ip); s.emit('}'); return
        if op == 'jmp':
            s.goto(ins.target, ins.ip); return
        if op == 'jmpf':
            s.emit('cyc += 8;'); s.exit(h(ins.target[0]), h(ins.target[1])); return
        if op == 'jmpi':
            o = s.prep(ops[0])
            s.emit('{ cyc += 6; uint16_t t_ = %s; if (c.on_jump) c.on_jump(%s, t_, false); EXIT(%s, t_); }' % (s.rd(o, 2), h(cs), h(cs))); return
        if op == 'jmpfi':
            m = s.prep(ops[0])
            s.emit('{ uint16_t o_ = e.rw(%d, %s), g_ = e.rw(%d, (uint16_t)(%s + 2)); if (c.on_jump) c.on_jump(g_, o_, true); EXIT(g_, o_); }'
                   % (m[3], m[2], m[3], m[2])); return
        if op == 'call':
            s.emit('cyc += 8; e.push(%s);' % h(nxt))
            s.emit('if (c.on_call) c.on_call(%s, %s, false);' % (h(cs), h(ins.target)))
            s.exit(h(cs), h(ins.target)); return
        if op == 'callf':
            s.emit('cyc += 12; e.push(%s); e.push(%s);' % (h(cs), h(nxt)))
            s.emit('if (c.on_call) c.on_call(%s, %s, true);' % (h(ins.target[0]), h(ins.target[1])))
            s.exit(h(ins.target[0]), h(ins.target[1])); return
        if op == 'calli':
            o = s.prep(ops[0])
            s.emit('{ cyc += 8; uint16_t t_ = %s; e.push(%s); if (c.on_call) c.on_call(%s, t_, false); EXIT(%s, t_); }'
                   % (s.rd(o, 2), h(nxt), h(cs), h(cs))); return
        if op == 'callfi':
            m = s.prep(ops[0])
            s.emit('{ uint16_t o_ = e.rw(%d, %s), g_ = e.rw(%d, (uint16_t)(%s + 2)); e.push(%s); e.push(%s);'
                   % (m[3], m[2], m[3], m[2], h(cs), h(nxt)))
            s.emit('  if (c.on_call) c.on_call(g_, o_, true); EXIT(g_, o_); }'); return
        if op == 'retn':
            n = ops[0][2] if ops else 0
            s.emit('{ cyc += 8; uint16_t t_ = e.pop();%s EXIT(%s, t_); }' % ((' c.r[V33::SP] += %d;' % n) if n else '', h(cs))); return
        if op == 'retf':
            n = ops[0][2] if ops else 0
            s.emit('{ cyc += 12; uint16_t t_ = e.pop(); uint16_t g_ = e.pop();%s EXIT(g_, t_); }' % ((' c.r[V33::SP] += %d;' % n) if n else '')); return
        raise Unsupported('op %s' % op)

    def run(s):
        f = s.f
        ips = sorted(f.insns)
        prev_end = None
        for ip in ips:
            ins = f.insns[ip]
            if prev_end is not None and ip != prev_end:
                s.exit(h(f.cs), h(prev_end))      # the code runs on past what discovery decoded
            s.out.append('L_%04x:' % ip)
            s.out.append('    {   // %04x  %r' % (ip, ins))   # a block per instruction: gotos never skip a local
            s.insn(ins)
            s.out.append('    }')
            prev_end = ins.next_ip
            if ins.op in ('jmp', 'jmpf', 'jmpi', 'jmpfi', 'call', 'callf', 'calli', 'callfi', 'retn', 'retf', 'iret',
                          'hlt', 'sti', 'popf', 'out'):
                prev_end = None
        if prev_end is not None:
            s.exit(h(f.cs), h(prev_end))
        return s.out


def gen_func(f):
    g = Gen(f)
    body = g.run()
    out = ['void %s(V33& c) {' % fname(f), '    v33x::Exec e(c);', '    int cyc = 0;', '    goto L_%04x;' % f.ip]
    targets = g.labels | {f.ip}
    for ln in body:
        if ln.startswith('L_') and ln.endswith(':') and int(ln[2:6], 16) not in targets:
            continue
        out.append(ln)
    out.append('}')
    return out


def write_if_changed(path, text):
    try:
        if open(path).read() == text:
            return
    except OSError:
        pass
    with open(path, 'w') as o:
        o.write(text)


def main():
    # run-time targets recorded by the desktop interpreter
    try:
        for l in open(os.path.join(P, 'tools', 'v33_runtime_targets.txt')):
            cs, ip = l.split()[0].split(':')
            D.EXTRA[(int(cs, 16), int(ip, 16))] = 'runtime'
    except OSError:
        pass
    D.run()
    # Continuation points are entries too: where a call returns to, and where a routine resumes after it left for
    # the dispatcher (I/O, sti/popf, hlt). Repeat until no new ones appear.
    CONT = ('call', 'callf', 'calli', 'callfi', 'out', 'sti', 'popf', 'hlt', 'int', 'into')
    while True:
        n = len(D.funcs)
        for f in list(D.funcs.values()):
            for ins in f.insns.values():
                if ins.op in CONT:
                    D.add(f.cs, ins.next_ip, 'cont')
        while D.pending:
            D.walk(D.funcs[D.pending.popleft()])
        if len(D.funcs) == n:
            break
    funcs = sorted(D.funcs.values(), key=lambda f: (((f.cs << 4) + f.ip) & 0xFFFFF, f.cs))
    os.makedirs(OUT, exist_ok=True)
    ok, failed = [], collections.Counter()
    for f in funcs:
        try:
            ok.append((f, gen_func(f)))
        except Unsupported as e:
            failed[str(e)] += 1
    buckets = collections.defaultdict(list)
    for f, lines in ok:
        buckets[((f.cs << 4) + f.ip) >> 16 & 0xF].append(lines)
    hdr = ('// generated by tools/v33rec.py from the V33 ROM - do not edit\n#include "v33_exec.h"\n\n'
           '#define EXIT(cs_, ip_) do { c.sreg[V33::CS] = (uint16_t)(cs_); c.ip = (uint16_t)(ip_); c.cycles += cyc; return; } while (0)\n\n')
    wanted = set()
    for b, items in sorted(buckets.items()):
        name = 'v33_rec_%x.cpp' % b
        wanted.add(name)
        write_if_changed(os.path.join(OUT, name), hdr + ''.join('\n'.join(l) + '\n\n' for l in items))
    for old in os.listdir(OUT):
        if old.startswith('v33_rec_') and old not in wanted:
            os.remove(os.path.join(OUT, old))
    tab = ['// generated by tools/v33rec.py - do not edit', '#include "rec.h"', '']
    tab += ['void %s(V33& c);' % fname(f) for f, _ in ok]
    tab += ['', 'const rec::Entry rec::table[] = {']
    tab += ['    {0x%05xu, 0x%04xu, %s},' % (((f.cs << 4) + f.ip) & 0xFFFFF, f.cs, fname(f)) for f, _ in ok]
    tab += ['};', 'const unsigned rec::table_count = %d;' % len(ok), '']
    write_if_changed(os.path.join(OUT, 'v33_table.cpp'), '\n'.join(tab))
    ninsn = sum(len(f.insns) for f, _ in ok)
    nnat = sum(1 for f, _ in ok for i in f.insns.values() if i.op in NATIVE and not i.rep)
    print('translated %d routines (%d instructions, %d native, %d via the interpreter) into %d files; %d failed'
          % (len(ok), ninsn, nnat, ninsn - nnat, len(wanted), sum(failed.values())))
    for why, n in failed.most_common(10):
        print('  %4d  %s' % (n, why))


if __name__ == '__main__':
    main()
