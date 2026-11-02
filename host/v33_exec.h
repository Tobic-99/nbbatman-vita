// The V33 execution core shared by the interpreter (v33.cpp) and the recompiled code (gen/): operand
// access, flags and the semantics of every instruction. Header-only so both inline it.
#pragma once
#include "v33.h"

namespace v33x {
enum Rep { NONE, REPE, REPNE, REPC, REPNC };

struct Ins {           // per-instruction decode state
    int seg = -1;      // segment override
    Rep rep = NONE;
    // ModRM
    int mod, reg, rm;
    int ea_seg;
    uint16_t ea_off;
};

inline bool parity(uint8_t v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return !(v & 1); }

struct Exec {
    V33& c;
    Ins in;
    int cyc = 2;
    explicit Exec(V33& cpu) : c(cpu) {}

    // ---- memory ----
    uint8_t fetch() {
        uint32_t a = c.phys(V33::CS, c.ip++);
        if (c.fetch8) return c.fetch8(a);
        if (const uint8_t* p = c.rpage[a >> 12]) return p[a & 0xfff];
        return c.rd8(a);
    }
    uint8_t fetch_op() {                 // an opcode or prefix byte (decrypted on the Irem V35)
        uint8_t b = fetch();
        return c.optable ? c.optable[b] : b;
    }
    uint16_t fetch16() { uint16_t lo = fetch(); return lo | (fetch() << 8); }
    uint8_t rb(int seg, uint16_t off) {
        uint32_t a = c.phys(seg, off);
        if (const uint8_t* p = c.rpage[a >> 12]) return p[a & 0xfff];
        return c.rd8(a);
    }
    uint16_t rw(int seg, uint16_t off) { return rb(seg, off) | (rb(seg, (uint16_t)(off + 1)) << 8); }
    void wb(int seg, uint16_t off, uint8_t v) {
        uint32_t a = c.phys(seg, off);
        if (uint8_t* p = c.wpage[a >> 12]) { p[a & 0xfff] = v; return; }
        c.wr8(a, v);
    }
    void ww(int seg, uint16_t off, uint16_t v) { wb(seg, off, (uint8_t)v); wb(seg, (uint16_t)(off + 1), v >> 8); }
    void push(uint16_t v) { c.r[V33::SP] -= 2; ww(V33::SS, c.r[V33::SP], v); }
    uint16_t pop() { uint16_t v = rw(V33::SS, c.r[V33::SP]); c.r[V33::SP] += 2; return v; }
    int dseg() const { return in.seg >= 0 ? in.seg : V33::DS; }

    // ---- registers ----
    uint8_t r8(int i) { return i < 4 ? (uint8_t)c.r[i] : (uint8_t)(c.r[i - 4] >> 8); }
    void w8(int i, uint8_t v) {
        if (i < 4) c.r[i] = (c.r[i] & 0xff00) | v;
        else c.r[i - 4] = (c.r[i - 4] & 0x00ff) | (v << 8);
    }

    // ---- ModRM ----
    void modrm() {
        uint8_t m = fetch();
        in.mod = m >> 6; in.reg = (m >> 3) & 7; in.rm = m & 7;
        if (in.mod == 3) return;
        cyc += 4;
        uint16_t off = 0; int seg = V33::DS;
        switch (in.rm) {
        case 0: off = c.r[V33::BX] + c.r[V33::SI]; break;
        case 1: off = c.r[V33::BX] + c.r[V33::DI]; break;
        case 2: off = c.r[V33::BP] + c.r[V33::SI]; seg = V33::SS; break;
        case 3: off = c.r[V33::BP] + c.r[V33::DI]; seg = V33::SS; break;
        case 4: off = c.r[V33::SI]; break;
        case 5: off = c.r[V33::DI]; break;
        case 6: if (in.mod == 0) off = 0; else { off = c.r[V33::BP]; seg = V33::SS; } break;
        case 7: off = c.r[V33::BX]; break;
        }
        if (in.mod == 0 && in.rm == 6) off = fetch16();
        else if (in.mod == 1) off += (int8_t)fetch();
        else if (in.mod == 2) off += fetch16();
        in.ea_off = off;
        in.ea_seg = in.seg >= 0 ? in.seg : seg;
    }
    uint8_t getE8() { return in.mod == 3 ? r8(in.rm) : rb(in.ea_seg, in.ea_off); }
    uint16_t getE16() { return in.mod == 3 ? c.r[in.rm] : rw(in.ea_seg, in.ea_off); }
    void putE8(uint8_t v) { if (in.mod == 3) w8(in.rm, v); else wb(in.ea_seg, in.ea_off, v); }
    void putE16(uint16_t v) { if (in.mod == 3) c.r[in.rm] = v; else ww(in.ea_seg, in.ea_off, v); }

    // ---- flags / ALU (W = 8 or 16 bits) ----
    template <int W> static uint32_t mask() { return W == 8 ? 0xff : 0xffff; }
    template <int W> static bool sign(uint32_t v) { return (v >> (W - 1)) & 1; }
    template <int W> void szp(uint32_t r) { r &= mask<W>(); c.SF = sign<W>(r); c.ZF = r == 0; c.PF = parity((uint8_t)r); }

    template <int W> uint32_t add(uint32_t a, uint32_t b, bool carry) {
        uint32_t r = a + b + carry;
        c.CF = r > mask<W>(); c.AF = ((a ^ b ^ r) & 0x10) != 0;
        c.OF = sign<W>((r ^ a) & (r ^ b)); szp<W>(r); return r & mask<W>();
    }
    template <int W> uint32_t sub(uint32_t a, uint32_t b, bool borrow) {
        uint32_t r = a - b - borrow;
        c.CF = (uint32_t)b + borrow > a; c.AF = ((a ^ b ^ r) & 0x10) != 0;
        c.OF = sign<W>((a ^ b) & (a ^ r)); szp<W>(r); return r & mask<W>();
    }
    template <int W> uint32_t logic(uint32_t r) { c.CF = c.OF = false; c.AF = false; szp<W>(r); return r & mask<W>(); }
    template <int W> uint32_t alu(int op, uint32_t a, uint32_t b) {   // 0 add 1 or 2 adc 3 sbb 4 and 5 sub 6 xor 7 cmp
        switch (op) {
        case 0: return add<W>(a, b, false);
        case 1: return logic<W>(a | b);
        case 2: return add<W>(a, b, c.CF);
        case 3: return sub<W>(a, b, c.CF);
        case 4: return logic<W>(a & b);
        case 5: return sub<W>(a, b, false);
        case 6: return logic<W>(a ^ b);
        default: sub<W>(a, b, false); return a;
        }
    }
    template <int W> uint32_t inc(uint32_t a) { bool cf = c.CF; uint32_t r = add<W>(a, 1, false); c.CF = cf; return r; }
    template <int W> uint32_t dec(uint32_t a) { bool cf = c.CF; uint32_t r = sub<W>(a, 1, false); c.CF = cf; return r; }

    template <int W> uint32_t shift(int op, uint32_t v, unsigned n) {   // rol ror rcl rcr shl shr (sal) sar
        if (n == 0) return v;
        cyc += n;
        const uint32_t m = mask<W>();
        for (unsigned i = 0; i < n; i++) {
            bool top = sign<W>(v), bot = v & 1;
            switch (op) {
            case 0: v = ((v << 1) | top) & m; c.CF = top; break;
            case 1: v = (v >> 1) | (bot << (W - 1)); c.CF = bot; break;
            case 2: v = ((v << 1) | c.CF) & m; c.CF = top; break;
            case 3: v = (v >> 1) | ((uint32_t)c.CF << (W - 1)); c.CF = bot; break;
            case 4: case 6: v = (v << 1) & m; c.CF = top; break;
            case 5: v >>= 1; c.CF = bot; break;
            case 7: v = (v >> 1) | (top << (W - 1)); c.CF = bot; break;
            }
        }
        switch (op) {
        case 0: case 2: c.OF = sign<W>(v) != c.CF; break;
        case 1: case 3: c.OF = sign<W>(v ^ (v << 1)); break;
        case 4: case 6: c.OF = sign<W>(v) != c.CF; szp<W>(v); break;
        case 5: c.OF = n == 1 && sign<W>(v << 1); szp<W>(v); break;   // OF = original sign, for a 1-bit shift
        case 7: c.OF = false; szp<W>(v); break;
        }
        return v;
    }

    bool cond(int cc) {
        switch (cc & 15) {
        case 0: return c.OF;            case 1: return !c.OF;
        case 2: return c.CF;            case 3: return !c.CF;
        case 4: return c.ZF;            case 5: return !c.ZF;
        case 6: return c.CF || c.ZF;    case 7: return !c.CF && !c.ZF;
        case 8: return c.SF;            case 9: return !c.SF;
        case 10: return c.PF;           case 11: return !c.PF;
        case 12: return c.SF != c.OF;   case 13: return c.SF == c.OF;
        case 14: return c.ZF || c.SF != c.OF;
        default: return !c.ZF && c.SF == c.OF;
        }
    }

    void jump_near(uint16_t to) { c.ip = to; cyc += 6; }
    void call_near(uint16_t to) {
        push(c.ip); c.ip = to; cyc += 8;
        if (c.on_call) c.on_call(c.sreg[V33::CS], to, false);
    }
    void call_far(uint16_t seg, uint16_t off) {
        push(c.sreg[V33::CS]); push(c.ip); c.sreg[V33::CS] = seg; c.ip = off; cyc += 12;
        if (c.on_call) c.on_call(seg, off, true);
    }

    // ---- string ops ----
    int sd() const { return c.DF ? -1 : 1; }
    // one iteration; returns false for cmps/scas when the REP condition ends the loop
    void string_op(uint8_t op) {
        uint16_t& si = c.r[V33::SI]; uint16_t& di = c.r[V33::DI];
        int w = op & 1, d = sd() * (w ? 2 : 1);
        switch (op) {
        case 0xa4: wb(V33::ES, di, rb(dseg(), si)); si += d; di += d; break;
        case 0xa5: ww(V33::ES, di, rw(dseg(), si)); si += d; di += d; break;
        case 0xa6: sub<8>(rb(dseg(), si), rb(V33::ES, di), false); si += d; di += d; break;
        case 0xa7: sub<16>(rw(dseg(), si), rw(V33::ES, di), false); si += d; di += d; break;
        case 0xaa: wb(V33::ES, di, (uint8_t)c.r[V33::AX]); di += d; break;
        case 0xab: ww(V33::ES, di, c.r[V33::AX]); di += d; break;
        case 0xac: w8(0, rb(dseg(), si)); si += d; break;
        case 0xad: c.r[V33::AX] = rw(dseg(), si); si += d; break;
        case 0xae: sub<8>((uint8_t)c.r[V33::AX], rb(V33::ES, di), false); di += d; break;
        case 0xaf: sub<16>(c.r[V33::AX], rw(V33::ES, di), false); di += d; break;
        case 0x6c: wb(V33::ES, di, c.in8(c.r[V33::DX])); di += d; break;
        case 0x6d: { uint16_t lo = c.in8(c.r[V33::DX]); ww(V33::ES, di, lo | (c.in8(c.r[V33::DX] + 1) << 8)); di += d; break; }
        case 0x6e: c.out8(c.r[V33::DX], rb(dseg(), si)); si += d; break;
        case 0x6f: { uint16_t v = rw(dseg(), si); c.out8(c.r[V33::DX], (uint8_t)v); c.out8(c.r[V33::DX] + 1, v >> 8); si += d; break; }
        }
        cyc += 8;
    }
    void string(uint8_t op) {
        bool compare = op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf;
        if (in.rep == NONE) { string_op(op); return; }
        uint16_t& cx = c.r[V33::CX];
        while (cx) {
            string_op(op);
            cx--;
            if (compare) {
                if (in.rep == REPE && !c.ZF) break;
                if (in.rep == REPNE && c.ZF) break;
            }
            if (in.rep == REPC && !c.CF) break;
            if (in.rep == REPNC && c.CF) break;
        }
    }

    // ---- BCD ----
    void daa() {
        uint8_t al = r8(0), old = al; bool cf = c.CF;
        if ((al & 0x0f) > 9 || c.AF) { al += 6; c.AF = true; } else c.AF = false;
        if (old > 0x99 || cf) { al += 0x60; c.CF = true; } else c.CF = false;
        w8(0, al); szp<8>(al);
    }
    void das() {
        uint8_t al = r8(0), old = al; bool cf = c.CF;
        if ((al & 0x0f) > 9 || c.AF) { al -= 6; c.AF = true; } else c.AF = false;
        if (old > 0x99 || cf) { al -= 0x60; c.CF = true; } else c.CF = false;
        w8(0, al); szp<8>(al);
    }
    void aaa(bool sub_) {
        uint16_t ax = c.r[V33::AX];
        if ((ax & 0x0f) > 9 || c.AF) {
            if (sub_) { ax = (uint16_t)(((ax - 6) & 0xff) | ((ax - 0x100) & 0xff00)); }
            else { ax = (uint16_t)(((ax + 6) & 0xff) | ((ax + 0x100) & 0xff00)); }
            c.AF = c.CF = true;
        } else c.AF = c.CF = false;
        c.r[V33::AX] = ax & 0xff0f;
    }

    // ---- NEC 0F instructions ----
    void nec_0f() {
        uint8_t b = fetch();
        if (b >= 0x10 && b <= 0x1f) {   // TEST1/CLR1/SET1/NOT1 E, CL (10-17) or E, imm (18-1f)
            modrm();
            bool word = b & 1;
            unsigned bit = (b >= 0x18 ? fetch() : r8(1)) & (word ? 15 : 7);
            uint32_t v = word ? getE16() : getE8(), m = 1u << bit;
            switch ((b >> 1) & 3) {
            case 0: c.ZF = !(v & m); c.CF = c.OF = false; return;
            case 1: v &= ~m; break;
            case 2: v |= m; break;
            case 3: v ^= m; break;
            }
            if (word) putE16((uint16_t)v); else putE8((uint8_t)v);
            return;
        }
        switch (b) {
        case 0x20: case 0x22: case 0x26: {   // ADD4S / SUB4S / CMP4S: packed BCD strings, CL digits
            int n = (r8(1) + 1) / 2;
            bool carry = false, zero = true;
            for (int i = 0; i < n; i++) {
                uint8_t s = rb(dseg(), c.r[V33::SI] + i), d = rb(V33::ES, c.r[V33::DI] + i);
                int lo, hi;
                if (b == 0x20) {
                    lo = (d & 15) + (s & 15) + carry; hi = (d >> 4) + (s >> 4);
                    if (lo > 9) { lo -= 10; hi++; }
                    carry = hi > 9; if (carry) hi -= 10;
                } else {
                    lo = (d & 15) - (s & 15) - carry; hi = (d >> 4) - (s >> 4);
                    if (lo < 0) { lo += 10; hi--; }
                    carry = hi < 0; if (carry) hi += 10;
                }
                uint8_t r = (uint8_t)((hi << 4) | lo);
                if (r) zero = false;
                if (b != 0x26) wb(V33::ES, c.r[V33::DI] + i, r);
            }
            c.CF = carry; c.ZF = zero; cyc += 8 * n;
            return;
        }
        case 0x28: case 0x2a: {   // ROL4 / ROR4 Eb: rotate the nibbles of AL(low) and the operand
            modrm();
            uint8_t t = getE8(), al = r8(0);
            if (b == 0x28) { putE8((uint8_t)((t << 4) | (al & 15))); w8(0, (al & 0xf0) | (t >> 4)); }
            else { putE8((uint8_t)((al << 4) | (t >> 4))); w8(0, (al & 0xf0) | (t & 15)); }
            return;
        }
        case 0x31: case 0x39: case 0x33: case 0x3b: {   // INS / EXT: bit fields at ES:DI / DS:SI
            modrm();
            int offreg = in.rm;
            unsigned len = ((b & 8) ? fetch() : r8(in.reg)) & 15;
            unsigned off = r8(offreg) & 15;
            len += 1;
            uint32_t m = ((1u << len) - 1) << off;
            if (b == 0x31 || b == 0x39) {
                uint32_t w = rw(V33::ES, c.r[V33::DI]) | ((uint32_t)rw(V33::ES, c.r[V33::DI] + 2) << 16);
                w = (w & ~m) | (((uint32_t)c.r[V33::AX] << off) & m);
                ww(V33::ES, c.r[V33::DI], (uint16_t)w); ww(V33::ES, c.r[V33::DI] + 2, w >> 16);
                off += len; if (off > 15) { off -= 16; c.r[V33::DI] += 2; }
            } else {
                uint32_t w = rw(dseg(), c.r[V33::SI]) | ((uint32_t)rw(dseg(), c.r[V33::SI] + 2) << 16);
                c.r[V33::AX] = (uint16_t)((w & m) >> off);
                off += len; if (off > 15) { off -= 16; c.r[V33::SI] += 2; }
            }
            w8(offreg, (uint8_t)off);
            cyc += 20;
            return;
        }
        }
        if (b == 0x92 && c.fint) { c.fint(); return; }   // V35 FINT
        c.interrupt(6);   // unknown: treat as an invalid opcode
    }

    // ---- MUL/DIV group (F6/F7) ----
    template <int W> void grp3() {
        uint32_t v = W == 8 ? getE8() : getE16();
        switch (in.reg) {
        case 0: case 1: logic<W>(v & (W == 8 ? fetch() : fetch16())); return;
        case 2: if (W == 8) putE8((uint8_t)~v); else putE16((uint16_t)~v); return;
        case 3: { uint32_t r = sub<W>(0, v, false); c.CF = v != 0; if (W == 8) putE8((uint8_t)r); else putE16((uint16_t)r); return; }
        case 4:
            cyc += W == 8 ? 12 : 20;
            if (W == 8) { uint16_t r = (uint16_t)(r8(0) * v); c.r[V33::AX] = r; c.CF = c.OF = (r >> 8) != 0; }
            else { uint32_t r = (uint32_t)c.r[V33::AX] * v; c.r[V33::AX] = (uint16_t)r; c.r[V33::DX] = r >> 16; c.CF = c.OF = (r >> 16) != 0; }
            c.ZF = W == 8 ? (c.r[V33::AX] & 0xffff) == 0 : (c.r[V33::AX] | c.r[V33::DX]) == 0;
            return;
        case 5:
            cyc += W == 8 ? 14 : 22;
            if (W == 8) { int16_t r = (int16_t)((int8_t)r8(0) * (int8_t)v); c.r[V33::AX] = (uint16_t)r; c.CF = c.OF = r != (int8_t)r; }
            else { int32_t r = (int32_t)(int16_t)c.r[V33::AX] * (int16_t)v; c.r[V33::AX] = (uint16_t)r; c.r[V33::DX] = (uint16_t)(r >> 16); c.CF = c.OF = r != (int16_t)r; }
            return;
        case 6:
            cyc += W == 8 ? 16 : 24;
            if (v == 0) { c.interrupt(0); return; }
            if (W == 8) {
                uint16_t ax = c.r[V33::AX]; uint32_t q = ax / v;
                if (q > 0xff) { c.interrupt(0); return; }
                c.r[V33::AX] = (uint16_t)(((ax % v) << 8) | q);
            } else {
                uint32_t n = ((uint32_t)c.r[V33::DX] << 16) | c.r[V33::AX]; uint32_t q = n / v;
                if (q > 0xffff) { c.interrupt(0); return; }
                c.r[V33::AX] = (uint16_t)q; c.r[V33::DX] = (uint16_t)(n % v);
            }
            return;
        case 7:
            cyc += W == 8 ? 18 : 26;
            if (v == 0) { c.interrupt(0); return; }
            if (W == 8) {
                int16_t n = (int16_t)c.r[V33::AX]; int d = (int8_t)v; int q = n / d;
                if (q > 127 || q < -128) { c.interrupt(0); return; }
                c.r[V33::AX] = (uint16_t)((((n % d) & 0xff) << 8) | (q & 0xff));
            } else {
                int32_t n = (int32_t)(((uint32_t)c.r[V33::DX] << 16) | c.r[V33::AX]); int d = (int16_t)v;
                int32_t q = n / d;
                if (q > 32767 || q < -32768) { c.interrupt(0); return; }
                c.r[V33::AX] = (uint16_t)q; c.r[V33::DX] = (uint16_t)(n % d);
            }
            return;
        }
    }

    void exec(uint8_t op) {
        switch (op) {
        // ALU 00-3d (except the segment/BCD slots)
        case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: {
            modrm(); uint32_t r = alu<8>(op >> 3, getE8(), r8(in.reg)); if ((op >> 3) != 7) putE8((uint8_t)r); return; }
        case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39: {
            modrm(); uint32_t r = alu<16>(op >> 3, getE16(), c.r[in.reg]); if ((op >> 3) != 7) putE16((uint16_t)r); return; }
        case 0x02: case 0x0a: case 0x12: case 0x1a: case 0x22: case 0x2a: case 0x32: case 0x3a: {
            modrm(); uint32_t r = alu<8>(op >> 3, r8(in.reg), getE8()); if ((op >> 3) != 7) w8(in.reg, (uint8_t)r); return; }
        case 0x03: case 0x0b: case 0x13: case 0x1b: case 0x23: case 0x2b: case 0x33: case 0x3b: {
            modrm(); uint32_t r = alu<16>(op >> 3, c.r[in.reg], getE16()); if ((op >> 3) != 7) c.r[in.reg] = (uint16_t)r; return; }
        case 0x04: case 0x0c: case 0x14: case 0x1c: case 0x24: case 0x2c: case 0x34: case 0x3c: {
            uint32_t r = alu<8>(op >> 3, r8(0), fetch()); if ((op >> 3) != 7) w8(0, (uint8_t)r); return; }
        case 0x05: case 0x0d: case 0x15: case 0x1d: case 0x25: case 0x2d: case 0x35: case 0x3d: {
            uint32_t r = alu<16>(op >> 3, c.r[V33::AX], fetch16()); if ((op >> 3) != 7) c.r[V33::AX] = (uint16_t)r; return; }
        case 0x06: push(c.sreg[V33::ES]); return;
        case 0x07: c.sreg[V33::ES] = pop(); return;
        case 0x0e: push(c.sreg[V33::CS]); return;
        case 0x0f: nec_0f(); return;
        case 0x16: push(c.sreg[V33::SS]); return;
        case 0x17: c.sreg[V33::SS] = pop(); return;
        case 0x1e: push(c.sreg[V33::DS]); return;
        case 0x1f: c.sreg[V33::DS] = pop(); return;
        case 0x27: daa(); return;
        case 0x2f: das(); return;
        case 0x37: aaa(false); return;
        case 0x3f: aaa(true); return;
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
            c.r[op & 7] = (uint16_t)inc<16>(c.r[op & 7]); return;
        case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
            c.r[op & 7] = (uint16_t)dec<16>(c.r[op & 7]); return;
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
            if ((op & 7) == V33::SP) { uint16_t sp = c.r[V33::SP] - 2; push(sp); } else push(c.r[op & 7]);
            return;
        case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
            c.r[op & 7] = pop(); return;
        case 0x60: {
            uint16_t sp = c.r[V33::SP];
            for (int i = 0; i < 8; i++) push(i == V33::SP ? sp : c.r[i]);
            cyc += 30; return;
        }
        case 0x61:
            for (int i = 7; i >= 0; i--) { uint16_t v = pop(); if (i != V33::SP) c.r[i] = v; }
            cyc += 30; return;
        case 0x62: {   // BOUND
            modrm();
            int16_t lo = (int16_t)rw(in.ea_seg, in.ea_off), hi = (int16_t)rw(in.ea_seg, in.ea_off + 2), v = (int16_t)c.r[in.reg];
            if (v < lo || v > hi) c.interrupt(5);
            return;
        }
        case 0x68: push(fetch16()); return;
        case 0x69: case 0x6b: {
            modrm();
            int32_t a = (int16_t)getE16(), b = op == 0x69 ? (int16_t)fetch16() : (int8_t)fetch();
            int32_t r = a * b; c.r[in.reg] = (uint16_t)r; c.CF = c.OF = r != (int16_t)r; cyc += 20;
            return;
        }
        case 0x6a: push((uint16_t)(int8_t)fetch()); return;
        case 0x6c: case 0x6d: case 0x6e: case 0x6f: string(op); return;
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f: {
            int8_t d = (int8_t)fetch(); if (cond(op)) jump_near(c.ip + d); return; }
        case 0x80: case 0x82: { modrm(); uint8_t v = fetch(); uint32_t r = alu<8>(in.reg, getE8(), v); if (in.reg != 7) putE8((uint8_t)r); return; }
        case 0x81: { modrm(); uint16_t v = fetch16(); uint32_t r = alu<16>(in.reg, getE16(), v); if (in.reg != 7) putE16((uint16_t)r); return; }
        case 0x83: { modrm(); uint16_t v = (uint16_t)(int8_t)fetch(); uint32_t r = alu<16>(in.reg, getE16(), v); if (in.reg != 7) putE16((uint16_t)r); return; }
        case 0x84: modrm(); logic<8>(getE8() & r8(in.reg)); return;
        case 0x85: modrm(); logic<16>(getE16() & c.r[in.reg]); return;
        case 0x86: { modrm(); uint8_t t = getE8(); putE8(r8(in.reg)); w8(in.reg, t); return; }
        case 0x87: { modrm(); uint16_t t = getE16(); putE16(c.r[in.reg]); c.r[in.reg] = t; return; }
        case 0x88: modrm(); putE8(r8(in.reg)); return;
        case 0x89: modrm(); putE16(c.r[in.reg]); return;
        case 0x8a: modrm(); w8(in.reg, getE8()); return;
        case 0x8b: modrm(); c.r[in.reg] = getE16(); return;
        case 0x8c: modrm(); putE16(c.sreg[in.reg & 3]); return;
        case 0x8d: modrm(); c.r[in.reg] = in.ea_off; return;
        case 0x8e: modrm(); c.sreg[in.reg & 3] = getE16(); return;
        case 0x8f: { modrm(); uint16_t v = pop(); putE16(v); return; }
        case 0x90: return;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            uint16_t t = c.r[op & 7]; c.r[op & 7] = c.r[V33::AX]; c.r[V33::AX] = t; return; }
        case 0x98: c.r[V33::AX] = (uint16_t)(int8_t)r8(0); return;
        case 0x99: c.r[V33::DX] = (c.r[V33::AX] & 0x8000) ? 0xffff : 0; return;
        case 0x9a: { uint16_t off = fetch16(), seg = fetch16(); call_far(seg, off); return; }
        case 0x9b: return;   // POLL
        case 0x9c: push(c.flags()); return;
        case 0x9d: c.set_flags(pop()); return;
        case 0x9e: { uint8_t ah = r8(4); c.set_flags((c.flags() & 0xff00) | ah); return; }
        case 0x9f: w8(4, (uint8_t)c.flags()); return;
        case 0xa0: w8(0, rb(dseg(), fetch16())); return;
        case 0xa1: c.r[V33::AX] = rw(dseg(), fetch16()); return;
        case 0xa2: wb(dseg(), fetch16(), r8(0)); return;
        case 0xa3: ww(dseg(), fetch16(), c.r[V33::AX]); return;
        case 0xa4: case 0xa5: case 0xa6: case 0xa7: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
            string(op); return;
        case 0xa8: logic<8>(r8(0) & fetch()); return;
        case 0xa9: logic<16>(c.r[V33::AX] & fetch16()); return;
        case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7: w8(op & 7, fetch()); return;
        case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf: c.r[op & 7] = fetch16(); return;
        case 0xc0: { modrm(); unsigned n = fetch(); putE8((uint8_t)shift<8>(in.reg, getE8(), n)); return; }
        case 0xc1: { modrm(); unsigned n = fetch(); putE16((uint16_t)shift<16>(in.reg, getE16(), n)); return; }
        case 0xc2: { uint16_t n = fetch16(); c.ip = pop(); c.r[V33::SP] += n; cyc += 8; return; }
        case 0xc3: c.ip = pop(); cyc += 8; return;
        case 0xc4: case 0xc5: {
            modrm();
            c.r[in.reg] = rw(in.ea_seg, in.ea_off);
            c.sreg[op == 0xc4 ? V33::ES : V33::DS] = rw(in.ea_seg, in.ea_off + 2);
            return;
        }
        case 0xc6: modrm(); putE8(fetch()); return;
        case 0xc7: modrm(); putE16(fetch16()); return;
        case 0xc8: {   // ENTER
            uint16_t size = fetch16(); uint8_t level = fetch() & 31;
            push(c.r[V33::BP]);
            uint16_t frame = c.r[V33::SP];
            for (int i = 1; i < level; i++) { c.r[V33::BP] -= 2; push(rw(V33::SS, c.r[V33::BP])); }
            if (level) push(frame);
            c.r[V33::BP] = frame; c.r[V33::SP] -= size; cyc += 16;
            return;
        }
        case 0xc9: c.r[V33::SP] = c.r[V33::BP]; c.r[V33::BP] = pop(); return;
        case 0xca: { uint16_t n = fetch16(); c.ip = pop(); c.sreg[V33::CS] = pop(); c.r[V33::SP] += n; cyc += 12; return; }
        case 0xcb: c.ip = pop(); c.sreg[V33::CS] = pop(); cyc += 12; return;
        case 0xcc: c.interrupt(3); return;
        case 0xcd: c.interrupt(fetch()); return;
        case 0xce: if (c.OF) c.interrupt(4); return;
        case 0xcf: c.ip = pop(); c.sreg[V33::CS] = pop(); c.set_flags(pop()); cyc += 12; return;
        case 0xd0: modrm(); putE8((uint8_t)shift<8>(in.reg, getE8(), 1)); return;
        case 0xd1: modrm(); putE16((uint16_t)shift<16>(in.reg, getE16(), 1)); return;
        case 0xd2: modrm(); putE8((uint8_t)shift<8>(in.reg, getE8(), r8(1))); return;
        case 0xd3: modrm(); putE16((uint16_t)shift<16>(in.reg, getE16(), r8(1))); return;
        case 0xd4: { fetch(); uint8_t al = r8(0); w8(4, al / 10); w8(0, al % 10); szp<16>(c.r[V33::AX]); cyc += 15; return; }   // NEC: base 10
        case 0xd5: { fetch(); uint8_t al = (uint8_t)(r8(4) * 10 + r8(0)); c.r[V33::AX] = al; szp<8>(al); cyc += 7; return; }
        case 0xd6: w8(0, c.CF ? 0xff : 0); return;   // SALC (undocumented)
        case 0xd7: w8(0, rb(dseg(), c.r[V33::BX] + r8(0))); return;
        case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf: modrm(); return;   // FPU escape: no coprocessor
        case 0xe0: case 0xe1: case 0xe2: {
            int8_t d = (int8_t)fetch(); uint16_t cx = --c.r[V33::CX];
            bool go = cx != 0 && (op == 0xe2 || (op == 0xe1 ? c.ZF : !c.ZF));
            if (go) jump_near(c.ip + d);
            return;
        }
        case 0xe3: { int8_t d = (int8_t)fetch(); if (!c.r[V33::CX]) jump_near(c.ip + d); return; }
        case 0xe4: w8(0, c.in8(fetch())); return;
        case 0xe5: { uint8_t p = fetch(); c.r[V33::AX] = c.in8(p) | (c.in8(p + 1) << 8); return; }
        case 0xe6: c.out8(fetch(), r8(0)); return;
        case 0xe7: { uint8_t p = fetch(); c.out8(p, (uint8_t)c.r[V33::AX]); c.out8(p + 1, c.r[V33::AX] >> 8); return; }
        case 0xe8: { int16_t d = (int16_t)fetch16(); call_near(c.ip + d); return; }
        case 0xe9: { int16_t d = (int16_t)fetch16(); jump_near(c.ip + d); return; }
        case 0xea: { uint16_t off = fetch16(), seg = fetch16(); c.sreg[V33::CS] = seg; c.ip = off; cyc += 8; return; }
        case 0xeb: { int8_t d = (int8_t)fetch(); jump_near(c.ip + d); return; }
        case 0xec: w8(0, c.in8(c.r[V33::DX])); return;
        case 0xed: c.r[V33::AX] = c.in8(c.r[V33::DX]) | (c.in8(c.r[V33::DX] + 1) << 8); return;
        case 0xee: c.out8(c.r[V33::DX], r8(0)); return;
        case 0xef: c.out8(c.r[V33::DX], (uint8_t)c.r[V33::AX]); c.out8(c.r[V33::DX] + 1, c.r[V33::AX] >> 8); return;
        case 0xf4: c.halted = true; return;
        case 0xf5: c.CF = !c.CF; return;
        case 0xf6: modrm(); grp3<8>(); if (in.reg >= 4) return; return;
        case 0xf7: modrm(); grp3<16>(); return;
        case 0xf8: c.CF = false; return;
        case 0xf9: c.CF = true; return;
        case 0xfa: c.IF = false; return;
        case 0xfb: c.IF = true; return;
        case 0xfc: c.DF = false; return;
        case 0xfd: c.DF = true; return;
        case 0xfe:
            modrm();
            if (in.reg == 0) putE8((uint8_t)inc<8>(getE8()));
            else if (in.reg == 1) putE8((uint8_t)dec<8>(getE8()));
            else c.interrupt(6);
            return;
        case 0xff:
            modrm();
            switch (in.reg) {
            case 0: putE16((uint16_t)inc<16>(getE16())); return;
            case 1: putE16((uint16_t)dec<16>(getE16())); return;
            case 2: call_near(getE16()); return;
            case 3: call_far(rw(in.ea_seg, in.ea_off + 2), rw(in.ea_seg, in.ea_off)); return;
            case 4: { uint16_t t = getE16(); if (c.on_jump) c.on_jump(c.sreg[V33::CS], t, false); jump_near(t); return; }
            case 5: {
                uint16_t off = rw(in.ea_seg, in.ea_off), seg = rw(in.ea_seg, in.ea_off + 2);
                if (c.on_jump) c.on_jump(seg, off, true);
                c.sreg[V33::CS] = seg; c.ip = off; cyc += 8; return;
            }
            case 6: push(getE16()); return;
            default: c.interrupt(6); return;
            }
        default:
            c.interrupt(6);   // invalid opcode
            return;
        }
    }
};
}  // namespace v33x
