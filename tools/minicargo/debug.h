#pragma once

#include <functional>
#include <vector>
#include <sstream>

extern void Debug_Print(::std::function<void(::std::ostream& os)> cb);

#define DEBUG(fmt)  do { Debug_Print([&](auto& os){ os << "DEBUG: " << fmt; }); } while(0)
#define TODO(fmt)   do { Debug_Print([&](auto& os){ os << "DEBUG: " << fmt; }); abort(); } while(0)

namespace {
    static inline void format_to_stream(::std::ostream& os) {
    }
    template<typename T, typename... A>
    static inline void format_to_stream(::std::ostream& os, const T& v, const A&... a) {
        os << v;
        format_to_stream(os, a...);
    }
}

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

