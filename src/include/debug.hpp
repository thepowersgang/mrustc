/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * include/debug.hpp
 * - Common compiler debugging macros/helpers
 *
 * see also src/include/span.hpp
 */
#pragma once
#include <sstream>
#include <cassert>
#include <functional>

extern int g_debug_indent_level;

#ifndef DEBUG_EXTRA_ENABLE
# define DEBUG_EXTRA_ENABLE  // Files can override this with their own flag if needed (e.g. `&& g_my_debug_on`)
#endif

#ifndef DISABLE_DEBUG
# define MAX_INDENT_LEVEL   400
# define DEBUG_ENABLED  (debug_enabled() DEBUG_EXTRA_ENABLE)
# define INDENT()    do { g_debug_indent_level += 1; assert(g_debug_indent_level<MAX_INDENT_LEVEL); } while(0)
# define UNINDENT()    do { g_debug_indent_level -= 1; } while(0)
# define DEBUG(ss)   do{ if(DEBUG_ENABLED) { debug_output(g_debug_indent_level, __FUNCTION__) << ss << std::dec << ::std::endl; } } while(0)
# define TRACE_FUNCTION  TraceLog _tf_( DEBUG_ENABLED ? __func__ : nullptr)
# define TRACE_FUNCTION_F(ss)    TraceLog _tf_(DEBUG_ENABLED ? __func__ : nullptr, [&](::std::ostream&__os){ __os << ss; })
# define TRACE_FUNCTION_FR(ss,ss2)    TraceLog _tf_(DEBUG_ENABLED ? __func__ : nullptr, [&](::std::ostream&__os){ __os << ss; }, [&](::std::ostream&__os){ __os << ss2;})
#else
# define INDENT()    do { } while(0)
# define UNINDENT()    do {} while(0)
# define DEBUG(ss)   do{ if(false) (void)(::NullSink() << ss); } while(0)
# define TRACE_FUNCTION  do{} while(0)
# define TRACE_FUNCTION_F(ss)  do{ if(false) (void)(::NullSink() << ss); } while(0)
# define TRACE_FUNCTION_FR(ss,ss2)  do{ if(false) (void)(::NullSink() << ss); if(false) (void)(::NullSink() << ss2); } while(0)
#endif

extern bool debug_enabled();
extern ::std::ostream& debug_output(int indent, const char* function);

struct RepeatLitStr
{
    const char *s;
    int n;

    friend ::std::ostream& operator<<(::std::ostream& os, const RepeatLitStr& r) {
        for(int i = 0; i < r.n; i ++ )
            os << r.s;
        return os;
    }
};

class NullSink
{
public:
    NullSink()
    {}

    template<typename T>
    const NullSink& operator<<(const T&) const { return *this;  }
};

class TraceLog
{
    const char* m_tag;
    ::std::function<void(::std::ostream&)>  m_ret;
public:
    TraceLog(const char* tag, ::std::function<void(::std::ostream&)> info_cb, ::std::function<void(::std::ostream&)> ret);
    TraceLog(const char* tag, ::std::function<void(::std::ostream&)> info_cb);
    TraceLog(const char* tag);
    ~TraceLog();
};

struct FmtLambda
{
    ::std::function<void(::std::ostream&)>  m_cb;
    FmtLambda(::std::function<void(::std::ostream&)> cb):
        m_cb(cb)
    { }
    friend ::std::ostream& operator<<(::std::ostream& os, const FmtLambda& x) {
        x.m_cb(os);
        return os;
    }
};
#define FMT_CB(os, ...)  ::FmtLambda( [&](auto& os) { __VA_ARGS__; } )
#define FMT_CB_S(...)  ::FmtLambda( [&](auto& _os) { _os << __VA_ARGS__; } )


