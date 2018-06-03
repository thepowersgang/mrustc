/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * debug.cpp
 * - Interpreter debug logging
 */
#include "debug.hpp"
#include <fstream>

unsigned DebugSink::s_indent = 0;
::std::unique_ptr<std::ofstream> DebugSink::s_out_file;

DebugSink::DebugSink(::std::ostream& inner):
    m_inner(inner)
{
}
DebugSink::~DebugSink()
{
    m_inner << "\n";
    m_inner.flush();
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
        break;
    case DebugLevel::Fatal:
        sink << "FATAL: ";
        break;
    case DebugLevel::Bug:
        sink << "BUG: " << file << ":" << line << ": ";
        break;
    }
    return DebugSink(sink);
}
void DebugSink::inc_indent()
{
    s_indent ++;
}
void DebugSink::dec_indent()
{
    s_indent --;
}
