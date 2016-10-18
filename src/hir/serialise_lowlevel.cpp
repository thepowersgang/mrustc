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
    m_backing( filename )
{
    m_is.push( ::boost::iostreams::zlib_decompressor() );
    m_is.push( m_backing );
}

void ::HIR::serialise::Reader::read(void* buf, size_t len)
{
    m_is.read(reinterpret_cast<char*>(buf), len);
    if( !m_is ) {
        throw "";
    }
}
