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

#ifndef DISABLE_DEBUG
# define INDENT()    do { g_debug_indent_level += 1; assert(g_debug_indent_level<300); } while(0)
# define UNINDENT()    do { g_debug_indent_level -= 1; } while(0)
# define DEBUG(ss)   do{ if(debug_enabled()) { debug_output(g_debug_indent_level, __FUNCTION__) << ss << ::std::endl; } } while(0)
# define TRACE_FUNCTION  TraceLog _tf_(__func__)
# define TRACE_FUNCTION_F(ss)    TraceLog _tf_(__func__, [&](::std::ostream&__os){ __os << ss; })
# define TRACE_FUNCTION_FR(ss,ss2)    TraceLog _tf_(__func__, [&](::std::ostream&__os){ __os << ss; }, [&](::std::ostream&__os){ __os << ss2;})
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

// This function should be used in situations where you want to unconditionally
// run some code (probably because it has side effects), and in debug builds,
// you want to check the result of the code. In non-debug builds, this function
// will still evaluate its arguments (which is what we want), but will ignore
// the results.
//
// Don't convert this to a preprocessor macro. That would defeat the whole
// purpose of this function.
static inline void assert_or_ignore(bool value) {
    assert(value);
}

// Similar to assert_or_ignore(), this function allows you to compute a value
// and then ignore its results. For expressions that have side-effects, the
// side-effects will still occur. For expressions that don't have side-effects
// (such as "a * 2"), the optimizer will remove all of the parts of the
// expression that are obviously side-effect free (dead-code elimination).
//
// This function is intended to be used in situations where we compute a value,
// store it in a local variable, and then use assert() to check things related
// to that value, so that we can avoid "unused value" warnings on non-debug
// builds. Example:
//
//     size_t foo = compute_some_size();
//     assert(foo > 0);         // it's big
//     assert(foo % 4 == 0);    // it's aligned
//     ignore(foo);             // no warning on debug builds
//
// Don't convert this to a preprocessor macro. That would defeat the whole
// purpose of this function.
static inline void ignore(bool value) {}
