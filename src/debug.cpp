/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * debug.cpp
 * - Debug printing (with indenting)
 */
#include <debug_inner.hpp>
#include <debug.hpp>
#include <set>
#include <iostream>
#include <iomanip>
#include <common.hpp>   // FmtEscaped
#include <cstring>	// strchr

// TODO: Inline debug filter/caching
// - Cache messages for the current phase, clearing the cache (dropping) when various signatures match
//  > Similar to the `log_get_last_function.py` script

int g_debug_indent_level = 0;
bool g_debug_enabled = true;
::std::string g_cur_phase;
::std::set< ::std::string>    g_debug_disable_map;

TraceLog::TraceLog(const char* tag, ::std::function<void(::std::ostream&)> info_cb, ::std::function<void(::std::ostream&)> ret):
    m_tag(tag),
    m_ret(ret)
{
    if(debug_enabled() && m_tag) {
        auto& os = debug_output(g_debug_indent_level, m_tag);
        os << ">> (";
        info_cb(os);
        os << ")" << ::std::endl;
    }
    INDENT();
}
TraceLog::TraceLog(const char* tag, ::std::function<void(::std::ostream&)> info_cb):
    m_tag(tag),
    m_ret([](const auto&){})
{
    if(debug_enabled() && m_tag) {
        auto& os = debug_output(g_debug_indent_level, m_tag);
        os << ">> (";
        info_cb(os);
        os << ")" << ::std::endl;
    }
    INDENT();
}
TraceLog::TraceLog(const char* tag):
    m_tag(tag),
    m_ret([](const auto&){})
{
    if(debug_enabled() && m_tag) {
        auto& os = debug_output(g_debug_indent_level, m_tag);
        os << ">>" << ::std::endl;
    }
    INDENT();
}
TraceLog::~TraceLog() {
    UNINDENT();
    if(debug_enabled() && m_tag) {
        auto& os = debug_output(g_debug_indent_level, m_tag);
        os << "<< (";
        m_ret(os);
        os << ")" << ::std::endl;
    }
}



bool debug_enabled_update() {
    if( g_debug_disable_map.count(g_cur_phase) != 0 ) {
        return false;
    }
    else {
        return true;
    }
}
bool debug_enabled()
{
    return g_debug_enabled;
}
::std::ostream& debug_output(int indent, const char* function)
{
    return ::std::cout << g_cur_phase << "- " << RepeatLitStr { " ", indent } << function << ": ";
}

DebugTimedPhase::DebugTimedPhase(const char* name):
    m_name(name)
{
    ::std::cout << m_name << ": V V V" << ::std::endl;
    g_cur_phase = m_name;
    g_debug_enabled = debug_enabled_update();
    m_start = clock();
}
DebugTimedPhase::~DebugTimedPhase()
{
    auto end = clock();
    g_cur_phase = "";
    g_debug_enabled = debug_enabled_update();

    // TODO: Show wall time too?
    ::std::cout << "(" << ::std::fixed << ::std::setprecision(2) << static_cast<double>(end - m_start) / static_cast<double>(CLOCKS_PER_SEC) << " s) ";
    ::std::cout << m_name << ": DONE";
    ::std::cout << ::std::endl;
}

extern void debug_init_phases(const char* env_var_name, std::initializer_list<const char*> il)
{
    for(const char* e : il)
    {
        g_debug_disable_map.insert(e);
    }

    // Mutate this map using an environment variable
    const char* debug_string = ::std::getenv(env_var_name);
    if( debug_string )
    {
        while( debug_string[0] )
        {
            const char* end = strchr(debug_string, ':');

            ::std::string   s;
            if( end )
            {
                s = ::std::string { debug_string, end };
                debug_string = end + 1;
            }
            else
            {
                s = debug_string;
            }
            if( g_debug_disable_map.erase(s) == 0 )
            {
                ::std::cerr << "WARN: Unknown compiler phase '" << s << "' in $" << env_var_name << ::std::endl;
            }
            if( !end ) {
                break;
            }
        }
    }
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

