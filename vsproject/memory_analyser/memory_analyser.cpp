// memory_analyser.cpp : This file contains the 'main' function. Program execution begins and ends there.

#undef NDEBUG
#include "error_handling.h"
#include "debug_windows.h"
#include "types.h"

#include <Windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <cassert>
#include <memory>   // unique_ptr
#include <algorithm>    // std::sort

//#include <cvconst.h>

const bool DUPLICATION_TypeRef = false;
const bool DUPLICATION_SimplePath = false;

struct MemoryStats
{
    /// TypeRef pointer values that have been seen (avoids double-visiting)
    std::unordered_set<uint64_t>    m_seen_types;

    std::map<std::string, unsigned>   m_counts;
    std::map<std::string, std::pair<DWORD64, unsigned> >   m_duplicates_SimplePath;
    std::map<std::string, std::pair<DWORD64, unsigned> >   m_duplicates_TypeRef;

    std::string get_enum_key(const TypeRef& ty, DWORD64 addr) const;
    const TypeRef& get_real_type(const TypeRef& ty, DWORD64 addr) const;
    bool are_equal(const TypeRef& ty, DWORD64 addr1, DWORD64 addr2);
    void enum_type_at(const TypeRef& ty, DWORD64 addr, bool is_top_level=true);
};


namespace {
    ::std::pair<DWORD64,DWORD64> get_checked_thinvector(DWORD64 addr, size_t inner_size)
    {
        auto meta_size = 2*8;
        auto meta_ofs_c = (meta_size + inner_size - 1) / inner_size;
        auto meta_ofs = meta_ofs_c * inner_size;
        auto start = ReadMemory::read_ptr(addr);
        auto meta_addr = start - meta_ofs;
        if( start == 0 ) {
            return std::make_pair(inner_size,inner_size);
        }
        else {
            auto len = ReadMemory::read_ptr(meta_addr);
            return std::make_pair(start, start + len * inner_size);
        }
    }
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
            if( addr == 0 ) {
                return "#NULL#";
            }
            auto refcnt = ReadMemory::read_u32(addr + 0);
            auto len = ReadMemory::read_u32(addr + 4);
            //auto ordering = ReadMemory::read_u32(addr + 8);
            //s += FMT_STRING("(@" << (void*)addr << " " << len << "/" << refcnt << ")");
            std::string s;
            s.resize(len);
            if( !ReadMemory::read(nullptr, addr + 12, const_cast<char*>(s.data()), len, nullptr) ) {
                ::std::cout << "fmt_rcstring: malformed (@" << (void*)addr << " " << len << "/" << refcnt << ")" << "\n";
                return FMT_STRING("fmt_rcstring: malformed (@" << (void*)addr << " " << len << "/" << refcnt << ")");
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
            auto inner_ty = ty.get_field({"m_members", "m_ptr"}).deref();
            auto inner_size = inner_ty.size();
            auto v = get_checked_thinvector( addr + ty.get_field_ofs({"m_members"}), inner_size );
            if( v.first == v.second ) {
                return "::\"\"";
            }
            else {
                std::string s;
                s += "::\"";
                s += fmt_rcstring(v.first);
                s += "\"";

                for(auto cur = v.first + inner_size; cur != v.second; cur += inner_size)
                {
                    s += "::";
                    s += fmt_rcstring(ReadMemory::read_ptr(cur));
                }
                return s;
            }
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
            const auto& ty_TypeInner_p = outer_ty.any_udt()->fields.at(0).ty;
            const auto ty_TypeInner = ty_TypeInner_p.deref();
            if( !ty_TypeInner.is_udt("HIR::TypeInner") ) {
                ::std::cerr << "Expected UDT HIR::TypeInner, got " << ty_TypeInner << std::endl;
                abort();
            }
            addr = ReadMemory::read_ptr(addr);
            auto ref_count = ReadMemory::read_u32(addr);
            addr += ty_TypeInner.any_udt()->fields.at(1).offset;
            const auto& ty = ty_TypeInner.any_udt()->fields.at(1).ty;
            if( !ty.is_udt("HIR::TypeData") ) {
                ::std::cerr << "Expected UDT HIR::TypeData, got " << ty << std::endl;
                abort();
            }
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
                            std::cout << "#" << pSymInfo->TypeIndex << " " << pSymInfo->Name << " UDT VTable " << v << std::endl;
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
    else {
        ::std::cout << "memory64_list: " << memory64_list->NumberOfMemoryRanges << " range\n";
    }

    // TODO: Dump all types?

    auto rm = ReadMemory { view, memory_list, memory64_list };

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
        if( DUPLICATION_SimplePath )
        {
            const auto& ty = TypeRef::lookup_by_name("HIR::SimplePath");
            const auto& list = el.ms.m_duplicates_SimplePath;
            std::cout << "Duplicates of " << ty << std::endl;
            ::std::vector<decltype(&*list.begin())>   vals;
            vals.reserve(list.size());
            for(auto it = list.begin(); it != list.end(); ++it) {
                vals.push_back(&*it);
            }
            ::std::sort(vals.begin(), vals.end(), [](const decltype(&*list.begin())& a, const decltype(&*list.begin())& b) {
                return a->second.second < b->second.second;
                });

            unsigned unique = 0;
            for(auto it : vals)
            {
                const auto& e = *it;
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
        if( DUPLICATION_TypeRef )
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
                    ::std::cout << "VTABLE " << " 0x" << std::hex << vtable << " for virtual type " << ty << " not known - assuming leaf type\n";
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
    // std::vector<bool>
    else if( ty.is_udt_suffix("std::vector<bool") ) {
        size_t ofs_vec, ofs_len;
        auto ty_vec = ty.get_field({"_Myvec"}, &ofs_vec);
        auto ty_len = ty.get_field({"_Myvec"}, &ofs_len);
        if( !this->are_equal(ty_vec, addr1+ofs_vec, addr2+ofs_vec) ) {
            return false;
        }
        if( !this->are_equal(ty_len, addr1+ofs_len, addr2+ofs_len) ) {
            return false;
        }
        return true;
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
    // std::vector
    else if( ty.is_udt_suffix("ThinVector<") ) {
        auto inner_ty = ty.get_field({"m_ptr"}).deref();
        auto inner_size = inner_ty.size();
        try
        {
            auto v1 = get_checked_thinvector(addr1, inner_size);
            auto v2 = get_checked_thinvector(addr2, inner_size);
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
#define DO_DEBUG_F(force, v) do { if(force || (true && s_indent.level < 4)) { std::cout << s_indent << v << std::endl; } } while(0)
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

            if( DUPLICATION_SimplePath )
            {
                auto& list = m_duplicates_SimplePath;
                auto it = list.insert( ::std::make_pair(dbg_name, std::make_pair(addr, 0)) );
                it.first->second.second += 1;
            }

            m_counts[FMT_STRING("~TOTAL " << ty << " [" << ty.size() << "]")] ++;
        }
        if( true && ty.is_udt("HIR::TypeRef") )
        {
            if( DUPLICATION_TypeRef )
            {
                auto dbg_name = virt_types::fmt_typeref(ty, addr);

                auto& list = m_duplicates_TypeRef;
                auto it = list.insert( ::std::make_pair(dbg_name, std::make_pair(addr, 0)) );
                it.first->second.second += 1;
            }

            m_counts[FMT_STRING("~TOTAL " << ty << " [" << ty.size() << "]")] ++;
        }
        if( true && ty.is_udt("HIR::TypeData") )
        {
            if( DUPLICATION_TypeRef )
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
        else if(enum_key != "")
        {
            // Actually, just count everything?
            m_counts[FMT_STRING(ty << " [" << ty.size() << "]")] ++;
        }

        // 2. Recurse (using special handlers if required)
        if( ty.is_any_basic() || (!ty.wrappers.empty() && ty.wrappers.back().is_pointer()) ) {
            DWORD64 v = 0;
            ReadMemory::read(NULL, addr, &v, static_cast<DWORD>(ty.size()), NULL);
            DO_DEBUG("enum_type_at: " << ty << " @ " << (void*)addr << " = " << (void*)v);
            //if( v ) {
            //    this->enum_type_at(ty.deref(), v, /*is_top_level=*/false);
            //}
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
        // std::vector<bool
        else if( ty.is_udt_suffix("std::vector<bool") ) {
            m_counts[FMT_STRING(">" << ty)] ++;
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
        // std::vector
        else if( ty.is_udt_suffix("ThinVector<") ) {
            auto inner_ty = ty.get_field({"m_ptr"}).deref();
            auto inner_size = inner_ty.size();

            auto meta_size = 2*8;
            auto meta_ofs_c = (meta_size + inner_size - 1) / inner_size;
            auto meta_ofs = meta_ofs_c * inner_size;
            auto start = ReadMemory::read_ptr(addr);
            auto meta_addr = start - meta_ofs;
            auto len = start == 0 ? 0 : ReadMemory::read_ptr(meta_addr);
            auto cap = start == 0 ? 0 : ReadMemory::read_ptr(meta_addr+8);
            auto end = start + len * inner_size;
            auto max = start + cap * inner_size;
            if( start <= end && end <= max
                && (end - start) % inner_size == 0 && (max - start) % inner_size == 0
                && start != 0
                )
            {
                DO_DEBUG("ThinVector< " << inner_ty << " >: " << (void*)start << "+" << len << " (end=" << (void*)end << ")");
                try {
                    if(len > 0) {
                        ReadMemory::read_u8(start);   // Triggers an exception if the read fails
                        ReadMemory::read_u8(end - inner_size);   // Triggers an exception if the read fails
                    }
                    for(auto cur = start; cur < end; cur += inner_size)
                    {
                        this->enum_type_at(inner_ty, cur, /*is_top_level=*/true);
                    }
                }
                catch(const std::exception& e)
                {
                    DO_DEBUG_F(true, "enum_type_at: << " << val_ty << ", " << (void*)addr << " == EXCEPTION: " << e.what());
                    DO_DEBUG_F(true, "was ThinVector< " << inner_ty << " >: " << (void*)start << "+" << len << " (end=" << (void*)end << ")");
                }
            }
            else
            {
                // Invalid!
                DO_DEBUG("ThinVector< " << inner_ty << " >: BAD " << (void*)start << "-" << (void*)end << "-" << (void*)max);
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
                        struct Copied {
                            DWORD64 left_addr;
                            DWORD64 parent_addr;
                            DWORD64 right_addr;
                            char pad[25 - 3*8];
                            char is_nil;
                        } v;
                        if( !ReadMemory::read(0, addr, &v, sizeof(v), nullptr) ) {
                            throw std::runtime_error(FMT_STRING("Can't read Node at " << (void*)addr));
                        }
                        Node    rv;
                        rv.addr = addr;
                        rv.left_addr = v.left_addr;
                        rv.parent_addr = v.parent_addr;
                        rv.right_addr = v.right_addr;
                        rv.is_nil = (v.is_nil != 0);
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
                // See implementation of `_Tree_unchecked_const_iterator` in `xtree`
                // Get left, check if it's NIL
                Node cur_n = Node::read(head);
                cur_n = Node::read(cur_n.left_addr);
                while(!cur_n.is_nil)
                {
                    assert(size-- > 0);
                    // Visit the type
                    this->enum_type_at(inner_ty, cur_n.addr + val_ofs, true);

                    // Increment iterator
                    auto r_n = Node::read(cur_n.right_addr);
                    if(!r_n.is_nil) {
                        // Go to into right (to the leftmost)
                        cur_n = r_n.leftmost();
                    }
                    else {
                        Node parent_n = Node::read(cur_n.parent_addr);
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
        else if( ty.is_udt("HIR::TypeRef") ) {
            auto ptr = ReadMemory::read_ptr(addr);
            auto inner_ty = ty.get_field({"m_ptr", nullptr});
            DO_DEBUG("HIR::TypeRef: " << inner_ty << " @ " << (void*)ptr);
            if(ptr)
            {
                if( !m_seen_types.insert(ptr).second ) {
                    this->enum_type_at(inner_ty, ptr, true);
                }
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
