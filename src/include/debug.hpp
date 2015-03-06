/*
 */
#pragma once
#include <sstream>

extern int g_debug_indent_level;

#define INDENT()    do { g_debug_indent_level += 1; } while(0)
#define UNINDENT()    do { g_debug_indent_level -= 1; } while(0)
#define DEBUG(ss)   do{ if(debug_enabled()) { debug_output(g_debug_indent_level, __FUNCTION__) << ss << ::std::endl; } } while(0)

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

