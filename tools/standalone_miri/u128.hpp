#pragma once

#if 1
#include "../include/int128.h"
typedef S128    I128;
#else
class U128
{
    friend class I128;
    uint64_t    lo, hi;
public:
    U128(): lo(0), hi(0) {}

    explicit U128(uint64_t v): lo(v), hi(0) {}
    U128(uint8_t v): lo(v), hi(0) {}
    U128(int8_t v): lo(v), hi(v < 0 ? -1 : 0) {}

    void fmt(::std::ostream& os) const { os << hi << ":" << lo; }

    int cmp(U128 v) const {
        if( hi != v.hi ) {
            return (hi < v.hi ? -1 : 1);
        }
        if( lo != v.lo ) {
            return (lo < v.lo ? -1 : 1);
        }
        return 0;
    }
    int cmp(unsigned v) const {
        if(hi)
            return 1;
        if(lo < v)
            return -1;
        if(lo > v)
            return 1;
        return 0;
    }

    template<typename T> bool operator< (const T& v) const { return this->cmp(v) <  0; }
    template<typename T> bool operator<=(const T& v) const { return this->cmp(v) <= 0; }
    template<typename T> bool operator> (const T& v) const { return this->cmp(v) >  0; }
    template<typename T> bool operator>=(const T& v) const { return this->cmp(v) >= 0; }
    template<typename T> bool operator==(const T& v) const { return this->cmp(v) == 0; }
    template<typename T> bool operator!=(const T& v) const { return this->cmp(v) != 0; }

    operator uint8_t() const { return static_cast<uint8_t>(lo); }

    int operator&(int x) const {
        return this->lo & x;
    }
    unsigned operator&(unsigned x) const {
        return this->lo & x;
    }
    U128 operator&(U128 x) const {
        U128    rv;
        rv.lo = this->lo & x.lo;
        rv.hi = this->hi & x.hi;
        return rv;
    }
    U128 operator|(U128 x) const {
        U128    rv;
        rv.lo = this->lo | x.lo;
        rv.hi = this->hi | x.hi;
        return rv;
    }
    U128 operator^(U128 x) const {
        U128    rv;
        rv.lo = this->lo ^ x.lo;
        rv.hi = this->hi ^ x.hi;
        return rv;
    }

    U128 operator<<(U128 n) const
    {
        if( n < 128 )
        {
            return *this << static_cast<uint8_t>(n);
        }
        else
        {
            return U128();
        }
    }
    U128 operator<<(uint8_t n) const
    {
        if(n == 0)
        {
            return *this;
        }
        else if( n < 64 )
        {
            U128    rv;
            rv.lo = lo << n;
            rv.hi = (hi << n) | (lo >> (64-n));
            return rv;
        }
        else if( n < 128 )
        {
            U128    rv;
            rv.lo = 0;
            rv.hi = (lo << (n-64));
            return rv;
        }
        else
        {
            return U128();
        }
    }
    U128 operator>>(uint8_t n) const
    {
        if(n == 0)
        {
            return *this;
        }
        else if( n < 64 )
        {
            U128    rv;
            rv.lo = (lo >> n) | (hi << (64-n));
            rv.hi = (hi >> n);
            return rv;
        }
        else if( n < 128 )
        {
            U128    rv;
            rv.lo = (hi >> (n-64));
            rv.hi = 0;
            return rv;
        }
        else
        {
            return U128();
        }
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const U128& x) { x.fmt(os); return os; }
};

class I128
{
    U128    v;
public:
    I128() {}

    int cmp(I128 x) const {
        if(v.hi != x.v.hi)
            return (static_cast<int64_t>(v.hi) < static_cast<int64_t>(x.v.hi) ? -1 : 1);
        if(v.lo != x.v.lo)
        {
            if( static_cast<int64_t>(v.hi) < 0 )
            {
                // Negative, so larger raw value is the smaller
                return (v.lo > x.v.lo ? -1 : 1);
            }
            else
            {
                return (v.lo < x.v.lo ? -1 : 1);
            }
        }
        return 0;
    }
    //int cmp(int v) const {
    //    if(hi)
    //        return 1;
    //    if(lo < v)
    //        return -1;
    //    if(lo > v)
    //        return 1;
    //    return 0;
    //}

    template<typename T> bool operator< (const T& v) const { return this->cmp(v) <  0; }
    template<typename T> bool operator<=(const T& v) const { return this->cmp(v) <= 0; }
    template<typename T> bool operator> (const T& v) const { return this->cmp(v) >  0; }
    template<typename T> bool operator>=(const T& v) const { return this->cmp(v) >= 0; }
    template<typename T> bool operator==(const T& v) const { return this->cmp(v) == 0; }
    template<typename T> bool operator!=(const T& v) const { return this->cmp(v) != 0; }

    void fmt(::std::ostream& os) const { os << v.hi << ":" << v.lo; }

    friend ::std::ostream& operator<<(::std::ostream& os, const I128& x) { x.fmt(os); return os; }
};

#endif