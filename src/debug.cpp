/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * debug.cpp
 * - Debug printing (with indenting)
 */
#include <debug.hpp>

TraceLog::TraceLog(const char* tag, ::std::function<void(::std::ostream&)> info_cb, ::std::function<void(::std::ostream&)> ret):
    m_tag(tag),
    m_ret(ret)
{
    if(debug_enabled()) {
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
    if(debug_enabled()) {
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
    if(debug_enabled()) {
        auto& os = debug_output(g_debug_indent_level, m_tag);
        os << ">>" << ::std::endl;
    }
    INDENT();
}
TraceLog::~TraceLog() {
    UNINDENT();
    if(debug_enabled()) {
        auto& os = debug_output(g_debug_indent_level, m_tag);
        os << "<< (";
        m_ret(os);
        os << ")" << ::std::endl;
    }
}
