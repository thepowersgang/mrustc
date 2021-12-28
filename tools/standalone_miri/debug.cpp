/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * debug.cpp
 * - Interpreter debug logging
 */
#include "debug.hpp"
#include <fstream>
#include "../../src/common.hpp" // FmtEscaped

unsigned DebugSink::s_indent = 0;
::std::unique_ptr<std::ofstream> DebugSink::s_out_file;

DebugSink::DebugSink(::std::ostream& inner, bool stderr_too):
    m_inner(inner),
    m_stderr_too(stderr_too)
{
}
DebugSink::~DebugSink()
{
    m_inner << "\n";
    m_inner.flush();
    m_inner.flags({});
    if( m_stderr_too )
    {
        ::std::cerr.flags({});
        ::std::cerr << ::std::endl;
    }
}
void DebugSink::set_output_file(const ::std::string& s)
{
    s_out_file.reset(new ::std::ofstream(s));
}
bool DebugSink::enabled(const char* fcn_name)
{
    return true;
}
DebugSink DebugSink::get(const char* fcn_name, const char* file, unsigned line, DebugLevel lvl)
{
    bool stderr_too = false;
    auto& sink = s_out_file ? *s_out_file : ::std::cout;
    for(size_t i = s_indent; i--;)
        sink << " ";
    switch(lvl)
    {
    case DebugLevel::Trace:
        sink << "Trace: " << file << ":" << line << ": ";
        break;
    case DebugLevel::Debug:
        sink << "DEBUG: " << fcn_name << ": ";
        break;
    case DebugLevel::Notice:
        sink << "NOTE: ";
        break;
    case DebugLevel::Warn:
        sink << "WARN: ";
        break;
    case DebugLevel::Error:
        sink << "ERROR: ";
        std::cerr << "ERROR: ";
        stderr_too = true;
        break;
    case DebugLevel::Fatal:
        sink << "FATAL: ";
        std::cerr << "FATAL: ";
        stderr_too = true;
        break;
    case DebugLevel::Bug:
        sink << "BUG: " << file << ":" << line << ": ";
        std::cerr << "BUG: " << file << ":" << line << ": ";
        stderr_too = true;
        break;
    }
    return DebugSink(sink, stderr_too);
}
void DebugSink::inc_indent()
{
    s_indent ++;
}
void DebugSink::dec_indent()
{
    s_indent --;
}


::std::ostream& operator<<(::std::ostream& os, const FmtEscaped& x)
{
    os << ::std::hex;
    for(auto s = x.s; *s != '\0'; s ++)
    {
        switch(*s)
        {
        case '\0':  os << "\\0";    break;
        case '\n':  os << "\\n";    break;
        case '\\':  os << "\\\\";   break;
        case '"':   os << "\\\"";   break;
        default:
            uint8_t v = *s;
            if( v < 0x80 )
            {
                if( v < ' ' || v > 0x7F )
                    os << "\\u{" << ::std::hex << (unsigned int)v << "}";
                else
                    os << v;
            }
            else if( v < 0xC0 )
                ;
            else if( v < 0xE0 )
            {
                uint32_t    val = (uint32_t)(v & 0x1F) << 6;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)(v & 0x3F) << 0;
                os << "\\u{" << ::std::hex << val << "}";
            }
            else if( v < 0xF0 )
            {
                uint32_t    val = (uint32_t)(v & 0x0F) << 12;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)(v & 0x3F) << 6;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)(v & 0x3F) << 0;
                os << "\\u{" << ::std::hex << val << "}";
            }
            else if( v < 0xF8 )
            {
                uint32_t    val = (uint32_t)(v & 0x07) << 18;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)(v & 0x3F) << 12;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)(v & 0x3F) << 6;
                v = (uint8_t)*++s; if( (v & 0xC0) != 0x80 ) { s--; continue ; } val |= (uint32_t)(v & 0x3F) << 0;
                os << "\\u{" << ::std::hex << val << "}";
            }
            break;
        }
    }
    os << ::std::dec;
    return os;
}

