/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/codegen_c.cpp
 * - Code generation emitting C code
 */
#include "codegen.hpp"
#include "mangling.hpp"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <hir/hir.hpp>
#include <mir/mir.hpp>
#include <hir_typeck/static.hpp>
#include <mir/helpers.hpp>
#include "codegen_c.hpp"
#include "target.hpp"
#include "allocator.hpp"

namespace {
    struct FmtShell
    {
        const ::std::string& s;
        bool is_win;
        FmtShell(const ::std::string& s, bool is_win=false):
            s(s),
            is_win(is_win)
        {
        }
    };
    class StringList
    {
        ::std::vector<::std::string>    m_cached;
        ::std::vector<const char*>  m_strings;
    public:
        StringList()
        {
        }
        StringList(const StringList&) = delete;
        StringList(StringList&&) = default;

        const ::std::vector<const char*>& get_vec() const
        {
            return m_strings;
        }

        void push_back(::std::string s)
        {
            // If the cache list is about to move, update the pointers
            if(m_cached.capacity() == m_cached.size())
            {
                // Make a bitmap of entries in `m_strings` that are pointers into `m_cached`
                ::std::vector<bool> b;
                b.reserve(m_strings.size());
                size_t j = 0;
                for(const auto* s : m_strings)
                {
                    if(j == m_cached.size())
                        break;
                    if(s == m_cached[j].c_str())
                    {
                        j ++;
                        b.push_back(true);
                    }
                    else
                    {
                        b.push_back(false);
                    }
                }

                // Add the new one
                m_cached.push_back(::std::move(s));
                // Update pointers
                j = 0;
                for(size_t i = 0; i < b.size(); i ++)
                {
                    if(b[i])
                    {
                        m_strings[i] = m_cached.at(j++).c_str();
                    }
                }
            }
            else
            {
                m_cached.push_back(::std::move(s));
            }
            m_strings.push_back(m_cached.back().c_str());
        }
        void push_back(const char* s)
        {
            m_strings.push_back(s);
        }
    };
}

::std::ostream& operator<<(::std::ostream& os, const FmtShell& x)
{
    if( x.is_win )
    {
        // TODO: Two layers of translation, one for CommandLineToArgW (inner), and one for cmd.exe (outer)
        for(auto it = x.s.begin(); it != x.s.end(); ++it)
        {
            switch (*it)
            {
            // Backslashes are only escaped if they would trigger an escape
            case '\"':
            case '^':
                os << "^";
            default:
                break;
            }
            os << *it;
        }
    }
    else
    {
        for (char c : x.s)
        {
            // Backslash and double quote need escaping
            switch(c)
            {
            case '\\':
            case '\"':
            case ' ':
                os << "\\";
            default:
                os << c;
            }
        }
    }
    return os;
}

namespace {
    struct MsvcDetection
    {
        ::std::string   path_vcvarsall;
    };

    MsvcDetection detect_msvc()
    {
        auto rv = MsvcDetection {
            "C:\\Program Files (x86)\\Microsoft Visual Studio 14.0\\VC\\vcvarsall.bat"
            };
        if( ::std::ifstream("P:\\Program Files (x86)\\Microsoft Visual Studio\\VS2015\\VC\\vcvarsall.bat").is_open() )
        {
            rv.path_vcvarsall = "P:\\Program Files (x86)\\Microsoft Visual Studio\\VS2015\\VC\\vcvarsall.bat";
        }
        return rv;
    }

    enum class AtomicOp
    {
        Add,
        Sub,
        And,
        Or,
        Xor
    };

    class CodeGenerator_C:
        public CodeGenerator
    {
        enum class MetadataType {
            None,
            Slice,
            TraitObject,
        };
        enum class Mode {
            //FullStd,
            Gcc,    // Use GCC/Clang extensions
            Msvc,   // Use MSVC extensions
        };

        enum class Compiler {
            Gcc,
            Msvc
        };

        static Span sp;

        const ::HIR::Crate& m_crate;
        ::StaticTraitResolve    m_resolve;

        ::std::string   m_outfile_path;
        ::std::string   m_outfile_path_c;

        ::std::ofstream m_of;
        const ::MIR::TypeResolve* m_mir_res;

        Compiler    m_compiler = Compiler::Gcc;
        struct {
            bool emulated_i128 = false;
            bool disallow_empty_structs = false;
        } m_options;

        ::std::map<::HIR::GenericPath, ::std::vector<unsigned>> m_enum_repr_cache;

        ::std::vector< ::std::pair< ::HIR::GenericPath, const ::HIR::Struct*> >   m_box_glue_todo;
    public:
        CodeGenerator_C(const ::HIR::Crate& crate, const ::std::string& outfile):
            m_crate(crate),
            m_resolve(crate),
            m_outfile_path(outfile),
            m_outfile_path_c(outfile + ".c"),
            m_of(m_outfile_path_c)
        {
            switch(Target_GetCurSpec().m_codegen_mode)
            {
            case CodegenMode::Gnu11:
                m_compiler = Compiler::Gcc;
                m_options.emulated_i128 = false;
                break;
            case CodegenMode::Msvc:
                m_compiler = Compiler::Msvc;
                m_options.emulated_i128 = true;
                m_options.disallow_empty_structs = true;
                break;
            }

            m_of
                << "/*\n"
                << " * AUTOGENERATED by mrustc\n"
                << " */\n"
                << "#include <stddef.h>\n"
                << "#include <stdint.h>\n"
                << "#include <stdbool.h>\n"
                ;
            switch(m_compiler)
            {
            case Compiler::Gcc:
                m_of
                    << "#include <stdatomic.h>\n"   // atomic_*
                    << "#include <stdlib.h>\n"  // abort
                    << "#include <string.h>\n"  // mem*
                    << "#include <math.h>\n"  // round, ...
                    ;
                break;
            case Compiler::Msvc:
                m_of
                    << "#define INFINITY    ((float)(1e300*1e300))\n"
                    << "#define NAN ((float)(INFINITY*0.0))\n"
                    << "void abort(void);"
                    ;
                break;
            }
            m_of
                << "typedef uint32_t RUST_CHAR;\n"
                << "typedef struct { void* PTR; size_t META; } SLICE_PTR;\n"
                << "typedef struct { void* PTR; void* META; } TRAITOBJ_PTR;\n"
                << "typedef struct { size_t size; size_t align; void (*drop)(void*); } VTABLE_HDR;\n"
                ;
            if( m_options.disallow_empty_structs )
            {
                m_of
                    << "typedef struct { char _d; } tUNIT;\n"
                    << "typedef struct { char _d; } tBANG;\n"
                    << "typedef struct { char _d; } tTYPEID;\n"
                    ;
            }
            else
            {
                m_of
                    << "typedef struct { } tUNIT;\n"
                    << "typedef struct { } tBANG;\n"
                    << "typedef struct { } tTYPEID;\n"
                    ;
            }
            m_of
                << "\n"
                ;
            switch(m_compiler)
            {
            case Compiler::Gcc:
                m_of
                    << "typedef unsigned __int128 uint128_t;\n"
                    << "typedef signed __int128 int128_t;\n"
                    << "extern void _Unwind_Resume(void) __attribute__((noreturn));\n"
                    << "#define ALIGNOF(t) __alignof__(t)\n"
                    ;
                break;
            case Compiler::Msvc:
                m_of
                    << "__declspec(noreturn) void _Unwind_Resume(void) { abort(); }\n"
                    << "#define ALIGNOF(t) __alignof(t)\n"
                    ;
                break;
            //case Compilter::Std11:
            //    break;
            }
            switch (m_compiler)
            {
            case Compiler::Gcc:
                // 64-bit bit ops (gcc intrinsics)
                m_of
                    << "static inline uint64_t __builtin_clz64(uint64_t v) {\n"
                    << "\treturn (v >> 32 != 0 ? __builtin_clz(v>>32) : 32 + __builtin_clz(v));\n"
                    << "}\n"
                    << "static inline uint64_t __builtin_ctz64(uint64_t v) {\n"
                    << "\treturn ((v&0xFFFFFFFF) == 0 ? __builtin_ctz(v>>32) + 32 : __builtin_ctz(v));\n"
                    << "}\n"
                    ;
            case Compiler::Msvc:
                break;
            }

            if( m_options.emulated_i128 )
            {
                m_of
                    << "typedef struct { uint64_t lo, hi; } uint128_t;\n"
                    << "typedef struct { uint64_t lo, hi; } int128_t;\n"
                    << "static inline int128_t make128s(int64_t v) { int128_t rv = { v, (v < 0 ? -1 : 0) }; return rv; }\n"
                    << "static inline int128_t add128s(int128_t a, int128_t b) { int128_t v; v.lo = a.lo + b.lo; v.hi = a.hi + b.hi + (v.lo < a.lo ? 1 : 0); return v; }\n"
                    << "static inline int128_t sub128s(int128_t a, int128_t b) { int128_t v; v.lo = a.lo - b.lo; v.hi = a.hi - b.hi - (v.lo > a.lo ? 1 : 0); return v; }\n"
                    << "static inline int128_t mul128s(int128_t a, int128_t b) { abort(); }\n"
                    << "static inline int128_t div128s(int128_t a, int128_t b) { abort(); }\n"
                    << "static inline int128_t mod128s(int128_t a, int128_t b) { abort(); }\n"
                    << "static inline int128_t and128s(int128_t a, int128_t b) { int128_t v = { a.lo & b.lo, a.hi & b.hi }; return v; }\n"
                    << "static inline int128_t or128s (int128_t a, int128_t b) { int128_t v = { a.lo | b.lo, a.hi | b.hi }; return v; }\n"
                    << "static inline int128_t xor128s(int128_t a, int128_t b) { int128_t v = { a.lo ^ b.lo, a.hi ^ b.hi }; return v; }\n"
                    << "static inline int128_t shl128s(int128_t a, uint32_t b) { int128_t v; if(b < 64) { v.lo = a.lo << b; v.hi = (a.hi << b) | (a.lo >> (64 - b)); } else { v.hi = a.lo << (b - 64); v.lo = 0; } return v; }\n"
                    << "static inline int128_t shr128s(int128_t a, uint32_t b) { int128_t v; if(b < 64) { v.lo = (a.lo >> b)|(a.hi << (64 - b)); v.hi = a.hi >> b; } else { v.lo = a.hi >> (b - 64); v.hi = 0; } return v; }\n"
                    << "static inline uint128_t make128(uint64_t v) { uint128_t rv = { v, 0 }; return rv; }\n"
                    << "static inline uint128_t add128(uint128_t a, uint128_t b) { uint128_t v; v.lo = a.lo + b.lo; v.hi = a.hi + b.hi + (v.lo < a.lo ? 1 : 0); return v; }\n"
                    << "static inline uint128_t sub128(uint128_t a, uint128_t b) { uint128_t v; v.lo = a.lo - b.lo; v.hi = a.hi - b.hi - (v.lo > a.lo ? 1 : 0); return v; }\n"
                    << "static inline uint128_t mul128(uint128_t a, uint128_t b) { abort(); }\n"
                    << "static inline uint128_t div128(uint128_t a, uint128_t b) { abort(); }\n"
                    << "static inline uint128_t mod128(uint128_t a, uint128_t b) { abort(); }\n"
                    << "static inline uint128_t and128(uint128_t a, uint128_t b) { uint128_t v = { a.lo & b.lo, a.hi & b.hi }; return v; }\n"
                    << "static inline uint128_t or128 (uint128_t a, uint128_t b) { uint128_t v = { a.lo | b.lo, a.hi | b.hi }; return v; }\n"
                    << "static inline uint128_t xor128(uint128_t a, uint128_t b) { uint128_t v = { a.lo ^ b.lo, a.hi ^ b.hi }; return v; }\n"
                    << "static inline uint128_t shl128(uint128_t a, uint32_t b) { uint128_t v; if(b < 64) { v.lo = a.lo << b; v.hi = (a.hi << b) | (a.lo >> (64 - b)); } else { v.hi = a.lo << (b - 64); v.lo = 0; } return v; }\n"
                    << "static inline uint128_t shr128(uint128_t a, uint32_t b) { uint128_t v; if(b < 64) { v.lo = (a.lo >> b)|(a.hi << (64 - b)); v.hi = a.hi >> b; } else { v.lo = a.hi >> (b - 64); v.hi = 0; } return v; }\n"
                    << "static inline uint128_t popcount128(uint128_t a) { uint128_t v = { __builtin_popcount(a.lo) + __builtin_popcount(a.hi), 0 }; return v; }\n"
                    << "static inline uint128_t __builtin_bswap128(uint128_t v) { uint128_t rv = { __builtin_bswap64(v.hi), __builtin_bswap64(v.lo) }; return rv; }\n"
                    << "static inline uint128_t intrinsic_ctlz_u128(uint128_t v) {\n"
                    << "\tuint128_t rv = { (v.hi != 0 ? __builtin_clz64(v.hi) : (v.lo != 0 ? 64 + __builtin_clz64(v.lo) : 128)), 0 };\n"
                    << "\treturn rv;\n"
                    << "}\n"
                    << "static inline uint128_t intrinsic_cttz_u128(uint128_t v) {\n"
                    << "\tuint128_t rv = { (v.lo == 0 ? (v.hi == 0 ? 128 : __builtin_ctz64(v.hi) + 64) : __builtin_ctz64(v.lo)), 0 };\n"
                    << "\treturn rv;\n"
                    << "}\n"
                    ;
            }
            else
            {
                // GCC-only
                m_of
                    << "static inline uint128_t __builtin_bswap128(uint128_t v) {\n"
                    << "\tuint64_t lo = __builtin_bswap64((uint64_t)v);\n"
                    << "\tuint64_t hi = __builtin_bswap64((uint64_t)(v>>64));\n"
                    << "\treturn ((uint128_t)lo << 64) | (uint128_t)hi;\n"
                    << "}\n"
                    << "static inline uint128_t intrinsic_ctlz_u128(uint128_t v) {\n"
                    << "\treturn (v == 0 ? 128 : (v >> 64 != 0 ? __builtin_clz64(v>>64) : 64 + __builtin_clz64(v)));\n"
                    << "}\n"
                    << "static inline uint128_t intrinsic_cttz_u128(uint128_t v) {\n"
                    << "\treturn (v == 0 ? 128 : ((v&0xFFFFFFFFFFFFFFFF) == 0 ? __builtin_ctz64(v>>64) + 64 : __builtin_ctz64(v)));\n"
                    << "}\n"
                    ;
            }

            // Common helpers
            m_of
                << "\n"
                << "static inline int slice_cmp(SLICE_PTR l, SLICE_PTR r) {\n"
                << "\tint rv = memcmp(l.PTR, r.PTR, l.META < r.META ? l.META : r.META);\n"
                << "\tif(rv != 0) return rv;\n"
                << "\tif(l.META < r.META) return -1;\n"
                << "\tif(l.META > r.META) return 1;\n"
                << "\treturn 0;\n"
                << "}\n"
                << "static inline SLICE_PTR make_sliceptr(void* ptr, size_t s) { SLICE_PTR rv = { ptr, s }; return rv; }\n"
                << "static inline TRAITOBJ_PTR make_traitobjptr(void* ptr, void* vt) { TRAITOBJ_PTR rv = { ptr, vt }; return rv; }\n"
                << "\n"
                << "static inline size_t mrustc_max(size_t a, size_t b) { return a < b ? b : a; }\n"
                << "static inline void noop_drop(void *p) {}\n"
                << "\n"
                // A linear (fast-fail) search of a list of strings
                << "static inline size_t mrustc_string_search_linear(SLICE_PTR val, size_t count, SLICE_PTR* options) {\n"
                << "\tfor(size_t i = 0; i < count; i ++) {\n"
                << "\t\tint cmp = slice_cmp(val, options[i]);\n"
                << "\t\tif(cmp < 0) break;\n"
                << "\t\tif(cmp == 0) return i;\n"
                << "\t}\n"
                << "\treturn SIZE_MAX;\n"
                << "}\n"
                ;
        }

        ~CodeGenerator_C() {}

        void finalise(bool is_executable, const TransOptions& opt) override
        {
            // Emit box drop glue after everything else to avoid definition ordering issues
            for(auto& e : m_box_glue_todo)
            {
                emit_box_drop_glue( mv$(e.first), *e.second );
            }

            if( is_executable )
            {
                m_of << "int main(int argc, const char* argv[]) {\n";
                auto c_start_path = m_resolve.m_crate.get_lang_item_path_opt("mrustc-start");
                if( c_start_path == ::HIR::SimplePath() )
                {
                    m_of << "\treturn " << Trans_Mangle( ::HIR::GenericPath(m_resolve.m_crate.get_lang_item_path(Span(), "start")) ) << "("
                            << "(uint8_t*)" << Trans_Mangle( ::HIR::GenericPath(m_resolve.m_crate.get_lang_item_path(Span(), "mrustc-main")) ) << ", argc, (uint8_t**)argv"
                            << ");\n";
                }
                else
                {
                    m_of << "\treturn " << Trans_Mangle(::HIR::GenericPath(c_start_path)) << "(argc, argv);\n";
                }
                m_of << "}\n";

                // Emit allocator bindings
                for(const auto& method : ALLOCATOR_METHODS)
                {
                    ::std::vector<const char*>  args;
                    const char* ret_ty = nullptr;
                    // TODO: Configurable between __rg_, __rdl_, and __rde_
                    auto prefix = "__rdl_";

                    for(size_t i = 0; i < method.n_args; i++)
                    {
                        switch(method.args[i])
                        {
                        case AllocatorDataTy::Never:
                        case AllocatorDataTy::Unit:
                        case AllocatorDataTy::ResultPtr:
                        case AllocatorDataTy::ResultExcess:
                        case AllocatorDataTy::UsizePair:
                        case AllocatorDataTy::ResultUnit:
                            BUG(Span(), "Invalid data type for allocator argument");
                            break;
                        case AllocatorDataTy::Layout:
                            args.push_back("uintptr_t");
                            args.push_back("uintptr_t");
                            break;
                        case AllocatorDataTy::LayoutRef:
                            args.push_back("uint8_t*");
                            break;
                        case AllocatorDataTy::AllocError:
                            args.push_back("uint8_t*");
                            break;
                        case AllocatorDataTy::Ptr:
                            args.push_back("uint8_t*");
                            break;
                        }
                    }
                    switch(method.ret)
                    {
                    case AllocatorDataTy::Never:
                    case AllocatorDataTy::Unit:
                        ret_ty = "void";
                        break;
                    case AllocatorDataTy::ResultPtr:
                        args.push_back("uint8_t*");
                        ret_ty = "uint8_t*";
                        break;
                    case AllocatorDataTy::ResultExcess:
                        args.push_back("uint8_t*");
                        args.push_back("uint8_t*");
                        ret_ty = "uint8_t*";
                        break;
                    case AllocatorDataTy::UsizePair:
                        args.push_back("uintptr_t*");
                        args.push_back("uintptr_t*");
                        ret_ty = "void";
                        break;
                    case AllocatorDataTy::ResultUnit:
                        ret_ty = "int8_t";
                        break;
                    case AllocatorDataTy::Layout:
                    case AllocatorDataTy::AllocError:
                    case AllocatorDataTy::Ptr:
                    case AllocatorDataTy::LayoutRef:
                        BUG(Span(), "Invalid data type for allocator return");
                    }

                    m_of << "extern " << ret_ty << " " << prefix << method.name << "(";
                    for(size_t i = 0; i < args.size(); i++)
                    {
                        if(i > 0)   m_of << ", ";
                        m_of << args[i] << " arg" << i;
                    }
                    m_of << ");\n";

                    m_of << ret_ty << " __rust_" << method.name << "(";
                    for(size_t i = 0; i < args.size(); i++)
                    {
                        if(i > 0)   m_of << ", ";
                        m_of << args[i] << " arg" << i;
                    }
                    m_of << ") {\n";
                    m_of << "\treturn " << prefix << method.name << "(";
                    for(size_t i = 0; i < args.size(); i++)
                    {
                        if(i > 0)   m_of << ", ";
                        m_of << "arg" << i;
                    }
                    m_of << ");\n";
                    m_of << "}\n";
                }
            }

            m_of.flush();
            m_of.close();

            ::std::vector<const char*> link_dirs;
            auto add_link_dir = [&link_dirs](const char* d) {
                auto it = ::std::find_if(link_dirs.begin(), link_dirs.end(), [&](const char* s){ return ::std::strcmp(s, d) == 0; });
                if(it == link_dirs.end())
                    link_dirs.push_back(d);
                };
            for(const auto& path : opt.library_search_dirs ) {
                add_link_dir(path.c_str());
            }
            for(const auto& path : m_crate.m_link_paths ) {
                add_link_dir(path.c_str());
            }
            for( const auto& crate : m_crate.m_ext_crates )
            {
                for(const auto& path : crate.second.m_data->m_link_paths ) {
                    add_link_dir(path.c_str());
                }
            }

            // Execute $CC with the required libraries
            StringList  args;
            bool is_windows = false;
            switch( m_compiler )
            {
            case Compiler::Gcc:
                args.push_back( getenv("CC") ? getenv("CC") : "gcc" );
                args.push_back("-ffunction-sections");
                args.push_back("-pthread");
                switch(opt.opt_level)
                {
                case 0: break;
                case 1:
                    args.push_back("-O1");
                    break;
                case 2:
                    args.push_back("-O2");
                    break;
                }
                if( opt.emit_debug_info )
                {
                    args.push_back("-g");
                }
                args.push_back("-o");
                args.push_back(m_outfile_path.c_str());
                args.push_back(m_outfile_path_c.c_str());
                if( is_executable )
                {
                    for( const auto& crate : m_crate.m_ext_crates )
                    {
                        args.push_back(crate.second.m_path + ".o");
                    }
                    for(const auto& path : link_dirs )
                    {
                        args.push_back("-L"); args.push_back(path);
                    }
                    for(const auto& lib : m_crate.m_ext_libs) {
                        ASSERT_BUG(Span(), lib.name != "", "");
                        args.push_back("-l"); args.push_back(lib.name.c_str());
                    }
                    for( const auto& crate : m_crate.m_ext_crates )
                    {
                        for(const auto& lib : crate.second.m_data->m_ext_libs) {
                            ASSERT_BUG(Span(), lib.name != "", "Empty lib from " << crate.first);
                            args.push_back("-l"); args.push_back(lib.name.c_str());
                        }
                    }
                    for(const auto& path : opt.libraries )
                    {
                        args.push_back("-l"); args.push_back(path.c_str());
                    }
                    args.push_back("-z"); args.push_back("muldefs");
                    args.push_back("-Wl,--gc-sections");
                }
                else
                {
                    args.push_back("-c");
                }
                break;
            case Compiler::Msvc:
                is_windows = true;
                // TODO: Look up these paths in the registry and use CreateProcess instead of system
                args.push_back(detect_msvc().path_vcvarsall);
                if( Target_GetCurSpec().m_arch.m_pointer_bits == 64 )
                {
                    args.push_back("amd64");  // NOTE: Doesn't support inline assembly, only works with overrides
                }
                args.push_back("&");
                args.push_back("cl.exe");
                args.push_back("/nologo");
                args.push_back(m_outfile_path_c.c_str());
                switch(opt.opt_level)
                {
                case 0: break;
                case 1:
                    args.push_back("/O1");
                    break;
                case 2:
                    args.push_back("/O2");
                    break;
                }
                if(is_executable)
                {
                    args.push_back(FMT("/Fe" << m_outfile_path));

                    for( const auto& crate : m_crate.m_ext_crates )
                    {
                        args.push_back(crate.second.m_path + ".o");
                    }
                    // Crate-specified libraries
                    for(const auto& lib : m_crate.m_ext_libs) {
                        ASSERT_BUG(Span(), lib.name != "", "");
                        args.push_back(lib.name + ".lib");
                    }
                    for( const auto& crate : m_crate.m_ext_crates )
                    {
                        for(const auto& lib : crate.second.m_data->m_ext_libs) {
                            ASSERT_BUG(Span(), lib.name != "", "Empty lib from " << crate.first);
                            args.push_back(lib.name + ".lib");
                        }
                    }
                    for(const auto& path : opt.libraries )
                    {
                        args.push_back(path + ".lib");
                    }
                    args.push_back("kernel32.lib"); // Needed for Interlocked*

                    // Command-line specified linker search directories
                    args.push_back("/link");
                    args.push_back("/verbose");
                    for(const auto& path : link_dirs )
                    {
                        args.push_back(FMT("/LIBPATH:" << path));
                    }
                }
                else
                {
                    args.push_back("/c");
                    args.push_back(FMT("/Fo" << m_outfile_path));
                }
                break;
            }

            ::std::stringstream cmd_ss;
            if (is_windows)
            {
                cmd_ss << "echo \"\" & ";
            }
            for(const auto& arg : args.get_vec())
            {
                if(strcmp(arg, "&") == 0 && is_windows) {
                    cmd_ss << "&";
                }
                else {
                    if( is_windows && strchr(arg, ' ') == nullptr ) {
                        cmd_ss << arg << " ";
                        continue ;
                    }
                    cmd_ss << "\"" << FmtShell(arg, is_windows) << "\" ";
                }
            }
            //DEBUG("- " << cmd_ss.str());
            ::std::cout << "Running comamnd - " << cmd_ss.str() << ::std::endl;
            if( system(cmd_ss.str().c_str()) != 0 )
            {
                ::std::cerr << "C Compiler failed to execute" << ::std::endl;
                abort();
            }
        }

        void emit_box_drop_glue(::HIR::GenericPath p, const ::HIR::Struct& item)
        {
            auto struct_ty = ::HIR::TypeRef( p.clone(), &item );
            auto drop_glue_path = ::HIR::Path(struct_ty.clone(), "#drop_glue");
            auto struct_ty_ptr = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Owned, struct_ty.clone());
            // - Drop Glue
            const auto* ity = m_resolve.is_type_owned_box(struct_ty);

            auto inner_ptr = ::HIR::TypeRef::new_pointer( ::HIR::BorrowType::Unique, ity->clone() );
            auto box_free = ::HIR::GenericPath { m_crate.get_lang_item_path(sp, "box_free"), { ity->clone() } };

            ::std::vector< ::std::pair<::HIR::Pattern,::HIR::TypeRef> > args;
            args.push_back( ::std::make_pair( ::HIR::Pattern {}, mv$(inner_ptr) ) );

            ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), struct_ty_ptr, args, *(::MIR::Function*)nullptr };
            m_mir_res = &mir_res;
            m_of << "static void " << Trans_Mangle(drop_glue_path) << "(struct s_" << Trans_Mangle(p) << "* rv) {\n";

            // Obtain inner pointer
            // TODO: This is very specific to the structure of the official liballoc's Box.
            m_of << "\t"; emit_ctype(args[0].second, FMT_CB(ss, ss << "arg0"; ));    m_of << " = rv->_0._0._0;\n";
            // Call destructor of inner data
            emit_destructor_call( ::MIR::LValue::make_Deref({ box$(::MIR::LValue::make_Argument({0})) }), *ity, true, 1);
            // Emit a call to box_free for the type
            m_of << "\t" << Trans_Mangle(box_free) << "(arg0);\n";

            m_of << "}\n";
            m_mir_res = nullptr;
        }

        void emit_type_id(const ::HIR::TypeRef& ty) override
        {
            switch(m_compiler)
            {
            case Compiler::Gcc:
                m_of << "tTYPEID __typeid_" << Trans_Mangle(ty) << " __attribute__((weak));\n";
                break;
            case Compiler::Msvc:
                m_of << "__declspec(selectany) tTYPEID __typeid_" << Trans_Mangle(ty) << ";\n";
                break;
            }
        }
        void emit_type_proto(const ::HIR::TypeRef& ty) override
        {
            TRACE_FUNCTION_F(ty);
            TU_IFLET( ::HIR::TypeRef::Data, ty.m_data, Tuple, te,
                if( te.size() > 0 )
                {
                    m_of << "typedef struct "; emit_ctype(ty); m_of << " "; emit_ctype(ty); m_of << ";\n";
                }
            )
            else TU_IFLET( ::HIR::TypeRef::Data, ty.m_data, Function, te,
                emit_type_fn(ty); m_of << "\n";
            )
            else TU_IFLET( ::HIR::TypeRef::Data, ty.m_data, Array, te,
                m_of << "typedef struct "; emit_ctype(ty); m_of << " "; emit_ctype(ty); m_of << ";\n";
            )
            else TU_IFLET( ::HIR::TypeRef::Data, ty.m_data, Path, te,
                TU_MATCHA( (te.binding), (tpb),
                (Unbound,  throw ""; ),
                (Opaque,  throw ""; ),
                (Struct,
                    m_of << "struct s_" << Trans_Mangle(te.path) << ";\n";
                    ),
                (Union,
                    m_of << "union u_" << Trans_Mangle(te.path) << ";\n";
                    ),
                (Enum,
                    m_of << "struct e_" << Trans_Mangle(te.path) << ";\n";
                    )
                )
            )
            else if( ty.m_data.is_ErasedType() ) {
                // TODO: Is this actually a bug?
                return ;
            }
            else {
            }
        }
        void emit_type_fn(const ::HIR::TypeRef& ty)
        {
            const auto& te = ty.m_data.as_Function();
            m_of << "typedef ";
            // TODO: ABI marker, need an ABI enum?
            // TODO: Better emit_ctype call for return type.
            emit_ctype(*te.m_rettype); m_of << " (";
            if( m_compiler == Compiler::Msvc )
            {
                if( te.m_abi == ABI_RUST )
                {
                }
                else if( te.m_abi == "system" )
                {
                    m_of << "__stdcall";
                }
                else
                {
                }
            }
            m_of << "*"; emit_ctype(ty); m_of << ")(";
            if( te.m_arg_types.size() == 0 )
            {
                m_of << "void)";
            }
            else
            {
                for(unsigned int i = 0; i < te.m_arg_types.size(); i ++)
                {
                    if(i != 0)  m_of << ",";
                    m_of << " ";
                    emit_ctype(te.m_arg_types[i]);
                }
                m_of << " )";
            }
            m_of << ";";
        }
        void emit_type(const ::HIR::TypeRef& ty) override
        {
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "type " << ty;), ::HIR::TypeRef(), {}, *(::MIR::Function*)nullptr };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(ty);
            TU_IFLET( ::HIR::TypeRef::Data, ty.m_data, Tuple, te,
                if( te.size() > 0 )
                {
                    m_of << "typedef struct "; emit_ctype(ty); m_of << " {\n";
                    for(unsigned int i = 0; i < te.size(); i++)
                    {
                        m_of << "\t";
                        emit_ctype(te[i], FMT_CB(ss, ss << "_" << i;));
                        m_of << ";\n";
                    }
                    m_of << "} "; emit_ctype(ty); m_of << ";\n";
                }

                auto drop_glue_path = ::HIR::Path(ty.clone(), "#drop_glue");
                auto args = ::std::vector< ::std::pair<::HIR::Pattern,::HIR::TypeRef> >();
                auto ty_ptr = ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Owned, ty.clone());
                ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), ty_ptr, args, *(::MIR::Function*)nullptr };
                m_mir_res = &mir_res;
                m_of << "static void " << Trans_Mangle(drop_glue_path) << "("; emit_ctype(ty); m_of << "* rv) {";
                auto self = ::MIR::LValue::make_Deref({ box$(::MIR::LValue::make_Return({})) });
                auto fld_lv = ::MIR::LValue::make_Field({ box$(self), 0 });
                for(const auto& ity : te)
                {
                    emit_destructor_call(fld_lv, ity, /*unsized_valid=*/false, 1);
                    fld_lv.as_Field().field_index ++;
                }
                m_of << "}\n";
            )
            else TU_IFLET( ::HIR::TypeRef::Data, ty.m_data, Function, te,
                emit_type_fn(ty);
                m_of << " // " << ty << "\n";
            )
            else TU_IFLET( ::HIR::TypeRef::Data, ty.m_data, Array, te,
                m_of << "typedef struct "; emit_ctype(ty); m_of << " { "; emit_ctype(*te.inner); m_of << " DATA[" << te.size_val << "]; } "; emit_ctype(ty); m_of << ";\n";
            )
            else if( ty.m_data.is_ErasedType() ) {
                // TODO: Is this actually a bug?
                return ;
            }
            else {
            }

            m_mir_res = nullptr;
        }

        void emit_struct(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Struct& item) override
        {
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "struct " << p;), ::HIR::TypeRef(), {}, *(::MIR::Function*)nullptr };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);
            ::HIR::TypeRef  tmp;
            auto monomorph = [&](const auto& x)->const auto& {
                if( monomorphise_type_needed(x) ) {
                    tmp = monomorphise_type(sp, item.m_params, p.m_params, x);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return x;
                }
                };
            bool has_unsized = false;
            auto emit_struct_fld_ty = [&](const ::HIR::TypeRef& ty_raw, ::FmtLambda inner) {
                const auto& ty = monomorph(ty_raw);
                TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Slice, te,
                    emit_ctype( *te.inner, FMT_CB(ss, ss << inner << "[0]";) );
                    has_unsized = true;
                )
                else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, TraitObject, te,
                    m_of << "unsigned char " << inner << "[0]";
                    has_unsized = true;
                )
                else if( ty == ::HIR::CoreType::Str ) {
                    m_of << "uint8_t " << inner << "[0]";
                    has_unsized = true;
                }
                else {
                    emit_ctype( ty, inner );
                }
                };
            m_of << "// struct " << p << "\n";
            m_of << "struct s_" << Trans_Mangle(p) << " {\n";

            // HACK: For vtables, insert the alignment and size at the start
            {
                const auto& lc = p.m_path.m_components.back();
                if( lc.size() > 7 && ::std::strcmp(lc.c_str() + lc.size() - 7, "#vtable") == 0 ) {
                    m_of << "\tVTABLE_HDR hdr;\n";
                }
            }

            TU_MATCHA( (item.m_data), (e),
            (Unit,
                if( m_options.disallow_empty_structs )
                {
                    m_of << "\tchar _d;\n";
                }
                ),
            (Tuple,
                if( e.empty() )
                {
                    if( m_options.disallow_empty_structs )
                    {
                        m_of << "\tchar _d;\n";
                    }
                }
                else
                {
                    for(unsigned int i = 0; i < e.size(); i ++)
                    {
                        const auto& fld = e[i];
                        m_of << "\t";
                        emit_struct_fld_ty(fld.ent, FMT_CB(ss, ss << "_" << i;));
                        m_of << ";\n";
                    }
                }
                ),
            (Named,
                if( e.empty() )
                {
                    if( m_options.disallow_empty_structs )
                    {
                        m_of << "\tchar _d;\n";
                    }
                }
                else
                {
                    for(unsigned int i = 0; i < e.size(); i ++)
                    {
                        const auto& fld = e[i].second;
                        m_of << "\t";
                        emit_struct_fld_ty(fld.ent, FMT_CB(ss, ss << "_" << i;));
                        m_of << ";\n";
                    }
                }
                )
            )
            m_of << "};\n";

            auto struct_ty = ::HIR::TypeRef(p.clone(), &item);
            auto drop_glue_path = ::HIR::Path(struct_ty.clone(), "#drop_glue");
            auto struct_ty_ptr = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Owned, struct_ty.clone());
            // - Drop Glue

            ::std::vector< ::std::pair<::HIR::Pattern,::HIR::TypeRef> > args;
            if( item.m_markings.has_drop_impl ) {
                // If the type is defined outside the current crate, define as static (to avoid conflicts when we define it)
                if( p.m_path.m_crate_name != m_crate.m_crate_name )
                {
                    if( item.m_params.m_types.size() > 0 ) {
                        m_of << "static ";
                    }
                    else {
                        m_of << "extern ";
                    }
                }
                m_of << "tUNIT " << Trans_Mangle( ::HIR::Path(struct_ty.clone(), m_resolve.m_lang_Drop, "drop") ) << "("; emit_ctype(struct_ty_ptr, FMT_CB(ss, ss << "rv";)); m_of << ");\n";
            }
            else if( m_resolve.is_type_owned_box(struct_ty) )
            {
                m_box_glue_todo.push_back( ::std::make_pair( mv$(struct_ty.m_data.as_Path().path.m_data.as_Generic()), &item ) );
                m_of << "static void " << Trans_Mangle(drop_glue_path) << "("; emit_ctype(struct_ty_ptr, FMT_CB(ss, ss << "rv";)); m_of << ");\n";
                return ;
            }

            ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), struct_ty_ptr, args, *(::MIR::Function*)nullptr };
            m_mir_res = &mir_res;
            m_of << "static void " << Trans_Mangle(drop_glue_path) << "("; emit_ctype(struct_ty_ptr, FMT_CB(ss, ss << "rv";)); m_of << ") {\n";

            // If this type has an impl of Drop, call that impl
            if( item.m_markings.has_drop_impl ) {
                m_of << "\t" << Trans_Mangle( ::HIR::Path(struct_ty.clone(), m_resolve.m_lang_Drop, "drop") ) << "(rv);\n";
            }

            auto self = ::MIR::LValue::make_Deref({ box$(::MIR::LValue::make_Return({})) });
            auto fld_lv = ::MIR::LValue::make_Field({ box$(self), 0 });
            TU_MATCHA( (item.m_data), (e),
            (Unit,
                ),
            (Tuple,
                for(unsigned int i = 0; i < e.size(); i ++)
                {
                    const auto& fld = e[i];
                    fld_lv.as_Field().field_index = i;

                    emit_destructor_call(fld_lv, monomorph(fld.ent), true, 1);
                }
                ),
            (Named,
                for(unsigned int i = 0; i < e.size(); i ++)
                {
                    const auto& fld = e[i].second;
                    fld_lv.as_Field().field_index = i;

                    emit_destructor_call(fld_lv, monomorph(fld.ent), true, 1);
                }
                )
            )
            m_of << "}\n";
            m_mir_res = nullptr;
        }
        void emit_union(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Union& item) override
        {
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "union " << p;), ::HIR::TypeRef(), {}, *(::MIR::Function*)nullptr };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);

            ::HIR::TypeRef  tmp;
            auto monomorph = [&](const auto& x)->const auto& {
                if( monomorphise_type_needed(x) ) {
                    tmp = monomorphise_type(sp, item.m_params, p.m_params, x);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return x;
                }
                };
            m_of << "union u_" << Trans_Mangle(p) << " {\n";
            for(unsigned int i = 0; i < item.m_variants.size(); i ++)
            {
                m_of << "\t"; emit_ctype( monomorph(item.m_variants[i].second.ent), FMT_CB(ss, ss << "var_" << i;) ); m_of << ";\n";
            }
            m_of << "};\n";

            // Drop glue (calls destructor if there is one)
            auto item_ty = ::HIR::TypeRef(p.clone(), &item);
            auto drop_glue_path = ::HIR::Path(item_ty.clone(), "#drop_glue");
            auto item_ptr_ty = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Owned, item_ty.clone());
            auto drop_impl_path = (item.m_markings.has_drop_impl ? ::HIR::Path(item_ty.clone(), m_resolve.m_lang_Drop, "drop") : ::HIR::Path(::HIR::SimplePath()));
            ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), item_ptr_ty, {}, *(::MIR::Function*)nullptr };
            m_mir_res = &mir_res;

            if( item.m_markings.has_drop_impl )
            {
                m_of << "tUNIT " << Trans_Mangle(drop_impl_path) << "(union u_" << Trans_Mangle(p) << "*rv);\n";
            }

            m_of << "static void " << Trans_Mangle(drop_glue_path) << "(union u_" << Trans_Mangle(p) << "* rv) {\n";
            if( item.m_markings.has_drop_impl )
            {
                m_of << "\t" << Trans_Mangle(drop_impl_path) << "(rv);\n";
            }
            m_of << "}\n";
        }

        // TODO: Move this to codegen.cpp?
        bool get_nonzero_path(const Span& sp, const ::HIR::TypeRef& ty, ::std::vector<unsigned int>& out) const
        {
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
            (
                return false;
                ),
            (Path,
                if( te.binding.is_Struct() )
                {
                    const auto& str = *te.binding.as_Struct();
                    const auto& p = te.path.m_data.as_Generic();
                    ::HIR::TypeRef  tmp;
                    auto monomorph = [&](const auto& ty)->const auto& {
                        if( monomorphise_type_needed(ty) ) {
                            tmp = monomorphise_type(sp, str.m_params, p.m_params, ty);
                            m_resolve.expand_associated_types(sp, tmp);
                            return tmp;
                        }
                        else {
                            return ty;
                        }
                        };
                    TU_MATCHA( (str.m_data), (se),
                    (Unit,
                        ),
                    (Tuple,
                        for(size_t i = 0; i < se.size(); i ++)
                        {
                            if( get_nonzero_path(sp, monomorph(se[i].ent), out) )
                            {
                                out.push_back(i);
                                return true;
                            }
                        }
                        ),
                    (Named,
                        for(size_t i = 0; i < se.size(); i ++)
                        {
                            if( get_nonzero_path(sp, monomorph(se[i].second.ent), out) )
                            {
                                out.push_back(i);
                                return true;
                            }
                        }
                        )
                    )
                }
                return false;
                ),
            (Borrow,
                if( metadata_type(*te.inner) != MetadataType::None )
                {
                    // HACK: If the inner is a DST, emit ~0 as a marker
                    out.push_back(~0u);
                }
                return true;
                ),
            (Function,
                return true;
                )
            )
        }
        void emit_nonzero_path(const ::std::vector<unsigned int>& nonzero_path) {
            for(const auto v : nonzero_path)
            {
                if(v == ~0u) {
                    // NOTE: Should only ever be the last
                    m_of << ".PTR";
                }
                else {
                    m_of << "._" << v;
                }
            }
        }

        void emit_enum(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Enum& item) override
        {
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "enum " << p;), ::HIR::TypeRef(), {}, *(::MIR::Function*)nullptr };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);
            ::HIR::TypeRef  tmp;
            auto monomorph = [&](const auto& x)->const auto& {
                if( monomorphise_type_needed(x) ) {
                    tmp = monomorphise_type(sp, item.m_params, p.m_params, x);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return x;
                }
                };

            // TODO: Cache repr info elsewhere (and have core codegen be responsible instead)
            // TODO: Do more complex nonzero relations.
            ::std::vector<unsigned> nonzero_path;
            {
                // Detect Option (and similar enums)
                // - Matches two-variant enums where the first variant is unit-like, and the second is not
                if( item.m_data.is_Data() && item.m_data.as_Data().size() == 2
                 && item.m_data.as_Data()[0].type == ::HIR::TypeRef::new_unit()
                 && item.m_data.as_Data()[1].type != ::HIR::TypeRef::new_unit()
                    )
                {
                    const auto& data_type = monomorph(item.m_data.as_Data()[1].type);
                    if( get_nonzero_path(sp, data_type, nonzero_path) )
                    {
                        ::std::reverse( nonzero_path.begin(), nonzero_path.end() );
                        DEBUG("Correct format for NonZero to apply, and field found at " << nonzero_path);
                    }
                    else
                    {
                        DEBUG("Correct format for NonZero to apply, but no field");
                        assert(nonzero_path.size() == 0);
                    }
                }
            }

            m_of << "// enum " << p << "\n";
            if( nonzero_path.size() > 0 )
            {
                //MIR_ASSERT(*m_mir_res, item.num_variants() == 2, "");
                //MIR_ASSERT(*m_mir_res, item.m_variants[0].second.is_Unit(), "");
                //const auto& data_var = item.m_variants[1];
                //MIR_ASSERT(*m_mir_res, data_var.second.is_Tuple(), "");
                //MIR_ASSERT(*m_mir_res, data_var.second.as_Tuple().size() == 1, "");
                const auto& data_type = monomorph(item.m_data.as_Data()[1].type);
                m_of << "struct e_" << Trans_Mangle(p) << " {\n";
                m_of << "\t"; emit_ctype(data_type, FMT_CB(s, s << "_1";)); m_of << ";\n";
                m_of << "};\n";
            }
            else if( item.m_data.is_Value() )
            {
                m_of << "struct e_" << Trans_Mangle(p) << " {\n";
                switch(item.m_data.as_Value().repr)
                {
                case ::HIR::Enum::Repr::Rust:
                case ::HIR::Enum::Repr::C:
                    m_of << "\tunsigned int TAG;\n";
                    break;
                case ::HIR::Enum::Repr::Usize:
                    m_of << "\tuintptr_t TAG;\n";
                    break;
                case ::HIR::Enum::Repr::U8:
                    m_of << "\tuint8_t TAG;\n";
                    break;
                case ::HIR::Enum::Repr::U16:
                    m_of << "\tuint16_t TAG;\n";
                    break;
                case ::HIR::Enum::Repr::U32:
                    m_of << "\tuint32_t TAG;\n";
                    break;
                case ::HIR::Enum::Repr::U64:
                    m_of << "\tuint64_t TAG;\n";
                    break;
                }
                m_of << "};\n";
            }
            else
            {
                const auto& variants = item.m_data.as_Data();
                m_of << "struct e_" << Trans_Mangle(p) << " {\n";
                m_of << "\tunsigned int TAG;\n";
                if( variants.size() > 0 )
                {
                    m_of << "\tunion {\n";
                    for(unsigned int i = 0; i < variants.size(); i ++)
                    {
                        m_of << "\t\t";
                        emit_ctype( monomorph(variants[i].type) );
                        m_of << " var_" << i << ";\n";
                    }
                    m_of << "\t} DATA;\n";
                }
                m_of << "};\n";
            }

            // ---
            // - Drop Glue
            // ---
            auto struct_ty = ::HIR::TypeRef(p.clone(), &item);
            auto drop_glue_path = ::HIR::Path(struct_ty.clone(), "#drop_glue");
            auto struct_ty_ptr = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Owned, struct_ty.clone());
            auto drop_impl_path = (item.m_markings.has_drop_impl ? ::HIR::Path(struct_ty.clone(), m_resolve.m_lang_Drop, "drop") : ::HIR::Path(::HIR::SimplePath()));
            ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), struct_ty_ptr, {}, *(::MIR::Function*)nullptr };
            m_mir_res = &mir_res;

            if( item.m_markings.has_drop_impl )
            {
                m_of << "tUNIT " << Trans_Mangle(drop_impl_path) << "(struct e_" << Trans_Mangle(p) << "*rv);\n";
            }

            m_of << "static void " << Trans_Mangle(drop_glue_path) << "(struct e_" << Trans_Mangle(p) << "* rv) {\n";

            // If this type has an impl of Drop, call that impl
            if( item.m_markings.has_drop_impl )
            {
                m_of << "\t" << Trans_Mangle(drop_impl_path) << "(rv);\n";
            }
            auto self = ::MIR::LValue::make_Deref({ box$(::MIR::LValue::make_Return({})) });

            if( nonzero_path.size() > 0 )
            {
                // TODO: Fat pointers?
                m_of << "\tif( (*rv)._1"; emit_nonzero_path(nonzero_path); m_of << " ) {\n";
                emit_destructor_call( ::MIR::LValue::make_Field({ box$(self), 1 }), monomorph(item.m_data.as_Data()[1].type), false, 2 );
                m_of << "\t}\n";
            }
            else if( const auto* e = item.m_data.opt_Data() )
            {
                auto var_lv =::MIR::LValue::make_Downcast({ box$(self), 0 });

                m_of << "\tswitch(rv->TAG) {\n";
                for(unsigned int var_idx = 0; var_idx < e->size(); var_idx ++)
                {
                    var_lv.as_Downcast().variant_index = var_idx;
                    m_of << "\tcase " << var_idx << ":\n";
                    emit_destructor_call(var_lv, monomorph( (*e)[var_idx].type ), false, 2);
                    m_of << "\tbreak;\n";
                }
                m_of << "\t}\n";
            }
            else
            {
                // Value enum
                // Glue does nothing (except call the destructor, if there is one)
            }
            m_of << "}\n";
            m_mir_res = nullptr;

            if( nonzero_path.size() )
            {
                m_enum_repr_cache.insert( ::std::make_pair( p.clone(), mv$(nonzero_path) ) );
            }
        }

        void emit_constructor_enum(const Span& sp, const ::HIR::GenericPath& path, const ::HIR::Enum& item, size_t var_idx) override
        {
            TRACE_FUNCTION_F(path << " var_idx=" << var_idx);
            ::HIR::TypeRef  tmp;
            auto monomorph = [&](const auto& x)->const auto& {
                if( monomorphise_type_needed(x) ) {
                    tmp = monomorphise_type(sp, item.m_params, path.m_params, x);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return x;
                }
                };

            auto p = path.clone();
            p.m_path.m_components.pop_back();

            ASSERT_BUG(sp, item.m_data.is_Data(), "");
            const auto& var = item.m_data.as_Data().at(var_idx);
            ASSERT_BUG(sp, var.type.m_data.is_Path(), "");
            const auto& str = *var.type.m_data.as_Path().binding.as_Struct();
            ASSERT_BUG(sp, str.m_data.is_Tuple(), "");
            const auto& e = str.m_data.as_Tuple();


            m_of << "struct e_" << Trans_Mangle(p) << " " << Trans_Mangle(path) << "(";
            for(unsigned int i = 0; i < e.size(); i ++)
            {
                if(i != 0)
                    m_of << ", ";
                emit_ctype( monomorph(e[i].ent), FMT_CB(ss, ss << "_" << i;) );
            }
            m_of << ") {\n";
            auto it = m_enum_repr_cache.find(p);
            if( it != m_enum_repr_cache.end() )
            {
                m_of << "\tstruct e_" << Trans_Mangle(p) << " rv = { _0 };\n";
            }
            else
            {
                m_of << "\tstruct e_" << Trans_Mangle(p) << " rv = { .TAG = " << var_idx;

                if( e.empty() )
                {
                    if( m_options.disallow_empty_structs )
                    {
                        m_of << ", .DATA = { .var_" << var_idx << " = {0} }";
                    }
                    else
                    {
                        // No fields, don't initialise
                    }
                }
                else
                {
                    m_of << ", .DATA = { .var_" << var_idx << " = {";
                    for(unsigned int i = 0; i < e.size(); i ++)
                    {
                        if(i != 0)
                        m_of << ",";
                        m_of << "\n\t\t_" << i;
                    }
                    m_of << "\n\t\t}";
                }
                m_of << " }};\n";
            }
            m_of << "\treturn rv;\n";
            m_of << "}\n";
        }
        void emit_constructor_struct(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Struct& item) override
        {
            TRACE_FUNCTION_F(p);
            ::HIR::TypeRef  tmp;
            auto monomorph = [&](const auto& x)->const auto& {
                if( monomorphise_type_needed(x) ) {
                    tmp = monomorphise_type(sp, item.m_params, p.m_params, x);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return x;
                }
                };
            // Crate constructor function
            const auto& e = item.m_data.as_Tuple();
            m_of << "struct s_" << Trans_Mangle(p) << " " << Trans_Mangle(p) << "(";
            for(unsigned int i = 0; i < e.size(); i ++)
            {
                if(i != 0)
                    m_of << ", ";
                emit_ctype( monomorph(e[i].ent), FMT_CB(ss, ss << "_" << i;) );
            }
            m_of << ") {\n";
            m_of << "\tstruct s_" << Trans_Mangle(p) << " rv = {";
            for(unsigned int i = 0; i < e.size(); i ++)
            {
                if(i != 0)
                    m_of << ",";
                m_of << "\n\t\t_" << i;
            }
            m_of << "\n\t\t};\n";
            m_of << "\treturn rv;\n";
            m_of << "}\n";
        }

        void emit_static_ext(const ::HIR::Path& p, const ::HIR::Static& item, const Trans_Params& params) override
        {
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "extern static " << p;), ::HIR::TypeRef(), {}, *(::MIR::Function*)nullptr };
            m_mir_res = &top_mir_res;
            TRACE_FUNCTION_F(p);

            if( item.m_linkage.name != "" && m_compiler != Compiler::Gcc )
            {
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    // Handled with asm() later
                    break;
                case Compiler::Msvc:
                    //m_of << "#pragma comment(linker, \"/alternatename:_" << Trans_Mangle(p) << "=" << item.m_linkage.name << "\")\n";
                    m_of << "#define " << Trans_Mangle(p) << " " << item.m_linkage.name << "\n";
                    break;
                //case Compiler::Std11:
                //    m_of << "#define " << Trans_Mangle(p) << " " << item.m_linkage.name << "\n";
                //    break;
                }
            }

            auto type = params.monomorph(m_resolve, item.m_type);
            m_of << "extern ";
            emit_ctype( type, FMT_CB(ss, ss << Trans_Mangle(p);) );
            if( item.m_linkage.name != "" && m_compiler == Compiler::Gcc)
            {
                m_of << " asm(\"" << item.m_linkage.name << "\")";
            }
            m_of << ";";
            m_of << "\t// static " << p << " : " << type;
            m_of << "\n";

            m_mir_res = nullptr;
        }
        void emit_static_proto(const ::HIR::Path& p, const ::HIR::Static& item, const Trans_Params& params) override
        {
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "static " << p;), ::HIR::TypeRef(), {}, *(::MIR::Function*)nullptr };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);
            auto type = params.monomorph(m_resolve, item.m_type);
            emit_ctype( type, FMT_CB(ss, ss << Trans_Mangle(p);) );
            m_of << ";";
            m_of << "\t// static " << p << " : " << type;
            m_of << "\n";

            m_mir_res = nullptr;
        }
        void emit_static_local(const ::HIR::Path& p, const ::HIR::Static& item, const Trans_Params& params) override
        {
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "static " << p;), ::HIR::TypeRef(), {}, *(::MIR::Function*)nullptr };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);

            auto type = params.monomorph(m_resolve, item.m_type);
            emit_ctype( type, FMT_CB(ss, ss << Trans_Mangle(p);) );
            m_of << " = ";
            emit_literal(type, item.m_value_res, params);
            m_of << ";";
            m_of << "\t// static " << p << " : " << type;
            m_of << "\n";

            m_mir_res = nullptr;
        }
        void emit_float(double v) {
            if( ::std::isnan(v) ) {
                m_of << "NAN";
            }
            else if( ::std::isinf(v) ) {
                m_of << (v < 0 ? "-" : "") << "INFINITY";
            }
            else {
                m_of.precision(::std::numeric_limits<double>::max_digits10 + 1);
                m_of << ::std::scientific << v;
            }
        }
        void emit_literal(const ::HIR::TypeRef& ty, const ::HIR::Literal& lit, const Trans_Params& params) {
            TRACE_FUNCTION_F("ty=" << ty << ", lit=" << lit);
            ::HIR::TypeRef  tmp;
            auto monomorph_with = [&](const ::HIR::PathParams& pp, const ::HIR::TypeRef& ty)->const ::HIR::TypeRef& {
                if( monomorphise_type_needed(ty) ) {
                    tmp = monomorphise_type_with(sp, ty, monomorphise_type_get_cb(sp, nullptr, &pp, nullptr), false);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return ty;
                }
                };
            auto get_inner_type = [&](unsigned int var, unsigned int idx)->const ::HIR::TypeRef& {
                TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, te,
                    return *te.inner;
                )
                else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Path, te,
                    const auto& pp = te.path.m_data.as_Generic().m_params;
                    TU_MATCHA((te.binding), (pbe),
                    (Unbound, MIR_BUG(*m_mir_res, "Unbound type path " << ty); ),
                    (Opaque, MIR_BUG(*m_mir_res, "Opaque type path " << ty); ),
                    (Struct,
                        TU_MATCHA( (pbe->m_data), (se),
                        (Unit,
                            MIR_BUG(*m_mir_res, "Unit struct " << ty);
                            ),
                        (Tuple,
                            return monomorph_with(pp, se.at(idx).ent);
                            ),
                        (Named,
                            return monomorph_with(pp, se.at(idx).second.ent);
                            )
                        )
                        ),
                    (Union,
                        MIR_TODO(*m_mir_res, "Union literals");
                        ),
                    (Enum,
                        MIR_ASSERT(*m_mir_res, pbe->m_data.is_Data(), "");
                        const auto& evar = pbe->m_data.as_Data().at(var);
                        return monomorph_with(pp, evar.type);
                        )
                    )
                    throw "";
                )
                else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Tuple, te,
                    return te.at(idx);
                )
                else {
                    MIR_TODO(*m_mir_res, "Unknown type in list literal - " << ty);
                }
                };
            TU_MATCHA( (lit), (e),
            (Invalid, m_of << "/* INVALID */"; ),
            (List,
                if( ty.m_data.is_Array() )
                    m_of << "{";
                m_of << "{";
                for(unsigned int i = 0; i < e.size(); i ++) {
                    if(i != 0)  m_of << ",";
                    m_of << " ";
                    emit_literal(get_inner_type(0, i), e[i], params);
                }
                if( (ty.m_data.is_Path() || ty.m_data.is_Tuple()) && e.size() == 0 && m_options.disallow_empty_structs )
                    m_of << "0";
                m_of << " }";
                if( ty.m_data.is_Array() )
                    m_of << "}";
                ),
            (Variant,
                MIR_ASSERT(*m_mir_res, ty.m_data.is_Path(), "");
                MIR_ASSERT(*m_mir_res, ty.m_data.as_Path().binding.is_Enum(), "");
                const auto& enm = *ty.m_data.as_Path().binding.as_Enum();
                auto it = m_enum_repr_cache.find(ty.m_data.as_Path().path.m_data.as_Generic());
                if( it != m_enum_repr_cache.end() )
                {
                    if( e.idx == 0 ) {
                        m_of << "{0}";
                    }
                    else {
                        emit_literal(get_inner_type(e.idx, 0), *e.val, params);
                    }
                }
                else if( enm.is_value() )
                {
                    MIR_ASSERT(*m_mir_res, TU_TEST1((*e.val), List, .empty()), "Value-only enum with fields");
                    m_of << "{" << enm.get_value(e.idx) << "}";
                }
                else
                {
                    m_of << "{" << e.idx;
                    m_of << ", { .var_" << e.idx << " = ";
                    emit_literal(get_inner_type(e.idx, 0), *e.val, params);
                    m_of << " }";
                    m_of << "}";
                }
                ),
            (Integer,
                if( ty.m_data.is_Primitive() )
                {
                    switch(ty.m_data.as_Primitive())
                    {
                    case ::HIR::CoreType::Bool:
                        m_of << (e ? "true" : "false");
                        break;
                    case ::HIR::CoreType::U8:
                        m_of << ::std::hex << "0x" << (e & 0xFF) << ::std::dec;
                        break;
                    case ::HIR::CoreType::U16:
                        m_of << ::std::hex << "0x" << (e & 0xFFFF) << ::std::dec;
                        break;
                    case ::HIR::CoreType::U32:
                        m_of << ::std::hex << "0x" << (e & 0xFFFFFFFF) << ::std::dec;
                        break;
                    case ::HIR::CoreType::U64:
                    case ::HIR::CoreType::Usize:
                        m_of << ::std::hex << "0x" << e << ::std::dec;
                        break;
                    case ::HIR::CoreType::U128:
                        m_of << ::std::hex << "0x" << e << ::std::dec;
                        break;
                    case ::HIR::CoreType::I8:
                        m_of << static_cast<uint16_t>( static_cast<int8_t>(e) );
                        break;
                    case ::HIR::CoreType::I16:
                        m_of << static_cast<int16_t>(e);
                        break;
                    case ::HIR::CoreType::I32:
                        m_of << static_cast<int32_t>(e);
                        break;
                    case ::HIR::CoreType::I64:
                    case ::HIR::CoreType::I128:
                    case ::HIR::CoreType::Isize:
                        m_of << static_cast<int64_t>(e);
                        break;
                    case ::HIR::CoreType::Char:
                        assert(0 <= e && e <= 0x10FFFF);
                        if( e < 256 ) {
                            m_of << e;
                        }
                        else {
                            m_of << ::std::hex << "0x" << e << ::std::dec;
                        }
                        break;
                    default:
                        MIR_TODO(*m_mir_res, "Handle intger literal of type " << ty);
                    }
                }
                else if( ty.m_data.is_Pointer() )
                {
                    m_of << ::std::hex << "(void*)0x" << e << ::std::dec;
                }
                else
                {
                    MIR_BUG(*m_mir_res, "Integer literal for invalid type - " << ty);
                }
                ),
            (Float,
                this->emit_float(e);
                ),
            (BorrowPath,
                TU_MATCHA( (e.m_data), (pe),
                (Generic,
                    const auto& vi = m_crate.get_valitem_by_path(sp, pe.m_path);
                    if( vi.is_Function() )
                    {
                        if( !ty.m_data.is_Function() ) // TODO: Ensure that the type is `*const ()` or similar.
                            m_of << "(void*)";
                        else
                            ;
                    }
                    else
                    {
                        if( TU_TEST1(ty.m_data, Borrow, .inner->m_data.is_Slice()) )
                        {
                            // Since this is a borrow, it must be of an array.
                            MIR_ASSERT(*m_mir_res, vi.is_Static(), "BorrowOf returning &[T] not of a static - " << pe.m_path << " is " << vi.tag_str());
                            const auto& stat = vi.as_Static();
                            MIR_ASSERT(*m_mir_res, stat.m_type.m_data.is_Array(), "BorrowOf : &[T] of non-array static, " << pe.m_path << " - " << stat.m_type);
                            unsigned int size = stat.m_type.m_data.as_Array().size_val;
                            m_of << "{ &" << Trans_Mangle( params.monomorph(m_resolve, e)) << ", " << size << "}";
                            return ;
                        }
                        else if( TU_TEST1(ty.m_data, Borrow, .inner->m_data.is_TraitObject()) || TU_TEST1(ty.m_data, Pointer, .inner->m_data.is_TraitObject()) )
                        {
                            const auto& to = (ty.m_data.is_Borrow() ? ty.m_data.as_Borrow().inner : ty.m_data.as_Pointer().inner)->m_data.as_TraitObject();
                            const auto& trait_path = to.m_trait.m_path;
                            MIR_ASSERT(*m_mir_res, vi.is_Static(), "BorrowOf returning &TraitObject not of a static - " << pe.m_path << " is " << vi.tag_str());
                            const auto& stat = vi.as_Static();
                            auto vtable_path = ::HIR::Path(stat.m_type.clone(), trait_path.clone(), "#vtable");
                            m_of << "{ &" << Trans_Mangle( params.monomorph(m_resolve, e)) << ", &" << Trans_Mangle(vtable_path) << "}";
                            return ;
                        }
                        else
                        {
                            m_of << "&";
                        }
                    }
                    ),
                (UfcsUnknown,
                    MIR_BUG(*m_mir_res, "UfcsUnknown in trans " << e);
                    ),
                (UfcsInherent,
                    m_of << "&";
                    ),
                (UfcsKnown,
                    m_of << "&";
                    )
                )
                m_of << Trans_Mangle( params.monomorph(m_resolve, e));
                ),
            (BorrowData,
                MIR_TODO(*m_mir_res, "Handle BorrowData (emit_literal) - " << *e);
                ),
            (String,
                m_of << "{ ";
                this->print_escaped_string(e);
                m_of << ", " << e.size() << "}";
                )
            )
        }

        //void print_escaped_string(const ::std::string& s)
        template<typename T>
        void print_escaped_string(const T& s)
        {
            m_of << "\"" << ::std::hex;
            for(const auto& v : s)
            {
                switch(v)
                {
                case '"':
                    m_of << "\\\"";
                    break;
                case '\\':
                    m_of << "\\\\";
                    break;
                case '\n':
                    m_of << "\\n";
                    break;
                case '?':
                    if( *(&v + 1) == '?' )
                    {
                        if( *(&v + 2) == '!' )
                        {
                            // Trigraph! Needs an escape in it.
                            m_of << v;
                            m_of << "\"\"";
                            break;
                        }
                    }
                    // Fall through
                default:
                    if( ' ' <= v && static_cast<uint8_t>(v) < 0x7F )
                        m_of << v;
                    else {
                        if( static_cast<uint8_t>(v) < 16 )
                            m_of << "\\x0" << (unsigned int)static_cast<uint8_t>(v);
                        else
                            m_of << "\\x" << (unsigned int)static_cast<uint8_t>(v);
                        // If the next character is a hex digit,
                        // close/reopen the string.
                        if( isxdigit(*(&v+1)) )
                            m_of << "\"\"";
                    }
                }
            }
            m_of << "\"" << ::std::dec;
        }

        void emit_vtable(const ::HIR::Path& p, const ::HIR::Trait& trait) override
        {
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "vtable " << p;), ::HIR::TypeRef(), {}, *(::MIR::Function*)nullptr };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);
            const auto& trait_path = p.m_data.as_UfcsKnown().trait;
            const auto& type = *p.m_data.as_UfcsKnown().type;

            // TODO: Hack in fn pointer VTable handling
            if( const auto* te = type.m_data.opt_Function() )
            {
                const char* names[] = { "call", "call_mut" };
                const ::HIR::SimplePath* traits[] = { &m_resolve.m_lang_Fn, &m_resolve.m_lang_FnMut };
                size_t  offset;
                if( trait_path.m_path == m_resolve.m_lang_Fn )
                    offset = 0;
                else if( trait_path.m_path == m_resolve.m_lang_FnMut )
                    offset = 1;
                //else if( trait_path.m_path == m_resolve.m_lang_FnOnce )
                //    call_fcn_name = "call_once";
                else
                    offset = 2;

                while(offset < sizeof(names)/sizeof(names[0]))
                {
                    const auto& trait_name = *traits[offset];
                    const char* call_fcn_name = names[offset++];
                    auto fcn_p = p.clone();
                    fcn_p.m_data.as_UfcsKnown().item = call_fcn_name;
                    fcn_p.m_data.as_UfcsKnown().trait.m_path = trait_name.clone();
                    emit_ctype(*te->m_rettype);
                    auto  arg_ty = ::HIR::TypeRef::new_unit();
                    for(const auto& ty : te->m_arg_types)
                        arg_ty.m_data.as_Tuple().push_back( ty.clone() );
                    m_of << " " << Trans_Mangle(fcn_p) << "("; emit_ctype(type, FMT_CB(ss, ss << "*ptr";)); m_of << ", "; emit_ctype(arg_ty, FMT_CB(ss, ss << "args";)); m_of << ") {\n";
                    m_of << "\treturn (*ptr)(";
                        for(unsigned int i = 0; i < te->m_arg_types.size(); i++)
                        {
                            if(i != 0)  m_of << ", ";
                            m_of << "args._" << i;
                        }
                        m_of << ");\n";
                    m_of << "}\n";
                }
            }

            {
                auto vtable_sp = trait_path.m_path;
                vtable_sp.m_components.back() += "#vtable";
                auto vtable_params = trait_path.m_params.clone();
                for(const auto& ty : trait.m_type_indexes) {
                    auto aty = ::HIR::TypeRef( ::HIR::Path( type.clone(), trait_path.clone(), ty.first ) );
                    m_resolve.expand_associated_types(sp, aty);
                    vtable_params.m_types.push_back( mv$(aty) );
                }
                const auto& vtable_ref = m_crate.get_struct_by_path(sp, vtable_sp);
                ::HIR::TypeRef  vtable_ty( ::HIR::GenericPath(mv$(vtable_sp), mv$(vtable_params)), &vtable_ref );

                if( m_compiler == Compiler::Msvc )
                {
                    // Weak link for vtables
                    m_of << "__declspec(selectany) ";
                }

                emit_ctype(vtable_ty);
                m_of << " " << Trans_Mangle(p) << " = {\n";
            }

            auto monomorph_cb_trait = monomorphise_type_get_cb(sp, &type, &trait_path.m_params, nullptr);

            // Size, Alignment, and destructor
            m_of << "\t{ ";
            m_of << "sizeof("; emit_ctype(type); m_of << "),";
            m_of << "ALIGNOF("; emit_ctype(type); m_of << "),";
            if( type.m_data.is_Borrow() || m_resolve.type_is_copy(sp, type) )
            {
                m_of << "noop_drop,";
            }
            else
            {
                m_of << "(void*)" << Trans_Mangle(::HIR::Path(type.clone(), "#drop_glue")) << ",";
            }
            m_of << "}";    // No newline, added below

            for(unsigned int i = 0; i < trait.m_value_indexes.size(); i ++ )
            {
                m_of << ",\n";
                for(const auto& m : trait.m_value_indexes)
                {
                    if( m.second.first != i )
                        continue ;

                    //MIR_ASSERT(*m_mir_res, tr.m_values.at(m.first).is_Function(), "TODO: Handle generating vtables with non-function items");
                    DEBUG("- " << m.second.first << " = " << m.second.second << " :: " << m.first);

                    auto gpath = monomorphise_genericpath_with(sp, m.second.second, monomorph_cb_trait, false);
                    // NOTE: `void*` cast avoids mismatched pointer type errors due to the receiver being &mut()/&() in the vtable
                    m_of << "\t(void*)" << Trans_Mangle( ::HIR::Path(type.clone(), mv$(gpath), m.first) );
                }
            }
            m_of << "\n";
            m_of << "\t};\n";

            m_mir_res = nullptr;
        }

        void emit_function_ext(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params) override
        {
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "extern fn " << p;), ::HIR::TypeRef(), {}, *(::MIR::Function*)nullptr };
            m_mir_res = &top_mir_res;
            TRACE_FUNCTION_F(p);

            m_of << "// EXTERN extern \"" << item.m_abi << "\" " << p << "\n";
            m_of << "extern ";
            emit_function_header(p, item, params);
            if( item.m_linkage.name != "" )
            {
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    m_of << " asm(\"" << item.m_linkage.name << "\")";
                    break;
                case Compiler::Msvc:
                    m_of << " {\n";
                    m_of << "\t";
                    if( TU_TEST1(item.m_return.m_data, Tuple, .size() == 0) )
                        ;
                    else if( item.m_return.m_data.is_Diverge() )
                        ;
                    else {
                        m_of << "return ";
                        if( item.m_return.m_data.is_Pointer() )
                            m_of << "(void*)";
                    }
                    m_of << item.m_linkage.name << "(";
                    for(size_t i = 0; i < item.m_args.size(); i ++ )
                    {
                        if( i > 0 )
                            m_of << ", ";
                        m_of << "arg" << i;
                    }
                    m_of << ");\n";
                    m_of << "}";
                    break;
                }
            }
            m_of << ";\n";

            m_mir_res = nullptr;
        }
        void emit_function_proto(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params, bool is_extern_def) override
        {
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "/*proto*/ fn " << p;), ::HIR::TypeRef(), {}, *(::MIR::Function*)nullptr };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);
            m_of << "// PROTO extern \"" << item.m_abi << "\" " << p << "\n";
            if( item.m_linkage.name != "" )
            {
                m_of << "#define " << Trans_Mangle(p) << " " << item.m_linkage.name << "\n";
            }
            if( is_extern_def )
            {
                m_of << "static ";
            }
            emit_function_header(p, item, params);
            m_of << ";\n";

            m_mir_res = nullptr;
        }
        void emit_function_code(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params, bool is_extern_def, const ::MIR::FunctionPointer& code) override
        {
            TRACE_FUNCTION_F(p);

            ::MIR::TypeResolve::args_t  arg_types;
            for(const auto& ent : item.m_args)
                arg_types.push_back(::std::make_pair( ::HIR::Pattern{}, params.monomorph(m_resolve, ent.second) ));

            ::HIR::TypeRef  ret_type_tmp;
            const auto& ret_type = monomorphise_fcn_return(ret_type_tmp, item, params);

            ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << p;), ret_type, arg_types, *code };
            m_mir_res = &mir_res;

            m_of << "// " << p << "\n";
            if( is_extern_def ) {
                m_of << "static ";
            }
            emit_function_header(p, item, params);
            m_of << "\n";
            m_of << "{\n";
            // Variables
            m_of << "\t"; emit_ctype(ret_type, FMT_CB(ss, ss << "rv";)); m_of << ";\n";
            for(unsigned int i = 0; i < code->locals.size(); i ++) {
                DEBUG("var" << i << " : " << code->locals[i]);
                m_of << "\t"; emit_ctype(code->locals[i], FMT_CB(ss, ss << "var" << i;)); m_of << ";";
                m_of << "\t// " << code->locals[i];
                m_of << "\n";
            }
            for(unsigned int i = 0; i < code->drop_flags.size(); i ++) {
                m_of << "\tbool df" << i << " = " << code->drop_flags[i] << ";\n";
            }

            ::std::vector<unsigned> bb_use_counts( code->blocks.size() );
            for(const auto& blk : code->blocks)
            {
                TU_MATCHA( (blk.terminator), (te),
                (Incomplete,
                    ),
                (Return,
                    ),
                (Diverge,
                    ),
                (Goto,
                    bb_use_counts[te] ++;
                    ),
                (Panic,
                    bb_use_counts[te.dst] ++;
                    ),
                (If,
                    bb_use_counts[te.bb0] ++;
                    bb_use_counts[te.bb1] ++;
                    ),
                (Switch,
                    for(const auto& t : te.targets)
                        bb_use_counts[t] ++;
                    ),
                (SwitchValue,
                    for(const auto& t : te.targets)
                        bb_use_counts[t] ++;
                    bb_use_counts[te.def_target] ++;
                    ),
                (Call,
                    bb_use_counts[te.ret_block] ++;
                    )
                )
            }

            const bool EMIT_STRUCTURED = false; // Saves time.
            const bool USE_STRUCTURED = false;  // Still not correct.
            if( EMIT_STRUCTURED )
            {
                m_of << "#if " << USE_STRUCTURED << "\n";
                auto nodes = MIR_To_Structured(*code);
                for(const auto& node : nodes)
                {
                    emit_fcn_node(mir_res, node, 1);
                }

                m_of << "#else\n";
            }

            for(unsigned int i = 0; i < code->blocks.size(); i ++)
            {
                TRACE_FUNCTION_F(p << " bb" << i);

                // HACK: Ignore any blocks that only contain `diverge;`
                if( code->blocks[i].statements.size() == 0 && code->blocks[i].terminator.is_Diverge() ) {
                    DEBUG("- Diverge only, omitting");
                    m_of << "bb" << i << ": _Unwind_Resume(); // Diverge\n";
                    continue ;
                }

                // If the previous block is a goto/function call to this
                // block, AND this block only has a single reference, omit the
                // label.
                if( bb_use_counts.at(i) == 0 )
                {
                    if( i == 0 )
                    {
                        // First BB, don't print label
                    }
                    else
                    {
                        // Unused BB (likely part of unsupported panic path)
                        continue ;
                    }
                }
                else if( bb_use_counts.at(i) == 1 )
                {
                    if( i > 0 && (TU_TEST1(code->blocks[i-1].terminator, Goto, == i) || TU_TEST1(code->blocks[i-1].terminator, Call, .ret_block == i)) )
                    {
                        // Don't print the label, only use is previous block
                    }
                    else
                    {
                        m_of << "bb" << i << ":\n";
                    }
                }
                else
                {
                    m_of << "bb" << i << ":\n";
                }

                for(const auto& stmt : code->blocks[i].statements)
                {
                    mir_res.set_cur_stmt(i, (&stmt - &code->blocks[i].statements.front()));
                    emit_statement(mir_res, stmt);
                }

                mir_res.set_cur_stmt_term(i);
                DEBUG("- " << code->blocks[i].terminator);
                TU_MATCHA( (code->blocks[i].terminator), (e),
                (Incomplete,
                    m_of << "\tfor(;;);\n";
                    ),
                (Return,
                    m_of << "\treturn rv;\n";
                    ),
                (Diverge,
                    m_of << "\t_Unwind_Resume();\n";
                    ),
                (Goto,
                    if( e == i+1 )
                    {
                        // Let it flow on to the next block
                    }
                    else
                    {
                        m_of << "\tgoto bb" << e << ";\n";
                    }
                    ),
                (Panic,
                    m_of << "\tgoto bb" << e << "; /* panic */\n";
                    ),
                (If,
                    m_of << "\tif("; emit_lvalue(e.cond); m_of << ") goto bb" << e.bb0 << "; else goto bb" << e.bb1 << ";\n";
                    ),
                (Switch,
                    emit_term_switch(mir_res, e.val, e.targets.size(), 1, [&](size_t idx) {
                        m_of << "goto bb" << e.targets[idx] << ";";
                        });
                    ),
                (SwitchValue,
                    emit_term_switchvalue(mir_res, e.val, e.values, 1, [&](size_t idx) {
                        m_of << "goto bb" << (idx == SIZE_MAX ? e.def_target : e.targets[idx]) << ";";
                        });
                    ),
                (Call,
                    emit_term_call(mir_res, e, 1);
                    if( e.ret_block == i+1 )
                    {
                        // Let it flow on to the next block
                    }
                    else
                    {
                        m_of << "\tgoto bb" << e.ret_block << ";\n";
                    }
                    )
                )
                m_of << "\t// ^ " << code->blocks[i].terminator << "\n";
            }

            if( EMIT_STRUCTURED )
            {
                m_of << "#endif\n";
            }
            m_of << "}\n";
            m_of.flush();
            m_mir_res = nullptr;
        }

        void emit_fcn_node(::MIR::TypeResolve& mir_res, const Node& node, unsigned indent_level)
        {
            auto indent = RepeatLitStr { "\t", static_cast<int>(indent_level) };
            TU_MATCHA( (node), (e),
            (Block,
                for(size_t i = 0; i < e.nodes.size(); i ++)
                {
                    const auto& snr = e.nodes[i];
                    if( snr.node ) {
                        emit_fcn_node(mir_res, *snr.node, indent_level);
                    }
                    else {
                        DEBUG(mir_res << "BB" << snr.bb_idx);
                        m_of << indent << "bb" << snr.bb_idx << ":\n";
                        const auto& bb = mir_res.m_fcn.blocks.at(snr.bb_idx);
                        for(const auto& stmt : bb.statements)
                        {
                            mir_res.set_cur_stmt(snr.bb_idx, (&stmt - &bb.statements.front()));
                            this->emit_statement(mir_res, stmt, indent_level);
                        }

                        TU_MATCHA( (bb.terminator), (te),
                        (Incomplete, ),
                        (Return,
                            assert(i == e.nodes.size()-1 && "Return");
                            m_of << indent << "return rv;\n";
                            ),
                        (Goto,
                            // Ignore (handled by caller)
                            ),
                        (Diverge,
                            m_of << indent << "_Unwind_Resume();\n";
                            ),
                        (Panic,
                            ),
                        (If,
                            //assert(i == e.nodes.size()-1 && "If");
                            ),
                        (Call,
                            emit_term_call(mir_res, te, indent_level);
                            ),
                        (Switch,
                            //assert(i == e.nodes.size()-1 && "Switch");
                            ),
                        (SwitchValue,
                            //assert(i == e.nodes.size()-1 && "Switch");
                            )
                        )
                    }
                }
                ),
            (If,
                m_of << indent << "if("; emit_lvalue(*e.val); m_of << ") {\n";
                if( e.arm_true.node ) {
                    emit_fcn_node(mir_res, *e.arm_true.node, indent_level+1);
                }
                else {
                    m_of << indent << "\tgoto bb" << e.arm_true.bb_idx << ";\n";
                }
                m_of << indent << "}\n";
                m_of << indent << "else {\n";
                if( e.arm_false.node ) {
                    emit_fcn_node(mir_res, *e.arm_false.node, indent_level+1);
                }
                else {
                    m_of << indent << "\tgoto bb" << e.arm_false.bb_idx << ";\n";
                }
                m_of << indent << "}\n";
                ),
            (Switch,
                this->emit_term_switch(mir_res, *e.val, e.arms.size(), indent_level, [&](auto idx) {
                    const auto& arm = e.arms.at(idx);
                    if( arm.node ) {
                        m_of << "{\n";
                        this->emit_fcn_node(mir_res, *arm.node, indent_level+1);
                        m_of << indent  << "\t} ";
                        if( arm.has_target() && arm.target() != e.next_bb ) {
                            m_of << "goto bb" << arm.target() << ";";
                        }
                        else {
                            m_of << "break;";
                        }
                    }
                    else {
                        m_of << "goto bb" << arm.bb_idx << ";";
                    }
                    });
                ),
            (SwitchValue,
                this->emit_term_switchvalue(mir_res, *e.val, *e.vals, indent_level, [&](auto idx) {
                    const auto& arm = (idx == SIZE_MAX ? e.def_arm : e.arms.at(idx));
                    if( arm.node ) {
                        m_of << "{\n";
                        this->emit_fcn_node(mir_res, *arm.node, indent_level+1);
                        m_of << indent  << "\t} ";
                        if( arm.has_target() && arm.target() != e.next_bb ) {
                            m_of << "goto bb" << arm.target() << ";";
                        }
                    }
                    else {
                        m_of << "goto bb" << arm.bb_idx << ";";
                    }
                    });
                ),
            (Loop,
                m_of << indent << "for(;;) {\n";
                assert(e.code.node);
                assert(e.code.node->is_Block());
                this->emit_fcn_node(mir_res, *e.code.node, indent_level+1);
                m_of << indent << "}\n";
                )
            )
        }

        bool type_is_emulated_i128(const ::HIR::TypeRef& ty) const
        {
            return m_options.emulated_i128 && (ty == ::HIR::CoreType::I128 || ty == ::HIR::CoreType::U128);
        }

        void emit_statement(const ::MIR::TypeResolve& mir_res, const ::MIR::Statement& stmt, unsigned indent_level=1)
        {
            auto indent = RepeatLitStr { "\t", static_cast<int>(indent_level) };
            switch( stmt.tag() )
            {
            case ::MIR::Statement::TAGDEAD: throw "";
            case ::MIR::Statement::TAG_ScopeEnd:
                m_of << indent << "// " << stmt << "\n";
                break;
            case ::MIR::Statement::TAG_SetDropFlag: {
                const auto& e = stmt.as_SetDropFlag();
                m_of << indent << "df" << e.idx << " = ";
                if( e.other == ~0u )
                    m_of << e.new_val;
                else
                    m_of << (e.new_val ? "!" : "") << "df" << e.other;
                m_of << ";\n";
                break; }
            case ::MIR::Statement::TAG_Drop: {
                const auto& e = stmt.as_Drop();
                ::HIR::TypeRef  tmp;
                const auto& ty = mir_res.get_lvalue_type(tmp, e.slot);

                if( e.flag_idx != ~0u )
                    m_of << indent << "if( df" << e.flag_idx << " ) {\n";

                switch( e.kind )
                {
                case ::MIR::eDropKind::SHALLOW:
                    // Shallow drops are only valid on owned_box
                    if( const auto* ity = m_resolve.is_type_owned_box(ty) )
                    {
                        // Emit a call to box_free for the type
                        ::HIR::GenericPath  box_free { m_crate.get_lang_item_path(sp, "box_free"), { ity->clone() } };
                        // TODO: This is specific to the official liballoc's owned_box
                        m_of << indent << Trans_Mangle(box_free) << "("; emit_lvalue(e.slot); m_of << "._0._0._0);\n";
                    }
                    else
                    {
                        MIR_BUG(mir_res, "Shallow drop on non-Box - " << ty);
                    }
                    break;
                case ::MIR::eDropKind::DEEP:
                    emit_destructor_call(e.slot, ty, false, indent_level + (e.flag_idx != ~0u ? 1 : 0));
                    break;
                }
                if( e.flag_idx != ~0u )
                    m_of << indent << "}\n";
                break; }
            case ::MIR::Statement::TAG_Asm:
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    this->emit_asm_gcc(mir_res, stmt.as_Asm(), indent_level);
                    break;
                case Compiler::Msvc:
                    this->emit_asm_msvc(mir_res, stmt.as_Asm(), indent_level);
                    break;
                }
                break;
            case ::MIR::Statement::TAG_Assign: {
                const auto& e = stmt.as_Assign();
                DEBUG("- " << e.dst << " = " << e.src);
                m_of << indent;
                TU_MATCHA( (e.src), (ve),
                (Use,
                    ::HIR::TypeRef  tmp;
                    const auto& ty = mir_res.get_lvalue_type(tmp, ve);
                    if( ty == ::HIR::TypeRef::new_diverge() ) {
                        m_of << "abort()";
                        break;
                    }
                    emit_lvalue(e.dst);
                    m_of << " = ";
                    emit_lvalue(ve);
                    ),
                (Constant,
                    emit_lvalue(e.dst);
                    m_of << " = ";
                    emit_constant(ve, &e.dst);
                    ),
                (SizedArray,
                    if( ve.count == 0 ) {
                    }
                    else if( ve.count == 1 ) {
                        emit_lvalue(e.dst); m_of << ".DATA[0] = "; emit_param(ve.val);
                    }
                    else if( ve.count == 2 ) {
                        emit_lvalue(e.dst); m_of << ".DATA[0] = "; emit_param(ve.val); m_of << ";\n" << indent;
                        emit_lvalue(e.dst); m_of << ".DATA[1] = "; emit_param(ve.val);
                    }
                    else if( ve.count == 3 ) {
                        emit_lvalue(e.dst); m_of << ".DATA[0] = "; emit_param(ve.val); m_of << ";\n" << indent;
                        emit_lvalue(e.dst); m_of << ".DATA[1] = "; emit_param(ve.val); m_of << ";\n" << indent;
                        emit_lvalue(e.dst); m_of << ".DATA[2] = "; emit_param(ve.val);
                    }
                    else {
                        m_of << "for(unsigned int i = 0; i < " << ve.count << "; i ++)\n";
                        m_of << indent << "\t"; emit_lvalue(e.dst); m_of << ".DATA[i] = "; emit_param(ve.val);
                    }
                    ),
                (Borrow,
                    ::HIR::TypeRef  tmp;
                    const auto& ty = mir_res.get_lvalue_type(tmp, ve.val);
                    bool special = false;
                    // If the inner value has type [T] or str, create DST based on inner pointer and existing metadata
                    TU_IFLET(::MIR::LValue, ve.val, Deref, le,
                        if( metadata_type(ty) != MetadataType::None ) {
                            emit_lvalue(e.dst);
                            m_of << " = ";
                            emit_lvalue(*le.val);
                            special = true;
                        }
                    )
                    // Magic for taking a &-ptr to unsized field of a struct.
                    // - Needs to get metadata from bottom-level pointer.
                    else TU_IFLET(::MIR::LValue, ve.val, Field, le,
                        if( metadata_type(ty) != MetadataType::None ) {
                            const ::MIR::LValue* base_val = &*le.val;
                            while(base_val->is_Field())
                                base_val = &*base_val->as_Field().val;
                            MIR_ASSERT(mir_res, base_val->is_Deref(), "DST access must be via a deref");
                            const ::MIR::LValue& base_ptr = *base_val->as_Deref().val;

                            // Construct the new DST
                            emit_lvalue(e.dst); m_of << ".META = "; emit_lvalue(base_ptr); m_of << ".META;\n" << indent;
                            emit_lvalue(e.dst); m_of << ".PTR = &"; emit_lvalue(ve.val);
                            special = true;
                        }
                    )
                    if( !special )
                    {
                        emit_lvalue(e.dst);
                        m_of << " = ";
                        m_of << "& "; emit_lvalue(ve.val);
                    }
                    ),
                (Cast,
                    emit_rvalue_cast(mir_res, e.dst, ve);
                    ),
                (BinOp,
                    emit_lvalue(e.dst);
                    m_of << " = ";
                    ::HIR::TypeRef  tmp, tmp_r;
                    const auto& ty = mir_res.get_param_type(tmp, ve.val_l);
                    const auto& ty_r = mir_res.get_param_type(tmp_r, ve.val_r);
                    if( ty.m_data.is_Borrow() ) {
                        m_of << "(slice_cmp("; emit_param(ve.val_l); m_of << ", "; emit_param(ve.val_r); m_of << ")";
                        switch(ve.op)
                        {
                        case ::MIR::eBinOp::EQ: m_of << " == 0";    break;
                        case ::MIR::eBinOp::NE: m_of << " != 0";    break;
                        case ::MIR::eBinOp::GT: m_of << " >  0";    break;
                        case ::MIR::eBinOp::GE: m_of << " >= 0";    break;
                        case ::MIR::eBinOp::LT: m_of << " <  0";    break;
                        case ::MIR::eBinOp::LE: m_of << " <= 0";    break;
                        default:
                            MIR_BUG(mir_res, "Unknown comparison of a &-ptr - " << e.src << " with " << ty);
                        }
                        m_of << ")";
                        break;
                    }
                    else if( const auto* te = ty.m_data.opt_Pointer() ) {
                        if( metadata_type(*te->inner) != MetadataType::None )
                        {
                            switch(ve.op)
                            {
                            case ::MIR::eBinOp::EQ:
                                emit_param(ve.val_l); m_of << ".PTR == "; emit_param(ve.val_r); m_of << ".PTR && ";
                                emit_param(ve.val_l); m_of << ".META == "; emit_param(ve.val_r); m_of << ".META";
                                break;
                            case ::MIR::eBinOp::NE:
                                emit_param(ve.val_l); m_of << ".PTR != "; emit_param(ve.val_r); m_of << ".PTR || ";
                                emit_param(ve.val_l); m_of << ".META != "; emit_param(ve.val_r); m_of << ".META";
                                break;
                            default:
                                MIR_BUG(mir_res, "Unknown comparison of a *-ptr - " << e.src << " with " << ty);
                            }
                        }
                        else
                        {
                            emit_param(ve.val_l);
                            switch(ve.op)
                            {
                            case ::MIR::eBinOp::EQ: m_of << " == "; break;
                            case ::MIR::eBinOp::NE: m_of << " != "; break;
                            case ::MIR::eBinOp::GT: m_of << " > " ; break;
                            case ::MIR::eBinOp::GE: m_of << " >= "; break;
                            case ::MIR::eBinOp::LT: m_of << " < " ; break;
                            case ::MIR::eBinOp::LE: m_of << " <= "; break;
                            default:
                                MIR_BUG(mir_res, "Unknown comparison of a *-ptr - " << e.src << " with " << ty);
                            }
                            emit_param(ve.val_r);
                        }
                        break;
                    }
                    else if( ve.op == ::MIR::eBinOp::MOD && (ty == ::HIR::CoreType::F32 || ty == ::HIR::CoreType::F64) ) {
                        if( ty == ::HIR::CoreType::F32 )
                            m_of << "remainderf";
                        else
                            m_of << "remainder";
                        m_of << "("; emit_param(ve.val_l); m_of << ", "; emit_param(ve.val_r); m_of << ")";
                        break;
                    }
                    else if( type_is_emulated_i128(ty) )
                    {
                        switch (ve.op)
                        {
                        case ::MIR::eBinOp::ADD:   m_of << "add128";    if(0)
                        case ::MIR::eBinOp::SUB:   m_of << "sub128";    if(0)
                        case ::MIR::eBinOp::MUL:   m_of << "mul128";    if(0)
                        case ::MIR::eBinOp::DIV:   m_of << "div128";    if(0)
                        case ::MIR::eBinOp::MOD:   m_of << "mod128";    if(0)
                        case ::MIR::eBinOp::BIT_OR:    m_of << "or128";  if(0)
                        case ::MIR::eBinOp::BIT_AND:   m_of << "and128"; if(0)
                        case ::MIR::eBinOp::BIT_XOR:   m_of << "xor128";
                            if( ty == ::HIR::CoreType::I128 )
                                m_of << "s";
                            m_of << "("; emit_param(ve.val_l); m_of << ", "; emit_param(ve.val_r); m_of << ")";
                            break;
                        case ::MIR::eBinOp::BIT_SHR:   m_of << "shr128"; if (0)
                        case ::MIR::eBinOp::BIT_SHL:   m_of << "shl128";
                            if( ty == ::HIR::CoreType::I128 )
                                m_of << "s";
                            m_of << "("; emit_param(ve.val_l); m_of << ", "; emit_param(ve.val_r);
                            if( (ty_r == ::HIR::CoreType::I128 || ty_r == ::HIR::CoreType::U128) )
                            {
                                m_of << ".lo";
                            }
                            m_of << ")";
                            break;

                        case ::MIR::eBinOp::EQ:    m_of << "0 == "; if(0)
                        case ::MIR::eBinOp::NE:    m_of << "0 != "; if(0)
                        case ::MIR::eBinOp::GT:    m_of << "0 > ";  if(0)
                        case ::MIR::eBinOp::GE:    m_of << "0 >= "; if(0)
                        case ::MIR::eBinOp::LT:    m_of << "0 < ";  if(0)
                        case ::MIR::eBinOp::LE:    m_of << "0 <= ";
                            // NOTE: Reversed order due to reversed logic above
                            m_of << "cmp128";
                            if (ty == ::HIR::CoreType::I128)
                                m_of << "s";
                            m_of << "("; emit_param(ve.val_r); m_of << ", "; emit_param(ve.val_l); m_of << ")";
                            break;

                        case ::MIR::eBinOp::ADD_OV:
                        case ::MIR::eBinOp::SUB_OV:
                        case ::MIR::eBinOp::MUL_OV:
                        case ::MIR::eBinOp::DIV_OV:
                            MIR_TODO(mir_res, "Overflowing binops for emulated i128");
                            break;
                        }
                        break;
                    }
                    else {
                    }

                    emit_param(ve.val_l);
                    switch(ve.op)
                    {
                    case ::MIR::eBinOp::ADD:   m_of << " + ";    break;
                    case ::MIR::eBinOp::SUB:   m_of << " - ";    break;
                    case ::MIR::eBinOp::MUL:   m_of << " * ";    break;
                    case ::MIR::eBinOp::DIV:   m_of << " / ";    break;
                    case ::MIR::eBinOp::MOD:   m_of << " % ";    break;

                    case ::MIR::eBinOp::BIT_OR:    m_of << " | ";    break;
                    case ::MIR::eBinOp::BIT_AND:   m_of << " & ";    break;
                    case ::MIR::eBinOp::BIT_XOR:   m_of << " ^ ";    break;
                    case ::MIR::eBinOp::BIT_SHR:   m_of << " >> ";   break;
                    case ::MIR::eBinOp::BIT_SHL:   m_of << " << ";   break;
                    case ::MIR::eBinOp::EQ:    m_of << " == ";   break;
                    case ::MIR::eBinOp::NE:    m_of << " != ";   break;
                    case ::MIR::eBinOp::GT:    m_of << " > " ;   break;
                    case ::MIR::eBinOp::GE:    m_of << " >= ";   break;
                    case ::MIR::eBinOp::LT:    m_of << " < " ;   break;
                    case ::MIR::eBinOp::LE:    m_of << " <= ";   break;

                    case ::MIR::eBinOp::ADD_OV:
                    case ::MIR::eBinOp::SUB_OV:
                    case ::MIR::eBinOp::MUL_OV:
                    case ::MIR::eBinOp::DIV_OV:
                        MIR_TODO(mir_res, "Overflow");
                        break;
                    }
                    emit_param(ve.val_r);
                    if( type_is_emulated_i128(ty_r) )
                    {
                        m_of << ".lo";
                    }
                    ),
                (UniOp,
                    ::HIR::TypeRef  tmp;
                    const auto& ty = mir_res.get_lvalue_type(tmp, e.dst);

                    if( type_is_emulated_i128(ty) )
                    {
                        switch (ve.op)
                        {
                        case ::MIR::eUniOp::NEG:
                            emit_lvalue(e.dst);
                            m_of << ".lo = -"; emit_lvalue(ve.val); m_of << ".lo; ";
                            emit_lvalue(e.dst);
                            m_of << ".hi = -"; emit_lvalue(ve.val); m_of << ".hi";
                            break;
                        case ::MIR::eUniOp::INV:
                            emit_lvalue(e.dst);
                            m_of << ".lo = ~"; emit_lvalue(ve.val); m_of << ".lo; ";
                            emit_lvalue(e.dst);
                            m_of << ".hi = ~"; emit_lvalue(ve.val); m_of << ".hi";
                            break;
                        }
                        break ;
                    }


                    emit_lvalue(e.dst);
                    m_of << " = ";
                    switch(ve.op)
                    {
                    case ::MIR::eUniOp::NEG:    m_of << "-";    break;
                    case ::MIR::eUniOp::INV:
                        if( ty == ::HIR::CoreType::Bool )
                            m_of << "!";
                        else
                            m_of << "~";
                        break;
                    }
                    emit_lvalue(ve.val);
                    ),
                (DstMeta,
                    emit_lvalue(e.dst);
                    m_of << " = ";
                    emit_lvalue(ve.val);
                    m_of << ".META";
                    ),
                (DstPtr,
                    emit_lvalue(e.dst);
                    m_of << " = ";
                    emit_lvalue(ve.val);
                    m_of << ".PTR";
                    ),
                (MakeDst,
                    emit_lvalue(e.dst);  m_of << ".PTR = ";  emit_param(ve.ptr_val);  m_of << ";\n" << indent;
                    emit_lvalue(e.dst);  m_of << ".META = "; emit_param(ve.meta_val);
                    ),
                (Tuple,
                    for(unsigned int j = 0; j < ve.vals.size(); j ++) {
                        if( j != 0 )    m_of << ";\n" << indent;
                        emit_lvalue(e.dst);
                        m_of << "._" << j << " = ";
                        emit_param(ve.vals[j]);
                    }
                    ),
                (Array,
                    for(unsigned int j = 0; j < ve.vals.size(); j ++) {
                        if( j != 0 )    m_of << ";\n" << indent;
                        emit_lvalue(e.dst); m_of << ".DATA[" << j << "] = ";
                        emit_param(ve.vals[j]);
                    }
                    ),
                (Variant,
                    const auto& tyi = m_crate.get_typeitem_by_path(sp, ve.path.m_path);
                    if( tyi.is_Union() )
                    {
                        emit_lvalue(e.dst);
                        m_of << ".var_" << ve.index << " = "; emit_param(ve.val);
                    }
                    else if( const auto* enm_p = tyi.opt_Enum() )
                    {
                        ::HIR::TypeRef  tmp;
                        const auto& ty = mir_res.get_lvalue_type(tmp, e.dst);

                        auto it = m_enum_repr_cache.find(ty.m_data.as_Path().path.m_data.as_Generic());
                        if( it != m_enum_repr_cache.end() )
                        {
                            if( ve.index == 0 ) {
                                // TODO: Use nonzero_path
                                m_of << "memset(&"; emit_lvalue(e.dst); m_of << ", 0, sizeof("; emit_ctype(ty); m_of << "))";
                            }
                            else if( ve.index == 1 ) {
                                emit_lvalue(e.dst);
                                m_of << "._1 = ";
                                emit_param(ve.val);
                            }
                            else {
                            }
                            break;
                        }
                        else if( enm_p->is_value() )
                        {
                            emit_lvalue(e.dst); m_of << ".TAG = " << enm_p->get_value(ve.index) << "";
                        }
                        else
                        {
                            emit_lvalue(e.dst); m_of << ".TAG = " << ve.index << ";\n\t";
                            emit_lvalue(e.dst); m_of << ".DATA";
                            m_of << ".var_" << ve.index << " = "; emit_param(ve.val);
                        }
                    }
                    else
                    {
                        BUG(mir_res.sp, "Unexpected type in Variant");
                    }
                    ),
                (Struct,
                    bool is_val_enum = false;

                    if( ve.vals.empty() )
                    {
                        if( m_options.disallow_empty_structs && !is_val_enum)
                        {
                            emit_lvalue(e.dst);
                            m_of << "._d = 0";
                        }
                    }
                    else
                    {
                        for(unsigned int j = 0; j < ve.vals.size(); j ++)
                        {
                            // HACK: Don't emit assignment of PhantomData
                            ::HIR::TypeRef  tmp;
                            if( ve.vals[j].is_LValue() && m_resolve.is_type_phantom_data( mir_res.get_lvalue_type(tmp, ve.vals[j].as_LValue())) )
                                continue ;

                            if( j != 0 )    m_of << ";\n" << indent;
                            emit_lvalue(e.dst);
                            m_of << "._" << j << " = ";
                            emit_param(ve.vals[j]);
                        }
                    }
                    )
                )
                m_of << ";";
                m_of << "\t// " << e.dst << " = " << e.src;
                m_of << "\n";
                break; }
            }
        }
        void emit_rvalue_cast(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& dst, const ::MIR::RValue::Data_Cast& ve)
        {
            if (m_resolve.is_type_phantom_data(ve.type)) {
                m_of << "/* PhandomData cast */\n";
                return;
            }

            ::HIR::TypeRef  tmp;
            const auto& ty = mir_res.get_lvalue_type(tmp, ve.val);

            // A cast to a fat pointer doesn't actually change the C type.
            if ((ve.type.m_data.is_Pointer() && is_dst(*ve.type.m_data.as_Pointer().inner))
                || (ve.type.m_data.is_Borrow() && is_dst(*ve.type.m_data.as_Borrow().inner))
                // OR: If it's a no-op cast
                || ve.type == ty
                )
            {
                emit_lvalue(dst);
                m_of << " = ";
                emit_lvalue(ve.val);
                return;
            }

            // Emulated i128/u128 support
            if (m_options.emulated_i128 && (
                ve.type == ::HIR::CoreType::U128 || ve.type == ::HIR::CoreType::I128
                || ty == ::HIR::CoreType::U128 || ty == ::HIR::CoreType::I128
                ))
            {
                // Destination
                MIR_ASSERT(mir_res, ve.type.m_data.is_Primitive(), "i128/u128 cast to non-primitive");
                MIR_ASSERT(mir_res, ty.m_data.is_Primitive(), "i128/u128 cast from non-primitive");
                switch (ve.type.m_data.as_Primitive())
                {
                case ::HIR::CoreType::U128:
                    if (ty == ::HIR::CoreType::I128) {
                        // Cast from i128 to u128
                        emit_lvalue(dst);
                        m_of << ".lo = ";
                        emit_lvalue(ve.val);
                        m_of << ".lo; ";
                        emit_lvalue(dst);
                        m_of << ".hi = ";
                        emit_lvalue(ve.val);
                        m_of << ".hi";
                    }
                    else {
                        // Cast from small to u128
                        emit_lvalue(dst);
                        m_of << ".lo = ";
                        emit_lvalue(ve.val);
                        m_of << "; ";
                        emit_lvalue(dst);
                        m_of << ".hi = 0";
                    }
                    break;
                case ::HIR::CoreType::I128:
                    if (ty == ::HIR::CoreType::U128) {
                        // Cast from u128 to i128
                        emit_lvalue(dst);
                        m_of << ".lo = ";
                        emit_lvalue(ve.val);
                        m_of << ".lo; ";
                        emit_lvalue(dst);
                        m_of << ".hi = ";
                        emit_lvalue(ve.val);
                        m_of << ".hi";
                    }
                    else {
                        // Cast from small to i128
                        emit_lvalue(dst);
                        m_of << ".lo = ";
                        emit_lvalue(ve.val);
                        m_of << "; ";
                        emit_lvalue(dst);
                        m_of << ".hi = 0";  // TODO: Sign
                    }
                    break;
                case ::HIR::CoreType::I8:
                case ::HIR::CoreType::I16:
                case ::HIR::CoreType::I32:
                case ::HIR::CoreType::I64:
                case ::HIR::CoreType::Isize:
                    emit_lvalue(dst);
                    m_of << " = ";
                    switch (ty.m_data.as_Primitive())
                    {
                    case ::HIR::CoreType::U128:
                        emit_lvalue(ve.val);
                        m_of << ".lo";
                        break;
                    case ::HIR::CoreType::I128:
                        // TODO: Maintain sign
                        emit_lvalue(ve.val);
                        m_of << ".lo";
                        break;
                    default:
                        MIR_BUG(mir_res, "Unreachable");
                    }
                    break;
                case ::HIR::CoreType::U8:
                case ::HIR::CoreType::U16:
                case ::HIR::CoreType::U32:
                case ::HIR::CoreType::U64:
                case ::HIR::CoreType::Usize:
                    emit_lvalue(dst);
                    m_of << " = ";
                    switch (ty.m_data.as_Primitive())
                    {
                    case ::HIR::CoreType::U128:
                        emit_lvalue(ve.val);
                        m_of << ".lo";
                        break;
                    case ::HIR::CoreType::I128:
                        emit_lvalue(ve.val);
                        m_of << ".lo";
                        break;
                    default:
                        MIR_BUG(mir_res, "Unreachable");
                    }
                    break;
                default:
                    MIR_BUG(mir_res, "Bad i128/u128 cast");
                }
                return;
            }

            // Standard cast
            emit_lvalue(dst);
            m_of << " = ";
            m_of << "("; emit_ctype(ve.type); m_of << ")";
            // TODO: If the source is an unsized borrow, then extract the pointer
            bool special = false;
            // If the destination is a thin pointer
            if (ve.type.m_data.is_Pointer() && !is_dst(*ve.type.m_data.as_Pointer().inner))
            {
                // NOTE: Checks the result of the deref
                if ((ty.m_data.is_Borrow() && is_dst(*ty.m_data.as_Borrow().inner))
                    || (ty.m_data.is_Pointer() && is_dst(*ty.m_data.as_Pointer().inner))
                    )
                {
                    emit_lvalue(ve.val);
                    m_of << ".PTR";
                    special = true;
                }
            }
            if (ve.type.m_data.is_Primitive() && ty.m_data.is_Path() && ty.m_data.as_Path().binding.is_Enum())
            {
                emit_lvalue(ve.val);
                m_of << ".TAG";
                special = true;
            }
            if (!special)
            {
                emit_lvalue(ve.val);
            }
        }
        void emit_term_switch(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& val, size_t n_arms, unsigned indent_level, ::std::function<void(size_t)> cb)
        {
            auto indent = RepeatLitStr { "\t", static_cast<int>(indent_level) };

            ::HIR::TypeRef  tmp;
            const auto& ty = mir_res.get_lvalue_type(tmp, val);
            MIR_ASSERT(mir_res, ty.m_data.is_Path(), "Switch over non-Path type");
            MIR_ASSERT(mir_res, ty.m_data.as_Path().binding.is_Enum(), "Switch over non-enum");
            const auto* enm = ty.m_data.as_Path().binding.as_Enum();

            auto it = m_enum_repr_cache.find( ty.m_data.as_Path().path.m_data.as_Generic() );
            if( it != m_enum_repr_cache.end() )
            {
                //MIR_ASSERT(mir_res, e.targets.size() == 2, "NonZero optimised representation for an enum without two variants");
                MIR_ASSERT(mir_res, n_arms == 2, "NonZero optimised switch without two arms");
                m_of << indent << "if("; emit_lvalue(val); m_of << "._1"; emit_nonzero_path(it->second); m_of << ")\n";
                m_of << indent;
                cb(1);
                m_of << "\n";
                m_of << indent << "else\n";
                m_of << indent;
                cb(0);
                m_of << "\n";
            }
            else if( enm->is_value() )
            {
                m_of << indent << "switch("; emit_lvalue(val); m_of << ".TAG) {\n";
                for(size_t j = 0; j < n_arms; j ++)
                {
                    m_of << indent << "case " << enm->get_value(j) << ": ";
                    cb(j);
                    m_of << "\n";
                }
                m_of << indent << "default: abort();\n";
                m_of << indent << "}\n";
            }
            else
            {
                m_of << indent << "switch("; emit_lvalue(val); m_of << ".TAG) {\n";
                for(size_t j = 0; j < n_arms; j ++)
                {
                    m_of << indent << "case " << j << ": ";
                    cb(j);
                    m_of << "\n";
                }
                m_of << indent << "default: abort();\n";
                m_of << indent << "}\n";
            }
        }
        void emit_term_switchvalue(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& val, const ::MIR::SwitchValues& values, unsigned indent_level, ::std::function<void(size_t)> cb)
        {
            auto indent = RepeatLitStr { "\t", static_cast<int>(indent_level) };

            ::HIR::TypeRef  tmp;
            const auto& ty = mir_res.get_lvalue_type(tmp, val);
            if( const auto* ve = values.opt_String() ) {
                m_of << indent << "{ static SLICE_PTR switch_strings[] = {";
                for(const auto& v : *ve)
                {
                    m_of << " {"; this->print_escaped_string(v); m_of << "," << v.size() << "},";
                }
                m_of << " {0,0} };\n";
                m_of << indent << "switch( mrustc_string_search_linear("; emit_lvalue(val); m_of << ", " << ve->size() << ", switch_strings) ) {\n";
                for(size_t i = 0; i < ve->size(); i++)
                {
                    m_of << indent << "case " << i << ": "; cb(i); m_of << " break;\n";
                }
                m_of << indent << "default: "; cb(SIZE_MAX); m_of << "\n";
                m_of << indent << "} }\n";
            }
            else if( const auto* ve = values.opt_Unsigned() ) {
                m_of << indent << "switch("; emit_lvalue(val);
                // TODO: Ensure that .hi is zero
                if(m_options.emulated_i128 && ty == ::HIR::CoreType::U128)
                    m_of << ".lo";
                m_of << ") {\n";
                for(size_t i = 0; i < ve->size(); i++)
                {
                    m_of << indent << "\tcase " << (*ve)[i] << "ull: "; cb(i); m_of << " break;\n";
                }
                m_of << indent << "\tdefault: "; cb(SIZE_MAX); m_of << "\n";
                m_of << indent << "}\n";
            }
            else if( const auto* ve = values.opt_Signed() ) {
                //assert(ve->size() == e.targets.size());
                m_of << indent << "switch("; emit_lvalue(val);
                // TODO: Ensure that .hi is zero
                if(m_options.emulated_i128 && ty == ::HIR::CoreType::I128)
                    m_of << ".lo";
                m_of << ") {\n";
                for(size_t i = 0; i < ve->size(); i++)
                {
                    m_of << indent << "\tcase ";
                    if( (*ve)[i] == INT64_MIN )
                        m_of << "INT64_MIN";
                    else
                        m_of << (*ve)[i] << "ll";
                    m_of << ": "; cb(i); m_of << " break;\n";
                }
                m_of << indent << "\tdefault: "; cb(SIZE_MAX); m_of << "\n";
                m_of << indent << "}\n";
            }
            else {
                MIR_BUG(mir_res, "SwitchValue with unknown value type - " << values.tag_str());
            }
        }
        void emit_term_call(const ::MIR::TypeResolve& mir_res, const ::MIR::Terminator::Data_Call& e, unsigned indent_level)
        {
            auto indent = RepeatLitStr { "\t", static_cast<int>(indent_level) };
            m_of << indent;

            TU_MATCHA( (e.fcn), (e2),
            (Value,
                {
                    ::HIR::TypeRef  tmp;
                    const auto& ty = mir_res.get_lvalue_type(tmp, e2);
                    MIR_ASSERT(mir_res, ty.m_data.is_Function(), "Call::Value on non-function - " << ty);
                    if( !ty.m_data.as_Function().m_rettype->m_data.is_Diverge() )
                    {
                        emit_lvalue(e.ret_val); m_of << " = ";
                    }
                }
                m_of << "("; emit_lvalue(e2); m_of << ")";
                ),
            (Path,
                {
                    bool is_diverge = false;
                    TU_MATCHA( (e2.m_data), (pe),
                    (Generic,
                        const auto& fcn = m_crate.get_function_by_path(sp, pe.m_path);
                        is_diverge |= fcn.m_return.m_data.is_Diverge();
                        // TODO: Monomorph.
                        ),
                    (UfcsUnknown,
                        ),
                    (UfcsInherent,
                        // TODO: Check if the return type is !
                        is_diverge |= m_resolve.m_crate.find_type_impls(*pe.type, [&](const auto& ty)->const auto& { return ty; },
                            [&](const auto& impl) {
                                // Associated functions
                                {
                                    auto it = impl.m_methods.find(pe.item);
                                    if( it != impl.m_methods.end() ) {
                                        return it->second.data.m_return.m_data.is_Diverge();
                                    }
                                }
                                // Associated static (undef)
                                return false;
                            });
                        ),
                    (UfcsKnown,
                        // TODO: Check if the return type is !
                        const auto& tr = m_resolve.m_crate.get_trait_by_path(sp, pe.trait.m_path);
                        const auto& fcn = tr.m_values.find(pe.item)->second.as_Function();
                        const auto& rv_tpl = fcn.m_return;
                        if( rv_tpl.m_data.is_Diverge() )
                        {
                            is_diverge |= true;
                        }
                        else if( const auto* te = rv_tpl.m_data.opt_Generic() )
                        {
                            (void)te;
                            // TODO: Generic lookup
                        }
                        else if( const auto* te = rv_tpl.m_data.opt_Path() )
                        {
                            if( te->binding.is_Opaque() ) {
                                // TODO: Associated type lookup
                            }
                        }
                        else
                        {
                            // Not a ! type
                        }
                        )
                    )
                    if(!is_diverge)
                    {
                        emit_lvalue(e.ret_val); m_of << " = ";
                    }
                }
                m_of << Trans_Mangle(e2);
                ),
            (Intrinsic,
                const auto& name = e.fcn.as_Intrinsic().name;
                const auto& params = e.fcn.as_Intrinsic().params;
                emit_intrinsic_call(name, params, e);
                return ;
                )
            )
            m_of << "(";
            for(unsigned int j = 0; j < e.args.size(); j ++) {
                if(j != 0)  m_of << ",";
                m_of << " "; emit_param(e.args[j]);
            }
            m_of << " );\n";
        }
        void emit_asm_gcc(const ::MIR::TypeResolve& mir_res, const ::MIR::Statement::Data_Asm& e, unsigned indent_level)
        {
            auto indent = RepeatLitStr{ "\t", static_cast<int>(indent_level) };
            struct H {
                static bool has_flag(const ::std::vector<::std::string>& flags, const char* des) {
                    return ::std::find_if(flags.begin(), flags.end(), [des](const auto&x) {return x == des; }) != flags.end();
                }
                static const char* convert_reg(const char* r) {
                    if (::std::strcmp(r, "{eax}") == 0 || ::std::strcmp(r, "{rax}") == 0) {
                        return "a";
                    }
                    else {
                        return r;
                    }
                }
            };
            bool is_volatile = H::has_flag(e.flags, "volatile");
            bool is_intel = H::has_flag(e.flags, "intel");


            m_of << indent << "__asm__ ";
            if (is_volatile) m_of << "__volatile__";
            // TODO: Convert format string?
            // TODO: Use a C-specific escaper here.
            m_of << "(\"" << (is_intel ? ".syntax intel; " : "");
            for (auto it = e.tpl.begin(); it != e.tpl.end(); ++it)
            {
                if (*it == '\n')
                    m_of << ";\\n";
                else if (*it == '"')
                    m_of << "\\\"";
                else if (*it == '\\')
                    m_of << "\\\\";
                else if (*it == '/' && *(it + 1) == '/')
                {
                    while (it != e.tpl.end() || *it == '\n')
                        ++it;
                    --it;
                }
                else if (*it == '%' && *(it + 1) == '%')
                    m_of << "%";
                else if (*it == '%' && !isdigit(*(it + 1)))
                    m_of << "%%";
                else
                    m_of << *it;
            }
            m_of << (is_intel ? ".syntax att; " : "") << "\"";
            m_of << ": ";
            for (unsigned int i = 0; i < e.outputs.size(); i++)
            {
                const auto& v = e.outputs[i];
                if (i != 0)    m_of << ", ";
                m_of << "\"";
                switch (v.first[0])
                {
                case '=':   m_of << "=";    break;
                case '+':   m_of << "+";    break;
                default:    MIR_TODO(mir_res, "Handle asm! output leader '" << v.first[0] << "'");
                }
                m_of << H::convert_reg(v.first.c_str() + 1);
                m_of << "\"("; emit_lvalue(v.second); m_of << ")";
            }
            m_of << ": ";
            for (unsigned int i = 0; i < e.inputs.size(); i++)
            {
                const auto& v = e.inputs[i];
                if (i != 0)    m_of << ", ";
                m_of << "\"" << v.first << "\"("; emit_lvalue(v.second); m_of << ")";
            }
            m_of << ": ";
            for (unsigned int i = 0; i < e.clobbers.size(); i++)
            {
                if (i != 0)    m_of << ", ";
                m_of << "\"" << e.clobbers[i] << "\"";
            }
            m_of << ");\n";
        }
        void emit_asm_msvc(const ::MIR::TypeResolve& mir_res, const ::MIR::Statement::Data_Asm& e, unsigned indent_level)
        {
            auto indent = RepeatLitStr{ "\t", static_cast<int>(indent_level) };

            if( e.tpl == "fnstcw $0" )
            {
                // HARD CODE: `fnstcw` -> _control87
                if( !(e.inputs.size() == 0 && e.outputs.size() == 1 && e.outputs[0].first == "=*m") )
                    MIR_BUG(mir_res, "Hard-coded asm translation doesn't apply - `" << e.tpl << "` inputs=" << e.inputs << " outputs=" << e.outputs);
                m_of << indent << "*("; emit_lvalue(e.outputs[0].second); m_of << ") = _control87(0,0);\n";
                return ;
            }
            else if( e.tpl == "fldcw $0" )
            {
                // HARD CODE: `fldcw` -> _control87
                if( !(e.inputs.size() == 1 && e.inputs[0].first == "m" && e.outputs.size() == 0) )
                    MIR_BUG(mir_res, "Hard-coded asm translation doesn't apply - `" << e.tpl << "` inputs=" << e.inputs << " outputs=" << e.outputs);
                m_of << indent << "_control87("; emit_lvalue(e.inputs[0].second); m_of << ", 0xFFFF);\n";
                return ;
            }
            else if( e.tpl == "int $$0x29" )
            {
                if( !(e.inputs.size() == 1 && e.inputs[0].first == "{ecx}" && e.outputs.size() == 0) )
                    MIR_BUG(mir_res, "Hard-coded asm translation doesn't apply - `" << e.tpl << "` inputs=" << e.inputs << " outputs=" << e.outputs);
                m_of << indent << "__fastfail("; emit_lvalue(e.inputs[0].second); m_of << ");\n";
                return ;
            }
            else
            {
                // No hard-coded translations.
            }

            if( !e.inputs.empty() || !e.outputs.empty() )
            {
                MIR_TODO(mir_res, "Inputs/outputs in msvc inline assembly - `" << e.tpl << "` inputs=" << e.inputs << " outputs=" << e.outputs);
#if 0
                m_of << indent << "{\n";
                for(size_t i = 0; i < e.inputs.size(); i ++)
                {
                    m_of << indent << "auto asm_i_" << i << " = ";
                    emit_lvalue(e.inputs[i]);
                }
#endif
            }

            m_of << indent << "__asm {\n";

            m_of << indent << "\t";
            for (auto it = e.tpl.begin(); it != e.tpl.end(); ++it)
            {
                if (*it == ';')
                {
                    m_of << "\n";
                    m_of << indent << "\t";
                }
                else
                    m_of << *it;
            }

            m_of << "\n" << indent << "}";
            if (!e.inputs.empty() || !e.outputs.empty())
            {
                m_of << "}";
            }
            m_of << ";\n";
        }
    private:
        const ::HIR::TypeRef& monomorphise_fcn_return(::HIR::TypeRef& tmp, const ::HIR::Function& item, const Trans_Params& params)
        {
            if( visit_ty_with(item.m_return, [&](const auto& x){ return x.m_data.is_ErasedType() || x.m_data.is_Generic(); }) )
            {
                tmp = clone_ty_with(Span(), item.m_return, [&](const auto& tpl, auto& out){
                    TU_IFLET( ::HIR::TypeRef::Data, tpl.m_data, ErasedType, e,
                        out = params.monomorph(m_resolve, item.m_code.m_erased_types.at(e.m_index));
                        return true;
                    )
                    else if( tpl.m_data.is_Generic() ) {
                        out = params.get_cb()(tpl).clone();
                        return true;
                    }
                    else {
                        return false;
                    }
                    });
                m_resolve.expand_associated_types(Span(), tmp);
                return tmp;
            }
            else
            {
                return item.m_return;
            }
        }

        void emit_function_header(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params)
        {
            ::HIR::TypeRef  tmp;
            const auto& ret_ty = monomorphise_fcn_return(tmp, item, params);
            emit_ctype( ret_ty, FMT_CB(ss,
                // TODO: Cleaner ABI handling
                if( item.m_abi == "system" && m_compiler == Compiler::Msvc )
                {
                    ss << " __stdcall";
                }
                ss << " " << Trans_Mangle(p) << "(";
                if( item.m_args.size() == 0 )
                {
                    ss << "void)";
                }
                else
                {
                    for(unsigned int i = 0; i < item.m_args.size(); i ++)
                    {
                        if( i != 0 )    m_of << ",";
                        ss << "\n\t\t";
                        this->emit_ctype( params.monomorph(m_resolve, item.m_args[i].second), FMT_CB(os, os << "arg" << i;) );
                    }

                    if( item.m_variadic )
                        m_of << ", ...";

                    ss << "\n\t\t)";
                }
                ));
        }

        void emit_intrinsic_call(const ::std::string& name, const ::HIR::PathParams& params, const ::MIR::Terminator::Data_Call& e)
        {
            const auto& mir_res = *m_mir_res;
            auto get_atomic_ordering = [&](const ::std::string& name, size_t prefix_len)->const char* {
                    if( name.size() < prefix_len )
                    {
                        switch(m_compiler)
                        {
                        case Compiler::Gcc:     return "memory_order_seq_cst";
                        case Compiler::Msvc:    return "";
                        }
                    }
                    const char* suffix = name.c_str() + prefix_len;
                    if( ::std::strcmp(suffix, "acq") == 0 ) {
                        switch (m_compiler)
                        {
                        case Compiler::Gcc:     return "memory_order_acquire";
                        case Compiler::Msvc:    return "Acquire";
                        }
                    }
                    else if( ::std::strcmp(suffix, "rel") == 0 ) {
                        switch (m_compiler)
                        {
                        case Compiler::Gcc:     return "memory_order_release";
                        case Compiler::Msvc:    return "Release";
                        }
                    }
                    else if( ::std::strcmp(suffix, "relaxed") == 0 ) {
                        switch (m_compiler)
                        {
                        case Compiler::Gcc:     return "memory_order_relaxed";
                        case Compiler::Msvc:    return "NoFence";
                        }
                    }
                    else if( ::std::strcmp(suffix, "acqrel") == 0 ) {
                        switch (m_compiler)
                        {
                        case Compiler::Gcc:     return "memory_order_acq_rel";
                        case Compiler::Msvc:    return "";  // TODO: This is either Acquire or Release
                        }
                    }
                    else {
                        MIR_BUG(mir_res, "Unknown atomic ordering suffix - '" << suffix << "'");
                    }
                    throw "";
                };
            auto emit_msvc_atomic_op = [&](const char* name, const char* ordering) {
                switch (params.m_types.at(0).m_data.as_Primitive())
                {
                case ::HIR::CoreType::U8:
                case ::HIR::CoreType::I8:
                    m_of << name << "8" << ordering << "(";
                    break;
                case ::HIR::CoreType::U16:
                    m_of << name << "16" << ordering << "(";
                    break;
                case ::HIR::CoreType::U32:
                    m_of << name << ordering << "(";
                    break;
                case ::HIR::CoreType::U64:
                //case ::HIR::CoreType::I64:
                    m_of << name << "64" << ordering << "(";
                    break;
                case ::HIR::CoreType::Usize:
                case ::HIR::CoreType::Isize:
                    m_of << "(uintptr_t)" << name << "Pointer" << ordering << "((void**)";
                    break;
                default:
                    MIR_BUG(mir_res, "Unsupported atomic type - " << params.m_types.at(0));
                }
                };
            auto emit_atomic_cast = [&]() {
                m_of << "(_Atomic "; emit_ctype(params.m_types.at(0)); m_of << "*)";
                };
            auto emit_atomic_cxchg = [&](const auto& e, const char* o_succ, const char* o_fail, bool is_weak) {
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    emit_lvalue(e.ret_val); m_of << "._0 = "; emit_param(e.args.at(1)); m_of << ";\n\t";
                    emit_lvalue(e.ret_val); m_of << "._1 = atomic_compare_exchange_" << (is_weak ? "weak" : "strong") << "_explicit(";
                        emit_atomic_cast(); emit_param(e.args.at(0));
                        m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0";
                        m_of << ", "; emit_param(e.args.at(2));
                        m_of << ", "<<o_succ<<", "<<o_fail<<")";
                    break;
                case Compiler::Msvc:
                    emit_lvalue(e.ret_val); m_of << "._0 = ";
                    emit_msvc_atomic_op("InterlockedCompareExchange", "");  // TODO: Use ordering
                    if(params.m_types.at(0) == ::HIR::CoreType::Usize || params.m_types.at(0) == ::HIR::CoreType::Isize)
                    {
                        emit_param(e.args.at(0)); m_of << ", (void*)"; emit_param(e.args.at(1)); m_of << ", (void*)"; emit_param(e.args.at(2)); m_of << ")";
                    }
                    else
                    {
                        emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", "; emit_param(e.args.at(2)); m_of << ")";
                    }
                    m_of << ";\n\t";
                    emit_lvalue(e.ret_val); m_of << "._1 = ("; emit_lvalue(e.ret_val); m_of << "._0 == "; emit_param(e.args.at(2)); m_of << ")";
                    break;
                }
                };
            auto emit_atomic_arith = [&](AtomicOp op, const char* ordering) {
                emit_lvalue(e.ret_val); m_of << " = ";
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    switch(op)
                    {
                    case AtomicOp::Add: m_of << "atomic_fetch_add_explicit";   break;
                    case AtomicOp::Sub: m_of << "atomic_fetch_sub_explicit";   break;
                    case AtomicOp::And: m_of << "atomic_fetch_and_explicit";   break;
                    case AtomicOp::Or:  m_of << "atomic_fetch_or_explicit";    break;
                    case AtomicOp::Xor: m_of << "atomic_fetch_xor_explicit";   break;
                    }
                    m_of << "("; emit_atomic_cast(); emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", " << ordering << ")";
                    break;
                case Compiler::Msvc:
                    switch(op)
                    {
                    case AtomicOp::Add: emit_msvc_atomic_op("InterlockedAdd", ordering);    break;
                    case AtomicOp::Sub: emit_msvc_atomic_op("InterlockedSub", ordering);    break;
                    case AtomicOp::And: emit_msvc_atomic_op("InterlockedAnd", ordering);    break;
                    case AtomicOp::Or:  emit_msvc_atomic_op("InterlockedOr", ordering);    break;
                    case AtomicOp::Xor: emit_msvc_atomic_op("InterlockedXor", ordering);    break;
                    }
                    emit_param(e.args.at(0)); m_of << ", ";
                    if (params.m_types.at(0) == ::HIR::CoreType::Usize || params.m_types.at(0) == ::HIR::CoreType::Isize)
                    {
                        m_of << "(void*)";
                    }
                    emit_param(e.args.at(1)); m_of << ")";
                    break;
                }
                };
            if( name == "size_of" ) {
                emit_lvalue(e.ret_val); m_of << " = sizeof("; emit_ctype(params.m_types.at(0)); m_of << ")";
            }
            else if( name == "min_align_of" ) {
                //emit_lvalue(e.ret_val); m_of << " = alignof("; emit_ctype(params.m_types.at(0)); m_of << ")";
                emit_lvalue(e.ret_val); m_of << " = ALIGNOF("; emit_ctype(params.m_types.at(0)); m_of << ")";
            }
            else if( name == "size_of_val" ) {
                emit_lvalue(e.ret_val); m_of << " = ";
                const auto& ty = params.m_types.at(0);
                //TODO: Get the unsized type and use that in place of MetadataType
                #if 1
                auto inner_ty = get_inner_unsized_type(ty);
                if( inner_ty == ::HIR::TypeRef() ) {
                    m_of << "sizeof("; emit_ctype(ty); m_of << ")";
                }
                else if( const auto* te = inner_ty.m_data.opt_Slice() ) {
                    if( ! ty.m_data.is_Slice() ) {
                        m_of << "sizeof("; emit_ctype(ty); m_of << ") + ";
                    }
                    emit_param(e.args.at(0)); m_of << ".META * sizeof("; emit_ctype(*te->inner); m_of << ")";
                }
                else if( inner_ty == ::HIR::CoreType::Str ) {
                    if( ! ty.m_data.is_Primitive() ) {
                        m_of << "sizeof("; emit_ctype(ty); m_of << ") + ";
                    }
                    emit_param(e.args.at(0)); m_of << ".META";
                }
                else if( inner_ty.m_data.is_TraitObject() ) {
                    if( ! ty.m_data.is_TraitObject() ) {
                        m_of << "sizeof("; emit_ctype(ty); m_of << ") + ";
                    }
                    m_of << "((VTABLE_HDR*)"; emit_param(e.args.at(0)); m_of << ".META)->size";
                }
                else {
                    MIR_BUG(mir_res, "Unknown inner unsized type " << inner_ty << " for " << ty);
                }
                #else
                switch( metadata_type(ty) )
                {
                case MetadataType::None:
                    m_of << "sizeof("; emit_ctype(ty); m_of << ")";
                    break;
                case MetadataType::Slice: {
                    // TODO: Have a function that fetches the inner type for types like `Path` or `str`
                    const auto& ity = *ty.m_data.as_Slice().inner;
                    emit_param(e.args.at(0)); m_of << ".META * sizeof("; emit_ctype(ity); m_of << ")";
                    break; }
                case MetadataType::TraitObject:
                    m_of << "((VTABLE_HDR*)"; emit_param(e.args.at(0)); m_of << ".META)->size";
                    break;
                }
                #endif
            }
            else if( name == "min_align_of_val" ) {
                emit_lvalue(e.ret_val); m_of << " = ";
                const auto& ty = params.m_types.at(0);
                #if 1
                auto inner_ty = get_inner_unsized_type(ty);
                if( inner_ty == ::HIR::TypeRef() ) {
                    m_of << "ALIGNOF("; emit_ctype(ty); m_of << ")";
                }
                else if( const auto* te = inner_ty.m_data.opt_Slice() ) {
                    if( ! ty.m_data.is_Slice() ) {
                        m_of << "mrustc_max( ALIGNOF("; emit_ctype(ty); m_of << "), ";
                    }
                    m_of << "ALIGNOF("; emit_ctype(*te->inner); m_of << ")";
                    if( ! ty.m_data.is_Slice() ) {
                        m_of << " )";
                    }
                }
                else if( inner_ty == ::HIR::CoreType::Str ) {
                    if( ! ty.m_data.is_Primitive() ) {
                        m_of << "ALIGNOF("; emit_ctype(ty); m_of << ")";
                    }
                    else {
                        m_of << "1";
                    }
                }
                else if( inner_ty.m_data.is_TraitObject() ) {
                    if( ! ty.m_data.is_TraitObject() ) {
                        m_of << "mrustc_max( ALIGNOF("; emit_ctype(ty); m_of << "), ";
                    }
                    m_of << "((VTABLE_HDR*)"; emit_param(e.args.at(0)); m_of << ".META)->align";
                    if( ! ty.m_data.is_TraitObject() ) {
                        m_of << " )";
                    }
                }
                else {
                    MIR_BUG(mir_res, "Unknown inner unsized type " << inner_ty << " for " << ty);
                }
                #else
                switch( metadata_type(ty) )
                {
                case MetadataType::None:
                    m_of << "ALIGNOF("; emit_ctype(ty); m_of << ")";
                    break;
                case MetadataType::Slice: {
                    // TODO: Have a function that fetches the inner type for types like `Path` or `str`
                    const auto& ity = *ty.m_data.as_Slice().inner;
                    m_of << "ALIGNOF("; emit_ctype(ity); m_of << ")";
                    break; }
                case MetadataType::TraitObject:
                    m_of << "((VTABLE_HDR*)"; emit_param(e.args.at(0)); m_of << ".META)->align";
                    break;
                }
                #endif
            }
            else if( name == "type_id" ) {
                const auto& ty = params.m_types.at(0);
                // NOTE: Would define the typeid here, but it has to be public
                emit_lvalue(e.ret_val); m_of << " = (uintptr_t)&__typeid_" << Trans_Mangle(ty);
            }
            else if( name == "type_name" ) {
                auto s = FMT(params.m_types.at(0));
                emit_lvalue(e.ret_val); m_of << ".PTR = \"" << FmtEscaped(s) << "\";\n\t";
                emit_lvalue(e.ret_val); m_of << ".META = " << s.size() << "";
            }
            else if( name == "transmute" ) {
                const auto& ty_dst = params.m_types.at(0);
                const auto& ty_src = params.m_types.at(1);
                auto is_ptr = [](const ::HIR::TypeRef& ty){ return ty.m_data.is_Borrow() || ty.m_data.is_Pointer(); };
                if( e.args.at(0).is_Constant() )
                {
                    m_of << "{ "; emit_ctype(ty_src, FMT_CB(s, s << "v";)); m_of << " = "; emit_param(e.args.at(0)); m_of << ";";
                    m_of << "memcpy( &"; emit_lvalue(e.ret_val); m_of << ", &v, sizeof("; emit_ctype(ty_dst); m_of << ")); ";
                    m_of << "}";
                }
                else if( is_ptr(ty_dst) && is_ptr(ty_src) )
                {
                    auto src_meta = metadata_type(ty_src.m_data.is_Pointer() ? *ty_src.m_data.as_Pointer().inner : *ty_src.m_data.as_Borrow().inner);
                    auto dst_meta = metadata_type(ty_dst.m_data.is_Pointer() ? *ty_dst.m_data.as_Pointer().inner : *ty_dst.m_data.as_Borrow().inner);
                    if( src_meta == MetadataType::None )
                    {
                        assert(dst_meta == MetadataType::None);
                        emit_lvalue(e.ret_val); m_of << " = (void*)"; emit_param(e.args.at(0));
                    }
                    else if( src_meta != dst_meta )
                    {
                        emit_lvalue(e.ret_val); m_of << ".PTR = "; emit_param(e.args.at(0)); m_of << ".PTR; ";
                        emit_lvalue(e.ret_val); m_of << ".META = ";
                        switch(dst_meta)
                        {
                        case MetadataType::None: assert(!"Impossible");
                        case MetadataType::Slice:   m_of << "(size_t)"; break;
                        case MetadataType::TraitObject: m_of << "(const void*)"; break;
                        }
                        emit_param(e.args.at(0)); m_of << ".META";
                    }
                    else
                    {
                        emit_lvalue(e.ret_val); m_of << " = "; emit_param(e.args.at(0));
                    }
                }
                else
                {
                    m_of << "memcpy( &"; emit_lvalue(e.ret_val); m_of << ", &"; emit_param(e.args.at(0)); m_of << ", sizeof("; emit_ctype(params.m_types.at(0)); m_of << "))";
                }
            }
            else if( name == "copy_nonoverlapping" || name == "copy" ) {
                if( name == "copy" ) {
                    m_of << "memmove";
                }
                else {
                    m_of << "memcpy";
                }
                // 0: Source, 1: Destination, 2: Count
                m_of << "( "; emit_param(e.args.at(1));
                    m_of << ", "; emit_param(e.args.at(0));
                    m_of << ", "; emit_param(e.args.at(2)); m_of << " * sizeof("; emit_ctype(params.m_types.at(0)); m_of << ")";
                    m_of << ")";
            }
            else if( name == "write_bytes" ) {
                // 0: Destination, 1: Value, 2: Count
                m_of << "memset( "; emit_param(e.args.at(0));
                    m_of << ", "; emit_param(e.args.at(1));
                    m_of << ", "; emit_param(e.args.at(2)); m_of << " * sizeof("; emit_ctype(params.m_types.at(0)); m_of << ")";
                    m_of << ")";
            }
            else if( name == "forget" ) {
                // Nothing needs to be done, this just stops the destructor from running.
            }
            else if( name == "drop_in_place" ) {
                emit_destructor_call( ::MIR::LValue::make_Deref({ box$(e.args.at(0).as_LValue().clone()) }), params.m_types.at(0), true, 1 /* TODO: get from caller */ );
            }
            else if( name == "needs_drop" ) {
                // Returns `true` if the actual type given as `T` requires drop glue;
                // returns `false` if the actual type provided for `T` implements `Copy`. (Either otherwise)
                // NOTE: libarena assumes that this returns `true` iff T doesn't require drop glue.
                const auto& ty = params.m_types.at(0);
                emit_lvalue(e.ret_val);
                m_of << " = ";
                if( m_resolve.type_needs_drop_glue(mir_res.sp, ty) ) {
                    m_of << "true";
                }
                else {
                    m_of << "false";
                }
            }
            else if( name == "uninit" ) {
                // Do nothing, leaves the destination undefined
            }
            else if( name == "init" ) {
                m_of << "memset( &"; emit_lvalue(e.ret_val); m_of << ", 0, sizeof("; emit_ctype(params.m_types.at(0)); m_of << "))";
            }
            else if( name == "move_val_init" ) {
                m_of << "*"; emit_param(e.args.at(0)); m_of << " = "; emit_param(e.args.at(1));
            }
            else if( name == "abort" ) {
                m_of << "abort()";
            }
            else if( name == "try" ) {
                emit_param(e.args.at(0)); m_of << "("; emit_param(e.args.at(1)); m_of << "); ";
                emit_lvalue(e.ret_val); m_of << " = 0";
            }
            else if( name == "offset" ) {
                emit_lvalue(e.ret_val); m_of << " = "; emit_param(e.args.at(0)); m_of << " + "; emit_param(e.args.at(1));
            }
            else if( name == "arith_offset" ) {
                emit_lvalue(e.ret_val); m_of << " = "; emit_param(e.args.at(0)); m_of << " + "; emit_param(e.args.at(1));
            }
            else if( name == "bswap" ) {
                const auto& ty = params.m_types.at(0);
                MIR_ASSERT(mir_res, ty.m_data.is_Primitive(), "Invalid type passed to bwsap, must be a primitive, got " << ty);
                switch( ty.m_data.as_Primitive() )
                {
                case ::HIR::CoreType::U8:
                case ::HIR::CoreType::I8:
                    emit_lvalue(e.ret_val); m_of << " = "; emit_param(e.args.at(0));
                    break;
                case ::HIR::CoreType::U16:
                case ::HIR::CoreType::I16:
                    emit_lvalue(e.ret_val); m_of << " = __builtin_bswap16("; emit_param(e.args.at(0)); m_of << ")";
                    break;
                case ::HIR::CoreType::U32:
                case ::HIR::CoreType::I32:
                    emit_lvalue(e.ret_val); m_of << " = __builtin_bswap32("; emit_param(e.args.at(0)); m_of << ")";
                    break;
                case ::HIR::CoreType::U64:
                case ::HIR::CoreType::I64:
                    emit_lvalue(e.ret_val); m_of << " = __builtin_bswap64("; emit_param(e.args.at(0)); m_of << ")";
                    break;
                case ::HIR::CoreType::U128:
                case ::HIR::CoreType::I128:
                    emit_lvalue(e.ret_val); m_of << " = __builtin_bswap128("; emit_param(e.args.at(0)); m_of << ")";
                    break;
                default:
                    MIR_TODO(mir_res, "bswap<" << ty << ">");
                }
            }
            // > Obtain the discriminane of a &T as u64
            else if( name == "discriminant_value" ) {
                const auto& ty = params.m_types.at(0);
                emit_lvalue(e.ret_val); m_of << " = ";
                if( ty.m_data.is_Path() && ty.m_data.as_Path().binding.is_Enum() ) {
                    auto it = m_enum_repr_cache.find( ty.m_data.as_Path().path.m_data.as_Generic() );
                    if( it != m_enum_repr_cache.end() )
                    {
                        emit_param(e.args.at(0)); m_of << "->_1"; emit_nonzero_path(it->second); m_of << " != 0";
                    }
                    else
                    {
                        emit_param(e.args.at(0)); m_of << "->TAG";
                    }
                }
                else {
                    m_of << "0";
                }
            }
            // Hints
            else if( name == "unreachable" ) {
                m_of << "__builtin_unreachable()";
            }
            else if( name == "assume" ) {
                // I don't assume :)
            }
            else if( name == "likely" ) {
            }
            else if( name == "unlikely" ) {
            }
            // Overflowing Arithmatic
            // HACK: Uses GCC intrinsics
            else if( name == "add_with_overflow" ) {
                emit_lvalue(e.ret_val); m_of << "._1 = __builtin_add_overflow("; emit_param(e.args.at(0));
                    m_of << ", "; emit_param(e.args.at(1));
                    m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
            }
            else if( name == "sub_with_overflow" ) {
                emit_lvalue(e.ret_val); m_of << "._1 = __builtin_sub_overflow("; emit_param(e.args.at(0));
                    m_of << ", "; emit_param(e.args.at(1));
                    m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
            }
            else if( name == "mul_with_overflow" ) {
                emit_lvalue(e.ret_val); m_of << "._1 = __builtin_mul_overflow("; emit_param(e.args.at(0));
                    m_of << ", "; emit_param(e.args.at(1));
                    m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
            }
            else if( name == "overflowing_add" ) {
                m_of << "__builtin_add_overflow("; emit_param(e.args.at(0));
                    m_of << ", "; emit_param(e.args.at(1));
                    m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
            }
            else if( name == "overflowing_sub" ) {
                m_of << "__builtin_sub_overflow("; emit_param(e.args.at(0));
                    m_of << ", "; emit_param(e.args.at(1));
                    m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
            }
            else if( name == "overflowing_mul" ) {
                m_of << "__builtin_mul_overflow("; emit_param(e.args.at(0));
                    m_of << ", "; emit_param(e.args.at(1));
                    m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
            }
            // Unchecked Arithmatic
            else if( name == "unchecked_div" ) {
                emit_lvalue(e.ret_val); m_of << " = ";
                if( type_is_emulated_i128(params.m_types.at(0)) )
                {
                    m_of << "div128";
                    if(params.m_types.at(0) == ::HIR::CoreType::I128)   m_of << "s";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ")";
                }
                else
                {
                    emit_param(e.args.at(0)); m_of << " / "; emit_param(e.args.at(1));
                }
            }
            else if( name == "unchecked_rem" ) {
                emit_lvalue(e.ret_val); m_of << " = ";
                if (type_is_emulated_i128(params.m_types.at(0)))
                {
                    m_of << "mod128";
                    if(params.m_types.at(0) == ::HIR::CoreType::I128)   m_of << "s";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ")";
                }
                else
                {
                    emit_param(e.args.at(0)); m_of << " % "; emit_param(e.args.at(1));
                }
            }
            else if( name == "unchecked_shl" ) {
                emit_lvalue(e.ret_val); m_of << " = ";
                if (type_is_emulated_i128(params.m_types.at(0)))
                {
                    m_of << "shl128";
                    if(params.m_types.at(0) == ::HIR::CoreType::I128)   m_of << "s";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", ";
                    emit_param(e.args.at(1));
                    // If the shift type is a u128/i128, get the inner
                    ::HIR::TypeRef tmp;
                    const auto& shift_ty = mir_res.get_param_type(tmp, e.args.at(1));
                    if( shift_ty == ::HIR::CoreType::I128 || shift_ty == ::HIR::CoreType::U128 )
                    {
                        m_of << ".lo";
                    }
                    m_of << ")";
                }
                else
                {
                    emit_param(e.args.at(0)); m_of << " << "; emit_param(e.args.at(1));
                }
            }
            else if( name == "unchecked_shr" ) {
                emit_lvalue(e.ret_val); m_of << " = ";
                if (type_is_emulated_i128(params.m_types.at(0)))
                {
                    m_of << "shr128";
                    if (params.m_types.at(0) == ::HIR::CoreType::I128)   m_of << "s";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", ";
                    emit_param(e.args.at(1));
                    // If the shift type is a u128/i128, get the inner
                    ::HIR::TypeRef tmp;
                    const auto& shift_ty = mir_res.get_param_type(tmp, e.args.at(1));
                    if( shift_ty == ::HIR::CoreType::I128 || shift_ty == ::HIR::CoreType::U128 )
                    {
                        m_of << ".lo";
                    }
                    m_of << ")";
                }
                else
                {
                    emit_param(e.args.at(0)); m_of << " >> "; emit_param(e.args.at(1));
                }
            }
            // Bit Twiddling
            // - CounT Leading Zeroes
            // - CounT Trailing Zeroes
            else if( name == "ctlz" || name == "ctlz_nonzero" || name == "cttz" ) {
                auto emit_arg0 = [&](){ emit_param(e.args.at(0)); };
                const auto& ty = params.m_types.at(0);
                emit_lvalue(e.ret_val); m_of << " = (";
                if( ty == ::HIR::CoreType::U128 )
                {
                    if( name == "ctlz" || name == "ctlz_nonzero" ) {
                        m_of << "intrinsic_ctlz_u128("; emit_param(e.args.at(0)); m_of << ")";
                    }
                    else {
                        m_of << "intrinsic_cttz_u128("; emit_param(e.args.at(0)); m_of << ")";
                    }
                    m_of << ");";
                    return ;
                }
                else if( ty == ::HIR::CoreType::U64 || (ty == ::HIR::CoreType::Usize /*&& target_is_64_bit */) )
                {
                    emit_param(e.args.at(0)); m_of << " != 0 ? ";
                    if( name == "ctlz" || name == "ctlz_nonzero" ) {
                        m_of << "__builtin_clz64("; emit_arg0(); m_of << ")";
                    }
                    else {
                        m_of << "__builtin_ctz64("; emit_arg0(); m_of << ")";
                    }
                }
                else
                {
                    emit_param(e.args.at(0)); m_of << " != 0 ? ";
                    if( name == "ctlz" || name == "ctlz_nonzero" ) {
                        m_of << "__builtin_clz("; emit_param(e.args.at(0)); m_of << ")";
                    }
                    else {
                        m_of << "__builtin_ctz("; emit_param(e.args.at(0)); m_of << ")";
                    }
                }
                m_of << " : sizeof("; emit_ctype(ty); m_of << ")*8)";
            }
            // - CounT POPulated
            else if( name == "ctpop" ) {
                emit_lvalue(e.ret_val); m_of << " = ";

                if( type_is_emulated_i128(params.m_types.at(0)) )
                {
                    m_of << "popcount128";
                    if(params.m_types.at(0) == ::HIR::CoreType::I128)
                        m_of << "s";
                }
                else
                {
                    m_of << "__builtin_popcount";
                }
                m_of << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            // --- Floating Point
            // > Round to nearest integer, half-way rounds away from zero
            else if( name == "roundf32" && name == "roundf64" ) {
                emit_lvalue(e.ret_val); m_of << " = round" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "fabsf32" || name == "fabsf64" ) {
                emit_lvalue(e.ret_val); m_of << " = fabs" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "copysignf32" || name == "copysignf64" ) {
                emit_lvalue(e.ret_val); m_of << " = copysign" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ")";
            }
            // > Returns the integer part of an `f32`.
            else if( name == "truncf32" || name == "truncf64" ) {
                emit_lvalue(e.ret_val); m_of << " = trunc" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "powif32" || name == "powif64" ) {
                emit_lvalue(e.ret_val); m_of << " = pow" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ")";
            }
            else if( name == "powf32" || name == "powf64" ) {
                emit_lvalue(e.ret_val); m_of << " = pow" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ")";
            }
            else if( name == "expf32" || name == "expf64" ) {
                emit_lvalue(e.ret_val); m_of << " = exp" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "exp2f32" || name == "exp2f64" ) {
                emit_lvalue(e.ret_val); m_of << " = exp2" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "logf32" || name == "logf64" ) {
                emit_lvalue(e.ret_val); m_of << " = log" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "log10f32" || name == "log10f64" ) {
                emit_lvalue(e.ret_val); m_of << " = log10" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "log2f32" || name == "log2f64" ) {
                emit_lvalue(e.ret_val); m_of << " = log2" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "sqrtf32" || name == "sqrtf64" ) {
                emit_lvalue(e.ret_val); m_of << " = sqrt" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "ceilf32" || name == "ceilf64" ) {
                emit_lvalue(e.ret_val); m_of << " = ceil" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "floorf32" || name == "floorf64" ) {
                emit_lvalue(e.ret_val); m_of << " = floor" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "roundf32" || name == "roundf64" ) {
                emit_lvalue(e.ret_val); m_of << " = round" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "cosf32" || name == "cosf64" ) {
                emit_lvalue(e.ret_val); m_of << " = cos" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "sinf32" || name == "sinf64" ) {
                emit_lvalue(e.ret_val); m_of << " = sin" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "fmaf32" || name == "fmaf64" ) {
                emit_lvalue(e.ret_val); m_of << " = fma" << (name.back()=='2'?"f":"") << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", "; emit_param(e.args.at(2)); m_of << ")";
            }
            // --- Volatile Load/Store
            else if( name == "volatile_load" ) {
                emit_lvalue(e.ret_val); m_of << " = *(volatile "; emit_ctype(params.m_types.at(0)); m_of << "*)"; emit_param(e.args.at(0));
            }
            else if( name == "volatile_store" ) {
                m_of << "*(volatile "; emit_ctype(params.m_types.at(0)); m_of << "*)"; emit_param(e.args.at(0)); m_of << " = "; emit_param(e.args.at(1));
            }
            // --- Atomics!
            // > Single-ordering atomics
            else if( name == "atomic_xadd" || name.compare(0, 7+4+1, "atomic_xadd_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+4+1);
                emit_atomic_arith(AtomicOp::Add, ordering);
            }
            else if( name == "atomic_xsub" || name.compare(0, 7+4+1, "atomic_xsub_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+4+1);
                emit_atomic_arith(AtomicOp::Sub, ordering);
            }
            else if( name == "atomic_and" || name.compare(0, 7+3+1, "atomic_and_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+3+1);
                emit_atomic_arith(AtomicOp::And, ordering);
            }
            else if( name == "atomic_or" || name.compare(0, 7+2+1, "atomic_or_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+2+1);
                emit_atomic_arith(AtomicOp::Or, ordering);
            }
            else if( name == "atomic_xor" || name.compare(0, 7+3+1, "atomic_xor_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+3+1);
                emit_atomic_arith(AtomicOp::Xor, ordering);
            }
            else if( name == "atomic_load" || name.compare(0, 7+4+1, "atomic_load_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+4+1);
                emit_lvalue(e.ret_val); m_of << " = ";
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    m_of << "atomic_load_explicit("; emit_atomic_cast(); emit_param(e.args.at(0)); m_of << ", " << ordering << ")";
                    break;
                case Compiler::Msvc:
                    //emit_msvc_atomic_op("InterlockedRead", ordering); emit_param(e.args.at(0)); m_of << ")";
                    emit_msvc_atomic_op("InterlockedCompareExchange", ordering); emit_param(e.args.at(0)); m_of << ", 0, 0)";
                    break;
                }
            }
            else if( name == "atomic_store" || name.compare(0, 7+5+1, "atomic_store_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+5+1);
                switch (m_compiler)
                {
                case Compiler::Gcc:
                    m_of << "atomic_store_explicit("; emit_atomic_cast(); emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", " << ordering << ")";
                    break;
                case Compiler::Msvc:
                    emit_msvc_atomic_op("InterlockedCompareExchange", ordering); emit_param(e.args.at(0)); m_of << ", ";
                    if (params.m_types.at(0) == ::HIR::CoreType::Usize || params.m_types.at(0) == ::HIR::CoreType::Isize)
                    {
                        m_of << "(void*)";
                    }
                    emit_param(e.args.at(1));
                    m_of << ", ";
                    if (params.m_types.at(0) == ::HIR::CoreType::Usize || params.m_types.at(0) == ::HIR::CoreType::Isize)
                    {
                        m_of << "(void*)";
                    }
                    emit_param(e.args.at(1));
                    m_of << ")";
                    break;
                }
            }
            // Comare+Exchange (has two orderings)
            else if( name == "atomic_cxchg_acq_failrelaxed" ) {
                emit_atomic_cxchg(e, "memory_order_acquire", "memory_order_relaxed", false);
            }
            else if( name == "atomic_cxchg_acqrel_failrelaxed" ) {
                emit_atomic_cxchg(e, "memory_order_acq_rel", "memory_order_relaxed", false);
            }
            // _rel = Release, Relaxed (not Release,Release)
            else if( name == "atomic_cxchg_rel" ) {
                emit_atomic_cxchg(e, "memory_order_release", "memory_order_relaxed", false);
            }
            // _acqrel = Release, Acquire (not AcqRel,AcqRel)
            else if( name == "atomic_cxchg_acqrel" ) {
                emit_atomic_cxchg(e, "memory_order_acq_rel", "memory_order_acquire", false);
            }
            else if( name.compare(0, 7+6+4, "atomic_cxchg_fail") == 0 ) {
                auto fail_ordering = get_atomic_ordering(name, 7+6+4);
                emit_atomic_cxchg(e, "memory_order_seq_cst", fail_ordering, false);
            }
            else if( name == "atomic_cxchg" || name.compare(0, 7+6, "atomic_cxchg_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+6);
                emit_atomic_cxchg(e, ordering, ordering, false);
            }
            else if( name == "atomic_cxchgweak_acq_failrelaxed" ) {
                emit_atomic_cxchg(e, "memory_order_acquire", "memory_order_relaxed", true);
            }
            else if( name == "atomic_cxchgweak_acqrel_failrelaxed" ) {
                emit_atomic_cxchg(e, "memory_order_acq_rel", "memory_order_relaxed", true);
            }
            else if( name.compare(0, 7+10+4, "atomic_cxchgweak_fail") == 0 ) {
                auto fail_ordering = get_atomic_ordering(name, 7+10+4);
                emit_atomic_cxchg(e, "memory_order_seq_cst", fail_ordering, true);
            }
            else if( name == "atomic_cxchgweak" ) {
                emit_atomic_cxchg(e, "memory_order_seq_cst", "memory_order_seq_cst", true);
            }
            else if( name == "atomic_cxchgweak_acq" ) {
                emit_atomic_cxchg(e, "memory_order_acquire", "memory_order_acquire", true);
            }
            else if( name == "atomic_cxchgweak_rel" ) {
                emit_atomic_cxchg(e, "memory_order_release", "memory_order_relaxed", true);
            }
            else if( name == "atomic_cxchgweak_acqrel" ) {
                emit_atomic_cxchg(e, "memory_order_acq_rel", "memory_order_acquire", true);
            }
            else if( name == "atomic_cxchgweak_relaxed" ) {
                emit_atomic_cxchg(e, "memory_order_relaxed", "memory_order_relaxed", true);
            }
            else if( name == "atomic_xchg" || name.compare(0, 7+5, "atomic_xchg_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+5);
                emit_lvalue(e.ret_val); m_of << " = ";
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    m_of << "atomic_exchange_explicit("; emit_atomic_cast(); emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", " << ordering << ")";
                    break;
                case Compiler::Msvc:
                    emit_msvc_atomic_op("InterlockedExchange", ordering);
                    emit_param(e.args.at(0)); m_of << ", ";
                    if (params.m_types.at(0) == ::HIR::CoreType::Usize || params.m_types.at(0) == ::HIR::CoreType::Isize)
                    {
                        m_of << "(void*)";
                    }
                    emit_param(e.args.at(1)); m_of << ")";
                    break;
                }
            }
            else if( name == "atomic_fence" || name.compare(0, 7+6, "atomic_fence_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+6);
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    m_of << "atomic_thread_fence(" << ordering << ")";
                    break;
                case Compiler::Msvc:
                    break;
                }
            }
            else if( name == "atomic_singlethreadfence" || name.compare(0, 7+18, "atomic_singlethreadfence_") == 0 ) {
                // TODO: Does this matter?
            }
            else {
                MIR_BUG(mir_res, "Unknown intrinsic '" << name << "'");
            }
            m_of << ";\n";
        }

        void emit_destructor_call(const ::MIR::LValue& slot, const ::HIR::TypeRef& ty, bool unsized_valid, unsigned indent_level)
        {
            auto indent = RepeatLitStr { "\t", static_cast<int>(indent_level) };
            TU_MATCHA( (ty.m_data), (te),
            // Impossible
            (Diverge, ),
            (Infer, ),
            (ErasedType, ),
            (Closure, ),
            (Generic, ),

            // Nothing
            (Primitive,
                ),
            (Pointer,
                ),
            (Function,
                ),
            // Has drop glue/destructors
            (Borrow,
                if( te.type == ::HIR::BorrowType::Owned )
                {
                    // Call drop glue on inner.
                    emit_destructor_call( ::MIR::LValue::make_Deref({ box$(slot.clone()) }), *te.inner, true, indent_level );
                }
                ),
            (Path,
                // Call drop glue
                // - TODO: If the destructor is known to do nothing, don't call it.
                auto p = ::HIR::Path(ty.clone(), "#drop_glue");
                const char* make_fcn = nullptr;
                switch( metadata_type(ty) )
                {
                case MetadataType::None:
                    m_of << indent << Trans_Mangle(p) << "(&"; emit_lvalue(slot); m_of << ");\n";
                    break;
                case MetadataType::Slice:
                    make_fcn = "make_sliceptr"; if(0)
                case MetadataType::TraitObject:
                    make_fcn = "make_traitobjptr";
                    m_of << indent << Trans_Mangle(p) << "( " << make_fcn << "(";
                    if( slot.is_Deref() )
                    {
                        emit_lvalue(*slot.as_Deref().val);
                        m_of << ".PTR";
                    }
                    else
                    {
                        m_of << "&"; emit_lvalue(slot);
                    }
                    m_of << ", ";
                    const auto* lvp = &slot;
                    while(const auto* le = lvp->opt_Field())  lvp = &*le->val;
                    MIR_ASSERT(*m_mir_res, lvp->is_Deref(), "Access to unized type without a deref - " << *lvp << " (part of " << slot << ")");
                    emit_lvalue(*lvp->as_Deref().val); m_of << ".META";
                    m_of << ") );\n";
                    break;
                }
                ),
            (Array,
                // Emit destructors for all entries
                if( te.size_val > 0 )
                {
                    m_of << indent << "for(unsigned i = 0; i < " << te.size_val << "; i++) {\n";
                    emit_destructor_call(::MIR::LValue::make_Index({ box$(slot.clone()), box$(::MIR::LValue::make_Local(~0u)) }), *te.inner, false, indent_level+1);
                    m_of << "\n" << indent << "}";
                }
                ),
            (Tuple,
                // Emit destructors for all entries
                if( te.size() > 0 )
                {
                    ::MIR::LValue   lv = ::MIR::LValue::make_Field({ box$(slot.clone()), 0 });
                    for(unsigned int i = 0; i < te.size(); i ++)
                    {
                        lv.as_Field().field_index = i;
                        emit_destructor_call(lv, te[i], unsized_valid && (i == te.size()-1), indent_level);
                    }
                }
                ),
            (TraitObject,
                MIR_ASSERT(*m_mir_res, unsized_valid, "Dropping TraitObject without a pointer");
                // Call destructor in vtable
                const auto* lvp = &slot;
                while(const auto* le = lvp->opt_Field())  lvp = &*le->val;
                MIR_ASSERT(*m_mir_res, lvp->is_Deref(), "Access to unized type without a deref - " << *lvp << " (part of " << slot << ")");
                m_of << indent << "((VTABLE_HDR*)"; emit_lvalue(*lvp->as_Deref().val); m_of << ".META)->drop(";
                if( const auto* ve = slot.opt_Deref() )
                {
                    emit_lvalue(*ve->val); m_of << ".PTR";
                }
                else
                {
                    m_of << "&"; emit_lvalue(slot);
                }
                m_of << ");";
                ),
            (Slice,
                MIR_ASSERT(*m_mir_res, unsized_valid, "Dropping Slice without a pointer");
                const auto* lvp = &slot;
                while(const auto* le = lvp->opt_Field())  lvp = &*le->val;
                MIR_ASSERT(*m_mir_res, lvp->is_Deref(), "Access to unized type without a deref - " << *lvp << " (part of " << slot << ")");
                // Call destructor on all entries
                m_of << indent << "for(unsigned i = 0; i < "; emit_lvalue(*lvp->as_Deref().val); m_of << ".META; i++) {\n";
                emit_destructor_call(::MIR::LValue::make_Index({ box$(slot.clone()), box$(::MIR::LValue::make_Local(~0u)) }), *te.inner, false, indent_level+1);
                m_of << "\n" << indent << "}";
                )
            )
        }

        const ::HIR::Literal& get_literal_for_const(const ::HIR::Path& path, ::HIR::TypeRef& ty)
        {
            MonomorphState  params;
            auto v = m_resolve.get_value(m_mir_res->sp, path, params);
            if( const auto* e = v.opt_Constant() )
            {
                ty = params.monomorph(m_mir_res->sp, (*e)->m_type);
                return (*e)->m_value_res;
            }
            else
            {
                MIR_BUG(*m_mir_res, "get_literal_for_const - Not a constant - " << path);
            }
        }

        void assign_from_literal(::std::function<void()> emit_dst, const ::HIR::TypeRef& ty, const ::HIR::Literal& lit)
        {
            //TRACE_FUNCTION_F("ty=" << ty << ", lit=" << lit);
            Span    sp;
            ::HIR::TypeRef  tmp;
            auto monomorph_with = [&](const ::HIR::PathParams& pp, const ::HIR::TypeRef& ty)->const ::HIR::TypeRef& {
                if( monomorphise_type_needed(ty) ) {
                    tmp = monomorphise_type_with(sp, ty, monomorphise_type_get_cb(sp, nullptr, &pp, nullptr), false);
                    m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return ty;
                }
                };
            auto get_inner_type = [&](unsigned int var, unsigned int idx)->const ::HIR::TypeRef& {
                TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, te,
                    return *te.inner;
                )
                else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Path, te,
                    const auto& pp = te.path.m_data.as_Generic().m_params;
                    TU_MATCHA((te.binding), (pbe),
                    (Unbound, MIR_BUG(*m_mir_res, "Unbound type path " << ty); ),
                    (Opaque, MIR_BUG(*m_mir_res, "Opaque type path " << ty); ),
                    (Struct,
                        TU_MATCHA( (pbe->m_data), (se),
                        (Unit,
                            MIR_BUG(*m_mir_res, "Unit struct " << ty);
                            ),
                        (Tuple,
                            return monomorph_with(pp, se.at(idx).ent);
                            ),
                        (Named,
                            return monomorph_with(pp, se.at(idx).second.ent);
                            )
                        )
                        ),
                    (Union,
                        MIR_TODO(*m_mir_res, "Union literals");
                        ),
                    (Enum,
                        MIR_ASSERT(*m_mir_res, pbe->m_data.is_Data(), "");
                        const auto& evar = pbe->m_data.as_Data().at(var);
                        return monomorph_with(pp, evar.type);
                        )
                    )
                    throw "";
                )
                else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Tuple, te,
                    return te.at(idx);
                )
                else {
                    MIR_TODO(*m_mir_res, "Unknown type in list literal - " << ty);
                }
                };
            TU_MATCHA( (lit), (e),
            (Invalid,
                m_of << "/* INVALID */";
                ),
            (List,
                if( ty.m_data.is_Array() )
                {
                    for(unsigned int i = 0; i < e.size(); i ++) {
                        if(i != 0)  m_of << ";\n\t";
                        assign_from_literal([&](){ emit_dst(); m_of << ".DATA[" << i << "]"; }, *ty.m_data.as_Array().inner, e[i]);
                    }
                }
                else
                {
                    for(unsigned int i = 0; i < e.size(); i ++) {
                        if(i != 0)  m_of << ";\n\t";
                        assign_from_literal([&](){ emit_dst(); m_of << "._" << i; }, get_inner_type(0, i), e[i]);
                    }
                }
                ),
            (Variant,
                MIR_ASSERT(*m_mir_res, ty.m_data.is_Path(), "");
                MIR_ASSERT(*m_mir_res, ty.m_data.as_Path().binding.is_Enum(), "");
                const auto& enm = *ty.m_data.as_Path().binding.as_Enum();
                auto it = m_enum_repr_cache.find(ty.m_data.as_Path().path.m_data.as_Generic());
                if( it != m_enum_repr_cache.end() )
                {
                    if( e.idx == 0 ) {
                        emit_nonzero_path(it->second);
                        m_of << " = 0";
                    }
                    else {
                        assign_from_literal([&](){ emit_dst(); }, get_inner_type(e.idx, 0), *e.val);
                    }
                }
                else if( enm.is_value() )
                {
                    MIR_ASSERT(*m_mir_res, TU_TEST1((*e.val), List, .empty()), "Value-only enum with fields");
                    emit_dst(); m_of << ".TAG = " << enm.get_value(e.idx);
                }
                else
                {
                    emit_dst(); m_of << ".TAG = " << e.idx;
                    m_of << ";\n\t";
                    assign_from_literal([&](){ emit_dst(); m_of << ".DATA.var_" << e.idx; }, get_inner_type(e.idx, 0), *e.val);
                }
                ),
            (Integer,
                emit_dst(); m_of << " = ";
                emit_literal(ty, lit, {});
                ),
            (Float,
                emit_dst(); m_of << " = ";
                emit_literal(ty, lit, {});
                ),
            (BorrowPath,
                if( ty.m_data.is_Function() )
                {
                    emit_dst(); m_of << " = " << Trans_Mangle(e);
                }
                else if( ty.m_data.is_Borrow() )
                {
                    const auto& ity = *ty.m_data.as_Borrow().inner;
                    switch( metadata_type(ity) )
                    {
                    case MetadataType::None:
                        emit_dst(); m_of << " = &" << Trans_Mangle(e);
                        break;
                    case MetadataType::Slice:
                        emit_dst(); m_of << ".PTR = &" << Trans_Mangle(e) << ";\n\t";
                        // HACK: Since getting the size is hard, use two sizeofs
                        emit_dst(); m_of << ".META = sizeof(" << Trans_Mangle(e) << ") / ";
                        if( ity.m_data.is_Slice() ) {
                            m_of << "sizeof("; emit_ctype(*ity.m_data.as_Slice().inner); m_of << ")";
                        }
                        else {
                            m_of << "/*TODO*/";
                        }
                        break;
                    case MetadataType::TraitObject:
                        emit_dst(); m_of << ".PTR = &" << Trans_Mangle(e) << ";\n\t";
                        emit_dst(); m_of << ".META = /* TODO: Const VTable */";
                        break;
                    }
                }
                else
                {
                    emit_dst(); m_of << " = &" << Trans_Mangle(e);
                }
                ),
            (BorrowData,
                MIR_TODO(*m_mir_res, "Handle BorrowData (assign_from_literal) - " << *e);
                ),
            (String,
                emit_dst(); m_of << ".PTR = ";
                this->print_escaped_string(e);
                m_of << ";\n\t";
                emit_dst(); m_of << ".META = " << e.size();
                )
            )
        }

        void emit_lvalue(const ::MIR::LValue& val) {
            TU_MATCHA( (val), (e),
            (Return,
                m_of << "rv";
                ),
            (Argument,
                m_of << "arg" << e.idx;
                ),
            (Local,
                if( e == ~0u )
                    m_of << "i";
                else
                    m_of << "var" << e;
                ),
            (Static,
                m_of << Trans_Mangle(e);
                ),
            (Field,
                ::HIR::TypeRef  tmp;
                const auto& ty = m_mir_res->get_lvalue_type(tmp, *e.val);
                if( ty.m_data.is_Slice() ) {
                    if( e.val->is_Deref() )
                    {
                        m_of << "(("; emit_ctype(*ty.m_data.as_Slice().inner); m_of << "*)";
                        emit_lvalue(*e.val->as_Deref().val);
                        m_of << ".PTR)";
                    }
                    else
                    {
                        emit_lvalue(*e.val);
                    }
                    m_of << "[" << e.field_index << "]";
                }
                else if( ty.m_data.is_Array() ) {
                    emit_lvalue(*e.val);
                    m_of << ".DATA[" << e.field_index << "]";
                }
                else if( e.val->is_Deref() ) {
                    auto dst_type = metadata_type(ty);
                    if( dst_type != MetadataType::None )
                    {
                        m_of << "(("; emit_ctype(ty); m_of << "*)"; emit_lvalue(*e.val->as_Deref().val); m_of << ".PTR)->_" << e.field_index;
                    }
                    else
                    {
                        emit_lvalue(*e.val->as_Deref().val);
                        m_of << "->_" << e.field_index;
                    }
                }
                else {
                    emit_lvalue(*e.val);
                    m_of << "._" << e.field_index;
                }
                ),
            (Deref,
                // TODO: If the type is unsized, then this pointer is a fat pointer, so we need to cast the data pointer.
                ::HIR::TypeRef  tmp;
                const auto& ty = m_mir_res->get_lvalue_type(tmp, val);
                auto dst_type = metadata_type(ty);
                if( dst_type != MetadataType::None )
                {
                    m_of << "(*("; emit_ctype(ty); m_of << "*)";
                    emit_lvalue(*e.val);
                    m_of << ".PTR)";
                }
                else
                {
                    m_of << "(*";
                    emit_lvalue(*e.val);
                    m_of << ")";
                }
                ),
            (Index,
                ::HIR::TypeRef  tmp;
                const auto& ty = m_mir_res->get_lvalue_type(tmp, *e.val);
                m_of << "(";
                if( ty.m_data.is_Slice() ) {
                    if( e.val->is_Deref() )
                    {
                        m_of << "("; emit_ctype(*ty.m_data.as_Slice().inner); m_of << "*)";
                        emit_lvalue(*e.val->as_Deref().val);
                        m_of << ".PTR";
                    }
                    else {
                        emit_lvalue(*e.val);
                    }
                }
                else if( ty.m_data.is_Array() ) {
                    emit_lvalue(*e.val);
                    m_of << ".DATA";
                }
                else {
                    emit_lvalue(*e.val);
                }
                m_of << ")[";
                emit_lvalue(*e.idx);
                m_of << "]";
                ),
            (Downcast,
                ::HIR::TypeRef  tmp;
                const auto& ty = m_mir_res->get_lvalue_type(tmp, *e.val);
                emit_lvalue(*e.val);
                MIR_ASSERT(*m_mir_res, ty.m_data.is_Path(), "Downcast on non-Path type - " << ty);
                if( ty.m_data.as_Path().binding.is_Enum() )
                {
                    auto it = m_enum_repr_cache.find(ty.m_data.as_Path().path.m_data.as_Generic());
                    if( it != m_enum_repr_cache.end() )
                    {
                        MIR_ASSERT(*m_mir_res, e.variant_index == 1, "");
                        // NOTE: Downcast returns a magic tuple
                        m_of << "._1";
                        break ;
                    }
                    else
                    {
                        m_of << ".DATA";
                    }
                }
                m_of << ".var_" << e.variant_index;
                )
            )
        }
        void emit_constant(const ::MIR::Constant& ve, const ::MIR::LValue* dst_ptr=nullptr)
        {
            TU_MATCHA( (ve), (c),
            (Int,
                if( c.v == INT64_MIN )
                    m_of << "INT64_MIN";
                else
                {
                    switch(c.t)
                    {
                    case ::HIR::CoreType::I64:
                    case ::HIR::CoreType::Isize:
                        m_of << c.v;
                        m_of << "ll";
                        break;
                    case ::HIR::CoreType::I128:
                        if( m_options.emulated_i128 )
                        {
                            m_of << "make128s(" << c.v << "ll)";
                        }
                        else
                        {
                            m_of << c.v;
                            m_of << "ll";
                        }
                        break;
                    default:
                        m_of << c.v;
                        break;
                    }
                }
                ),
            (Uint,
                switch(c.t)
                {
                case ::HIR::CoreType::U8:
                    m_of << ::std::hex << "0x" << (c.v & 0xFF) << ::std::dec;
                    break;
                case ::HIR::CoreType::U16:
                    m_of << ::std::hex << "0x" << (c.v & 0xFFFF) << ::std::dec;
                    break;
                case ::HIR::CoreType::U32:
                    m_of << ::std::hex << "0x" << (c.v & 0xFFFFFFFF) << ::std::dec;
                    break;
                case ::HIR::CoreType::U64:
                case ::HIR::CoreType::Usize:
                    m_of << ::std::hex << "0x" << c.v << "ull" << ::std::dec;
                    break;
                case ::HIR::CoreType::U128:
                    if( m_options.emulated_i128 )
                    {
                        m_of << "make128(" << ::std::hex << "0x" << c.v << "ull)" << ::std::dec;
                    }
                    else
                    {
                        m_of << ::std::hex << "0x" << c.v << "ull" << ::std::dec;
                    }
                    break;
                case ::HIR::CoreType::Char:
                    assert(0 <= c.v && c.v <= 0x10FFFF);
                    if( c.v < 256 ) {
                        m_of << c.v;
                    }
                    else {
                        m_of << ::std::hex << "0x" << c.v << ::std::dec;
                    }
                    break;
                default:
                    MIR_BUG(*m_mir_res, "Invalid type for UInt literal - " << c.t);
                }
                ),
            (Float,
                this->emit_float(c.v);
                ),
            (Bool,
                m_of << (c.v ? "true" : "false");
                ),
            (Bytes,
                // Array borrow : Cast the C string to the array
                // - Laziness
                m_of << "(void*)";
                this->print_escaped_string(c);
                ),
            (StaticString,
                m_of << "make_sliceptr(";
                this->print_escaped_string(c);
                m_of << ", " << ::std::dec << c.size() << ")";
                ),
            (Const,
                // TODO: This should have been eliminated? ("MIR Cleanup" should have removed all inline Const references)
                ::HIR::TypeRef  ty;
                const auto& lit = get_literal_for_const(c.p, ty);
                if(lit.is_Integer() || lit.is_Float() || lit.is_String())
                {
                    emit_literal(ty, lit, {});
                }
                else
                {
                    // NOTE: GCC hack - statement expressions
                    MIR_ASSERT(*m_mir_res, m_compiler == Compiler::Gcc, "TODO: Support inline constants without statement expressions");
                    m_of << "({"; emit_ctype(ty, FMT_CB(ss, ss<<"v";)); m_of << "; ";
                    assign_from_literal([&](){ m_of << "v"; }, ty, lit);
                    m_of << "; v;})";
                }
                ),
            (ItemAddr,
                TU_MATCHA( (c.m_data), (pe),
                (Generic,
                    if( pe.m_path.m_components.size() > 1 && m_crate.get_typeitem_by_path(sp, pe.m_path, false, true).is_Enum() )
                        ;
                    else
                    {
                        const auto& vi = m_crate.get_valitem_by_path(sp, pe.m_path);
                        if( vi.is_Function() || vi.is_StructConstructor() )
                        {
                        }
                        else
                        {
                            m_of << "&";
                        }
                    }
                    ),
                (UfcsUnknown,
                    MIR_BUG(*m_mir_res, "UfcsUnknown in trans " << c);
                    ),
                (UfcsInherent,
                    // TODO: If the target is a function, don't emit the &
                    m_of << "&";
                    ),
                (UfcsKnown,
                    // TODO: If the target is a function, don't emit the &
                    m_of << "&";
                    )
                )
                m_of << Trans_Mangle(c);
                )
            )
        }
        void emit_param(const ::MIR::Param& p) {
            TU_MATCHA( (p), (e),
            (LValue,
                emit_lvalue(e);
                ),
            (Constant,
                emit_constant(e);
                )
            )
        }
        void emit_ctype(const ::HIR::TypeRef& ty) {
            emit_ctype(ty, FMT_CB(_,));
        }
        void emit_ctype(const ::HIR::TypeRef& ty, ::FmtLambda inner, bool is_extern_c=false) {
            TU_MATCHA( (ty.m_data), (te),
            (Infer,
                m_of << "@" << ty << "@" << inner;
                ),
            (Diverge,
                m_of << "tBANG " << inner;
                ),
            (Primitive,
                switch(te)
                {
                case ::HIR::CoreType::Usize:    m_of << "uintptr_t";   break;
                case ::HIR::CoreType::Isize:    m_of << "intptr_t";  break;
                case ::HIR::CoreType::U8:  m_of << "uint8_t"; break;
                case ::HIR::CoreType::I8:  m_of << "int8_t"; break;
                case ::HIR::CoreType::U16: m_of << "uint16_t"; break;
                case ::HIR::CoreType::I16: m_of << "int16_t"; break;
                case ::HIR::CoreType::U32: m_of << "uint32_t"; break;
                case ::HIR::CoreType::I32: m_of << "int32_t"; break;
                case ::HIR::CoreType::U64: m_of << "uint64_t"; break;
                case ::HIR::CoreType::I64: m_of << "int64_t"; break;
                case ::HIR::CoreType::U128: m_of << "uint128_t"; break;
                case ::HIR::CoreType::I128: m_of << "int128_t"; break;

                case ::HIR::CoreType::F32: m_of << "float"; break;
                case ::HIR::CoreType::F64: m_of << "double"; break;

                case ::HIR::CoreType::Bool: m_of << "bool"; break;
                case ::HIR::CoreType::Char: m_of << "RUST_CHAR";  break;
                case ::HIR::CoreType::Str:
                    MIR_BUG(*m_mir_res, "Raw str");
                }
                m_of << " " << inner;
                ),
            (Path,
                //if( const auto* ity = m_resolve.is_type_owned_box(ty) ) {
                //    emit_ctype_ptr(*ity, inner);
                //    return ;
                //}
                TU_MATCHA( (te.binding), (tpb),
                (Struct,
                    m_of << "struct s_" << Trans_Mangle(te.path);
                    ),
                (Union,
                    m_of << "union u_" << Trans_Mangle(te.path);
                    ),
                (Enum,
                    m_of << "struct e_" << Trans_Mangle(te.path);
                    ),
                (Unbound,
                    MIR_BUG(*m_mir_res, "Unbound type path in trans - " << ty);
                    ),
                (Opaque,
                    MIR_BUG(*m_mir_res, "Opaque path in trans - " << ty);
                    )
                )
                m_of << " " << inner;
                ),
            (Generic,
                MIR_BUG(*m_mir_res, "Generic in trans - " << ty);
                ),
            (TraitObject,
                MIR_BUG(*m_mir_res, "Raw trait object - " << ty);
                ),
            (ErasedType,
                MIR_BUG(*m_mir_res, "ErasedType in trans - " << ty);
                ),
            (Array,
                m_of << "t_" << Trans_Mangle(ty) << " " << inner;
                //emit_ctype(*te.inner, inner);
                //m_of << "[" << te.size_val << "]";
                ),
            (Slice,
                MIR_BUG(*m_mir_res, "Raw slice object - " << ty);
                ),
            (Tuple,
                if( te.size() == 0 )
                    m_of << "tUNIT";
                else {
                    m_of << "TUP_" << te.size();
                    for(const auto& t : te)
                        m_of << "_" << Trans_Mangle(t);
                }
                m_of << " " << inner;
                ),
            (Borrow,
                emit_ctype_ptr(*te.inner, inner);
                ),
            (Pointer,
                emit_ctype_ptr(*te.inner, inner);
                ),
            (Function,
                m_of << "t_" << Trans_Mangle(ty) << " " << inner;
                ),
            (Closure,
                MIR_BUG(*m_mir_res, "Closure during trans - " << ty);
                )
            )
        }

        ::HIR::TypeRef get_inner_unsized_type(const ::HIR::TypeRef& ty)
        {
            if( ty == ::HIR::CoreType::Str || ty.m_data.is_Slice() ) {
                return ty.clone();
            }
            else if( ty.m_data.is_TraitObject() ) {
                return ty.clone();
            }
            else if( ty.m_data.is_Path() )
            {
                TU_MATCH_DEF( ::HIR::TypeRef::TypePathBinding, (ty.m_data.as_Path().binding), (tpb),
                (
                    MIR_BUG(*m_mir_res, "Unbound/opaque path in trans - " << ty);
                    throw "";
                    ),
                (Struct,
                    switch( tpb->m_struct_markings.dst_type )
                    {
                    case ::HIR::StructMarkings::DstType::None:
                        return ::HIR::TypeRef();
                    case ::HIR::StructMarkings::DstType::Slice:
                    case ::HIR::StructMarkings::DstType::TraitObject:
                    case ::HIR::StructMarkings::DstType::Possible: {
                        // TODO: How to figure out? Lazy way is to check the monomorpised type of the last field (structs only)
                        const auto& path = ty.m_data.as_Path().path.m_data.as_Generic();
                        const auto& str = *ty.m_data.as_Path().binding.as_Struct();
                        auto monomorph = [&](const auto& tpl) {
                            // TODO: expand_associated_types
                            auto rv = monomorphise_type(sp, str.m_params, path.m_params, tpl);
                            m_resolve.expand_associated_types(sp, rv);
                            return rv;
                            };
                        TU_MATCHA( (str.m_data), (se),
                        (Unit,  MIR_BUG(*m_mir_res, "Unit-like struct with DstType::Possible"); ),
                        (Tuple, return get_inner_unsized_type( monomorph(se.back().ent) ); ),
                        (Named, return get_inner_unsized_type( monomorph(se.back().second.ent) ); )
                        )
                        throw "";
                        }
                    }
                    ),
                (Union,
                    return ::HIR::TypeRef();
                    ),
                (Enum,
                    return ::HIR::TypeRef();
                    )
                )
                throw "";
            }
            else
            {
                return ::HIR::TypeRef();
            }
        }
        // TODO: Move this to a more common location
        MetadataType metadata_type(const ::HIR::TypeRef& ty) const
        {
            if( ty == ::HIR::CoreType::Str || ty.m_data.is_Slice() ) {
                return MetadataType::Slice;
            }
            else if( ty.m_data.is_TraitObject() ) {
                return MetadataType::TraitObject;
            }
            else if( ty.m_data.is_Path() )
            {
                TU_MATCH_DEF( ::HIR::TypeRef::TypePathBinding, (ty.m_data.as_Path().binding), (tpb),
                (
                    MIR_BUG(*m_mir_res, "Unbound/opaque path in trans - " << ty);
                    ),
                (Struct,
                    switch( tpb->m_struct_markings.dst_type )
                    {
                    case ::HIR::StructMarkings::DstType::None:
                        return MetadataType::None;
                    case ::HIR::StructMarkings::DstType::Possible: {
                        // TODO: How to figure out? Lazy way is to check the monomorpised type of the last field (structs only)
                        const auto& path = ty.m_data.as_Path().path.m_data.as_Generic();
                        const auto& str = *ty.m_data.as_Path().binding.as_Struct();
                        auto monomorph = [&](const auto& tpl) {
                            auto rv = monomorphise_type(sp, str.m_params, path.m_params, tpl);
                            m_resolve.expand_associated_types(sp, rv);
                            return rv;
                            };
                        TU_MATCHA( (str.m_data), (se),
                        (Unit,  MIR_BUG(*m_mir_res, "Unit-like struct with DstType::Possible"); ),
                        (Tuple, return metadata_type( monomorph(se.back().ent) ); ),
                        (Named, return metadata_type( monomorph(se.back().second.ent) ); )
                        )
                        //MIR_TODO(*m_mir_res, "Determine DST type when ::Possible - " << ty);
                        return MetadataType::None;
                        }
                    case ::HIR::StructMarkings::DstType::Slice:
                        return MetadataType::Slice;
                    case ::HIR::StructMarkings::DstType::TraitObject:
                        return MetadataType::TraitObject;
                    }
                    ),
                (Union,
                    return MetadataType::None;
                    ),
                (Enum,
                    return MetadataType::None;
                    )
                )
                throw "";
            }
            else {
                return MetadataType::None;
            }
        }

        void emit_ctype_ptr(const ::HIR::TypeRef& inner_ty, ::FmtLambda inner) {
            //if( inner_ty.m_data.is_Array() ) {
            //    emit_ctype(inner_ty, FMT_CB(ss, ss << "(*" << inner << ")";));
            //}
            //else
            {
                switch( metadata_type(inner_ty) )
                {
                case MetadataType::None:
                    emit_ctype(inner_ty, FMT_CB(ss, ss << "*" << inner;));
                    break;
                case MetadataType::Slice:
                    m_of << "SLICE_PTR " << inner;
                    break;
                case MetadataType::TraitObject:
                    m_of << "TRAITOBJ_PTR " << inner;
                    break;
                }
            }
        }

        bool is_dst(const ::HIR::TypeRef& ty) const
        {
            return metadata_type(ty) != MetadataType::None;
        }
    };
    Span CodeGenerator_C::sp;
}

::std::unique_ptr<CodeGenerator> Trans_Codegen_GetGeneratorC(const ::HIR::Crate& crate, const ::std::string& outfile)
{
    return ::std::unique_ptr<CodeGenerator>(new CodeGenerator_C(crate, outfile));
}
