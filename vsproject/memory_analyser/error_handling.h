#pragma once

#include <Windows.h>
#include <iostream>
#include <sstream>

#define FMT_STRING(...) (dynamic_cast<::std::stringstream&>(::std::stringstream() << __VA_ARGS__).str())

class WinErrStr {
    char* ptr;

public:
    WinErrStr(DWORD ec) { 
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, ec, 0, (LPSTR)&this->ptr, 0, NULL);
        *strrchr(ptr, '\r') = '\0';
    }
    WinErrStr(WinErrStr&& x): ptr(x.ptr) {
        x.ptr = nullptr;
    }
    WinErrStr& operator=(WinErrStr&& x) {
        this->~WinErrStr();
        this->ptr = x.ptr;
        x.ptr = nullptr;
    }

    ~WinErrStr() {
        if(ptr) {
            LocalFree(ptr);
            ptr = nullptr;
        }
    }
    friend std::ostream& operator<<(std::ostream& os, const WinErrStr& x) {
        return os << x.ptr;
    }
};
static inline int error(const char* name, DWORD ec) {
    std::cout << name << ": " << WinErrStr(ec) << "\n";
    return 1;
}
static inline int error(const char* name) {
    return error(name, GetLastError());
}


struct Indent {
    const char* str;
    unsigned level;

    class Handle {
        friend Indent;
        Indent& i;
        Handle(Indent& i): i(i) {
            i.level ++;
        }
    public:
        ~Handle() {
            i.level --;
        }
    };

    Indent(const char* s): str(s), level(0) {
    }

    Handle inc() {
        return Handle(*this);
    }
    friend std::ostream& operator<<(std::ostream& os, const Indent& x) {
        for(unsigned i = 0; i < x.level; i ++)
            os << x.str;
        return os;
    }
};

