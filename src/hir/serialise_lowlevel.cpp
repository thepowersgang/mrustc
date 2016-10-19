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


::HIR::serialise::ReadBuffer::ReadBuffer(size_t cap):
    m_ofs(0)
{
    m_backing.reserve(cap);
}
size_t ::HIR::serialise::ReadBuffer::read(void* dst, size_t len)
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
void ::HIR::serialise::ReadBuffer::populate(::std::istream& is)
{
    m_backing.resize( m_backing.capacity(), 0 );
    is.read(reinterpret_cast<char*>(m_backing.data()), m_backing.capacity());
    m_backing.resize( is.gcount() );
    m_ofs = 0;
}

::HIR::serialise::Reader::Reader(const ::std::string& filename):
    m_backing( filename ),
    m_buffer(1024)
{
    m_is.push( ::boost::iostreams::zlib_decompressor() );
    m_is.push( m_backing );
}

void ::HIR::serialise::Reader::read(void* buf, size_t len)
{
    auto used = m_buffer.read(buf, len);
    if( used == len ) {
        return ;
    }
    buf = reinterpret_cast<uint8_t*>(buf) + used;
    len -= used;
    
    if( len >= m_buffer.capacity() )
    {
        m_is.read(reinterpret_cast<char*>(buf), len);
        if( !m_is )
            throw "";
    }
    else
    {
        m_buffer.populate( m_is );
        used = m_buffer.read(buf, len);
        if( used != len )
            throw "";
    }
}
