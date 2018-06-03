/*
 * mrustc Standalone MIRI
 * - by John Hodge (Mutabah)
 *
 * debug.hpp
 * - Interpreter debug logging
 */
#pragma once

#include <iostream>
#include <functional>
#include <memory>

enum class DebugLevel {
    Trace,
    Debug,
    Notice,
    Warn,
    Error,
    Fatal,
    Bug,
};

class DebugSink
{
    static unsigned s_indent;
    static ::std::unique_ptr<std::ofstream> s_out_file;
    ::std::ostream& m_inner;
    DebugSink(::std::ostream& inner);
public:
    ~DebugSink();

    template<typename T>
    ::std::ostream& operator<<(const T& v) { return m_inner << v; }

    static void set_output_file(const ::std::string& s);
    static bool enabled(const char* fcn_name);
    static DebugSink get(const char* fcn_name, const char* file, unsigned line, DebugLevel lvl);
    // TODO: Add a way to insert an annotation before/after an abort/warning/... that indicates what input location caused it.
    //static void set_position();

    static void inc_indent();
    static void dec_indent();
};
template<typename T, typename U>
class FunctionTrace
{
    const char* m_fname;
    const char* m_file;
    unsigned m_line;
    U   m_exit;
public:
    FunctionTrace(const char* fname, const char* file, unsigned line, T entry, U exit):
        m_fname(fname),
        m_file(file),
        m_line(line),
        m_exit(exit)
    {
        if( DebugSink::enabled(fname) ) {
            auto s = DebugSink::get(fname, file, line, DebugLevel::Debug);
            s << "(";
            (entry)(s);
            s << ")";
            DebugSink::inc_indent();
        }
    }
    ~FunctionTrace() {
        if( DebugSink::enabled(m_fname) ) {
            DebugSink::dec_indent();
            auto s = DebugSink::get(m_fname, m_file, m_line, DebugLevel::Debug);
            s << "(";
            m_exit(s);
            s << ")";
        }
    }
};
template<typename T, typename U>
FunctionTrace<T,U> FunctionTrace_d(const char* fname, const char* file, unsigned line, T entry, U exit) {
    return FunctionTrace<T,U>(fname, file, line, entry, exit);
}

struct DebugExceptionTodo:
    public ::std::exception
{
    const char* what() const noexcept override {
        return "TODO hit";
    }
};
struct DebugExceptionError:
    public ::std::exception
{
    const char* what() const noexcept override {
        return "error";
    }
};

#define TRACE_FUNCTION_R(entry, exit) auto ftg##__LINE__ = FunctionTrace_d(__FUNCTION__,__FILE__,__LINE__,[&](DebugSink& FunctionTrace_ss){FunctionTrace_ss << entry;}, [&](DebugSink& FunctionTrace_ss) {FunctionTrace_ss << exit;} )
#define LOG_TRACE(strm) do { if(DebugSink::enabled(__FUNCTION__)) DebugSink::get(__FUNCTION__,__FILE__,__LINE__,DebugLevel::Trace) << strm; } while(0)
#define LOG_DEBUG(strm) do { if(DebugSink::enabled(__FUNCTION__)) DebugSink::get(__FUNCTION__,__FILE__,__LINE__,DebugLevel::Debug) << strm; } while(0)
#define LOG_NOTICE(strm) do { DebugSink::get(__FUNCTION__,__FILE__,__LINE__,DebugLevel::Notice) << strm; } while(0)
#define LOG_ERROR(strm) do { DebugSink::get(__FUNCTION__,__FILE__,__LINE__,DebugLevel::Error) << strm; throw DebugExceptionError{}; } while(0)
#define LOG_FATAL(strm) do { DebugSink::get(__FUNCTION__,__FILE__,__LINE__,DebugLevel::Fatal) << strm; exit(1); } while(0)
#define LOG_TODO(strm) do { DebugSink::get(__FUNCTION__,__FILE__,__LINE__,DebugLevel::Bug) << "TODO: " << strm; throw DebugExceptionTodo{}; } while(0)
#define LOG_BUG(strm) do { DebugSink::get(__FUNCTION__,__FILE__,__LINE__,DebugLevel::Bug) << "BUG: " << strm; abort(); } while(0)
#define LOG_ASSERT(cnd,strm) do { if( !(cnd) ) { LOG_BUG("Assertion failure: " #cnd " - " << strm); } } while(0)
