/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/serialise_lowlevel.cpp
 * - HIR (De)Serialisation low-level "protocol"
 */
#include "serialise_lowlevel.hpp"
#include <boost/iostreams/filter/zlib.hpp>

::HIR::serialise::Writer::Writer(const ::std::string& filename):
    m_backing( filename )
{
    m_os.push( ::boost::iostreams::zlib_compressor() );
    m_os.push( m_backing );
}

void ::HIR::serialise::Writer::write(const void* buf, size_t len)
{
    m_os.write(reinterpret_cast<const char*>(buf), len);
}


::HIR::serialise::Reader::Reader(const ::std::string& filename):
    m_backing( filename ),
    m_buffer(),
    m_buffer_ofs(0)
{
    m_buffer.reserve(1024);
    m_is.push( ::boost::iostreams::zlib_decompressor() );
    m_is.push( m_backing );
}

void ::HIR::serialise::Reader::read(void* buf, size_t len)
{
    size_t rem = m_buffer.size() - m_buffer_ofs;
    if( m_buffer_ofs < m_buffer.size() ) {
        if( len <= rem ) {
            memcpy(buf, m_buffer.data() + m_buffer_ofs, len);
            m_buffer_ofs += len;
            return ;
        }
        else {
            memcpy(buf, m_buffer.data() + m_buffer_ofs, rem);
            m_buffer_ofs += rem;
            len -= rem;
        }
    }
    // m_bufer_ofs == m_buffer.size()
    
    if( len >= m_buffer.capacity() )
    {
        // If the new read is longer than the buffer size, skip the buffer.
        m_is.read(reinterpret_cast<char*>(buf), len);
    }
    else
    {
        m_buffer.resize( m_buffer.capacity(), 0 );
        m_buffer_ofs = 0;
        m_is.read(reinterpret_cast<char*>(m_buffer.data()), m_buffer.capacity());
        size_t dat_len = m_is.gcount();
        
        if( dat_len == 0 )
            throw "";
        if( dat_len < len )
            throw "";
        
        m_buffer.resize( dat_len );
        memcpy(reinterpret_cast<uint8_t*>(buf) + rem, m_buffer.data(), len);
        m_buffer_ofs = len;
    }
}
