#pragma once

#include <Windows.h>
#pragma warning(push)
#pragma warning(disable : 4091)
#include <DbgHelp.h>
#pragma warning(pop)
#include <ostream>
#include "error_handling.h"
#include <map>

// #include <cvconst.h>
// https://docs.microsoft.com/en-us/visualstudio/debugger/debug-interface-access/symtagenum?view=vs-2019
enum SymTagEnum {
    SymTagNull,
    SymTagExe,
    SymTagCompiland,
    SymTagCompilandDetails,
    SymTagCompilandEnv,
    SymTagFunction,
    SymTagBlock,
    SymTagData,
    SymTagAnnotation,
    SymTagLabel,
    SymTagPublicSymbol,
    SymTagUDT,
    SymTagEnum,
    SymTagFunctionType,
    SymTagPointerType,
    SymTagArrayType,
    SymTagBaseType,
    SymTagTypedef,
    SymTagBaseClass,
    SymTagFriend,
    SymTagFunctionArgType,
    SymTagFuncDebugStart,
    SymTagFuncDebugEnd,
    SymTagUsingNamespace,
    SymTagVTableShape,
    SymTagVTable,
    SymTagCustom,
    SymTagThunk,
    SymTagCustomType,
    SymTagManagedType,
    SymTagDimension,
    SymTagCallSite,
    SymTagInlineSite,
    SymTagBaseInterface,
    SymTagVectorType,
    SymTagMatrixType,
    SymTagHLSLType
};

enum BasicType {
    btNoType = 0,
    btVoid = 1,
    btChar = 2,
    btInt = 6,
    btUint = 7,
    btFloat = 8,    // TODO: `double` also shows up as this
    btBool = 10,
    btLong = 13,
    btULong = 14,
};



struct fmt_u16z
{
    const WCHAR* ptr;

    fmt_u16z(const WCHAR* ptr): ptr(ptr) {
    }

    friend std::ostream& operator<<(std::ostream& os, const fmt_u16z& x) {
        for(auto p = x.ptr; *p; p ++)
        {
            if( *p < 128 ) {
                os << static_cast<char>(*p);
            }
            else {
                os << "?";
            }
        }
        return os;
    }
};

class WideStrLocalFree
{
    WCHAR* ptr;

public:
    WideStrLocalFree(WCHAR* ptr): ptr(ptr)
    {
    }

    ~WideStrLocalFree() {
        if(ptr) {
            LocalFree(ptr);
            ptr = nullptr;
        }
    }
    friend std::ostream& operator<<(std::ostream& os, const WideStrLocalFree& x) {
        return os << fmt_u16z(x.ptr);
    }
};

struct ReadMemory
{
private:
    static ReadMemory* s_self;

    LPVOID  view_base;
    struct MemoryRange {
        uint64_t    ofs;
        uint64_t    len;
    };
    ::std::map<uint64_t, MemoryRange>   m_ranges;

public:
    ReadMemory(LPVOID view, const MINIDUMP_MEMORY_LIST* memory, const MINIDUMP_MEMORY64_LIST* memory64);

    static BOOL read(HANDLE /*hProcess*/, DWORD64 qwBaseAddress, PVOID lpBuffer, DWORD nSize, PDWORD lpNumberOfBytesRead);

    static PVOID FunctionTableAccess64(HANDLE hProcess, DWORD64 AddrBase) {
        auto rv = SymFunctionTableAccess64AccessRoutines(hProcess, AddrBase, ReadMemory::read, SymGetModuleBase64);
        //auto rv = SymFunctionTableAccess64(hProcess, AddrBase);
        auto ec = GetLastError();
        //std::cout << "SymFunctionTableAccess64(" << hProcess << ", " << (void*)AddrBase << ") = " << rv << std::endl;
        if(!rv)
            error("SymFunctionTableAccess64AccessRoutines", ec);
        return rv;
    }


    static uint8_t read_u8(DWORD64 qwBaseAddress) {
        BYTE   buf;
        DWORD   n_read;
        if( !ReadMemory::read(NULL, qwBaseAddress, &buf, sizeof(buf), &n_read) )
            throw std::runtime_error(FMT_STRING("Can't read u8 at " << (void*)qwBaseAddress));
        return buf;
    }
    static DWORD read_u32(DWORD64 qwBaseAddress) {
        DWORD   buf;
        DWORD   n_read;
        if( !ReadMemory::read(NULL, qwBaseAddress, &buf, sizeof(buf), &n_read) )
            throw std::runtime_error(FMT_STRING("Can't read u32 at " << (void*)qwBaseAddress));
        return buf;
    }

    static DWORD64 read_ptr(DWORD64 qwBaseAddress) {
        DWORD64   buf;
        DWORD   n_read;
        if( !ReadMemory::read(NULL, qwBaseAddress, &buf, sizeof(buf), &n_read) )
            throw std::runtime_error(FMT_STRING("Can't read ptr at " << (void*)qwBaseAddress));
        return buf;
    }
};
