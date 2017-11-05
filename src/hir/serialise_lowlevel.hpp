/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/serialise_lowlevel.hpp
 * - HIR (De)Serialisation low-level "protocol"
 */
#pragma once

#include <vector>
#include <string>
#include <stddef.h>
#include <assert.h>

namespace HIR {
namespace serialise {

class WriterInner;
class ReaderInner;

class Writer
{
    WriterInner*    m_inner;
public:
    Writer(const ::std::string& path);
    Writer(const Writer&) = delete;
    Writer(Writer&&) = delete;
    ~Writer();

    void write(const void* data, size_t count);

    void write_u8(uint8_t v) {
        write(reinterpret_cast<const char*>(&v), 1);
    }
    void write_u16(uint16_t v) {
        uint8_t buf[] = { static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8) };
        this->write(buf, 2);
    }
    void write_u32(uint32_t v) {
        uint8_t buf[] = {
            static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8),
            static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24) };
        this->write(buf, 4);
    }
    void write_u64(uint64_t v) {
        uint8_t buf[] = {
            static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24),
            static_cast<uint8_t>(v >> 32), static_cast<uint8_t>(v >> 40), static_cast<uint8_t>(v >> 48), static_cast<uint8_t>(v >> 56)
            };
        this->write(buf, 8);
    }
    void write_i64(int64_t v) {
        write_u64(static_cast<uint64_t>(v));
    }
    // Variable-length encoded u64 (for array sizes)
    void write_u64c(uint64_t v) {
        if( v < (1<<7) ) {
            write_u8(static_cast<uint8_t>(v));
        }
        else if( v < (1<<(6+16)) ) {
            uint8_t buf[] = {
                static_cast<uint8_t>(0x80 + (v >> 16)),   // 0x80 -- 0xBF
                static_cast<uint8_t>(v >> 8),
                static_cast<uint8_t>(v & 0xFF)
                };
            this->write(buf, sizeof buf);
        }
        else if( v < (1ull << (5 + 32)) ) {
            uint8_t buf[] = {
                static_cast<uint8_t>(0xC0 + (v >> 32)), // 0xC0 -- 0xDF
                static_cast<uint8_t>(v >> 24),
                static_cast<uint8_t>(v >> 16),
                static_cast<uint8_t>(v >> 8),
                static_cast<uint8_t>(v)
                };
            this->write(buf, sizeof buf);
        }
        else {
            uint8_t buf[] = {
                0xFF,
                static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24),
                static_cast<uint8_t>(v >> 32), static_cast<uint8_t>(v >> 40), static_cast<uint8_t>(v >> 48), static_cast<uint8_t>(v >> 56)
                };
            this->write(buf, sizeof buf);
        }
    }
    void write_i64c(int64_t v) {
        // Convert from 2's completement
        bool sign = (v < 0);
        uint64_t va = (v < 0 ? -v : v);
        va <<= 1;
        va |= (sign ? 1 : 0);
        write_u64c(va);
    }
    void write_double(double v) {
        // - Just raw-writes the double
        this->write(&v, sizeof v);
    }
    void write_tag(unsigned int t) {
        assert(t < 256);
        write_u8( static_cast<uint8_t>(t) );
    }
    void write_count(size_t c) {
        //DEBUG("c = " << c);
        if(c < 0xFE) {
            write_u8( static_cast<uint8_t>(c) );
        }
        else if( c == ~0u ) {
            write_u8( 0xFF );
        }
        else {
            assert(c < (1u<<16));
            write_u8( 0xFE );
            write_u16( static_cast<uint16_t>(c) );
        }
    }
    void write_string(const ::std::string& v) {
        if(v.size() < 128) {
            write_u8( static_cast<uint8_t>(v.size()) );
        }
        else {
            assert(v.size() < (1u<<(16+7)));
            write_u8( static_cast<uint8_t>(128 + (v.size() >> 16)) );
            write_u16( static_cast<uint16_t>(v.size() & 0xFFFF) );
        }
        this->write(v.data(), v.size());
    }
    void write_bool(bool v) {
        write_u8(v ? 0xFF : 0x00);
    }
};


class ReadBuffer
{
    ::std::vector<uint8_t>  m_backing;
    unsigned int    m_ofs;
public:
    ReadBuffer(size_t size);

    size_t capacity() const { return m_backing.capacity(); }
    size_t read(void* dst, size_t len);
    void populate(ReaderInner& is);
};

class Reader
{
    ReaderInner*    m_inner;
    ReadBuffer  m_buffer;
public:
    Reader(const ::std::string& path);
    Reader(const Writer&) = delete;
    Reader(Writer&&) = delete;
    ~Reader();

    void read(void* dst, size_t count);

    uint8_t read_u8() {
        uint8_t v;
        read(&v, sizeof v);
        return v;
    }
    uint16_t read_u16() {
        uint8_t buf[2];
        read(buf, sizeof buf);
        return static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
    }
    uint32_t read_u32() {
        uint8_t buf[4];
        read(buf, sizeof buf);
        return static_cast<uint32_t>(buf[0])
            | (static_cast<uint32_t>(buf[1]) << 8)
            | (static_cast<uint32_t>(buf[2]) << 16)
            | (static_cast<uint32_t>(buf[3]) << 24)
            ;
    }
    uint64_t read_u64() {
        uint8_t buf[8];
        read(buf, sizeof buf);
        return static_cast<uint64_t>(buf[0])
            | (static_cast<uint64_t>(buf[1]) << 8)
            | (static_cast<uint64_t>(buf[2]) << 16)
            | (static_cast<uint64_t>(buf[3]) << 24)
            | (static_cast<uint64_t>(buf[4]) << 32)
            | (static_cast<uint64_t>(buf[5]) << 40)
            | (static_cast<uint64_t>(buf[6]) << 48)
            | (static_cast<uint64_t>(buf[7]) << 56)
            ;
    }
    int64_t read_i64() {
        return static_cast<int64_t>(read_u64());
    }
    // Variable-length encoded u64 (for array sizes)
    uint64_t read_u64c() {
        auto v = read_u8();
        if( v < (1<<7) ) {
            return static_cast<uint64_t>(v);
        }
        else if( v < 0xC0 ) {
            uint64_t    rv = static_cast<uint64_t>(v & 0x3F) << 16;
            rv |= static_cast<uint64_t>(read_u8()) << 8;
            rv |= static_cast<uint64_t>(read_u8());
            return rv;
        }
        else if( v < 0xFF ) {
            uint64_t    rv = static_cast<uint64_t>(v & 0x3F) << 32;
            rv |= static_cast<uint64_t>(read_u8()) << 24;
            rv |= static_cast<uint64_t>(read_u8()) << 16;
            rv |= static_cast<uint64_t>(read_u8()) << 8;
            rv |= static_cast<uint64_t>(read_u8());
            return rv;
        }
        else {
            return read_u64();
        }
    }
    int64_t read_i64c() {
        uint64_t va = read_u64c();
        bool sign = (va & 0x1) != 0;
        va >>= 1;

        if( va == 0 && sign ) {
            return INT64_MIN;
        }
        else if( sign ) {
            return -static_cast<int64_t>(va);
        }
        else {
            return static_cast<int64_t>(va);
        }
    }
    double read_double() {
        double v;
        read(reinterpret_cast<char*>(&v), sizeof v);
        return v;
    }
    unsigned int read_tag() {
        return static_cast<unsigned int>( read_u8() );
    }
    size_t read_count() {
        auto v = read_u8();
        if( v < 0xFE ) {
            return v;
        }
        else if( v == 0xFE ) {
            return read_u16( );
        }
        else /*if( v == 0xFF )*/ {
            return ~0u;
        }
    }
    ::std::string read_string() {
        size_t len = read_u8();
        if( len < 128 ) {
        }
        else {
            len = (len & 0x7F) << 16;
            len |= read_u16();
        }
        ::std::string   rv(len, '\0');
        read( const_cast<char*>(rv.data()), len);
        return rv;
    }
    bool read_bool() {
        return read_u8() != 0x00;
    }
};

}   // namespace serialise
}   // namespace HIR

