// memory_analyser.cpp : This file contains the 'main' function. Program execution begins and ends there.

#undef NDEBUG

#include <Windows.h>
#pragma warning(push)
#pragma warning(disable : 4091)
#include <DbgHelp.h>
#pragma warning(pop)
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <unordered_map>
#include <map>
#include <set>
#include <cassert>
#include <memory>   // unique_ptr

//#include <cvconst.h>

static bool ENABLE_DEBUG_SYMINFO = false;

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
int error(const char* name, DWORD ec) {
    std::cout << name << ": " << WinErrStr(ec);
    return 1;
}
int error(const char* name) {
    return error(name, GetLastError());
}


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
    LPVOID  view;
    const MINIDUMP_MEMORY_LIST* memory;
    const MINIDUMP_MEMORY64_LIST* memory64;

    static ReadMemory* s_self;

    static BOOL read(HANDLE /*hProcess*/, DWORD64 qwBaseAddress, PVOID lpBuffer, DWORD nSize, PDWORD lpNumberOfBytesRead);

    static PVOID FunctionTableAccess64(HANDLE hProcess, DWORD64 AddrBase) {
        auto rv = SymFunctionTableAccess64AccessRoutines(hProcess, AddrBase, ReadMemory::read, SymGetModuleBase64);
        //auto rv = SymFunctionTableAccess64(hProcess, AddrBase);
        auto ec = GetLastError();
        //std::cout << "SymFunctionTableAccess64(" << hProcess << ", " << (void*)AddrBase << ") = " << rv << std::endl;
        if(!rv)
            error("SymFunctionTableAccess64", ec);
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
ReadMemory* ReadMemory::s_self = 0;

struct TypeDefinition;
struct TypeRef
{
private:
    typedef std::unordered_map<DWORD, TypeRef>  t_cache;
    static t_cache  s_cache;
public:
    struct Wrapper {
        unsigned    count;
        DWORD   inner_ty_idx;
        Wrapper(unsigned c): count(c) {}

        static Wrapper new_pointer() {
            return Wrapper(~0u);
        }
        bool is_pointer() const { return count == ~0u; }
    };

    std::vector<Wrapper>    wrappers;
    enum {
        ClassBasic,
        ClassUdt,
        ClassMisc,
    } m_class;
    union {
        struct {
            BasicType   bt;
            uint8_t size;
        } basic;
        struct {
            const char* name;
            uint8_t size;
        } misc;
        TypeDefinition* udt;
    } m_data;

    static TypeRef lookup(HANDLE hProcess, ULONG64 mod_base, DWORD type_id);
    static TypeRef lookup_by_name(const std::string& name);

    size_t size() const;

    bool is_any_basic() const { return wrappers.empty() && m_class == ClassBasic; }
    bool is_basic(BasicType bt) const { return wrappers.empty() && m_class == ClassBasic && m_data.basic.bt == bt; }
    const TypeDefinition* any_udt() const { return (wrappers.empty() && m_class == ClassUdt) ? m_data.udt : nullptr; }
    bool is_udt(const char* name) const;
    bool is_udt_suffix(const char* name) const;

    TypeRef get_field(std::initializer_list<const char*> fields, size_t* ofs=nullptr) const;
    size_t get_field_ofs(std::initializer_list<const char*> fields, TypeRef* out_ty=nullptr) const
    {
        size_t rv;
        auto ty = get_field(fields, &rv);
        if(out_ty)  *out_ty = ty;
        return rv;
    }
    TypeRef deref() const;
    void fmt(std::ostream& os, unsigned recurse_depth, unsigned indent_level=0) const;

    friend std::ostream& operator<<(std::ostream& os, const TypeRef& x) {
        x.fmt(os, 0);
        return os;
    }
};
struct TypeDefinition
{
    struct Field {
        std::string name;
        size_t  offset;
        TypeRef ty;
    };

    std::string name;
    size_t  size;
    std::vector<Field>  fields;

    void fmt(std::ostream& os, unsigned recurse_depth, unsigned indent_level=0) const;

    static TypeDefinition* from_syminfo(HANDLE hProcess, ULONG64 mod_base, DWORD type_id);
};


struct MemoryStats
{
    std::map<std::string, unsigned>   m_counts;
    std::map<std::string, std::pair<DWORD64, unsigned> >   m_duplicates_SimplePath;
    std::map<std::string, std::pair<DWORD64, unsigned> >   m_duplicates_TypeRef;

    std::string get_enum_key(const TypeRef& ty, DWORD64 addr) const;
    const TypeRef& get_real_type(const TypeRef& ty, DWORD64 addr) const;
    bool are_equal(const TypeRef& ty, DWORD64 addr1, DWORD64 addr2);
    void enum_type_at(const TypeRef& ty, DWORD64 addr, bool is_top_level=true);
};


namespace {
    ::std::pair<DWORD64,DWORD64> get_checked_vector(DWORD64 addr, size_t inner_size)
    {
        auto start = ReadMemory::read_ptr(addr + 0*8);
        auto end = ReadMemory::read_ptr(addr + 1*8);
        auto max = ReadMemory::read_ptr(addr + 2*8);
        if( !(start <= end && end <= max) )
            throw std::runtime_error("");
        if( !((end - start) % inner_size == 0) && !((max - start) % inner_size == 0) )
            throw std::runtime_error("");
        if( start == 0)
            return std::make_pair(0, 0);
        return std::make_pair(start, end);
    }
}

namespace virt_types {
    struct StdVector
    {
        const TypeRef& ty;
        DWORD64 start;
        DWORD64 end;

        DWORD64 cur;

        StdVector(const TypeRef& inner_ty, DWORD64 addr):
            ty(inner_ty)
        {
            size_t inner_size = inner_ty.size();
            start = ReadMemory::read_ptr(addr + 0*8);
            end = ReadMemory::read_ptr(addr + 1*8);
            auto max = ReadMemory::read_ptr(addr + 2*8);
            if( !(start <= end && end <= max) )
                throw std::runtime_error("");
            if( !((end - start) % inner_size == 0) && !((max - start) % inner_size == 0) )
                throw std::runtime_error("");
            if( start == 0 )
            {
                end = 0;
                cur = 0;
            }
            else
            {
                cur = start;
            }
        }

        static StdVector at(const TypeRef& vec_ty, DWORD64 addr)
        {
            return StdVector(vec_ty.get_field({"_Mypair", "_Myval2", "_Myfirst"}).deref(), addr);
        }
    };

    std::string fmt_rcstring(/*const TypeRef& ty,*/ DWORD64 addr) {
        try
        {
            std::string s;
            if( addr == 0 ) {
                return "#NULL#";
            }
            auto refcnt = ReadMemory::read_u32(addr + 0);
            auto len = ReadMemory::read_u32(addr + 4);
            //s += FMT_STRING("(@" << (void*)addr << " " << len << "/" << refcnt << ")");
            for(size_t i = 0; i < len; i ++)
            {
                s += ReadMemory::read_u8(addr + 8 + i);
            }
            return s;
        }
        catch(const std::exception& e)
        {
            return FMT_STRING("EXCEPTION(fmt_rcstring): " << e.what());
        }
    }
    std::string fmt_simplepath(const TypeRef& ty, DWORD64 addr) {
        try
        {
            std::string s;
            s += "::\"";
            s += fmt_rcstring(ReadMemory::read_ptr(addr + ty.get_field_ofs({"m_crate_name"})));
            s += "\"";

            auto inner_ty = ty.get_field({"m_components", "_Mypair", "_Myval2", "_Myfirst"}).deref();
            auto inner_size = inner_ty.size();
            auto v = get_checked_vector( addr + ty.get_field_ofs({"m_components"}), inner_size );
            for(auto cur = v.first; cur != v.second; cur += inner_size)
            {
                s += "::";
                s += fmt_rcstring(ReadMemory::read_ptr(cur));
            }
            return s;
        }
        catch(const std::exception& e)
        {
            return FMT_STRING("EXCEPTION(fmt_simplepath): " << e.what());
        }
    }
    std::string fmt_typeref(const TypeRef& outer_ty, DWORD64 addr) {
        try
        {
            assert(outer_ty.is_udt("HIR::TypeRef"));
            const auto& ty = outer_ty.any_udt()->fields.at(0).ty;
            assert(ty.is_udt("HIR::TypeData"));
            const auto* udt = ty.any_udt();
            const auto& flds = udt->fields;
            const auto* data_udt = flds[1].ty.any_udt();
            assert(data_udt);
            auto tag = ReadMemory::read_u32(addr) - 1;
            if( tag + 1 == 0 ) {
                // Dead, just log
                return FMT_STRING("TypeRef #DEAD");
            }
            else if( tag < data_udt->fields.size() ) {
                const auto& name = data_udt->fields[tag].name;
                const auto& ity = data_udt->fields[tag].ty;
                const auto iaddr = addr + ty.get_field_ofs({"m_data", name.c_str()});
                if( name == "Diverge" ) {
                    return "!";
                }
                else if( name == "Infer" ) {
                    auto idx = ReadMemory::read_u32( iaddr + ity.get_field_ofs({"index"}) );
                    auto cls = ReadMemory::read_u32( iaddr + ity.get_field_ofs({"ty_class"}) );
                    if( idx == 0xFFFFFFFFull ) {
                        return FMT_STRING("_#?:" << cls);
                    }
                    else {
                        return FMT_STRING("_#" << idx << ":" << cls);
                    }
                }
                else if( name == "Generic" ) {
                    auto binding = ReadMemory::read_u32( iaddr + ity.get_field_ofs({"binding"}) );
                    //auto cls = ReadMemory::read_u32( iaddr + ity.get_field_ofs({"name"}) );
                    if( binding == 0xFFFF ) {
                        return FMT_STRING("Self");
                    }
                    else {
                        switch(binding >> 8)
                        {
                        case 0:
                            return FMT_STRING("I:" << (binding&0xFF));
                        case 1:
                            return FMT_STRING("F:" << (binding&0xFF));
                        default:
                            return FMT_STRING("?" << (binding>>8) << ":" << (binding&0xFF));
                        }
                    }
                }
                else {
                    return FMT_STRING("TODO#TypeData::" << name);
                }
            }
            else {
                return FMT_STRING("TypeRef#BAD" << tag);
            }
        }
        catch(const std::exception& e)
        {
            return FMT_STRING("EXCEPTION(fmt_typeref): " << e.what());
        }
    }
}


std::map<uint64_t, TypeRef> g_vtable_cache;

int main(int argc, char* argv[])
{
    std::map<std::string, std::set<std::string>>    variables;
    const char* infile = nullptr;
    std::cout << argv[0];
    for(int i = 1; i < argc; i ++)
    {
        std::cout << " " << argv[i];
        if(!infile) {
            infile = argv[i];
        }
        else {
            variables["main"].insert(argv[i]);
        }
    }
    std::cout << std::endl;
    if( !infile || variables.empty() ) {
        std::cerr << "USAGE: " << argv[0] << " <dump> <main_var>+" << std::endl;
        return 1;
    }
    //const char* infile = "mrustc-0-Parsed.dmp";
    //const char* infile = "mrustc-2-Resolved.dmp";   // Last with the AST
    //const char* infile = "mrustc-4-HIR.dmp";
    //const char* infile = "mrustc-7-MIR Opt.dmp";
    //const char* infile = "mrustc-8-Trans.dmp";
    //variables["main"].insert("crate");
    //variables["main"].insert("hir_crate");
    //variables["main"].insert("items");

    auto file = CreateFileA(infile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if( file == INVALID_HANDLE_VALUE ) {
        return error("CreateFileA");
    }
    LARGE_INTEGER   size;
    if( !GetFileSizeEx(file, &size) )
        return error("GetFileSizeEx");
    if( size.HighPart != 0 ) {
        std::cout << "size_lo = " << size.QuadPart << " - too large!" << std::endl;
        return 1;
    }
    auto map_handle = CreateFileMappingA(file, NULL, PAGE_READONLY, 0,0, NULL);
    if( !map_handle ) {
        return error("CreateFileMappingA");
    }
    auto view = MapViewOfFile(map_handle, FILE_MAP_READ, 0,0, size.LowPart);
    if( !view ) {
        return error("MapViewOfFile");
    }

    auto proc_handle = (HANDLE)1;
    if( !SymInitialize(proc_handle, NULL, FALSE) )
        return error("SymInitialize");

    MINIDUMP_MODULE_LIST*   module_list = NULL;
    if( !MiniDumpReadDumpStream(view, ModuleListStream, NULL, (PVOID*)&module_list, NULL) || !module_list ) {
        return error("MiniDumpReadDumpStream - ModuleListStream");
    }
    std::vector<std::string>    module_names;
    for(ULONG32 i = 0; i < module_list->NumberOfModules; i ++)
    {
        const auto& mod = module_list->Modules[i];
        const auto* mod_name = reinterpret_cast<const MINIDUMP_STRING*>((char*)view + mod.ModuleNameRva);
        module_names.push_back(FMT_STRING(fmt_u16z(mod_name->Buffer)));
        std::cout << (void*)mod.BaseOfImage << " + " << /*for hex formatting*/reinterpret_cast<void*>(static_cast<uintptr_t>(mod.SizeOfImage)) << " : " << module_names.back()  << std::endl;
        if( !SymLoadModule(proc_handle, NULL, module_names.back().c_str(), NULL, mod.BaseOfImage, mod.SizeOfImage) )
            return error("SymLoadModule ext");
    }
    SymRefreshModuleList(proc_handle);

    // TODO: Obtain a mapping of vtables addresses to types
    // - Could fetch the type name form the vtable itself?
    {
        struct H {
            static BOOL ty_enum_cb(PSYMBOL_INFO pSymInfo, ULONG SymbolSize, PVOID UserContext)
            {
                auto proc_handle = (HANDLE)1;
                auto mod_base = pSymInfo->ModBase;
                auto get_dword = [proc_handle,mod_base](DWORD type_id, const char* name, IMAGEHLP_SYMBOL_TYPE_INFO info_ty) {
                    DWORD v;
                    if( !SymGetTypeInfo(proc_handle, mod_base, type_id, info_ty, &v) )
                    {
                        throw std::runtime_error(FMT_STRING("#" << type_id << " " << name << "=" << WinErrStr(GetLastError())));
                    }
                    return v;
                };
                auto get_str = [proc_handle,mod_base](DWORD type_id, const char* name, IMAGEHLP_SYMBOL_TYPE_INFO info_ty)->WideStrLocalFree {
                    WCHAR* v;
                    if( !SymGetTypeInfo(proc_handle, mod_base, type_id, info_ty, &v) )
                    {
                        throw std::runtime_error(FMT_STRING("#" << type_id << " " << name << "=" << WinErrStr(GetLastError())));
                    }
                    return WideStrLocalFree(v);
                };
                try
                {
                    if( pSymInfo->Tag == SymTagUDT )
                    {
                        //std::cout << "#" << pSymInfo->TypeIndex << " " << pSymInfo->Name << " VClass " << std::endl;
                        TypeRef::lookup(proc_handle, mod_base, pSymInfo->Index);
                    }
                    auto symtag = get_dword(pSymInfo->TypeIndex, "SYMTAG", TI_GET_SYMTAG);
                    switch(symtag)
                    {
                    case SymTagVTable:
                        if(false)
                        {
                            std::cout << "#" << pSymInfo->TypeIndex << " VTable" << std::endl;
                        }
                        break;
                    case SymTagUDT:
                        if(false)
                        {
                            //if( get_dword(pSymInfo->TypeIndex, "VIRTUALBASECLASS", TI_GET_VIRTUALBASECLASS) )
                            //{
                            auto v = get_dword(pSymInfo->TypeIndex, "VIRTUALTABLESHAPEID", TI_GET_VIRTUALTABLESHAPEID);
                            std::cout << "#" << pSymInfo->TypeIndex << " " << pSymInfo->Name << " VTable " << v << std::endl;
                            //}
                        }
                        break;
                    }
                }
                catch(const std::exception& e)
                {
                    std::cout << "EXCEPTION in ty_enum_cb: " << e.what() << std::endl;
                }
                return TRUE;
            }
            static BOOL vt_enum_cb(PSYMBOL_INFO pSymInfo, ULONG SymbolSize, PVOID UserContext)
            {
                auto proc_handle = (HANDLE)1;
                auto mod_base = pSymInfo->ModBase;
                auto get_dword = [proc_handle,mod_base](DWORD type_id, const char* name, IMAGEHLP_SYMBOL_TYPE_INFO info_ty) {
                    DWORD v;
                    if( !SymGetTypeInfo(proc_handle, mod_base, type_id, info_ty, &v) )
                    {
                        throw std::runtime_error(FMT_STRING("#" << type_id << " " << name << "=" << WinErrStr(GetLastError())));
                    }
                    return v;
                };
                auto get_str = [proc_handle,mod_base](DWORD type_id, const char* name, IMAGEHLP_SYMBOL_TYPE_INFO info_ty)->WideStrLocalFree {
                    WCHAR* v;
                    if( !SymGetTypeInfo(proc_handle, mod_base, type_id, info_ty, &v) )
                    {
                        throw std::runtime_error(FMT_STRING("#" << type_id << " " << name << "=" << WinErrStr(GetLastError())));
                    }
                    return WideStrLocalFree(v);
                };
                try
                {
                    //auto symtag = get_dword(pSymInfo->Index, "SYMTAG", TI_GET_SYMTAG);
                    const auto* suf = "::`vftable'";
                    const size_t suf_len = strlen(suf);
                    auto len = strlen(pSymInfo->Name);
                    auto ofs = len > suf_len ? len - suf_len : 0;
                    if( ofs > 0 && strcmp(pSymInfo->Name + ofs, suf) == 0 )
                    {
                        if(strncmp(pSymInfo->Name, "std::_Func_impl", 5+5+5) != 0 )
                        {
                            std::string tyname( pSymInfo->Name, pSymInfo->Name + ofs );
                            std::cout << "vt_enum_cb: " << tyname << " = " << std::hex << pSymInfo->Address << std::dec << std::endl;
                            g_vtable_cache[pSymInfo->Address] = TypeRef::lookup_by_name(tyname);
                        }
                    }
                    else
                    {
                        if(strstr(pSymInfo->Name, "vftable"))
                        {
                            std::cerr << "ofs=" << ofs << " " << (ofs > 0 ? pSymInfo->Name + ofs : pSymInfo->Name) << std::endl;
                            assert(false);
                        }
                    }
                    //switch(symtag)
                    //{
                    //case SymTagData:
                    //    break;
                    //}
                }
                catch(const std::exception& e)
                {
                    std::cout << "EXCEPTION in vt_enum_cb: " << e.what() << std::endl;
                }
                return TRUE;
            }
        };
        std::cout << "--- Enumerating all UDT types" << std::endl; std::cout.flush();
        SymEnumTypes(proc_handle, module_list->Modules[0].BaseOfImage, H::ty_enum_cb, nullptr);
        std::cout << "--- Enumerating all VTables" << std::endl; std::cout.flush();
        SymEnumSymbols(proc_handle, module_list->Modules[0].BaseOfImage, "", H::vt_enum_cb, nullptr);
    }

    std::cout << "--- Loading dump file" << std::endl;
    std::cout.flush();

    MINIDUMP_THREAD_LIST*   thread_list = NULL;
    if( !MiniDumpReadDumpStream(view, ThreadListStream, NULL, (PVOID*)&thread_list, NULL) || !thread_list ) {
        return error("MiniDumpReadDumpStream - ThreadListStream");
    }

    MINIDUMP_MEMORY_LIST*   memory_list = NULL;
    if( !MiniDumpReadDumpStream(view, MemoryListStream, NULL, (PVOID*)&memory_list, NULL) || !memory_list ) {
        if( GetLastError() != ERROR_NOT_FOUND )
        {
            return error("MiniDumpReadDumpStream - MemoryListStream");
        }
    }

    MINIDUMP_MEMORY64_LIST*   memory64_list = NULL;
    if( !MiniDumpReadDumpStream(view, Memory64ListStream, NULL, (PVOID*)&memory64_list, NULL) || !memory64_list ) {
        if( GetLastError() != ERROR_NOT_FOUND )
        {
            return error("MiniDumpReadDumpStream - Memory64ListStream");
        }
    }

    // TODO: Dump all types?

    auto rm = ReadMemory { view, memory_list, memory64_list };
    ReadMemory::s_self = &rm;

    std::cout << "--- Enumerating memory" << std::endl;
    std::cout.flush();
    if(true)
    {
        struct EnumLocals
        {
            HANDLE  proc_handle;
            const STACKFRAME* frame;
            const CONTEXT* context;
            MemoryStats ms;
            std::set<std::string>*  var_list;

            static BOOL cb_static(PSYMBOL_INFO pSymInfo, ULONG SymbolSize, PVOID UserContext) {
                return reinterpret_cast<EnumLocals*>(UserContext)->cb(pSymInfo, SymbolSize);
            }
            BOOL cb(PSYMBOL_INFO pSymInfo, ULONG SymbolSize) {
                if( !(pSymInfo->Flags & SYMFLAG_LOCAL) )
                    return TRUE;

                // TODO: How to know if the variable is valid yet?

                try
                {

                    auto addr = pSymInfo->Address;
                    if( pSymInfo->Flags & SYMFLAG_REGREL ) {
                        switch(pSymInfo->Register)
                        {
                        case 335:   // CV_AMD64_RSP
                            addr += this->context->Rsp;
                            break;
                        default:
                            throw std::runtime_error(FMT_STRING("Unknown register " << pSymInfo->Register));
                        }
                    }
                    else if( pSymInfo->Flags & SYMFLAG_FRAMEREL ) {
                        addr += this->frame->AddrFrame.Offset;
                    }
                    else if( pSymInfo->Flags & SYMFLAG_REGISTER ) {
                        // TODO: Handle register values
                        addr = 0;
                    }
                    else {
                    }

                    auto ty = TypeRef::lookup(this->proc_handle, pSymInfo->ModBase, pSymInfo->TypeIndex);
                    std::cout << "VAR: (" << std::hex << pSymInfo->Flags << std::dec << ") " << pSymInfo->Name << " @ " << addr << "+" << ty.size() <<  ": ";
                    std::cout << ty;
                    std::cout << std::endl;

                    // Do type/memory walk on this variable
                    if( addr )
                    {
                        if( var_list->count(pSymInfo->Name) > 0 )
                        {
                            std::cout << "FULL TYPE: ";
                            ty.fmt(std::cout, /*recurse_depth=*/12, /*indent_level=*/1);
                            std::cout << std::endl;
                            ms.enum_type_at(ty, addr);
                        }
                    }
                }
                catch(const std::exception& e)
                {
                    std::cout << "VAR: " << pSymInfo->Name << ": EXCEPTION " << e.what() << std::endl;
                }

                return TRUE;
            }
        };
        EnumLocals  el;

        MINIDUMP_EXCEPTION_STREAM*  exception_stream = NULL;
        if( !MiniDumpReadDumpStream(view, ExceptionStream, NULL, (PVOID*)&exception_stream, NULL) || !exception_stream ) {
            if( GetLastError() != ERROR_NOT_FOUND )
            {
                return error("MiniDumpReadDumpStream - ExceptionStream");
            }
        }
        else 
        {
            std::cout << "Exception: TID=" << exception_stream->ThreadId << std::endl;

            CONTEXT context;
            memcpy(&context, (const char*)view + exception_stream->ThreadContext.Rva, min(sizeof(CONTEXT), exception_stream->ThreadContext.DataSize));
            STACKFRAME  frame;
            memset(&frame, 0, sizeof(frame));
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Mode = AddrModeFlat;
            frame.AddrPC.Segment = 0;
            frame.AddrFrame.Segment = 0;
            frame.AddrStack.Segment = 0;
            frame.AddrPC.Offset = context.Rip;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrStack.Offset = context.Rsp;

            do
            {
                char symbolBuffer[sizeof(SYMBOL_INFO ) + 256];
                memset(symbolBuffer, 0, sizeof(symbolBuffer));
                PSYMBOL_INFO  symbol = (PSYMBOL_INFO )symbolBuffer;
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 255;
                if( !SymFromAddr(proc_handle, frame.AddrPC.Offset, NULL, symbol) ) {
                    error("SymFromAddr");
                    std::cout << "+" << (void*)frame.AddrPC.Offset << " ?" << std::endl;
                    continue ;
                }

                std::cout << "+" << (void*)frame.AddrPC.Offset << " " << symbol->Name
                    << " BP=" << (void*)frame.AddrFrame.Offset
                    << " SP=" << (void*)frame.AddrStack.Offset
                    << std::endl;
                IMAGEHLP_STACK_FRAME    ih_frame = {0};
                ih_frame.InstructionOffset = context.Rip;
                ih_frame.FrameOffset = context.Rbp;
                ih_frame.StackOffset = context.Rsp;
                //ih_frame.FuncTableEntry = (DWORD64)ReadMemory::FunctionTableAccess64(proc_handle, symbol->Address);
                SymSetContext(proc_handle, &ih_frame, /*(unused) Context=*/NULL);
                el.frame = &frame;
                el.context = &context;
                el.proc_handle = proc_handle;

                auto it = variables.find(symbol->Name);
                if( it != variables.end() )
                {
                    std::cout << "Enumerating for " << symbol->Name << std::endl;
                    el.var_list = &it->second;
                    try {
                        SymEnumSymbols(proc_handle, NULL, "", EnumLocals::cb_static, &el);
                    }
                    catch(const std::exception& e)
                    {
                        std::cout << std::endl;
                        std::cout << "EXCEPTION: " << e.what() << std::endl;
                    }

                }
            } while( StackWalk(IMAGE_FILE_MACHINE_AMD64, proc_handle, (HANDLE)2, &frame, &context, ReadMemory::read, ReadMemory::FunctionTableAccess64, SymGetModuleBase64, NULL) );
        }

        for(const auto& e : el.ms.m_counts)
        {
            std::cout << e.first << " = " << e.second << std::endl;
        }
        {
            const auto& ty = TypeRef::lookup_by_name("HIR::SimplePath");
            const auto& list = el.ms.m_duplicates_SimplePath;
            std::cout << "Duplicates of " << ty << std::endl;
            unsigned unique = 0;
            for(const auto& e : list)
            {
                if(e.second.second > 1)
                {
                    std::cout << "> SimplePath:" << (void*)e.second.first << " * " << e.second.second << " - " << e.first << std::endl;
                }
                else
                {
                    unique += 1;
                }
            }
            std::cout << "> UNIQUE * " << unique << std::endl;
        }
        {
            const auto& ty = TypeRef::lookup_by_name("HIR::TypeRef");
            const auto& list = el.ms.m_duplicates_TypeRef;
            std::cout << "Duplicates of " << ty << std::endl;
            unsigned unique = 0;
            for(const auto& e : list)
            {
                if(e.second.second > 1)
                {
                    std::cout << "> TypeRef:" << (void*)e.second.first << " * " << e.second.second << " - " << e.first << std::endl;
                }
                else
                {
                    unique += 1;
                }
            }
            std::cout << "> UNIQUE * " << unique << std::endl;
        }
        return 0;
    }

#if 0
    std::cout << thread_list->NumberOfThreads << " threads" << std::endl;
    for(ULONG32 i = 0; i < thread_list->NumberOfThreads; i ++)
    {
        const auto& thread = thread_list->Threads[i];
        std::cout << i << ": " << thread.ThreadId
            << std::hex << " context=(" << thread.ThreadContext.Rva << "+" << thread.ThreadContext.DataSize << ")" << std::dec
            << std::endl;


        CONTEXT context;
        memcpy(&context, (const char*)view + thread.ThreadContext.Rva, min(sizeof(CONTEXT), thread.ThreadContext.DataSize));
        STACKFRAME  frame;
        memset(&frame, 0, sizeof(frame));
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Mode = AddrModeFlat;
        frame.AddrPC.Segment = 0;
        frame.AddrFrame.Segment = 0;
        frame.AddrStack.Segment = 0;
        frame.AddrPC.Offset = context.Rip;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrStack.Offset = context.Rsp;
        std::cout
            << " AX " << (void*)context.Rax
            << " CX " << (void*)context.Rcx
            << " DX " << (void*)context.Rdx
            << " BX " << (void*)context.Rbx
            << std::endl
            ;
        do
        {
            char symbolBuffer[sizeof(SYMBOL_INFO ) + 256];
            memset(symbolBuffer, 0, sizeof(symbolBuffer));
            PSYMBOL_INFO  symbol = (PSYMBOL_INFO )symbolBuffer;
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            if( !SymFromAddr(proc_handle, frame.AddrPC.Offset, NULL, symbol) ) {
                error("SymFromAddr");
            }

            const char* modname = "UNKNOWN";
            for(int i = 0; i < module_list->NumberOfModules; i ++)
            {
                const auto& mod = module_list->Modules[i];
                if(mod.BaseOfImage <= frame.AddrPC.Offset && frame.AddrPC.Offset <= mod.BaseOfImage + mod.SizeOfImage)
                {
                    modname = module_names[i].c_str();
                }
            }
            std::cout << "+" << (void*)frame.AddrPC.Offset << " " << modname << " " << symbol->Name
                << " BP=" << (void*)frame.AddrFrame.Offset
                << " SP=" << (void*)frame.AddrStack.Offset
                << std::endl;
            //std::cout << sym_info->Name << std::endl;
        } while( StackWalk(IMAGE_FILE_MACHINE_AMD64, proc_handle, (HANDLE)2, &frame, &context, ReadMemory::read, ReadMemory::FunctionTableAccess64, SymGetModuleBase64, NULL) );
       // break ;
    }
#endif

    return 0;
}


/*static*/ BOOL ReadMemory::read(HANDLE /*hProcess*/, DWORD64 qwBaseAddress, PVOID lpBuffer, DWORD nSize, PDWORD lpNumberOfBytesRead)
{
    //std::cout << "> " << std::hex << qwBaseAddress << "+" << nSize << std::endl;
    // Find memory range containing the address
    if( s_self->memory )
    {
        for(ULONG32 i = 0; i < s_self->memory->NumberOfMemoryRanges; i ++)
        {
            const auto& rng = s_self->memory->MemoryRanges[i];
            //std::cout << "- " << std::hex << rng.StartOfMemoryRange << "+" << rng.Memory.DataSize << std::endl;

            if( rng.StartOfMemoryRange <= qwBaseAddress && qwBaseAddress <= rng.StartOfMemoryRange + rng.Memory.DataSize )
            {
                auto ofs = qwBaseAddress - rng.StartOfMemoryRange;
                auto max_size = min(nSize, rng.Memory.DataSize - ofs);
                const auto* src = (const char*)s_self->view + rng.Memory.Rva + ofs;
                memcpy(lpBuffer, src, max_size);
                if(lpNumberOfBytesRead)
                    *lpNumberOfBytesRead = static_cast<DWORD>(max_size);
                return TRUE;
            }
        }
    }
    if( s_self->memory64 )
    {
        DWORD64 base_rva = s_self->memory64->BaseRva;
        for(ULONG32 i = 0; i < s_self->memory64->NumberOfMemoryRanges; i ++)
        {
            const auto& rng = s_self->memory64->MemoryRanges[i];
            //std::cout << "- " << (void*)base_rva << " " << std::hex << rng.StartOfMemoryRange << "+" << rng.DataSize << std::endl;

            if( rng.StartOfMemoryRange <= qwBaseAddress && qwBaseAddress <= rng.StartOfMemoryRange + rng.DataSize )
            {
                auto ofs = qwBaseAddress - rng.StartOfMemoryRange;
                auto max_size = min(nSize, rng.DataSize - ofs);
                const auto* src = (const char*)s_self->view + base_rva + ofs;
                memcpy(lpBuffer, src, max_size);
                if(lpNumberOfBytesRead)
                    *lpNumberOfBytesRead = static_cast<DWORD>(max_size);
                //if( nSize == 8 )
                //    std::cout << "> " << std::hex << qwBaseAddress << "+" << nSize << " = " << *(void**)lpBuffer << std::endl;
                return TRUE;
            }

            base_rva += rng.DataSize;
        }
    }
    std::cout << "> " << std::hex << qwBaseAddress << "+" << nSize << " == Not found" << std::dec << std::endl;
    return FALSE;
}



TypeRef::t_cache    TypeRef::s_cache;

/*static*/ TypeRef TypeRef::lookup(HANDLE hProcess, ULONG64 mod_base, DWORD type_id)
{
    auto it = s_cache.find(type_id);
    if( it != s_cache.end() )
        return it->second;

    auto get_dword = [=](const char* name, IMAGEHLP_SYMBOL_TYPE_INFO info_ty) {
        DWORD v;
        if( !SymGetTypeInfo(hProcess, mod_base, type_id, info_ty, &v) )
        {
            throw std::runtime_error(FMT_STRING("#" << type_id << " " << name << "=" << WinErrStr(GetLastError())));
        }
        return v;
        };
    auto get_str = [=](const char* name, IMAGEHLP_SYMBOL_TYPE_INFO info_ty)->WideStrLocalFree {
        WCHAR* v;
        if( !SymGetTypeInfo(hProcess, mod_base, type_id, info_ty, &v) )
        {
            throw std::runtime_error(FMT_STRING("#" << type_id << " " << name << "=" << WinErrStr(GetLastError())));
        }
        return WideStrLocalFree(v);
        };


    DWORD   symtag = get_dword("SYMTAG", TI_GET_SYMTAG);

    TypeRef rv;
    switch(symtag)
    {
    case SymTagPointerType:
        rv = TypeRef::lookup(hProcess, mod_base, get_dword("TYPE", TI_GET_TYPE));
        rv.wrappers.push_back(Wrapper::new_pointer());
        break;
    case SymTagArrayType:
        rv = TypeRef::lookup(hProcess, mod_base, get_dword("TYPE", TI_GET_TYPE));
        rv.wrappers.push_back(Wrapper(get_dword("COUNT", TI_GET_COUNT)));
        break;
    case SymTagEnum:
        rv = TypeRef::lookup(hProcess, mod_base, get_dword("TYPE", TI_GET_TYPE));
        break;
    case SymTagBaseType:
        rv.m_class = ClassBasic;
        rv.m_data.basic.bt = static_cast<BasicType>(get_dword("BASETYPE", TI_GET_BASETYPE));
        rv.m_data.basic.size = static_cast<uint8_t>(get_dword("LENGTH", TI_GET_LENGTH));
        break;
    case SymTagUDT:
        rv.m_class = ClassUdt;
        rv.m_data.udt = TypeDefinition::from_syminfo(hProcess, mod_base, type_id);
        assert(rv.m_data.udt);
        break;
    case SymTagFunctionType:
        rv.m_class = ClassMisc;
        rv.m_data.misc.name = _strdup(FMT_STRING(get_str("SYMNAME", TI_GET_SYMNAME)).c_str());
        rv.m_data.misc.size = static_cast<uint8_t>(get_dword("LENGTH", TI_GET_LENGTH));
        break;
    case SymTagVTableShape:
        rv.m_class = ClassUdt;
        rv.m_data.udt = TypeDefinition::from_syminfo(hProcess, mod_base, type_id);
        assert(rv.m_data.udt);
        break;
    default:
        throw std::runtime_error(FMT_STRING("#" << type_id << " SYMTAG=" << symtag));
    }
    s_cache.insert(std::make_pair(type_id, rv));
    return rv;
}
/*static*/ TypeRef TypeRef::lookup_by_name(const std::string& name)
{
    for(const auto& e : s_cache)
    {
        if( e.second.is_udt(name.c_str()) )
        {
            return e.second;
        }
    }
    throw ::std::runtime_error("Type not loaded");
}

bool TypeRef::is_udt(const char* name) const {
    return wrappers.empty() && m_class == ClassUdt && m_data.udt->name == name;
}
bool TypeRef::is_udt_suffix(const char* name) const {
    return wrappers.empty() && m_class == ClassUdt && m_data.udt->name.compare(0, strlen(name), name) == 0;
}

void TypeRef::fmt(std::ostream& os, unsigned recurse_depth, unsigned indent_level/*=0*/) const
{
    for(const auto& w : wrappers)
    {
        if( w.is_pointer() ) {
            os << "*ptr ";
        }
        else {
            os << "[" << w.count << "]";
        }
    }
    switch(m_class)
    {
    case ClassBasic:
        switch(m_data.basic.bt)
        {
        case btChar:    os << "char";   return;
        case btInt:     os << "i" << (m_data.basic.size * 8);    return;
        case btUint:    os << "u" << (m_data.basic.size * 8);   return;
        case btFloat:   os << "f" << (m_data.basic.size * 8);  return;
        case btBool:    os << "bool";   return;
        }
        os << "bt#" << m_data.basic.bt;
        break;
    case ClassUdt:
        m_data.udt->fmt(os, recurse_depth, indent_level);
        break;
    case ClassMisc:
        os << m_data.misc.name;
        break;
    }
}

size_t TypeRef::size() const
{
    size_t  mul = 1;
    for(auto it = wrappers.rbegin(); it != wrappers.rend(); ++it)
    {
        if( it->is_pointer() )
        {
            return mul*8;
        }
        mul *= it->count;
    }

    switch(m_class)
    {
    case ClassBasic:
        return m_data.basic.size;
    case ClassUdt:
        return mul * m_data.udt->size;
    }
    throw std::runtime_error(FMT_STRING("Unknown size for " << *this));
}

TypeRef TypeRef::get_field(std::initializer_list<const char*> fields, size_t* out_ofs/*=nullptr*/) const
{
    //std::cout << "get_field() > " << *this << std::endl;
    auto cur = *this;
    size_t ofs = 0;
    for(const char* fld_name : fields)
    {
        if( fld_name )
        {
            //std::cout << "get_field(): " << fld_name << " in " << *cur << std::endl;
            if(!cur.wrappers.empty())
                throw std::runtime_error(FMT_STRING("Getting field of " << cur));
            if(cur.m_class != ClassUdt)
                throw std::runtime_error(FMT_STRING("Getting field of " << cur));
            bool found = false;
            for(const auto& fld : cur.m_data.udt->fields)
            {
                if( fld.name == fld_name ) {
                    ofs += fld.offset;
                    cur = fld.ty;
                    found = true;
                    break;
                }
            }
            if(!found)
                throw std::runtime_error(FMT_STRING("Cannot find field '" << fld_name << "' in " << cur));
        }
        else
        {
            // Deref
            if(cur.wrappers.empty() || !cur.wrappers.back().is_pointer())
                throw std::runtime_error(FMT_STRING("Deref of " << cur));
            if(out_ofs)
                throw std::runtime_error("Dereference can't get offset");
            cur = cur.deref();
        }
    }
    if(out_ofs)
        *out_ofs = ofs;
    return cur;
}
TypeRef TypeRef::deref() const
{
    if(this->wrappers.empty())
        throw std::runtime_error(FMT_STRING("Dereferencing " << *this));
    auto rv = *this;
    rv.wrappers.pop_back();
    return rv;
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

/*static*/ TypeDefinition* TypeDefinition::from_syminfo(HANDLE hProcess, ULONG64 mod_base, DWORD type_id)
{
    static Indent s_indent = Indent(" ");
#define DO_DEBUG(v) do { if(ENABLE_DEBUG_SYMINFO) { std::cout << s_indent << v << std::endl; } } while(0)
    static std::unordered_map<DWORD, TypeDefinition*>   s_anti_recurse;
    if( s_anti_recurse.find(type_id) != s_anti_recurse.end() ) {
        DO_DEBUG("Recursion on #" << type_id);
        return s_anti_recurse.at(type_id);
    }

    DO_DEBUG("TypeDefinition::from_syminfo(" << type_id << ")");
    auto _ = s_indent.inc();

    auto get_raw = [hProcess,mod_base](DWORD type_id, const char* name, IMAGEHLP_SYMBOL_TYPE_INFO info_ty, void* out) {
        if( !SymGetTypeInfo(hProcess, mod_base, type_id, info_ty, out) )
        {
            throw std::runtime_error(FMT_STRING("#" << type_id << " " << name << "=" << WinErrStr(GetLastError())));
        }
    };
    auto get_dword = [=](DWORD child_type_id, const char* name, IMAGEHLP_SYMBOL_TYPE_INFO info_ty) {
        DWORD v;
        get_raw(child_type_id, name, info_ty, &v);
        return v;
    };
    auto get_str = [=](DWORD child_type_id, const char* name, IMAGEHLP_SYMBOL_TYPE_INFO info_ty)->WideStrLocalFree {
        WCHAR* v;
        get_raw(child_type_id, name, info_ty, &v);
        return WideStrLocalFree(v);
    };

    auto rv = ::std::unique_ptr<TypeDefinition>(new TypeDefinition);

    auto symtag = get_dword(type_id, "SYMTAG", TI_GET_SYMTAG);
    if( symtag == SymTagUDT ) {
        rv->name = FMT_STRING(get_str(type_id, "SYMNAME", TI_GET_SYMNAME));
        rv->size = get_dword(type_id, "LENGTH", TI_GET_LENGTH);
    }
    else if( symtag == SymTagVTableShape ) {
        rv->name = FMT_STRING("VTABLE #" << type_id);
        rv->size = 0;
    }
    else {
        throw std::runtime_error(FMT_STRING("BUG #" << type_id << " SYMTAG=" << get_dword(type_id, "SYMTAG", TI_GET_SYMTAG)));
    }

    s_anti_recurse.insert(std::make_pair(type_id, rv.get()));

    DWORD child_count = get_dword(type_id, "CHILDRENCOUNT", TI_GET_CHILDRENCOUNT);
    DO_DEBUG(rv->name << " child_count=" << child_count);
    if(child_count > 0)
    {
        try
        {
            std::vector<ULONG>  buf( (sizeof(TI_FINDCHILDREN_PARAMS) + (child_count-1) * sizeof(ULONG))/sizeof(ULONG) );
            auto* child_params = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(buf.data());
            child_params->Count = child_count;
            child_params->Start = 0;

            get_raw(type_id, "FINDCHILDREN", TI_FINDCHILDREN, child_params);

            for(DWORD i = 0; i < child_count; i ++)
            {
                auto child_type_id = child_params->ChildId[i];
                auto child_tag = get_dword(child_type_id, "SYMTAG", TI_GET_SYMTAG);
                switch(child_tag)
                {
                case SymTagData: {
                    auto data_kind = get_dword(child_type_id, "DATAKIND", TI_GET_DATAKIND);
                    //DO_DEBUG(i << ":" << child_type_id << " " << child_tag << "/" << data_kind);
                    switch(data_kind)
                    {
                    case 7: { // Field?
                        Field   f;
                        // TODO: Sometimes this fails with "Incorrect function"
                        f.name = FMT_STRING(get_str(child_type_id, "SYMNAME", TI_GET_SYMNAME));
                        f.offset = get_dword(child_type_id, "OFFSET", TI_GET_OFFSET);
                        f.ty = TypeRef::lookup(hProcess, mod_base, get_dword(child_type_id, "TYPE", TI_GET_TYPE));
                        rv->fields.push_back(std::move(f));
                        } break;
                    case 8: // Constant?
                        // Seen for `npos` on std::string
                        break;
                    default:
                        throw std::runtime_error(FMT_STRING("#" << type_id << "->" << child_type_id << " DATAKIND=" << data_kind));
                    }
                    } break;
                case SymTagFunction:
                    //DO_DEBUG(rv->name << " fn " << get_str(child_type_id, "SYMNAME", TI_GET_SYMNAME));
                    break;
                case SymTagBaseClass:
                    // TODO: Recuse into this type (or look it up?)
                    //DO_DEBUG(rv->name << " => " << child_type_id);
                    DO_DEBUG(rv->name << " => " << get_str(child_type_id, "SYMNAME", TI_GET_SYMNAME));
                    {
                        DO_DEBUG(">> " << child_type_id << " " << get_dword(child_type_id, "TYPE", TI_GET_TYPE));
                        auto p = TypeRef::lookup(hProcess, mod_base, get_dword(child_type_id, "TYPE", TI_GET_TYPE));
                        switch(p.m_class)
                        {
                        case TypeRef::ClassUdt:
                            for(auto& f : p.m_data.udt->fields)
                            {
                                rv->fields.push_back(f);
                            }
                            break;
                        }
                    }
                    break;
                case SymTagVTable:{ // VTable: Add as field
                    Field   f;
                    f.name = "#VTABLE";
                    f.offset = get_dword(child_type_id, "OFFSET", TI_GET_OFFSET);
                    DO_DEBUG(">> VT " << child_type_id << " " << get_dword(child_type_id, "TYPE", TI_GET_TYPE));
                    f.ty = TypeRef::lookup(hProcess, mod_base, get_dword(child_type_id, "TYPE", TI_GET_TYPE));
                    rv->fields.push_back(std::move(f));
                    } break;
                case SymTagTypedef:
                    break;
                case SymTagEnum:
                    break;
                // Why? Inline structure?
                case SymTagUDT:
                    break;
                default:
                    throw std::runtime_error(FMT_STRING("#" << type_id << "->" << child_type_id << " SYMTAG=" << child_tag));
                }
            }
        }
        catch(const std::exception& e)
        {
            ::std::cout << "Exception " << e.what() << " in TypeDefinition::from_syminfo(" << type_id << "): " << rv->name << std::endl;
            //rv->
        }
    }
    DO_DEBUG("TypeDefinition::from_syminfo(" << type_id << "): " << rv->name);

    s_anti_recurse.erase(type_id);
    return rv.release();
#undef DO_DEBUG
}

void TypeDefinition::fmt(std::ostream& os, unsigned recurse_depth, unsigned indent_level/*=0*/) const
{
    os << this->name;
    if(recurse_depth > 0)
    {
        static std::vector<const TypeDefinition*>   s_stack;
        if(std::find(s_stack.begin(), s_stack.end(), this) != s_stack.end()) {
            os << " { ^ }";
            return ;
        }
        s_stack.push_back(this);
        os << " { ";
        auto print_indent = [&]() {
            if(indent_level > 0)
            {
                os << "\n";
                for(unsigned i = 1; i < indent_level; i++)
                    os << "    ";
            }
            };
        if(indent_level > 0)
        {
            indent_level += 1;
        }
        for(const auto& fld : this->fields)
        {
            print_indent();
            os << fld.name << " @ " << fld.offset << "+" << fld.ty.size() << ": ";
            fld.ty.fmt(os, recurse_depth-1, indent_level);
            os << ", ";
        }
        if(indent_level > 0)
        {
            indent_level -= 1;
            print_indent();
        }
        os << "}";
        s_stack.pop_back();
    }
}

namespace  {
    bool is_tagged_union(const TypeRef& ty) {
        if( const auto* udt = ty.any_udt() ) {
            const auto& flds = udt->fields;
            if( flds.size() == 2
                && flds[0].name == "m_tag" && flds[0].ty.is_basic(btInt)
                && flds[1].name == "m_data" && flds[1].ty.any_udt() && flds[1].ty.any_udt()->name == udt->name + "::DataUnion"
                )
            {
                return true;
            }
        }
        return false;
    }
};

std::string MemoryStats::get_enum_key(const TypeRef& ty, DWORD64 addr) const
{
    if( is_tagged_union(ty) )
    {
        const auto* udt = ty.any_udt();
        const auto& flds = udt->fields;
        const auto* data_udt = flds[1].ty.any_udt();

        //std::cout << ty << " is a TAGGED_UNION @ " << (void*)addr << std::endl;
        auto tag = ReadMemory::read_u32(addr);
        std::stringstream   rv;
        rv << udt->name << "#";
        if( tag == 0 ) {
            rv << "DEAD";
        }
        else if( tag-1 < data_udt->fields.size() ) {
            rv << data_udt->fields[tag-1].name;
            rv << " (" <<data_udt->fields[tag-1].ty.size() << ")";
        }
        else {
            rv << "?" << tag;
        }
        return std::move(rv.str());
    }
    return "";
}

const TypeRef& MemoryStats::get_real_type(const TypeRef& ty, DWORD64 addr) const
{
    const TypeRef* typ = &ty;
    for(;;)
    {
        assert(typ);
        const auto& ty = *typ;
        typ = nullptr;

        if( const auto* udt = ty.any_udt() )
        {
            if( udt->fields.size() > 0 && udt->fields[0].name == "#VTABLE" ) {
                auto vtable = ReadMemory::read_ptr(addr + udt->fields[0].offset);
                //std::cout << s_indent << ty << " vtable = " << (void*)vtable << std::endl;
                // Look up the actual type from a list of vtables
                auto it = g_vtable_cache.find(vtable);
                if( it != g_vtable_cache.end() )
                {
                    if( it->second.any_udt() != udt )
                    {
                        typ = &it->second;
                        continue ;
                    }
                    else
                    {
                        // Recursion!
                        //throw std::runtime_error(FMT_STRING("VTABLE recursion on " << ty));
                    }
                }
                else
                {
                    // VTable not found!
                    throw std::runtime_error(FMT_STRING("VTABLE " << ty << " 0x" << std::hex << vtable << " not known"));
                }
            }
        }
        return ty;
    }
}

bool MemoryStats::are_equal(const TypeRef& val_ty, DWORD64 addr1, DWORD64 addr2)
{
    // Recurse on fields
    const auto& ty = this->get_real_type(val_ty, addr1);
    const auto& ty2 = this->get_real_type(val_ty, addr2);
    // If the real type (concrete of a virtual type) is different, return early
    if( &ty != &ty2 ) {
        return false;
    }
    //std::cout << ty << " @ " << (void*)addr1 << " & " << (void*)addr2 << std::endl;

    // HACK: Compare pointers literally
    if( ty.is_any_basic() || (!ty.wrappers.empty() && ty.wrappers.back().is_pointer()) ) {
        DWORD64 v1 = 0;
        DWORD64 v2 = 0;
        ReadMemory::read(NULL, addr1, &v1, static_cast<DWORD>(ty.size()), NULL);
        ReadMemory::read(NULL, addr2, &v2, static_cast<DWORD>(ty.size()), NULL);
        //std::cout << ty << " " << v1 << " ?= " << v2 << std::endl;
        return v1 == v2;
    }
    else if( is_tagged_union(ty) ) {
        const auto* udt = ty.any_udt();
        const auto& flds = udt->fields;
        const auto* data_udt = flds[1].ty.any_udt();
        auto tag1 = ReadMemory::read_u32(addr1) - 1;
        auto tag2 = ReadMemory::read_u32(addr2) - 1;
        if( tag1 != tag2 )
            return false;

        if( tag1 + 1 == 0 || tag2 + 1 == 0 ) {
            // Dead, just log
            //DO_DEBUG("TAGGED_UNION " << ty << " #DEAD");
        }
        else if( tag1 < data_udt->fields.size() && tag2 < data_udt->fields.size() ) {
            //DO_DEBUG("TAGGED_UNION " << ty << " #" << tag << " : " << data_udt->fields[tag].name);
            return this->are_equal(data_udt->fields[tag1].ty, addr1 + flds[1].offset, addr2 + flds[1].offset);
        }
        else {
            throw std::runtime_error(FMT_STRING("TAGGED_UNION " << ty << " #" << tag1 << " || #" << tag2));
        }
    }
    // std::vector
    else if( ty.is_udt_suffix("std::vector<") ) {
        auto inner_ty = ty.get_field({"_Mypair", "_Myval2", "_Myfirst"}).deref();
        auto inner_size = inner_ty.size();
        try
        {
            auto v1 = get_checked_vector(addr1, inner_size);
            auto v2 = get_checked_vector(addr2, inner_size);
            auto s1 = v1.second - v1.first;
            auto s2 = v2.second - v2.first;
            //std::cout << " > " << s1 << " & " << s2 << std::endl;
            if( s1 != s2 )
                return false;
            for(auto cur1 = v1.first, cur2 = v2.first; cur1 < v1.second; cur1 += inner_size, cur2 += inner_size)
            {
                if( !this->are_equal(inner_ty, cur1, cur2) )
                    return false;
            }
            return true;
        }
        catch(...)
        {
        }
    }
    // TODO: Further checks
    else {
        // TODO: What about unions?
        // Enumerate inner types of a struct
        if( ty.wrappers.empty() && ty.m_class == TypeRef::ClassUdt )
        {
            // TODO: If this is a union, don't recurse!
            bool is_union = false;
            for(const auto& f : ty.m_data.udt->fields)
            {
                for(const auto& f2 : ty.m_data.udt->fields)
                {
                    if(&f == &f2)
                        continue;
                    if(f.offset == f2.offset) {
                        is_union = true;
                        break;
                    }
                }
                if(is_union) {
                    break;
                }
            }
            if( !is_union ) {
                for(const auto& f : ty.m_data.udt->fields)
                {
                    if( !this->are_equal(f.ty, addr1 + f.offset, addr2 + f.offset) )
                        return false;
                }
                return true;
            }
        }
    }
    return false;
}

void MemoryStats::enum_type_at(const TypeRef& val_ty, DWORD64 addr, bool is_top_level/*=true*/)
{
    static Indent s_indent = Indent(" ");
#define DO_DEBUG_F(force, v) do { if(force || (true && s_indent.level < 5)) { std::cout << s_indent << v << std::endl; } } while(0)
#define DO_DEBUG(v) DO_DEBUG_F(false, v)
    auto _ = s_indent.inc();
    try {
    DO_DEBUG("enum_type_at: >> " << val_ty << ", " << (void*)addr);
    const auto& ty = this->get_real_type(val_ty, addr);

    // 1. Get the enumeration key for the instance (e.g. TypeRef(Prim) or AST::Item(Fcn))
    // - If none, skip
    auto enum_key = this->get_enum_key(ty, addr);
    if(enum_key != "")
    {
        m_counts[enum_key] ++;
    }

    // TODO: For certain types, check if de-duplication is needed
    if( true && ty.is_udt("HIR::SimplePath") )
    {
        auto dbg_name = virt_types::fmt_simplepath(ty, addr);

        auto& list = m_duplicates_SimplePath;
        auto it = list.insert( ::std::make_pair(dbg_name, std::make_pair(addr, 0)) );
        it.first->second.second += 1;

        m_counts[FMT_STRING("~TOTAL " << ty << " [" << ty.size() << "]")] ++;
    }
    if( true && ty.is_udt("HIR::TypeRef") )
    {
        if(true)
        {
            auto dbg_name = virt_types::fmt_typeref(ty, addr);

            auto& list = m_duplicates_TypeRef;
            auto it = list.insert( ::std::make_pair(dbg_name, std::make_pair(addr, 0)) );
            it.first->second.second += 1;
        }

        m_counts[FMT_STRING("~TOTAL " << ty << " [" << ty.size() << "]")] ++;
    }

    // IDEA: If top-level (either pointed-to or part of a vector), then count raw type
    if( is_top_level )
    {
        m_counts[FMT_STRING(ty << " [" << ty.size() << "]")] ++;
        m_counts["~TOTAL"] += static_cast<unsigned>(ty.size());
    }

    // 2. Recurse (using special handlers if required)
    if( ty.is_any_basic() || (!ty.wrappers.empty() && ty.wrappers.back().is_pointer()) ) {
        DWORD64 v = 0;
        ReadMemory::read(NULL, addr, &v, static_cast<DWORD>(ty.size()), NULL);
        DO_DEBUG("enum_type_at: " << ty << " @ " << (void*)addr << " = " << (void*)v);
    }
    else if( !ty.wrappers.empty() ) {
        // What should happen with arrays?
    }
    else if( is_tagged_union(ty) ) {
        const auto* udt = ty.any_udt();
        const auto& flds = udt->fields;
        const auto* data_udt = flds[1].ty.any_udt();
        auto tag = ReadMemory::read_u32(addr) - 1;
        if( tag + 1 == 0 ) {
            // Dead, just log
            DO_DEBUG("TAGGED_UNION " << ty << " #DEAD");
        }
        else if( tag < data_udt->fields.size() ) {
            DO_DEBUG("TAGGED_UNION " << ty << " #" << tag << " : " << data_udt->fields[tag].name);
            this->enum_type_at(data_udt->fields[tag].ty, addr + flds[1].offset, /*is_top_level=*/false);
        }
        else {
            throw std::runtime_error(FMT_STRING("TAGGED_UNION " << ty << " #" << tag));
        }
    }
    // std::string
    else if( ty.is_udt("std::basic_string<char,std::char_traits<char>,std::allocator<char> >") ) {
        auto cap = ReadMemory::read_u32(addr + 24);
        if( cap < 16 ) {
        }
        else {
        }
    }
    // std::vector
    else if( ty.is_udt_suffix("std::vector<") ) {
        auto start = ReadMemory::read_ptr(addr + 0*8);
        auto end = ReadMemory::read_ptr(addr + 1*8);
        auto max = ReadMemory::read_ptr(addr + 2*8);
        auto inner_ty = ty.get_field({"_Mypair", "_Myval2", "_Myfirst"}).deref();
        auto inner_size = inner_ty.size();
        if( start <= end && end <= max
            && (end - start) % inner_size == 0 && (max - start) % inner_size == 0
            && start != 0
            )
        {
            auto len = (end - start) / inner_size;
            auto cap = (max - start) / inner_size;
            DO_DEBUG("std::vector< " << inner_ty << " >: " << (void*)start << "+" << len << "<" << cap);
            for(auto cur = start; cur < end; cur += inner_size)
            {
                this->enum_type_at(inner_ty, cur, /*is_top_level=*/true);
            }
        }
        else
        {
            // Invalid!
            DO_DEBUG("std::vector< " << inner_ty << " >: BAD " << (void*)start << "-" << (void*)end << "-" << (void*)max);
        }
        m_counts[FMT_STRING(">" << ty)] ++;
    }
    // std::map
    else if( ty.is_udt_suffix("std::map<") ) {

        auto node_ty = ty.get_field({"_Mypair", "_Myval2", "_Myval2", "_Myhead", nullptr});
        size_t val_ofs = 0;
        auto inner_ty = node_ty.get_field({"_Myval"}, &val_ofs);

        auto head = ReadMemory::read_ptr(addr + 0*8);
        auto size = ReadMemory::read_ptr(addr + 1*8);
        DO_DEBUG("std::map< " << inner_ty << " >: head=" << (void*)head << ", size=" << size);
        if(size > 0)
        {
            struct Node {
                DWORD64 addr;

                DWORD64 left_addr;
                DWORD64 parent_addr;
                DWORD64 right_addr;
                bool    is_nil;

                static Node read(DWORD64 addr) {
                    Node    rv;
                    rv.addr = addr;
                    rv.left_addr = ReadMemory::read_ptr(addr + 0*8);
                    rv.parent_addr = ReadMemory::read_ptr(addr + 1*8);
                    rv.right_addr = ReadMemory::read_ptr(addr + 2*8);
                    rv.is_nil = (ReadMemory::read_u8(addr + 25) != 0);
                    DO_DEBUG("Node(@" << (void*)rv.addr << " " << (void*)rv.left_addr << "," << (void*)rv.right_addr << " " << (rv.is_nil ? "NIL" : "") << ")");
                    return rv;
                }
                Node leftmost() const {
                    assert(!this->is_nil);
                    auto cur_n = *this;
                    while(true)
                    {
                        auto n = Node::read(cur_n.left_addr);
                        if(n.is_nil)
                            break;
                        cur_n = n;
                    }
                    return cur_n;
                }
            };
            // Get left, check if it's NIL
            Node cur_n = Node::read(head);
            cur_n = Node::read(cur_n.left_addr);
            while(!cur_n.is_nil)
            {
                assert(size-- > 0);
                this->enum_type_at(inner_ty, cur_n.addr + val_ofs, true);


                auto r_n = Node::read(cur_n.right_addr);
                if(!r_n.is_nil) {
                    // Go to into right (to the leftmost)
                    cur_n = r_n.leftmost();
                }
                else {
                    Node parent_n;
                    parent_n = Node::read(cur_n.parent_addr);
                    // If parent isn't NIL, and the parent's right is the current
                    // - Go up again
                    while( !parent_n.is_nil && cur_n.addr == parent_n.right_addr )
                    {
                        cur_n = parent_n;
                        parent_n = Node::read(cur_n.parent_addr);
                    }
                    cur_n = parent_n;
                }
            }
        }
        m_counts[FMT_STRING(">" << ty)] ++;
    }
    // std::unordered_map
    else if( ty.is_udt_suffix("std::unordered_map<") ) {
        auto list_ty = ty.get_field({"_List", "_Mypair", "_Myval2"});
        TypeRef list_node_ptr_ty;
        size_t head_ofs = ty.get_field_ofs({"_List", "_Mypair", "_Myval2", "_Myhead"}, &list_node_ptr_ty);
        size_t size_ofs = ty.get_field_ofs({"_List", "_Mypair", "_Myval2", "_Mysize"});

        size_t val_ofs;
        auto val_ty = list_node_ptr_ty.deref().get_field({"_Myval"}, &val_ofs);
        auto head_node = ReadMemory::read_ptr(addr + head_ofs);
        auto size = ReadMemory::read_ptr(addr + size_ofs);
        DO_DEBUG("std::unordered_map< " << val_ty << " >: head=" << (void*)head_node << ", size=" << size);
        if(size > 0)
        {
            // First node doesn't contain data (I assume it's a pointer to the start/end?)
            auto cur_node = ReadMemory::read_ptr(head_node + 0*8);
            while(cur_node != head_node)
            {
                auto next_addr = ReadMemory::read_ptr(cur_node + 0*8);
                auto prev_addr = ReadMemory::read_ptr(cur_node + 1*8);
                auto data_addr = cur_node + val_ofs;
                DO_DEBUG("+ " << (void*)next_addr << "," << (void*)prev_addr << " D@" << (void*)data_addr);
                this->enum_type_at(val_ty, data_addr, true);
                cur_node = next_addr;
            }
        }
        m_counts[FMT_STRING(">" << ty)] ++;
    }
    // unique_ptr
    else if( ty.is_udt_suffix("std::unique_ptr<") ) {
        auto ptr = ReadMemory::read_ptr(addr);
        auto inner_ty = ty.get_field({"_Mypair", "_Myval2", nullptr});
        DO_DEBUG("std::unique_ptr< " << inner_ty << " >: " << (void*)ptr);
        if(ptr)
        {
            this->enum_type_at(inner_ty, ptr, true);
        }
        m_counts[FMT_STRING(">" << ty)] ++;
    }
    // shared_ptr
    else if( ty.is_udt_suffix("std::shared_ptr<") ) {
        auto ptr = ReadMemory::read_ptr(addr);
        auto inner_ty = ty.get_field({"_Ptr", nullptr});
        DO_DEBUG("std::shared_ptr< " << inner_ty << " >: " << (void*)ptr);
        if(ptr)
        {
            static std::set<DWORD64>    s_shared_ptrs;
            if( s_shared_ptrs.count(ptr) == 0 )
            {
                s_shared_ptrs.insert(ptr);
                this->enum_type_at(inner_ty, ptr, true);
            }
        }
        m_counts[FMT_STRING(">" << ty)] ++;
    }
    // HIR::CratePtr
    else if( ty.is_udt("HIR::CratePtr") ) {
        auto ptr = ReadMemory::read_ptr(addr);
        auto inner_ty = ty.get_field({"m_ptr", nullptr});
        DO_DEBUG("HIR::CratePtr: " << inner_ty << " @ " << (void*)ptr);
        if(ptr)
        {
            this->enum_type_at(inner_ty, ptr, true);
        }
        m_counts[FMT_STRING(">" << ty)] ++;
    }
    // HIR::ExprPtrInner
    else if( ty.is_udt("HIR::ExprPtrInner") ) {
        auto ptr = ReadMemory::read_ptr(addr);
        auto inner_ty = ty.get_field({"ptr", nullptr});
        DO_DEBUG("HIR::ExprPtrInner: " << inner_ty << " @ " << (void*)ptr);
        if(ptr)
        {
            this->enum_type_at(inner_ty, ptr, true);
        }
        m_counts[FMT_STRING(">" << ty)] ++;
    }
    // HIR::ExprPtrInner
    else if( ty.is_udt("MIR::FunctionPointer") ) {
        auto ptr = ReadMemory::read_ptr(addr);
        auto inner_ty = ty.get_field({"ptr", nullptr});
        DO_DEBUG("MIR::FunctionPointer: " << inner_ty << " @ " << (void*)ptr);
        if(ptr)
        {
            this->enum_type_at(inner_ty, ptr, true);
        }
        m_counts[FMT_STRING(">" << ty)] ++;
    }
    else {
        // TODO: What about unions?
        // Enumerate inner types of a struct
        if( ty.wrappers.empty() && ty.m_class == TypeRef::ClassUdt )
        {
            // TODO: If this is a union, don't recurse!
            bool is_union = false;
            for(const auto& f : ty.m_data.udt->fields)
            {
                for(const auto& f2 : ty.m_data.udt->fields)
                {
                    if(&f == &f2)
                        continue;
                    if(f.offset == f2.offset) {
                        is_union = true;
                        break;
                    }
                }
                if(is_union) {
                    break;
                }
            }
            if( !is_union ) {
                for(const auto& f : ty.m_data.udt->fields)
                {
                    DO_DEBUG("enum_type_at: " << (void*)addr << "+" << f.offset << " " << f.name << ": " << f.ty);
                    enum_type_at(f.ty, addr + f.offset, /*is_top_level=*/false);
                }
            }
        }
    }
    DO_DEBUG("enum_type_at: << " << ty << ", " << (void*)addr);
    }
    catch(const std::exception& e)
    {
        DO_DEBUG_F(true, "enum_type_at: << " << val_ty << ", " << (void*)addr << " == EXCEPTION: " << e.what());
    }
#undef DO_DEBUG
}
