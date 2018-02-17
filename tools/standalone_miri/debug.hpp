//
//
//
#pragma once

#include <iostream>

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
    ::std::ostream& m_inner;
    DebugSink(::std::ostream& inner);
public:
    ~DebugSink();

    template<typename T>
    ::std::ostream& operator<<(const T& v) { return m_inner << v; }

    static bool enabled(const char* fcn_name);
    static DebugSink get(const char* fcn_name, const char* file, unsigned line, DebugLevel lvl);
};

#define LOG_TRACE(strm) do { if(DebugSink::enabled(__FUNCTION__)) DebugSink::get(__FUNCTION__,__FILE__,__LINE__,DebugLevel::Trace) << strm; } while(0)
#define LOG_DEBUG(strm) do { if(DebugSink::enabled(__FUNCTION__)) DebugSink::get(__FUNCTION__,__FILE__,__LINE__,DebugLevel::Debug) << strm; } while(0)
#define LOG_ERROR(strm) do { DebugSink::get(__FUNCTION__,__FILE__,__LINE__,DebugLevel::Error) << strm; exit(1); } while(0)
#define LOG_FATAL(strm) do { DebugSink::get(__FUNCTION__,__FILE__,__LINE__,DebugLevel::Fatal) << strm; exit(1); } while(0)
#define LOG_TODO(strm) do { DebugSink::get(__FUNCTION__,__FILE__,__LINE__,DebugLevel::Bug) << "TODO: " << strm; abort(); } while(0)
#define LOG_BUG(strm) do { DebugSink::get(__FUNCTION__,__FILE__,__LINE__,DebugLevel::Bug) << "BUG: " << strm; abort(); } while(0)
#define LOG_ASSERT(cnd,strm) do { if( !(cnd) ) { LOG_BUG("Assertion failure: " #cnd " - " << strm); } } while(0)
