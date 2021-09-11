/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/int128.cpp
 * - Compiler-agnostic 128-bit integers
 */
#pragma once

class U128
{
    friend class S128;
    uint64_t    lo;
    uint64_t    hi;
public:
    U128(uint64_t lo, uint64_t hi=0)
        : lo(lo)
        , hi(hi)
    {
    }

    /// Cast operator
    operator uint64_t() { return lo; }

    U128 operator~() const { return U128(~lo, ~hi); }
    U128 operator+(unsigned x) const { U128 rv(0); add128_o(*this, x, &rv); return rv; }
    U128 operator+(U128 x) const { U128 rv(0); add128_o(*this, x, &rv); return rv; }
    U128 operator-(U128 x) const { U128 rv(0); sub128_o(*this, x, &rv); return rv; }
    U128 operator*(U128 x) const { U128 rv(0); mul128_o(*this, x, &rv); return rv; }
    U128 operator/(U128 x) const { U128 rv(0); div128_o(*this, x, &rv, nullptr); return rv; }
    U128 operator%(U128 x) const { U128 rv(0); div128_o(*this, x, nullptr, &rv); return rv; }

    U128 operator<<(unsigned bits) const {
        if(bits == 0)
            return *this;
        if(bits > 128)
            return U128(0);
        if(bits >= 64)
           return U128(0, lo << (bits-64));
        return U128(lo << bits, (hi << bits) | (lo >> (64-bits)));
    }
    U128 operator>>(unsigned bits) const {
        if(bits == 0)
            return *this;
        if(bits > 128)
            return U128(0);
        if(bits >= 64)
           return U128(hi >> (bits-64), 0);
        return U128(lo >> bits | (hi << (64-bits)), hi >> bits);
    }

    bool operator< (const U128& x) const { return cmp128(*this, x) <  0; }
    bool operator<=(const U128& x) const { return cmp128(*this, x) <= 0; }
    bool operator> (const U128& x) const { return cmp128(*this, x) >  0; }
    bool operator>=(const U128& x) const { return cmp128(*this, x) >= 0; }
    bool operator==(const U128& x) const { return cmp128(*this, x) == 0; }
    bool operator!=(const U128& x) const { return cmp128(*this, x) != 0; }

    bool bit(unsigned idx) const {
        if(idx <  64) return ((lo >> idx) & 1) != 0;
        if(idx < 128) return ((hi >> (idx - 64)) & 1) != 0;
        return false;
    }
private:
    // TODO: All of these are functionally identical to code in `codegen_c.cpp` - could it be shared?
    static int cmp128(U128 a, U128 b) { if(a.hi != b.hi) return a.hi < b.hi ? -1 : 1; if(a.lo != b.lo) return a.lo < b.lo ? -1 : 1; return 0; }
    static bool add128_o(U128 a, U128 b, U128* o) { o->lo = a.lo + b.lo; o->hi = a.hi + b.hi + (o->lo < a.lo ? 1 : 0); return (o->hi < a.hi); }
    static bool sub128_o(U128 a, U128 b, U128* o) { o->lo = a.lo - b.lo; o->hi = a.hi - b.hi - (o->lo > a.lo ? 1 : 0); return (o->hi > a.hi); }
    static bool mul128_o(U128 a, U128 b, U128* o) {
        bool of = false;
        o->hi = 0; o->lo = 0;
        for(int i=0;i<128;i++)
        {
            uint64_t m = (1ull << (i % 64));
            if(a.hi==0&&a.lo<m)   break;
            if(i>=64&&a.hi<m) break;
            if( m & (i >= 64 ? a.hi : a.lo) ) of |= add128_o(*o, b, o);
            b.hi = (b.hi << 1) | (b.lo >> 63);
            b.lo = (b.lo << 1);
        }
        return of;
    }
    // Long division
    static inline bool div128_o(U128 a, U128 b, U128* q, U128* r) {
        if(a.hi == 0 && b.hi == 0) {
            if(q) { q->hi=0; q->lo = a.lo / b.lo; }
            if(r) { r->hi=0; r->lo = a.lo % b.lo; }
            return false;
        }
        if(cmp128(a, b) < 0) {
            if(q) { q->hi=0; q->lo=0; }
            if(r) *r = a;
            return false;
        }
        U128 a_div_2( (a.lo>>1)|(a.hi << 63), a.hi>>1 );
        int shift = 0;
        while( cmp128(a_div_2, b) >= 0 && shift < 128 ) {
            shift += 1;
            b.hi = (b.hi<<1)|(b.lo>>63); b.lo <<= 1;
        }
        if(shift == 128) return true;   // true = overflowed
        U128 mask( /*lo=*/(shift >= 64 ? 0 : (1ull << shift)), /*hi=*/(shift < 64 ? 0 : 1ull << (shift-64)) );
        shift ++;
        if(q) { q->hi = 0; q->lo = 0; }
        while(shift--) {
            if( cmp128(a, b) >= 0 ) { if(q) add128_o(*q, mask, q); sub128_o(a, b, &a); }
            mask.lo = (mask.lo >> 1) | (mask.hi << 63); mask.hi >>= 1;
            b.lo = (b.lo >> 1) | (b.hi << 63); b.hi >>= 1;
        }
        if(r) *r = a;
        return false;
    }
};
class S128
{
    U128    inner;
public:
    S128(int64_t v):    inner(v, v < 0 ? UINT64_MAX : 0) {}
    S128(U128 v): inner(v) {}

    /// Cast operator
    operator int64_t() { /*assert(inner.hi == 0 || inner.hi == UINT64_MAX);*/ return inner.lo; }

    S128 operator~() const { return S128(~inner); }
    S128 operator-() const { return S128(~inner) + S128(1); }
    S128 operator+(S128 x) const { return S128(inner + x.inner); }
    S128 operator-(S128 x) const { return S128(inner - x.inner); }
    S128 operator*(S128 x) const {
        auto ret_neg = is_neg() != x.is_neg();
        auto rv_u = u_abs() * x.u_abs();
        return ret_neg ? -S128(rv_u) : S128(rv_u);
    }
    S128 operator/(S128 x) const {
        auto ret_neg = is_neg() != x.is_neg();
        auto rv_u = u_abs() / x.u_abs();
        return ret_neg ? -S128(rv_u) : S128(rv_u);
    }

    bool is_neg() const { return static_cast<uint64_t>(inner >> 127) != 0; }
    /// Unsigned absolute value (handles MIN correctly)
    U128 u_abs() const { if(inner.hi == UINT64_MAX && inner.lo == 0) return inner; if(is_neg()) return (-*this).inner; else return (*this).inner; }

    bool operator< (const S128& x) const { return cmp128s(this->inner, x.inner) <  0; }
    bool operator<=(const S128& x) const { return cmp128s(this->inner, x.inner) <= 0; }
    bool operator> (const S128& x) const { return cmp128s(this->inner, x.inner) >  0; }
    bool operator>=(const S128& x) const { return cmp128s(this->inner, x.inner) >= 0; }
    bool operator==(const S128& x) const { return cmp128s(this->inner, x.inner) == 0; }
    bool operator!=(const S128& x) const { return cmp128s(this->inner, x.inner) != 0; }

private:
    static inline int cmp128s(U128 a, U128 b) { if(a.hi != b.hi) return (int64_t)a.hi < (int64_t)b.hi ? -1 : 1; if(a.lo != b.lo) return a.lo < b.lo ? -1 : 1; return 0; }
};

