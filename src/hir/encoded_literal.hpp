//
//
//
#pragma once
#include "../common.hpp"

struct Reloc {
    size_t  ofs;
    size_t  len;
    ::std::unique_ptr<::HIR::Path>  p;
    ::std::string   bytes;

    static Reloc new_named(size_t ofs, size_t len, ::HIR::Path p) {
        return Reloc { ofs, len, box$(p), "" };
    }
    static Reloc new_bytes(size_t ofs, size_t len, ::std::string bytes) {
        return Reloc { ofs, len, nullptr, ::std::move(bytes) };
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const Reloc& x) {
        os << "@" << std::hex << "0x" << x.ofs << std::dec << "+" << x.len << " = ";
        if(x.p) {
            os << "&" << *x.p;
        }
        else {
            os << "\"" << FmtEscaped(x.bytes) << "\"";
        }
        return os;
    }
    Ordering ord(const Reloc& x) const {
        ORD(ofs, x.ofs);
        ORD(len, x.len);
        if( p ) {
            if( !x.p )
                return OrdLess;
            return p->ord(*x.p);
        }
        else {
            if( x.p )
                return OrdGreater;
            return ::ord(bytes, x.bytes);
        }
    }
    bool operator==(const Reloc& x) const {
        return ord(x) == OrdEqual;
    }
};
struct EncodedLiteral {
    static const unsigned PTR_BASE = 0x1000;

    std::vector<uint8_t>    bytes;
    std::vector<Reloc>  relocations;

    static EncodedLiteral make_usize(uint64_t v);
    EncodedLiteral clone() const;

    void write_uint(size_t ofs, size_t size,  uint64_t v);

    void write_usize(size_t ofs,  uint64_t v);
    uint64_t read_usize(size_t ofs) const;

    friend ::std::ostream& operator<<(std::ostream& os, const EncodedLiteral& x) {
        for(size_t i = 0; i < x.bytes.size(); i++)
        {
            const char* HEX = "0123456789ABCDEF";
            os << HEX[x.bytes[i]>>4] << HEX[x.bytes[i]&0xF];
            if( (i+1)%8 == 0 && i + 1 < x.bytes.size() ) {
                os << " ";
            }
        }
        os << "{" << x.relocations << "}";
        return os;
    }

    Ordering ord(const EncodedLiteral& x) const {
        ORD(bytes, x.bytes);
        ORD(relocations, x.relocations);
        return OrdEqual;
    }
    bool operator==(const EncodedLiteral& x) const { return ord(x) == OrdEqual; }
};

struct EncodedLiteralSlice
{
    const EncodedLiteral& m_base;
    size_t  m_ofs;
    size_t  m_size;
    //size_t  m_reloc_ofs;
    //size_t  m_reloc_size;

    EncodedLiteralSlice(const EncodedLiteral& base)
        : m_base(base)
        , m_ofs(0)
        , m_size(base.bytes.size())
        //, m_reloc_ofs(0)
        //, m_reloc_size(base.relocations.size())
    {
    }

    EncodedLiteralSlice slice(size_t ofs) const {
        assert(ofs <= m_size);
        return slice(ofs, m_size - ofs);
    }
    EncodedLiteralSlice slice(size_t ofs, size_t len) const {
        assert(ofs <= m_size);
        assert(len <= m_size);
        assert(ofs+len <= m_size);
        auto rv = EncodedLiteralSlice(m_base);
        rv.m_ofs = m_ofs + ofs;
        rv.m_size = len;
        return rv;
    }

    uint64_t read_uint(size_t size=0) const;
    int64_t read_sint(size_t size=0) const;
    double read_float(size_t size=0) const;
    const Reloc* get_reloc() const;

    bool operator==(const EncodedLiteralSlice& x) const;
    bool operator!=(const EncodedLiteralSlice& x) const { return !(*this == x); }
    Ordering ord(const EncodedLiteralSlice& x) const;

    friend ::std::ostream& operator<<(std::ostream& os, const EncodedLiteralSlice& x);
};
