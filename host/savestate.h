// Save states: one stream type for both directions, so each module lists its state once.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

struct State {
    std::vector<uint8_t>* out = nullptr;   // saving
    const uint8_t* in = nullptr;           // loading
    size_t left = 0;
    bool ok = true;
    void raw(void* p, size_t n) {
        if (out) { auto* b = static_cast<uint8_t*>(p); out->insert(out->end(), b, b + n); return; }
        if (n > left) { ok = false; return; }
        std::memcpy(p, in, n); in += n; left -= n;
    }
    template <class T> void pod(T& v) { raw(&v, sizeof v); }
};

template <class CPU> inline void cpu_state(State& st, CPU& c) {
    st.raw(c.r, sizeof c.r); st.raw(c.sreg, sizeof c.sreg); st.pod(c.ip);
    st.pod(c.CF); st.pod(c.PF); st.pod(c.AF); st.pod(c.ZF); st.pod(c.SF); st.pod(c.OF); st.pod(c.DF); st.pod(c.IF); st.pod(c.TF);
    st.pod(c.halted); st.pod(c.irq_line); st.pod(c.nmi_pending); st.pod(c.pending_int); st.pod(c.slice); st.pod(c.cycles);
}
