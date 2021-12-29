/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/serialise_lowlevel.hpp
 * - HIR (De)Serialisation low-level "protocol"
 */
#pragma once

// Encoding protocol ideas:
// > Semi-typed data format (encode length in the format)
// Purpose: Allows internal consistency checking and recovery (recovery not needed here)
//
// 0x00-0xBF are literal integer values.
// 0xC0-0xFB <data>: Short encoded length prefixed data (lengths 0 to 59 bytes)
// 0xFC <len+> <data>: Length prefixed literal data
// 0xFD indicates start of a named object (string index follows)
// 0xFE indicates start of an unnamed object
// 0xFF indicates end of an object

#include <vector>
#include <string>
#include <map>
#include <stddef.h>
#include <assert.h>
#include <rc_string.hpp>

namespace HIR {
namespace serialise {

class WriterInner;
class ReaderInner;

class Writer
{
    WriterInner*    m_inner;
    ::std::map<RcString, unsigned>  m_istring_cache;
    ::std::map<const char*, unsigned>  m_objname_cache;
public:
    Writer();
    Writer(const Writer&) = delete;
    Writer(Writer&&) = delete;
    ~Writer();

    void open(const ::std::string& filename);
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
        DEBUG(c);
        if(c < 0xFD) {
            write_u8( static_cast<uint8_t>(c) );
        }
        else if( c == ~0u ) {
            write_u8( 0xFF );
        }
        else if( c < (1u << 16) ) {
            write_u8( 0xFD );
            write_u16( static_cast<uint16_t>(c) );
        }
        else {
            assert(c < (1u<<31));
            write_u8( 0xFE );
            write_u32( static_cast<uint32_t>(c) );
        }
    }
    void write_string(const RcString& v);
    void write_string(size_t len, const char* s) {
        TRACE_FUNCTION;
        if(len < 128) {
            write_u8( static_cast<uint8_t>(len) );
        }
        else {
            assert(len < (1u<<(16+7)));
            write_u8( static_cast<uint8_t>(128 + (len >> 16)) );
            write_u16( static_cast<uint16_t>(len & 0xFFFF) );
        }
        this->write(s, len);
    }
    void write_string(const ::std::string& v) {
        write_string(v.size(), v.c_str());
    }
    void write_bool(bool v) {
        TRACE_FUNCTION_F(v);
        write_u8(v ? 0xFF : 0x00);
    }


    // Core protocol
    void raw_write_uint(uint64_t val) {
        if(val < 0xC0) {
            write_u8(static_cast<uint8_t>(val));
        }
        else {
            uint8_t bytes[8];
            uint8_t len = 0;
            while(val > 0) {
                assert(len < 8); 
                bytes[len] = static_cast<uint8_t>(val);
                val >>= 8;
                len += 1;
            }
            write_u8(0xC0 + len);
            this->write(bytes, len);
        }
    }
    void raw_write_len(size_t len) {
        if(len < (0xFC - 0xC0)) {
            write_u8(0xC0 + len);
        }
        else {
            write_u8(0xFC);
            raw_write_uint(len);
        }
    }
    void raw_write_bytes(size_t len, const void* data) {
        raw_write_len(len);
        this->write(data, len);
    }
    class CloseOnDrop {
        friend class Writer;
        Writer* r;
        CloseOnDrop(Writer& r): r(&r) {}
    public:
        CloseOnDrop(CloseOnDrop&& x): r(x.r) { x.r = nullptr; }
        ~CloseOnDrop(){ if(r) r->close_object(); r = nullptr; }
    };
    CloseOnDrop open_object(const char* name) {
        write_u8(0xFD);
        auto iv = m_objname_cache.insert(std::make_pair( name, static_cast<unsigned>(m_objname_cache.size()) ));
        raw_write_uint(iv.first->second);
        if(iv.second)
        {
            raw_write_bytes(strlen(name), name);
        }
        return CloseOnDrop(*this);
    }
    CloseOnDrop open_anon_object() {
        write_u8(0xFE);
        return CloseOnDrop(*this);
    }
    void close_object() {
        write_u8(0xFF);
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
    size_t  m_pos;
    ::std::vector<RcString> m_strings;

    ::std::vector<std::string>  m_objname_cache;
public:
    Reader(const ::std::string& path);
    Reader(const Writer&) = delete;
    Reader(Writer&&) = delete;
    ~Reader();

    size_t get_pos() const { return m_pos; }
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
        size_t rv;
        auto v = read_u8();
        if( v < 0xFD ) {
            rv = v;
        }
        else if( v == 0xFD ) {
            rv = read_u16( );
        }
        else if( v == 0xFE ) {
            rv = read_u32( );
        }
        else /*if( v == 0xFF )*/ {
            rv = ~0u;
        }
        DEBUG(rv);
        return rv;
    }
    RcString read_istring() {
        size_t idx = read_count();
        return m_strings.at(idx);
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
        auto v = read_u8();
        switch(v)
        {
        case 0: return false;
        case 255: return true;
        default:
            std::cerr << "Expected false(0)/true(255), got " << unsigned(v) << "u8" << ::std::endl;
            abort();
        }
    }


    // Core protocol
    uint64_t raw_read_uint() {
        auto v = read_u8();
        assert(v <= 0xC0 + 8);
        if( v < 0xC0 ) {
            return v;
        }
        else {
            size_t len = v - 0xC0;
            uint64_t rv = 0;
            for(size_t p = 0; p < len; p ++)
            {
                rv |= static_cast<uint64_t>(read_u8()) << (8*p);
            }
            return rv;
        }
    }
    size_t raw_read_len() {
        auto v = read_u8();
        if( v < 0xC0 ) {
            std::cerr << "Expected length, got literal integer " << unsigned(v) << ::std::endl;
            abort();
        }
        else if(v < 0xFC) {
            return v - 0xC0;
        }
        else if( v == 0xFC) {
            return raw_read_uint();
        }
        else {
            std::cerr << "Expected length, got tag " << unsigned(v) << ::std::endl;
            abort();
        }
    }
    std::string raw_read_bytes_stdstring() {
        auto len = raw_read_len();
        std::string rv(len, '\0');
        read( const_cast<char*>(rv.data()), len );
        return rv;
    }


    class CloseOnDrop {
        friend class Reader;
        Reader* r;
        CloseOnDrop(Reader& r): r(&r) {}
    public:
        CloseOnDrop(const CloseOnDrop&) = delete;
        CloseOnDrop(CloseOnDrop&& x): r(x.r) { x.r = nullptr; }
        ~CloseOnDrop(){ if(r) r->close_object(); r = nullptr; }
    };

    CloseOnDrop open_object(const char* name) {
        auto v = read_u8();
        if( v != 0xFD ) {
            std::cerr << "Expected OpenNamed(" << name << "), got " << unsigned(v) << "u8" << ::std::endl;
            abort();
        }
        auto key = raw_read_uint();
        //std::cout << key << " = " << "..." << std::endl;
        if(key == m_objname_cache.size()) {
            m_objname_cache.push_back( raw_read_bytes_stdstring() );
        }
        assert(key < m_objname_cache.size());
        //std::cout << key << " = " << m_objname_cache[key] << std::endl;
        if( m_objname_cache[key] != name ) {
            std::cerr << "Expecting OpenNamed(" << name << "), got OpenNamed(" << m_objname_cache[key] << ")" << std::endl;
            abort();
        }
        return CloseOnDrop(*this);
    }
    CloseOnDrop open_anon_object() {
        auto v = read_u8();
        if( v != 0xFE ) {
            std::cerr << "Expected OpenAnon, got " << unsigned(v) << ::std::endl;
            abort();
        }
        return CloseOnDrop(*this);
    }
    void close_object() {
        auto v = read_u8();
        if( v != 0xFF ) {
            std::cerr << "Expected CloseObject(0xFF), got " << unsigned(v) << ::std::endl;
            abort();
        }
    }
};

}   // namespace serialise
}   // namespace HIR

