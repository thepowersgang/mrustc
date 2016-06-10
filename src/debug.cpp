
#include <debug.hpp>

TraceLog::TraceLog(const char* tag, ::std::string info, ::std::function<void(::std::ostream&)> ret):
    m_tag(tag),
    m_ret(ret)
{
    if(debug_enabled()) {
        auto& os = debug_output(g_debug_indent_level, m_tag);
        os << ">> (" << info << ")" << ::std::endl;
    }
    INDENT();
}
TraceLog::TraceLog(const char* tag, ::std::string info):
    m_tag(tag),
    m_ret([](const auto&){})
{
    if(debug_enabled()) {
        auto& os = debug_output(g_debug_indent_level, m_tag);
        os << ">> (" << info << ")" << ::std::endl;
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
