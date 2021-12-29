/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/serialise_lowlevel.cpp
 * - HIR (De)Serialisation low-level "protocol"
 */
#include <debug.hpp>
#include "serialise_lowlevel.hpp"
#include <zlib.h>
#include <fstream>
#include <string.h>   // memcpy
#include <common.hpp>
#include <algorithm>
#include <iomanip>

namespace HIR {
namespace serialise {

class WriterInner
{
    ::std::ofstream m_backing;
    z_stream    m_zstream;
    ::std::vector<unsigned char> m_buffer;

    unsigned int    m_byte_out_count = 0;
    unsigned int    m_byte_in_count = 0;
public:
    WriterInner(const ::std::string& filename);
    ~WriterInner();
    void write(const void* buf, size_t len);
};

Writer::Writer():
    m_inner(nullptr)
{
}
Writer::~Writer()
{
    delete m_inner, m_inner = nullptr;
}
void Writer::open(const ::std::string& filename)
{
    // 1. Sort strings by frequency
    ::std::vector<::std::pair<RcString, unsigned>> sorted;
    sorted.reserve(m_istring_cache.size());
    for(const auto& e : m_istring_cache)
        sorted.push_back( e );
    // 2. Write out string table
    ::std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b){ return a.second > b.second; });

    m_objname_cache.clear();

    m_inner = new WriterInner(filename);
    // 3. Reset m_istring_cache to use the same value
    this->write_count(sorted.size());
    for(size_t i = 0; i < sorted.size(); i ++)
    {
        const auto& s = sorted[i].first;
        this->write_string(s.size(), s.c_str());
        DEBUG(i << " = " << m_istring_cache[s] << " '" << s << "'");
        m_istring_cache[s] = i;
    }
    for(const auto& e : m_istring_cache)
    {
        assert(e.second < sorted.size());
    }
}
void Writer::write(const void* buf, size_t len)
{
    if( m_inner ) {
        DEBUG("write(" << FMT_CB(ss, for(size_t i = 0; i < len; i ++) ss << std::setw(2) << std::setfill('0') << std::hex << unsigned( ((const uint8_t*)buf)[i] )) << ")");
        m_inner->write(buf, len);
    }
    else {
        // No-op, pre caching
    }
}
void Writer::write_string(const RcString& v)
{
    if( m_inner ) {
        // Emit ID from the cache
        this->write_count( m_istring_cache.at(v) );
    }
    else {
        // Find/add in cache
        m_istring_cache.insert(::std::make_pair(v, 0)).first->second += 1;
    }
}


WriterInner::WriterInner(const ::std::string& filename):
    m_backing( filename, ::std::ios_base::out | ::std::ios_base::binary),
    m_zstream(),
    m_buffer( 16*1024 )
    //m_buffer( 4*1024 )
{
    m_zstream.zalloc = Z_NULL;
    m_zstream.zfree = Z_NULL;
    m_zstream.opaque = Z_NULL;

    const int COMPRESSION_LEVEL = Z_BEST_COMPRESSION;
    int ret = deflateInit(&m_zstream, COMPRESSION_LEVEL);
    if(ret != Z_OK)
        throw ::std::runtime_error("zlib init failure");

    m_zstream.avail_out = m_buffer.size();
    m_zstream.next_out = m_buffer.data();
}
WriterInner::~WriterInner()
{
    assert( m_zstream.avail_in == 0 );

    // Complete the compression
    int ret;
    do
    {
        ret = deflate(&m_zstream, Z_FINISH);
        if(ret == Z_STREAM_ERROR) {
            ::std::cerr << "ERROR: zlib deflate stream error (cleanup)";
            abort();
        }
        if( m_zstream.avail_out != m_buffer.size() )
        {
            size_t rem = m_buffer.size() - m_zstream.avail_out;
            m_byte_out_count += rem;
            m_backing.write( reinterpret_cast<char*>(m_buffer.data()), rem );

            m_zstream.avail_out = m_buffer.size();
            m_zstream.next_out = m_buffer.data();
        }
    } while(ret == Z_OK);
    deflateEnd(&m_zstream);
}

void WriterInner::write(const void* buf, size_t len)
{
    m_zstream.avail_in = len;
    m_zstream.next_in = reinterpret_cast<unsigned char*>( const_cast<void*>(buf) );

    size_t last_avail_in = m_zstream.avail_in;

    // While there's data to compress
    while( m_zstream.avail_in > 0 )
    {
        assert(m_zstream.avail_out != 0);

        // Compress the data
        int ret = deflate(&m_zstream, Z_NO_FLUSH);
        if(ret == Z_STREAM_ERROR)
            throw ::std::runtime_error("zlib deflate stream error");

        size_t used_this_time = last_avail_in - m_zstream.avail_in;
        last_avail_in = m_zstream.avail_in;
        m_byte_in_count += used_this_time;

        // If the entire input wasn't consumed, then it was likely due to a lack of output space
        // - Flush the output buffer to the file
        if( m_zstream.avail_in > 0 )
        {
            size_t bytes = m_buffer.size() - m_zstream.avail_out;
            m_backing.write( reinterpret_cast<char*>(m_buffer.data()), bytes );
            m_byte_out_count += bytes;

            m_zstream.avail_out = m_buffer.size();
            m_zstream.next_out = m_buffer.data();
        }
    }

    // Flush stream contents if the output buffer is full.
    while( m_zstream.avail_out == 0 )
    {
        size_t bytes = m_buffer.size() - m_zstream.avail_out;
        m_backing.write( reinterpret_cast<char*>(m_buffer.data()), bytes );
        m_byte_out_count += bytes;

        m_zstream.avail_out = m_buffer.size();
        m_zstream.next_out = m_buffer.data();

        int ret = deflate(&m_zstream, Z_NO_FLUSH);
        if(ret == Z_STREAM_ERROR)
            throw ::std::runtime_error("zlib deflate stream error");
    }
}


// --------------------------------------------------------------------
class ReaderInner
{
    ::std::ifstream m_backing;
    z_stream    m_zstream;
    ::std::vector<unsigned char> m_buffer;

    unsigned int    m_byte_out_count = 0;
    unsigned int    m_byte_in_count = 0;
public:
    ReaderInner(const ::std::string& filename);
    ~ReaderInner();
    size_t read(void* buf, size_t len);
};


ReadBuffer::ReadBuffer(size_t cap):
    m_ofs(0)
{
    m_backing.reserve(cap);
}
size_t ReadBuffer::read(void* dst, size_t len)
{
    size_t rem = m_backing.size() - m_ofs;
    if( rem >= len )
    {
        memcpy(dst, m_backing.data() + m_ofs, len);
        m_ofs += len;
        return len;
    }
    else
    {
        memcpy(dst, m_backing.data() + m_ofs, rem);
        m_ofs = m_backing.size();
        return rem;
    }
}
void ReadBuffer::populate(ReaderInner& is)
{
    m_backing.resize( m_backing.capacity(), 0 );
    auto len = is.read(m_backing.data(), m_backing.size());
    m_backing.resize( len );
    m_ofs = 0;
}


Reader::Reader(const ::std::string& filename):
    m_inner( new ReaderInner(filename) ),
    m_buffer(1024),
    m_pos(0)
{
    size_t n_strings = read_count();
    m_strings.reserve(n_strings);
    DEBUG("n_strings = " << n_strings);
    for(size_t i = 0; i < n_strings; i ++)
    {
        auto s = read_string();
        m_strings.push_back( RcString::new_interned(s) );
    }
}
Reader::~Reader()
{
    delete m_inner, m_inner = nullptr;
}

void Reader::read(void* buf, size_t len)
{
    auto used = m_buffer.read(buf, len);
    if( used == len ) {
        m_pos += len;
        return ;
    }
    buf = reinterpret_cast<uint8_t*>(buf) + used;
    len -= used;

    if( len >= m_buffer.capacity() )
    {
        m_inner->read(buf, len);
    }
    else
    {
        m_buffer.populate( *m_inner );
        used = m_buffer.read(buf, len);
        if( used != len )
            throw ::std::runtime_error( FMT("Reader::read - Requested " << len << " bytes from buffer, got " << used) );
    }

    m_pos += len;
}


ReaderInner::ReaderInner(const ::std::string& filename):
    m_backing(filename, ::std::ios_base::in|::std::ios_base::binary),
    m_zstream(),
    m_buffer(16*1024)
{
    if( !m_backing.is_open() )
        throw ::std::runtime_error("Unable to open file");

    m_zstream.zalloc = Z_NULL;
    m_zstream.zfree = Z_NULL;
    m_zstream.opaque = Z_NULL;

    int ret = inflateInit(&m_zstream);
    if(ret != Z_OK)
        throw ::std::runtime_error("zlib init failure");

    m_zstream.avail_in = 0;
}
ReaderInner::~ReaderInner()
{
    inflateEnd(&m_zstream);
}
size_t ReaderInner::read(void* buf, size_t len)
{
    m_zstream.avail_out = len;
    m_zstream.next_out = reinterpret_cast<unsigned char*>(buf);
    do {
        // Reset input buffer if empty
        if( m_zstream.avail_in == 0 )
        {
            m_backing.read( reinterpret_cast<char*>(m_buffer.data()), m_buffer.size() );
            m_zstream.avail_in = m_backing.gcount();
            if( m_zstream.avail_in == 0 ) {
                m_byte_out_count += len  - m_zstream.avail_out;
                //::std::cerr << "Out of bytes, " << m_zstream.avail_out << " needed" << ::std::endl;
                return len - m_zstream.avail_out;
            }
            m_zstream.next_in = const_cast<unsigned char*>(m_buffer.data());

            m_byte_in_count += m_zstream.avail_in;
        }

        int ret = inflate(&m_zstream, Z_NO_FLUSH);
        if(ret == Z_STREAM_ERROR)
            throw ::std::runtime_error("zlib inflate stream error");
        switch(ret)
        {
        case Z_NEED_DICT:
            ret = Z_DATA_ERROR;
        case Z_DATA_ERROR:
        case Z_MEM_ERROR:
            throw ::std::runtime_error("zlib inflate error");
        default:
            break;
        }

    } while( m_zstream.avail_out > 0 );
    m_byte_out_count += len;

    return len;
}

}   // namespace serialise
}   // namespace HIR
