#include "debug.hpp"

unsigned DebugSink::s_indent = 0;

DebugSink::DebugSink(::std::ostream& inner):
    m_inner(inner)
{
}
DebugSink::~DebugSink()
{
    m_inner << "\n";
}
bool DebugSink::enabled(const char* fcn_name)
{
    return true;
}
DebugSink DebugSink::get(const char* fcn_name, const char* file, unsigned line, DebugLevel lvl)
{
    for(size_t i = s_indent; i--;)
        ::std::cout << " ";
    switch(lvl)
    {
    case DebugLevel::Trace:
        ::std::cout << "Trace: " << file << ":" << line << ": ";
        break;
    case DebugLevel::Debug:
        ::std::cout << "DEBUG: " << fcn_name << ": ";
        break;
    case DebugLevel::Notice:
        ::std::cout << "NOTE: ";
        break;
    case DebugLevel::Warn:
        ::std::cout << "WARN: ";
        break;
    case DebugLevel::Error:
        ::std::cout << "ERROR: ";
        break;
    case DebugLevel::Fatal:
        ::std::cout << "FATAL: ";
        break;
    case DebugLevel::Bug:
        ::std::cout << "BUG: " << file << ":" << line << ": ";
        break;
    }
    return DebugSink(::std::cout);
}
void DebugSink::inc_indent()
{
    s_indent ++;
}
void DebugSink::dec_indent()
{
    s_indent --;
}