/*
 */
#pragma once
#include <sstream>

extern int g_debug_indent_level;

#ifndef DISABLE_DEBUG
#define INDENT()    do { g_debug_indent_level += 1; assert(g_debug_indent_level<300); } while(0)
#define UNINDENT()    do { g_debug_indent_level -= 1; } while(0)
#define DEBUG(ss)   do{ if(debug_enabled()) { debug_output(g_debug_indent_level, __FUNCTION__) << ss << ::std::endl; } } while(0)
#else
#define INDENT()    do { } while(0)
#define UNINDENT()    do {} while(0)
#define DEBUG(ss)   do{ } while(0)
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

class TraceLog
{
    const char* m_tag;
public:
    TraceLog(const char* tag, ::std::string info): m_tag(tag) {
        DEBUG(" >> " << m_tag << "(" << info << ")");
        INDENT();
    }
    TraceLog(const char* tag): m_tag(tag) {
        DEBUG(" >> " << m_tag);
        INDENT();
    }
    ~TraceLog() {
        UNINDENT();
        DEBUG("<< " << m_tag);
    }
};
#define TRACE_FUNCTION  TraceLog _tf_(__func__)
#define TRACE_FUNCTION_F(ss)    TraceLog _tf_(__func__, FMT(ss))


