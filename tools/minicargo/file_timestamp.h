#pragma once

#include <path.h>

class Timestamp
{
#if _WIN32
    uint64_t m_val;

    Timestamp(uint64_t val):
        m_val(val)
    {
    }
#else
    time_t  m_val;
    Timestamp(time_t t):
        m_val(t)
    {
    }
#endif

public:
    static Timestamp for_file(const ::helpers::path& p);
    static Timestamp infinite_past() {
        return Timestamp { 0 };
    }

    bool operator==(const Timestamp& x) const {
        return m_val == x.m_val;
    }
    bool operator<(const Timestamp& x) const {
        return m_val < x.m_val;
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const Timestamp& x) {
#if _WIN32
        os << ::std::hex << x.m_val << ::std::dec;
#else
        os << x.m_val;
#endif
        return os;
    }
};