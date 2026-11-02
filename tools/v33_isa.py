#!/usr/bin/env python3
"""v33_isa.py: our own NEC V33 / V30 (x86-16) instruction decoder, used by the discovery and the recompiler.

decode(fetch, cs, ip) -> Insn or None, where fetch(physical_address) returns a byte (or None outside ROM).
Covers the 8086 set, the 80186 additions the V30/V33 have (push imm, pusha/popa, imul imm, shifts by imm,
enter/leave, bound, ins/outs) and the NEC-only instructions (0F-prefixed TEST1/CLR1/SET1/NOT1, ADD4S/SUB4S/
CMP4S, ROL4/ROR4, INS/EXT; REPC/REPNC prefixes 0x65/0x64). Operands are kept symbolic:
  ('reg', size, n)              n: 0-7 = AX CX DX BX SP BP SI DI (size 2) or AL CL DL BL AH CH DH BH (size 1)
  ('sreg', n)                   0-3 = ES CS SS DS
  ('mem', size, base, disp, seg) base: one of the 8 ModRM bases or None (direct address); seg: segment index
                                 (default DS, or SS for BP-based) after any override
  ('imm', size, value)
"""

REG16 = ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di']
REG8 = ['al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh']
SREG = ['es', 'cs', 'ss', 'ds']
# ModRM rm bases (mod != 3): bx+si bx+di bp+si bp+di si di bp bx
BASES = ['bx+si', 'bx+di', 'bp+si', 'bp+di', 'si', 'di', 'bp', 'bx']
COND = ['o', 'no', 'b', 'nb', 'z', 'nz', 'be', 'a', 's', 'ns', 'p', 'np', 'l', 'ge', 'le', 'g']


class Insn:
    __slots__ = ('cs', 'ip', 'size', 'op', 'ops', 'osize', 'rep', 'seg', 'cc', 'target', 'far', 'raw')

    def __init__(s, cs, ip):
        s.cs, s.ip = cs, ip
        s.size = 0
        s.op = None
        s.ops = []
        s.osize = 0          # operand size 1/2 (0 = n/a)
        s.rep = None         # 'rep', 'repne', 'repc', 'repnc'
        s.seg = None         # segment override index
        s.cc = None          # condition code index for jcc/setcc-like
        s.target = None      # near target IP, or (seg, off) for far
        s.far = False
        s.raw = b''

    @property
    def phys(s):
        return ((s.cs << 4) + s.ip) & 0xFFFFF

    @property
    def next_ip(s):
        return (s.ip + s.size) & 0xFFFF

    def __repr__(s):
        def fo(o):
            k = o[0]
            if k == 'reg': return (REG16 if o[1] == 2 else REG8)[o[2]]
            if k == 'sreg': return SREG[o[1]]
            if k == 'imm': return '0x%x' % o[2]
            if k == 'mem':
                b = BASES[o[2]] if o[2] is not None else ''
                d = ('%+d' % o[3]) if b and o[3] else ('0x%x' % o[3] if not b else '')
                return '%s[%s:%s%s]' % ('byte ' if o[1] == 1 else 'word ' if o[1] == 2 else '', SREG[o[4]], b, d)
            return str(o)
        m = s.op if s.cc is None else s.op.replace('cc', COND[s.cc])
        if s.rep: m = s.rep + ' ' + m
        t = ''
        if s.target is not None:
            t = ('%04x:%04x' % s.target) if s.far else ('%04x' % s.target)
        return '%s %s' % (m, ', '.join([fo(o) for o in s.ops] + ([t] if t else [])))


class Bad(Exception):
    pass


class Rd:
    def __init__(s, fetch, cs, ip, opmap=None):
        s.fetch, s.cs, s.ip, s.start = fetch, cs, ip, ip
        s.raw = bytearray()
        s.opmap = opmap      # V35 with encrypted opcodes: opcode / prefix bytes go through this 256-byte table

    def op(s):
        v = s.b()
        return s.opmap[v] if s.opmap is not None else v

    def b(s):
        v = s.fetch(((s.cs << 4) + s.ip) & 0xFFFFF)
        if v is None:
            raise Bad('outside ROM')
        s.ip = (s.ip + 1) & 0xFFFF
        s.raw.append(v)
        return v

    def w(s):
        lo = s.b()
        return lo | (s.b() << 8)

    def sb(s):
        v = s.b()
        return v - 256 if v & 0x80 else v

    def sw(s):
        v = s.w()
        return v - 65536 if v & 0x8000 else v


def modrm(r, size, seg_override):
    """Returns (reg field, rm operand)."""
    m = r.b()
    mod, reg, rm = m >> 6, (m >> 3) & 7, m & 7
    if mod == 3:
        return reg, ('reg', size, rm)
    if mod == 0 and rm == 6:
        disp = r.w()
        base = None
    else:
        base = rm
        disp = r.sb() if mod == 1 else r.sw() if mod == 2 else 0
    seg = seg_override if seg_override is not None else (2 if base in (2, 3, 6) else 3)   # BP-based -> SS
    return reg, ('mem', size, base, disp & 0xFFFF if base is None else disp, seg)


ALU = ['add', 'or', 'adc', 'sbb', 'and', 'sub', 'xor', 'cmp']
SHIFT = ['rol', 'ror', 'rcl', 'rcr', 'shl', 'shr', 'sal', 'sar']


def decode(fetch, cs, ip, opmap=None):
    r = Rd(fetch, cs, ip, opmap)
    ins = Insn(cs, ip)
    try:
        _decode(r, ins)
    except Bad:
        return None
    ins.size = (r.ip - ip) & 0xFFFF
    ins.raw = bytes(r.raw)
    return ins


def _decode(r, I):
    seg = None
    rep = None
    while True:
        b = r.op()
        if b in (0x26, 0x2E, 0x36, 0x3E):
            seg = (b >> 3) & 3
        elif b == 0xF3: rep = 'rep'
        elif b == 0xF2: rep = 'repne'
        elif b == 0x65: rep = 'repc'      # NEC
        elif b == 0x64: rep = 'repnc'     # NEC
        elif b == 0xF0: pass              # lock (bus lock: no effect here)
        else:
            break
    I.seg, I.rep = seg, rep
    op = b
    # ---- ALU r/m, reg / reg, r/m / acc, imm ----
    if op < 0x40 and (op & 7) < 6:
        k = ALU[op >> 3]
        sz = 1 if not op & 1 else 2
        if (op & 7) < 4:
            reg, rm = modrm(r, sz, seg)
            R = ('reg', sz, reg)
            I.op, I.osize, I.ops = k, sz, ([rm, R] if not op & 2 else [R, rm])
            return
        imm = r.b() if sz == 1 else r.w()
        I.op, I.osize, I.ops = k, sz, [('reg', sz, 0), ('imm', sz, imm)]
        return
    if op in (0x27, 0x2F, 0x37, 0x3F):
        I.op = {0x27: 'daa', 0x2F: 'das', 0x37: 'aaa', 0x3F: 'aas'}[op]; return
    if op < 0x40:
        lo = op & 7
        if lo == 6 or lo == 7:
            sr = op >> 3
            if op == 0x0F:
                return _nec0f(r, I, seg)
            I.op = 'push' if lo == 6 else 'pop'
            I.ops = [('sreg', sr)]; I.osize = 2
            return
        raise Bad('prefix in wrong place')
    if op == 0x27: I.op = 'daa'; return
    if op < 0x50:
        I.op = 'inc' if op < 0x48 else 'dec'; I.osize = 2; I.ops = [('reg', 2, op & 7)]; return
    if op < 0x58: I.op = 'push'; I.osize = 2; I.ops = [('reg', 2, op & 7)]; return
    if op < 0x60: I.op = 'pop'; I.osize = 2; I.ops = [('reg', 2, op & 7)]; return
    if op == 0x60: I.op = 'pusha'; return
    if op == 0x61: I.op = 'popa'; return
    if op == 0x62:
        reg, rm = modrm(r, 2, seg)
        if rm[0] != 'mem': raise Bad('bound reg')
        I.op = 'bound'; I.osize = 2; I.ops = [('reg', 2, reg), rm]; return
    if op == 0x68: I.op = 'push'; I.osize = 2; I.ops = [('imm', 2, r.w())]; return
    if op == 0x6A: I.op = 'push'; I.osize = 2; I.ops = [('imm', 2, r.sb() & 0xFFFF)]; return
    if op in (0x69, 0x6B):
        reg, rm = modrm(r, 2, seg)
        imm = r.w() if op == 0x69 else r.sb() & 0xFFFF
        I.op = 'imul3'; I.osize = 2; I.ops = [('reg', 2, reg), rm, ('imm', 2, imm)]; return
    if op in (0x6C, 0x6D, 0x6E, 0x6F):
        I.op = ['insb', 'insw', 'outsb', 'outsw'][op - 0x6C]; I.osize = 1 if not op & 1 else 2; return
    if 0x70 <= op < 0x80:
        d = r.sb()
        I.op = 'jcc'; I.cc = op & 15; I.target = (r.ip + d) & 0xFFFF; return
    if 0x80 <= op <= 0x83:
        sz = 1 if op in (0x80, 0x82) else 2
        reg, rm = modrm(r, sz, seg)
        imm = r.b() if sz == 1 else (r.w() if op == 0x81 else r.sb() & 0xFFFF)
        I.op = ALU[reg]; I.osize = sz; I.ops = [rm, ('imm', sz, imm)]; return
    if op in (0x84, 0x85):
        sz = 1 if op == 0x84 else 2
        reg, rm = modrm(r, sz, seg)
        I.op = 'test'; I.osize = sz; I.ops = [rm, ('reg', sz, reg)]; return
    if op in (0x86, 0x87):
        sz = 1 if op == 0x86 else 2
        reg, rm = modrm(r, sz, seg)
        I.op = 'xchg'; I.osize = sz; I.ops = [rm, ('reg', sz, reg)]; return
    if 0x88 <= op <= 0x8B:
        sz = 1 if not op & 1 else 2
        reg, rm = modrm(r, sz, seg)
        R = ('reg', sz, reg)
        I.op = 'mov'; I.osize = sz; I.ops = [rm, R] if not op & 2 else [R, rm]; return
    if op == 0x8C:
        reg, rm = modrm(r, 2, seg)
        I.op = 'mov'; I.osize = 2; I.ops = [rm, ('sreg', reg & 3)]; return
    if op == 0x8D:
        reg, rm = modrm(r, 2, seg)
        if rm[0] != 'mem': raise Bad('lea reg')
        I.op = 'lea'; I.osize = 2; I.ops = [('reg', 2, reg), rm]; return
    if op == 0x8E:
        reg, rm = modrm(r, 2, seg)
        I.op = 'mov'; I.osize = 2; I.ops = [('sreg', reg & 3), rm]; return
    if op == 0x8F:
        reg, rm = modrm(r, 2, seg)
        I.op = 'pop'; I.osize = 2; I.ops = [rm]; return
    if op == 0x90: I.op = 'nop'; return
    if op < 0x98: I.op = 'xchg'; I.osize = 2; I.ops = [('reg', 2, 0), ('reg', 2, op & 7)]; return
    if op == 0x98: I.op = 'cbw'; return
    if op == 0x99: I.op = 'cwd'; return
    if op == 0x9A:
        off = r.w(); sg = r.w()
        I.op = 'callf'; I.far = True; I.target = (sg, off); return
    if op == 0x9B: I.op = 'wait'; return
    if op == 0x9C: I.op = 'pushf'; return
    if op == 0x9D: I.op = 'popf'; return
    if op == 0x9E: I.op = 'sahf'; return
    if op == 0x9F: I.op = 'lahf'; return
    if 0xA0 <= op <= 0xA3:
        sz = 1 if not op & 1 else 2
        addr = r.w()
        M = ('mem', sz, None, addr, seg if seg is not None else 3)
        A = ('reg', sz, 0)
        I.op = 'mov'; I.osize = sz; I.ops = [A, M] if op < 0xA2 else [M, A]; return
    if 0xA4 <= op <= 0xAF and op not in (0xA8, 0xA9):
        names = {0xA4: 'movs', 0xA6: 'cmps', 0xAA: 'stos', 0xAC: 'lods', 0xAE: 'scas'}
        I.op = names[op & 0xFE]; I.osize = 1 if not op & 1 else 2; return
    if op in (0xA8, 0xA9):
        sz = 1 if op == 0xA8 else 2
        imm = r.b() if sz == 1 else r.w()
        I.op = 'test'; I.osize = sz; I.ops = [('reg', sz, 0), ('imm', sz, imm)]; return
    if 0xB0 <= op < 0xB8: I.op = 'mov'; I.osize = 1; I.ops = [('reg', 1, op & 7), ('imm', 1, r.b())]; return
    if 0xB8 <= op < 0xC0: I.op = 'mov'; I.osize = 2; I.ops = [('reg', 2, op & 7), ('imm', 2, r.w())]; return
    if op in (0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3):
        sz = 1 if not op & 1 else 2
        reg, rm = modrm(r, sz, seg)
        if op in (0xC0, 0xC1): cnt = ('imm', 1, r.b())
        elif op in (0xD0, 0xD1): cnt = ('imm', 1, 1)
        else: cnt = ('reg', 1, 1)   # CL
        I.op = SHIFT[reg]; I.osize = sz; I.ops = [rm, cnt]; return
    if op == 0xC2: I.op = 'retn'; I.ops = [('imm', 2, r.w())]; return
    if op == 0xC3: I.op = 'retn'; return
    if op in (0xC4, 0xC5):
        reg, rm = modrm(r, 2, seg)
        if rm[0] != 'mem': raise Bad('les reg')
        I.op = 'les' if op == 0xC4 else 'lds'; I.osize = 2; I.ops = [('reg', 2, reg), rm]; return
    if op in (0xC6, 0xC7):
        sz = 1 if op == 0xC6 else 2
        reg, rm = modrm(r, sz, seg)
        imm = r.b() if sz == 1 else r.w()
        I.op = 'mov'; I.osize = sz; I.ops = [rm, ('imm', sz, imm)]; return
    if op == 0xC8:
        sz = r.w(); lvl = r.b()
        I.op = 'enter'; I.ops = [('imm', 2, sz), ('imm', 1, lvl)]; return
    if op == 0xC9: I.op = 'leave'; return
    if op == 0xCA: I.op = 'retf'; I.ops = [('imm', 2, r.w())]; return
    if op == 0xCB: I.op = 'retf'; return
    if op == 0xCC: I.op = 'int'; I.ops = [('imm', 1, 3)]; return
    if op == 0xCD: I.op = 'int'; I.ops = [('imm', 1, r.b())]; return
    if op == 0xCE: I.op = 'into'; return
    if op == 0xCF: I.op = 'iret'; return
    if op == 0xD4: r.b(); I.op = 'aam'; return
    if op == 0xD5: r.b(); I.op = 'aad'; return
    if op == 0xD7: I.op = 'xlat'; return
    if 0xD8 <= op <= 0xDF: raise Bad('fpu escape')
    if op in (0xE0, 0xE1, 0xE2, 0xE3):
        d = r.sb()
        I.op = ['loopnz', 'loopz', 'loop', 'jcxz'][op - 0xE0]; I.target = (r.ip + d) & 0xFFFF; return
    if op in (0xE4, 0xE5): I.op = 'in'; I.osize = 1 if op == 0xE4 else 2; I.ops = [('imm', 1, r.b())]; return
    if op in (0xE6, 0xE7): I.op = 'out'; I.osize = 1 if op == 0xE6 else 2; I.ops = [('imm', 1, r.b())]; return
    if op == 0xE8: d = r.sw(); I.op = 'call'; I.target = (r.ip + d) & 0xFFFF; return
    if op == 0xE9: d = r.sw(); I.op = 'jmp'; I.target = (r.ip + d) & 0xFFFF; return
    if op == 0xEA: off = r.w(); sg = r.w(); I.op = 'jmpf'; I.far = True; I.target = (sg, off); return
    if op == 0xEB: d = r.sb(); I.op = 'jmp'; I.target = (r.ip + d) & 0xFFFF; return
    if op in (0xEC, 0xED): I.op = 'in'; I.osize = 1 if op == 0xEC else 2; I.ops = [('reg', 2, 2)]; return
    if op in (0xEE, 0xEF): I.op = 'out'; I.osize = 1 if op == 0xEE else 2; I.ops = [('reg', 2, 2)]; return
    if op == 0xF4: I.op = 'hlt'; return
    if op == 0xF5: I.op = 'cmc'; return
    if op in (0xF6, 0xF7):
        sz = 1 if op == 0xF6 else 2
        reg, rm = modrm(r, sz, seg)
        k = ['test', 'test', 'not', 'neg', 'mul', 'imul', 'div', 'idiv'][reg]
        I.op = k; I.osize = sz
        I.ops = [rm, ('imm', sz, r.b() if sz == 1 else r.w())] if k == 'test' else [rm]
        return
    if op == 0xF8: I.op = 'clc'; return
    if op == 0xF9: I.op = 'stc'; return
    if op == 0xFA: I.op = 'cli'; return
    if op == 0xFB: I.op = 'sti'; return
    if op == 0xFC: I.op = 'cld'; return
    if op == 0xFD: I.op = 'std'; return
    if op == 0xFE:
        reg, rm = modrm(r, 1, seg)
        if reg > 1: raise Bad('FE /%d' % reg)
        I.op = 'inc' if reg == 0 else 'dec'; I.osize = 1; I.ops = [rm]; return
    if op == 0xFF:
        reg, rm = modrm(r, 2, seg)
        k = ['inc', 'dec', 'calli', 'callfi', 'jmpi', 'jmpfi', 'push', None][reg]
        if k is None: raise Bad('FF /7')
        if k in ('callfi', 'jmpfi') and rm[0] != 'mem': raise Bad('far indirect via reg')
        I.op = k; I.osize = 2; I.ops = [rm]; return
    if op == 0x37: I.op = 'aaa'; return
    if op == 0x3F: I.op = 'aas'; return
    if op == 0x2F: I.op = 'das'; return
    raise Bad('opcode %02x' % op)


def _nec0f(r, I, seg):
    """NEC 0F-prefixed instructions."""
    b = r.b()
    if 0x10 <= b <= 0x1F:
        # TEST1/CLR1/SET1/NOT1 r/m, CL (10-17) or r/m, imm (18-1F); bit 0: 16-bit
        k = ['test1', 'clr1', 'set1', 'not1'][(b >> 1) & 3]
        sz = 2 if b & 1 else 1
        reg, rm = modrm(r, sz, seg)
        if b < 0x18: cnt = ('reg', 1, 1)
        else: cnt = ('imm', 1, r.b())
        I.op = k; I.osize = sz; I.ops = [rm, cnt]; return
    if b in (0x20, 0x22, 0x26):
        I.op = {0x20: 'add4s', 0x22: 'sub4s', 0x26: 'cmp4s'}[b]; return
    if b in (0x28, 0x2A):
        reg, rm = modrm(r, 1, seg)
        I.op = 'rol4' if b == 0x28 else 'ror4'; I.osize = 1; I.ops = [rm]; return
    if b in (0x31, 0x33, 0x39, 0x3B):
        if b in (0x31, 0x33):
            m = r.b()
            I.ops = [('reg', 1, m & 7), ('reg', 1, (m >> 3) & 7)]
        else:
            m = r.b()
            I.ops = [('reg', 1, m & 7), ('imm', 1, r.b())]
        I.op = 'ins' if b in (0x31, 0x39) else 'ext'; return
    if b == 0x92: I.op = 'fint'; return      # V25/V35: end of interrupt
    if b == 0xFF: r.b(); I.op = 'brkem'; return
    raise Bad('0F %02x' % b)
