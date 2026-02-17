#pragma once
#include <cstdint>

struct F16
{
    // 1.5.10
    uint16_t v;

    F16(): v(0) {}

    F16(float f)
    {
        union {
            // 1.8.23
            float f;
            uint32_t    i;
        } c;
        c.f = f;
        auto sign_exp = (c.i >> 23) >> 3;
        auto mantissa = c.i >> (23 - 10);
        this->v = (sign_exp << 10) | mantissa;
    }
    operator float() const {
        auto sign_exp = v >> 10;
        auto mantissa = v & ((1 << 10)-1);
        auto sign_ext_f = (sign_exp << 3) | ((sign_exp & 1) == 1 ? 0x7 : 0);
        auto mantissa_f = (mantissa << (23 - 10)) | ((mantissa & 1) == 1 ? (1 << (23 - 10))-1 : 0);
        union {
            float f;
            uint32_t    i;
        } dc;
        dc.i = (sign_ext_f << 23) | mantissa_f;
        return dc.f;
    }
};

struct F128
{
    // 1.15.112
    uint64_t lo;
    uint64_t hi;

    F128(): lo(0), hi(0) {}

    F128(double v)
    {
        typedef union {
            double f;
            // 1.11.52
            uint64_t    i;
        } double_cast;
        double_cast dc;
        dc.f = v;
        auto exp_sign_d = dc.i >> 52;
        // Trailing extend the exponent, so max stays max and min stays min
        auto exp_sign_q = exp_sign_d << (15-11) | ((exp_sign_d & 1) == 1 ? 0xF : 0);
        auto mantissa_d = dc.i & ((1LL << 52)-1);
        auto mantissa_qh = mantissa_d >> 4; // 4 bits of extra exponent
        auto mantissa_ql = (mantissa_d & 0xF) << 60;    // Those lost 4 bits
        // Fill the tail of the mantissa with the final bit (so INF stays INF, and doesn't become a NaN)
        if( mantissa_d & 1 ) {
            mantissa_ql |= (1LL << 60) - 1;
        }
        this->lo = mantissa_ql;
        this->hi = (exp_sign_q << (112-64)) | mantissa_qh;
    }
    operator double() const {
        auto exp_sign_q = hi >> (112-64);
        auto mantissa_d = (hi & ((1LL << (112-64)) - 1)) | (lo >> 60);
        auto exp_sign_d = exp_sign_q >> 4;
        union {
            double  f;
            uint64_t    i;
        } dc;
        dc.i = (exp_sign_d << 52) | mantissa_d;
        return dc.f;
    }
};