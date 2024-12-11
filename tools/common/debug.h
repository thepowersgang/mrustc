/*
 * mrustc common tools
 * - by John Hodge (Mutabah)
 *
 * tools/common/debug.h
 * - Generic debug interface (used by minicargo/standalone_miri)
 */
#pragma once

#include <functional>
#include <vector>
#include <sstream>

typedef ::std::function<void(::std::ostream& os)> dbg_cb_t;
extern void Debug_SetPhase(const char* phase_name);
extern void Debug_ProcessEnable(const char* enable_string);
extern void Debug_DisablePhase(const char* phase_name);
extern void Debug_EnablePhase(const char* phase_name);
extern bool Debug_IsEnabled();
extern void Debug_EnterScope(const char* name, dbg_cb_t );
extern void Debug_LeaveScope(const char* name, dbg_cb_t );
extern void Debug_Print(dbg_cb_t cb);

#if defined(NOLOG)
# define DEBUG(fmt)  do { } while(0)
# define TRACE_FUNCTION_F(fmt) do{}while(0)
#else
# define DEBUG(fmt)  do { Debug_Print([&](auto& os){ os << "DEBUG: " << fmt; }); } while(0)
# define TRACE_FUNCTION_F(fmt) DebugFunctionScope  trace_function_hdr { __FUNCTION__, [&](auto& os){ os << fmt; } }
#endif
#define TODO(fmt)   do { ::std::cerr << "TODO: " << fmt << ::std::endl; abort(); } while(0)

template<typename T>
::std::ostream& operator<<(::std::ostream& os, const ::std::vector<T>& v);

namespace {
    static inline void format_to_stream(::std::ostream& os) {
    }
    template<typename T, typename... A>
    static inline void format_to_stream(::std::ostream& os, const T& v, const A&... a) {
        os << v;
        format_to_stream(os, a...);
    }
}

struct DebugFunctionScope {
    const char* m_name;
    DebugFunctionScope(const char* name, dbg_cb_t cb):
        m_name(name)
    {
        Debug_EnterScope(m_name, cb);
    }
    ~DebugFunctionScope()
    {
        Debug_LeaveScope(m_name, [](auto& ){});
    }
};

template<typename ...T>
::std::string format(const T&... v)
{
    ::std::stringstream ss;
    format_to_stream(ss, v...);
    return ss.str();
}

template<typename T>
::std::ostream& operator<<(::std::ostream& os, const ::std::vector<T>& v)
{
    bool first = true;
    for(const auto& e : v)
    {
        if(!first)
            os << ",";
        os << e;
        first = false;
    }
    return os;
}

