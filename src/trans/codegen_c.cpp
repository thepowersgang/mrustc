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
#include <limits>
#include <mir/mir.hpp>
#include <hir_typeck/static.hpp>
#include <mir/helpers.hpp>
#include "codegen_c.hpp"
#include "target.hpp"
#include "allocator.hpp"
#include <iomanip>

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

        std::vector<const char*>::const_iterator begin() const { return m_strings.begin(); }
        std::vector<const char*>::const_iterator end() const { return m_strings.end(); }

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


        ::std::set< ::HIR::TypeRef> m_emitted_fn_types;
        ::std::set< const TypeRepr*>    m_embedded_tags;
    public:
        CodeGenerator_C(const ::HIR::Crate& crate, const ::std::string& outfile):
            m_crate(crate),
            m_resolve(crate),
            m_outfile_path(outfile),
            m_outfile_path_c(outfile + ".c"),
            m_of(m_outfile_path_c)
        {
            ASSERT_BUG(Span(), m_of.is_open(), "Failed to open `" << m_outfile_path_c << "` for writing");
            m_options.emulated_i128 = Target_GetCurSpec().m_backend_c.m_emulated_i128;
            switch(Target_GetCurSpec().m_backend_c.m_codegen_mode)
            {
            case CodegenMode::Gnu11:
                m_compiler = Compiler::Gcc;
                if( Target_GetCurSpec().m_arch.m_pointer_bits < 64 && !m_options.emulated_i128 )
                {
                    WARNING(Span(), W0000, "Potentially misconfigured target, 32-bit targets require i128 emulation");
                }
                m_options.disallow_empty_structs = true;
                break;
            case CodegenMode::Msvc:
                m_compiler = Compiler::Msvc;
                if( !m_options.emulated_i128 )
                {
                    WARNING(Span(), W0000, "Potentially misconfigured target, MSVC requires i128 emulation");
                }
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
                << "#include <stdarg.h>\n"
                << "#include <assert.h>\n"
                ;
            switch(m_compiler)
            {
            case Compiler::Gcc:
                m_of
                    << "#include <stdatomic.h>\n"   // atomic_*
                    << "#include <stdlib.h>\n"  // abort
                    << "#include <string.h>\n"  // mem*
                    << "#include <math.h>\n"  // round, ...
                    << "#include <setjmp.h>\n"  // setjmp/jmp_buf
                    ;
                break;
            case Compiler::Msvc:
                m_of
                    << "#include <windows.h>\n"
                    << "#include <math.h>\n"  // fabsf, ...
                    << "void abort(void);\n"
                    ;
                break;
            }
            m_of
                << "typedef uint32_t RUST_CHAR;\n"
                << "typedef uint8_t RUST_BOOL;\n"
                << "typedef struct { void* PTR; size_t META; } SLICE_PTR;\n"
                << "typedef struct { void* PTR; void* META; } TRAITOBJ_PTR;\n"
                << "typedef struct { void (*drop)(void*); size_t size; size_t align; } VTABLE_HDR;\n"
                ;
            if( m_options.disallow_empty_structs )
            {
                m_of
                    << "typedef struct { char _d; } tUNIT;\n"
                    << "typedef char tBANG;\n"
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
                << "static inline size_t ALIGN_TO(size_t s, size_t a) { return (s + a-1) / a * a; }\n"
                << "\n"
                ;
            switch(m_compiler)
            {
            case Compiler::Gcc:
                m_of
                    << "extern void _Unwind_Resume(void) __attribute__((noreturn));\n"
                    << "#define ALIGNOF(t) __alignof__(t)\n"
                    ;
                break;
            case Compiler::Msvc:
                m_of
                    << "__declspec(noreturn) static void _Unwind_Resume(void) { abort(); }\n"
                    << "#define ALIGNOF(t) __alignof(t)\n"
                    ;
                break;
            //case Compiler::Std11:
            //    break;
            }
            switch (m_compiler)
            {
            case Compiler::Gcc:
                m_of
                    << "extern __thread jmp_buf*    mrustc_panic_target;\n"
                    << "extern __thread void* mrustc_panic_value;\n"
                    ;
                // 64-bit bit ops (gcc intrinsics)
                m_of
                    << "static inline uint64_t __builtin_clz64(uint64_t v) {\n"
                    << "\treturn ( (v >> 32) != 0 ? __builtin_clz(v>>32) : 32 + __builtin_clz(v));\n"
                    << "}\n"
                    << "static inline uint64_t __builtin_ctz64(uint64_t v) {\n"
                    << "\treturn ((v&0xFFFFFFFF) == 0 ? __builtin_ctz(v>>32) + 32 : __builtin_ctz(v));\n"
                    << "}\n"
                    ;
                // Atomic hackery
                for(int sz = 8; sz <= 64; sz *= 2)
                {
                    m_of
                        << "static inline uint"<<sz<<"_t __mrustc_atomicloop"<<sz<<"(volatile uint"<<sz<<"_t* slot, uint"<<sz<<"_t param, int ordering, uint"<<sz<<"_t (*cb)(uint"<<sz<<"_t, uint"<<sz<<"_t)) {"
                        << " int ordering_load = (ordering == memory_order_release || ordering == memory_order_acq_rel ? memory_order_relaxed : ordering);" // If Release, Load with Relaxed
                        << " for(;;) {"
                        << " uint"<<sz<<"_t v = atomic_load_explicit((_Atomic uint"<<sz<<"_t*)slot, ordering_load);"
                        << " if( atomic_compare_exchange_strong_explicit((_Atomic uint"<<sz<<"_t*)slot, &v, cb(v, param), ordering, ordering_load) ) return v;"
                        << " }"
                        << "}\n"
                        ;
                }
                break;
            case Compiler::Msvc:
                m_of
                    << "static inline int32_t __builtin_popcountll(uint64_t v) {\n"
                    << "\treturn __popcnt(v & 0xFFFFFFFF) + __popcnt(v >> 32);\n"
                    << "}\n"
                    << "static inline int __builtin_ctz(uint32_t v) { int rv; _BitScanForward(&rv, v); return rv; }\n"
                    << "static inline int __builtin_clz(uint32_t v) { int rv; _BitScanReverse(&rv, v); return 31 - rv; }\n"
                    << "static inline uint64_t __builtin_clz64(uint64_t v) {\n"
                    << "\treturn ( (v >> 32) != 0 ? __builtin_clz(v>>32) : 32 + __builtin_clz(v) );\n"
                    << "}\n"
                    << "static inline uint64_t __builtin_ctz64(uint64_t v) {\n"
                    << "\treturn ((v&0xFFFFFFFF) == 0 ? __builtin_ctz(v>>32) + 32 : __builtin_ctz(v));\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_u8(uint8_t a, uint8_t b, uint8_t* out) {\n"
                    << "\t*out = a*b;\n"
                    << "\tif(b == 0) return false;\n"
                    << "\tif(a > UINT8_MAX/b)  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_i8(int8_t a, int8_t b, int8_t* out) {\n"
                    << "\t*out = a*b;\n"    // Wait, this isn't valid?
                    << "\tif(b == 0) return false;\n"
                    << "\tif(a > INT8_MAX/b)  return true;\n"
                    << "\tif(a < INT8_MIN/b)  return true;\n"
                    << "\tif( (a == -1) && (b == INT8_MIN))  return true;\n"
                    << "\tif( (b == -1) && (a == INT8_MIN))  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_u16(uint16_t a, uint16_t b, uint16_t* out) {\n"
                    << "\t*out = a*b;\n"
                    << "\tif(b == 0) return false;\n"
                    << "\tif(a > UINT16_MAX/b)  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_i16(int16_t a, int16_t b, int16_t* out) {\n"
                    << "\t*out = a*b;\n"    // Wait, this isn't valid?
                    << "\tif(b == 0) return false;\n"
                    << "\tif(a > INT16_MAX/b)  return true;\n"
                    << "\tif(a < INT16_MIN/b)  return true;\n"
                    << "\tif( (a == -1) && (b == INT16_MIN))  return true;\n"
                    << "\tif( (b == -1) && (a == INT16_MIN))  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_u32(uint32_t a, uint32_t b, uint32_t* out) {\n"
                    << "\t*out = a*b;\n"
                    << "\tif(b == 0) return false;\n"
                    << "\tif(a > UINT32_MAX/b)  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_i32(int32_t a, int32_t b, int32_t* out) {\n"
                    << "\t*out = a*b;\n"    // Wait, this isn't valid?
                    << "\tif(b == 0) return false;\n"
                    << "\tif(a > INT32_MAX/b)  return true;\n"
                    << "\tif(a < INT32_MIN/b)  return true;\n"
                    << "\tif( (a == -1) && (b == INT32_MIN))  return true;\n"
                    << "\tif( (b == -1) && (a == INT32_MIN))  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_u64(uint64_t a, uint64_t b, uint64_t* out) {\n"
                    << "\t*out = a*b;\n"
                    << "\tif(b == 0) return false;\n"
                    << "\tif(a > UINT64_MAX/b)  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_i64(int64_t a, int64_t b, int64_t* out) {\n"
                    << "\t*out = a*b;\n"    // Wait, this isn't valid?
                    << "\tif(b == 0) return false;\n"
                    << "\tif(a > INT64_MAX/b)  return true;\n"
                    << "\tif(a < INT64_MIN/b)  return true;\n"
                    << "\tif( (a == -1) && (b == INT64_MIN))  return true;\n"
                    << "\tif( (b == -1) && (a == INT64_MIN))  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_usize(uintptr_t a, uintptr_t b, uintptr_t* out) {\n"
                    << "\treturn __builtin_mul_overflow_u" << Target_GetCurSpec().m_arch.m_pointer_bits << "(a, b, out);\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_isize(intptr_t a, intptr_t b, intptr_t* out) {\n"
                    << "\treturn __builtin_mul_overflow_i" << Target_GetCurSpec().m_arch.m_pointer_bits << "(a, b, out);\n"
                    << "}\n"
                    << "static inline bool __builtin_sub_overflow_u64(uint64_t a, uint64_t b, uint64_t* o) {\n"
                    << "\t""*o = a - b;\n"
                    << "\t""return a < b;\n"
                    << "}\n"
                    << "static inline bool __builtin_sub_overflow_u32(uint32_t a, uint32_t b, uint32_t* o) {\n"
                    << "\t""*o = a - b;\n"
                    << "\t""return a < b;\n"
                    << "}\n"
                    << "static inline bool __builtin_sub_overflow_u16(uint16_t a, uint16_t b, uint16_t* o) {\n"
                    << "\t""*o = a - b;\n"
                    << "\t""return a < b;\n"
                    << "}\n"
                    << "static inline bool __builtin_sub_overflow_u8(uint8_t a, uint8_t b, uint8_t* o) {\n"
                    << "\t""*o = a - b;\n"
                    << "\t""return a < b;\n"
                    << "}\n"
                    << "static inline bool __builtin_sub_overflow_usize(uintptr_t a, uintptr_t b, uintptr_t* out) {\n"
                    << "\treturn __builtin_sub_overflow_u" << Target_GetCurSpec().m_arch.m_pointer_bits << "(a, b, out);\n"
                    << "}\n"
                    << "static inline bool __builtin_add_overflow_u64(uint64_t a, uint64_t b, uint64_t* o) {\n"
                    << "\t""*o = a + b;\n"
                    << "\t""return a > UINT64_MAX - b;\n"
                    << "}\n"
                    << "static inline bool __builtin_add_overflow_u32(uint32_t a, uint32_t b, uint32_t* o) {\n"
                    << "\t""*o = a + b;\n"
                    << "\t""return a > UINT32_MAX - b;\n"
                    << "}\n"
                    << "static inline bool __builtin_add_overflow_u16(uint16_t a, uint16_t b, uint16_t* o) {\n"
                    << "\t""*o = a + b;\n"
                    << "\t""return a > UINT16_MAX - b;\n"
                    << "}\n"
                    << "static inline bool __builtin_add_overflow_u8(uint8_t a, uint8_t b, uint8_t* o) {\n"
                    << "\t""*o = a + b;\n"
                    << "\t""return a > UINT8_MAX - b;\n"
                    << "}\n"
                    << "static inline bool __builtin_add_overflow_usize(uintptr_t a, uintptr_t b, uintptr_t* out) {\n"
                    << "\treturn __builtin_add_overflow_u" << Target_GetCurSpec().m_arch.m_pointer_bits << "(a, b, out);\n"
                    << "}\n"
                    << "static inline bool __builtin_sub_overflow_i64(int64_t a, int64_t b, int64_t* o) {\n"
                    << "\t""*o = a - b;\n"
                    << "\t""return ((b < 0) && (a > INT64_MAX + b)) || ((b > 0) && (a < INT64_MIN + b));\n"
                    << "}\n"
                    << "static inline bool __builtin_sub_overflow_i32(int32_t a, int32_t b, int32_t* o) {\n"
                    << "\t""*o = a - b;\n"
                    << "\t""return ((b < 0) && (a > INT32_MAX + b)) || ((b > 0) && (a < INT32_MIN + b));\n"
                    << "}\n"
                    << "static inline bool __builtin_sub_overflow_i16(int16_t a, int16_t b, int16_t* o) {\n"
                    << "\t""*o = a - b;\n"
                    << "\t""return ((b < 0) && (a > INT16_MAX + b)) || ((b > 0) && (a < INT16_MIN + b));\n"
                    << "}\n"
                    << "static inline bool __builtin_sub_overflow_i8(int8_t a, int8_t b, int8_t* o) {\n"
                    << "\t""*o = a - b;\n"
                    << "\t""return ((b < 0) && (a > INT8_MAX + b)) || ((b > 0) && (a < INT8_MIN + b));\n"
                    << "}\n"
                    << "static inline bool __builtin_sub_overflow_isize(intptr_t a, intptr_t b, intptr_t* out) {\n"
                    << "\treturn __builtin_sub_overflow_i" << Target_GetCurSpec().m_arch.m_pointer_bits << "(a, b, out);\n"
                    << "}\n"
                    << "static inline bool __builtin_add_overflow_i64(int64_t a, int64_t b, int64_t* o) {\n"
                    << "\t""*o = a + b;\n"
                    << "\t""return ((b > 0) && (a > INT64_MAX - b)) || ((b < 0) && (a < INT64_MIN - b));\n"
                    << "}\n"
                    << "static inline bool __builtin_add_overflow_i32(int32_t a, int32_t b, int32_t* o) {\n"
                    << "\t""*o = a + b;\n"
                    << "\t""return ((b > 0) && (a > INT32_MAX - b)) || ((b < 0) && (a < INT32_MIN - b));\n"
                    << "}\n"
                    << "static inline bool __builtin_add_overflow_i16(int16_t a, int16_t b, int16_t* o) {\n"
                    << "\t""*o = a + b;\n"
                    << "\t""return ((b > 0) && (a > INT16_MAX - b)) || ((b < 0) && (a < INT16_MIN - b));\n"
                    << "}\n"
                    << "static inline bool __builtin_add_overflow_i8(int8_t a, int8_t b, int8_t* o) {\n"
                    << "\t""*o = a + b;\n"
                    << "\t""return ((b > 0) && (a > INT8_MAX - b)) || ((b < 0) && (a < INT8_MIN - b));\n"
                    << "}\n"
                    << "static inline bool __builtin_add_overflow_isize(intptr_t a, intptr_t b, intptr_t* out) {\n"
                    << "\treturn __builtin_add_overflow_i" << Target_GetCurSpec().m_arch.m_pointer_bits << "(a, b, out);\n"
                    << "}\n"
                    << "static inline uint64_t __builtin_bswap64(uint64_t v) { return _byteswap_uint64(v); }\n"
                    << "#define InterlockedCompareExchange8Acquire _InterlockedCompareExchange8\n"
                    << "#define InterlockedCompareExchange8Release _InterlockedCompareExchange8\n"
                    << "#define InterlockedCompareExchange8NoFence _InterlockedCompareExchange8\n"
                    << "#define InterlockedCompareExchange8 _InterlockedCompareExchange8\n"
                    << "#define InterlockedCompareExchange16Acquire InterlockedCompareExchangeAcquire16\n"
                    << "#define InterlockedCompareExchange16Release InterlockedCompareExchangeRelease16\n"
                    << "#define InterlockedCompareExchange16NoFence InterlockedCompareExchangeNoFence16\n"
                    << "#define InterlockedCompareExchange64Acquire InterlockedCompareExchangeAcquire64\n"
                    << "#define InterlockedCompareExchange32 InterlockedCompareExchange\n"
                    << "#define InterlockedCompareExchange64Release InterlockedCompareExchangeRelease64\n"
                    << "#define InterlockedCompareExchange64NoFence InterlockedCompareExchangeNoFence64\n"
                    << "#define InterlockedExchangeAdd8Acquire InterlockedExchangeAdd8\n"
                    << "#define InterlockedExchangeAdd8Release InterlockedExchangeAdd8\n"
                    << "#define InterlockedExchangeAdd8NoFence InterlockedExchangeAdd8\n"
                    << "#define InterlockedExchangeAdd16Acquire _InterlockedExchangeAdd16\n"
                    << "#define InterlockedExchangeAdd16Release _InterlockedExchangeAdd16\n"
                    << "#define InterlockedExchangeAdd16NoFence _InterlockedExchangeAdd16\n"
                    << "#define InterlockedExchangeAdd16 _InterlockedExchangeAdd16\n"
                    << "#define InterlockedExchangeAdd64Acquire InterlockedExchangeAddAcquire64\n"
                    << "#define InterlockedExchangeAdd64Release InterlockedExchangeAddRelease64\n"
                    << "#define InterlockedExchangeAdd64NoFence InterlockedExchangeAddNoFence64\n"
                    << "#define InterlockedExchange8Acquire InterlockedExchange8\n"
                    << "#define InterlockedExchange8Release InterlockedExchange8\n"
                    << "#define InterlockedExchange8NoFence InterlockedExchange8\n"
                    << "#define InterlockedExchange16Acquire InterlockedExchange16\n"
                    << "#define InterlockedExchange16Release InterlockedExchange16\n"
                    << "#define InterlockedExchange16NoFence InterlockedExchange16\n"
                    << "#define InterlockedExchangeRelease InterlockedExchange\n"
                    << "#define InterlockedExchange64Acquire InterlockedExchangeAcquire64\n"
                    << "#define InterlockedExchange64Release InterlockedExchange64\n"
                    << "#define InterlockedExchange64NoFence InterlockedExchangeNoFence64\n"
                    << "#define InterlockedAnd8Acquire InterlockedAnd8\n"
                    << "#define InterlockedAnd8Release InterlockedAnd8\n"
                    << "#define InterlockedAnd8NoFence InterlockedAnd8\n"
                    << "#define InterlockedAnd16Acquire InterlockedAnd16\n"
                    << "#define InterlockedAnd16Release InterlockedAnd16\n"
                    << "#define InterlockedAnd16NoFence InterlockedAnd16\n"
                    << "#define InterlockedOr8Acquire InterlockedOr8\n"
                    << "#define InterlockedOr8Release InterlockedOr8\n"
                    << "#define InterlockedOr8NoFence InterlockedOr8\n"
                    << "#define InterlockedOr16Acquire InterlockedOr16\n"
                    << "#define InterlockedOr16Release InterlockedOr16\n"
                    << "#define InterlockedOr16NoFence InterlockedOr16\n"
                    << "#define InterlockedXor8Acquire InterlockedXor8\n"
                    << "#define InterlockedXor8Release InterlockedXor8\n"
                    << "#define InterlockedXor8NoFence InterlockedXor8\n"
                    << "#define InterlockedXor16Acquire InterlockedXor16\n"
                    << "#define InterlockedXor16Release InterlockedXor16\n"
                    << "#define InterlockedXor16NoFence InterlockedXor16\n"
                    ;
                // Atomic hackery
                for(int sz = 8; sz <= 64; sz *= 2)
                {
                    m_of
                        << "static inline uint"<<sz<<"_t __mrustc_atomicloop"<<sz<<"(volatile uint"<<sz<<"_t* slot, uint"<<sz<<"_t param, uint"<<sz<<"_t (*cb)(uint"<<sz<<"_t, uint"<<sz<<"_t)) {"
                        << " for(;;) {"
                        << " uint"<<sz<<"_t v = InterlockedCompareExchange" << sz << "(slot, 0,0);"
                        << " if( InterlockedCompareExchange" << sz << "(slot, v, cb(v, param)) == v ) return v;"
                        << " }"
                        << "}\n"
                        ;
                }
                break;
            }

            if( m_options.emulated_i128 )
            {
                m_of
                    << "typedef struct { uint64_t lo, hi; } uint128_t;\n"
                    << "typedef struct { uint64_t lo, hi; } int128_t;\n"
                    << "static inline float make_float(int is_neg, int exp, uint32_t mantissa_bits) { float rv; uint32_t vi=(mantissa_bits&((1<<23)-1))|((exp+127)<<23);if(is_neg)vi|=1<<31; memcpy(&rv, &vi, 4); return rv; }\n"
                    << "static inline double make_double(int is_neg, int exp, uint32_t mantissa_bits) { double rv; uint64_t vi=(mantissa_bits&((1ull<<52)-1))|((uint64_t)(exp+1023)<<52);if(is_neg)vi|=1ull<<63; memcpy(&rv, &vi, 4); return rv; }\n"
                    << "static inline uint128_t make128_raw(uint64_t hi, uint64_t lo) { uint128_t rv = { lo, hi }; return rv; }\n"
                    << "static inline uint128_t make128(uint64_t v) { uint128_t rv = { v, 0 }; return rv; }\n"
                    << "static inline float cast128_float(uint128_t v) { if(v.hi == 0) return v.lo; int exp = 0; uint32_t mant = 0; return make_float(0, exp, mant); }\n"
                    << "static inline double cast128_double(uint128_t v) { if(v.hi == 0) return v.lo; int exp = 0; uint64_t mant = 0; return make_double(0, exp, mant); }\n"
                    << "static inline int cmp128(uint128_t a, uint128_t b) { if(a.hi != b.hi) return a.hi < b.hi ? -1 : 1; if(a.lo != b.lo) return a.lo < b.lo ? -1 : 1; return 0; }\n"
                    // Returns true if overflow happens (res < a)
                    << "static inline bool add128_o(uint128_t a, uint128_t b, uint128_t* o) { o->lo = a.lo + b.lo; o->hi = a.hi + b.hi + (o->lo < a.lo ? 1 : 0); return (o->hi < a.hi); }\n"
                    // Returns true if overflow happens (res > a)
                    << "static inline bool sub128_o(uint128_t a, uint128_t b, uint128_t* o) { o->lo = a.lo - b.lo; o->hi = a.hi - b.hi - (a.lo < b.lo ? 1 : 0); return (o->hi > a.hi); }\n"
                    // Serial shift+add
                    << "static inline bool mul128_o(uint128_t a, uint128_t b, uint128_t* o) {"
                    << " bool of = false;"
                    << " o->hi = 0; o->lo = 0;"
                    << " for(int i=0;i<128;i++){"
                    <<  " uint64_t m = (1ull << (i % 64));"
                    <<  " if(a.hi==0&&a.lo<m)   break;"
                    <<  " if(i>=64&&a.hi<m) break;"
                    <<  " if( m & (i >= 64 ? a.hi : a.lo) ) of |= add128_o(*o, b, o);"
                    <<  " b.hi = (b.hi << 1) | (b.lo >> 63);"
                    <<  " b.lo = (b.lo << 1);"
                    << " }"
                    << " return of;"
                    << "}\n"
                    // Long division
                    << "static inline bool div128_o(uint128_t a, uint128_t b, uint128_t* q, uint128_t* r) {"
                    << " if(a.hi == 0 && b.hi == 0) { if(q) { q->hi=0; q->lo = a.lo / b.lo; } if(r) { r->hi=0; r->lo = a.lo % b.lo; } return false; }"
                    << " if(cmp128(a, b) < 0) { if(q) { q->hi=0; q->lo=0; } if(r) *r = a; return false; }"
                    << " uint128_t a_div_2 = {(a.lo>>1)|(a.hi << 63), a.hi>>1};"
                    << " int shift = 0;"
                    << " while( cmp128(a_div_2, b) >= 0 && shift < 128 ) {"
                    <<  " shift += 1;"
                    <<  " b.hi = (b.hi<<1)|(b.lo>>63); b.lo <<= 1;"
                    <<  " }"
                    << " if(shift == 128) return true;" // true = overflowed
                    << " uint128_t mask = { /*lo=*/(shift >= 64 ? 0 : (1ull << shift)), /*hi=*/(shift < 64 ? 0 : 1ull << (shift-64)) };"
                    << " shift ++;"
                    << " if(q) { q->hi = 0; q->lo = 0; }"
                    << " while(shift--) {"
                    <<  " if( cmp128(a, b) >= 0 ) { if(q) add128_o(*q, mask, q); sub128_o(a, b, &a); }"
                    <<  " mask.lo = (mask.lo >> 1) | (mask.hi << 63); mask.hi >>= 1;"
                    <<  " b.lo = (b.lo >> 1) | (b.hi << 63); b.hi >>= 1;"
                    << " }"
                    << " if(r) *r = a;"
                    << " return false;"
                    << "}\n"
                    << "static inline uint128_t add128(uint128_t a, uint128_t b) { uint128_t v; add128_o(a, b, &v); return v; }\n"
                    << "static inline uint128_t sub128(uint128_t a, uint128_t b) { uint128_t v; sub128_o(a, b, &v); return v; }\n"
                    << "static inline uint128_t mul128(uint128_t a, uint128_t b) { uint128_t v; mul128_o(a, b, &v); return v; }\n"
                    << "static inline uint128_t div128(uint128_t a, uint128_t b) { uint128_t v; div128_o(a, b, &v, NULL); return v; }\n"
                    << "static inline uint128_t mod128(uint128_t a, uint128_t b) { uint128_t v; div128_o(a, b, NULL, &v); return v;}\n"
                    << "static inline uint128_t and128(uint128_t a, uint128_t b) { uint128_t v = { a.lo & b.lo, a.hi & b.hi }; return v; }\n"
                    << "static inline uint128_t or128 (uint128_t a, uint128_t b) { uint128_t v = { a.lo | b.lo, a.hi | b.hi }; return v; }\n"
                    << "static inline uint128_t xor128(uint128_t a, uint128_t b) { uint128_t v = { a.lo ^ b.lo, a.hi ^ b.hi }; return v; }\n"
                    << "static inline uint128_t shl128(uint128_t a, uint32_t b) { uint128_t v; if(b == 0) { return a; } else if(b < 64) { v.lo = a.lo << b; v.hi = (a.hi << b) | (a.lo >> (64 - b)); } else { v.hi = a.lo << (b - 64); v.lo = 0; } return v; }\n"
                    << "static inline uint128_t shr128(uint128_t a, uint32_t b) { uint128_t v; if(b == 0) { return a; } else if(b < 64) { v.lo = (a.lo >> b)|(a.hi << (64 - b)); v.hi = a.hi >> b; } else { v.lo = a.hi >> (b - 64); v.hi = 0; } return v; }\n"
                    << "static inline uint128_t popcount128(uint128_t a) { uint128_t v = { __builtin_popcountll(a.lo) + __builtin_popcountll(a.hi), 0 }; return v; }\n"
                    << "static inline uint128_t __builtin_bswap128(uint128_t v) { uint128_t rv = { __builtin_bswap64(v.hi), __builtin_bswap64(v.lo) }; return rv; }\n"
                    << "static inline uint128_t intrinsic_ctlz_u128(uint128_t v) {\n"
                    << "\tuint128_t rv = { (v.hi != 0 ? __builtin_clz64(v.hi) : (v.lo != 0 ? 64 + __builtin_clz64(v.lo) : 128)), 0 };\n"
                    << "\treturn rv;\n"
                    << "}\n"
                    << "static inline uint128_t intrinsic_cttz_u128(uint128_t v) {\n"
                    << "\tuint128_t rv = { (v.lo == 0 ? (v.hi == 0 ? 128 : __builtin_ctz64(v.hi) + 64) : __builtin_ctz64(v.lo)), 0 };\n"
                    << "\treturn rv;\n"
                    << "}\n"
                    << "static inline int128_t make128s_raw(uint64_t hi, uint64_t lo) { int128_t rv = { lo, hi }; return rv; }\n"
                    << "static inline int128_t make128s(int64_t v) { int128_t rv = { v, (v < 0 ? -1 : 0) }; return rv; }\n"
                    << "static inline int128_t neg128s(int128_t v) { int128_t rv = { ~v.lo+1, ~v.hi + (v.lo == 0) }; return rv; }\n"
                    << "static inline float cast128s_float(int128_t v) { if(v.hi == 0) return v.lo; int exp = 0; uint32_t mant = 0; return make_float(0, exp, mant); }\n"
                    << "static inline double cast128s_double(int128_t v) { if(v.hi == 0) return v.lo; int exp = 0; uint64_t mant = 0; return make_double(0, exp, mant); }\n"
                    << "static inline int cmp128s(int128_t a, int128_t b) { if(a.hi != b.hi) return (int64_t)a.hi < (int64_t)b.hi ? -1 : 1; if(a.lo != b.lo) return a.lo < b.lo ? -1 : 1; return 0; }\n"
                    // Returns true if overflow happens (if negative with pos,pos or positive with neg,neg)
                    << "static inline bool add128s_o(int128_t a, int128_t b, int128_t* o) { bool sgna=a.hi>>63; bool sgnb=b.hi>>63; add128_o(*(uint128_t*)&a, *(uint128_t*)&b, (uint128_t*)o); bool sgno = o->hi>>63; return (sgna==sgnb && sgno != sgna); }\n"
                    // Returns true if overflow happens (if neg with pos,neg or pos with neg,pos)
                    << "static inline bool sub128s_o(int128_t a, int128_t b, int128_t* o) { bool sgna=a.hi>>63; bool sgnb=b.hi>>63; sub128_o(*(uint128_t*)&a, *(uint128_t*)&b, (uint128_t*)o); bool sgno = o->hi>>63; return (sgna!=sgnb && sgno != sgna); }\n"
                    << "static inline bool mul128s_o(int128_t a, int128_t b, int128_t* o) {"
                    << " bool sgna = (a.hi >> 63);"
                    << " bool sgnb = (b.hi >> 63);"
                    << " if(sgna) a = neg128s(a);"
                    << " if(sgnb) b = neg128s(b);"
                    << " bool rv = mul128_o(*(uint128_t*)&a, *(uint128_t*)&b, (uint128_t*)o);"
                    << " if(sgnb != sgnb) *o = neg128s(*o);"
                    << " return rv;"
                    << " }\n"
                    << "static inline bool div128s_o(int128_t a, int128_t b, int128_t* q, int128_t* r) {"
                    << " bool sgna = a.hi & (1ull<<63);"
                    << " bool sgnb = b.hi & (1ull<<63);"
                    << " if(sgna) { a.hi = ~a.hi; a.lo = ~a.lo; a.lo += 1; if(a.lo == 0) a.hi += 1; }"
                    << " if(sgnb) { b.hi = ~b.hi; b.lo = ~b.lo; b.lo += 1; if(b.lo == 0) b.hi += 1; }"
                    << " bool rv = div128_o(*(uint128_t*)&a, *(uint128_t*)&b, (uint128_t*)q, (uint128_t*)r);"
                    << " if(sgnb != sgnb) { r->hi = ~r->hi; r->lo = ~r->lo; r->lo += 1; if(r->lo == 0) r->hi += 1; }"
                    << " return rv;"
                    << " }\n"
                    << "static inline int128_t add128s(int128_t a, int128_t b) { int128_t v; add128s_o(a, b, &v); return v; }\n"
                    << "static inline int128_t sub128s(int128_t a, int128_t b) { int128_t v; sub128s_o(a, b, &v); return v; }\n"
                    << "static inline int128_t mul128s(int128_t a, int128_t b) { int128_t v; mul128s_o(a, b, &v); return v; }\n"
                    << "static inline int128_t div128s(int128_t a, int128_t b) { int128_t v; div128s_o(a, b, &v, NULL); return v; }\n"
                    << "static inline int128_t mod128s(int128_t a, int128_t b) { int128_t v; div128s_o(a, b, NULL, &v); return v; }\n"
                    << "static inline int128_t and128s(int128_t a, int128_t b) { int128_t v = { a.lo & b.lo, a.hi & b.hi }; return v; }\n"
                    << "static inline int128_t or128s (int128_t a, int128_t b) { int128_t v = { a.lo | b.lo, a.hi | b.hi }; return v; }\n"
                    << "static inline int128_t xor128s(int128_t a, int128_t b) { int128_t v = { a.lo ^ b.lo, a.hi ^ b.hi }; return v; }\n"
                    << "static inline int128_t shl128s(int128_t a, uint32_t b) { int128_t v; if(b == 0) { return a; } else if(b < 64) { v.lo = a.lo << b; v.hi = (a.hi << b) | (a.lo >> (64 - b)); } else { v.hi = a.lo << (b - 64); v.lo = 0; } return v; }\n"
                    << "static inline int128_t shr128s(int128_t a, uint32_t b) { int128_t v; if(b == 0) { return a; } else if(b < 64) { v.lo = (a.lo >> b)|(a.hi << (64 - b)); v.hi = (int64_t)a.hi >> b; } else { v.lo = (int64_t)a.hi >> (b - 64); v.hi = (int64_t)a.hi < 0 ? -1 : 0; } return v; }\n"
                    ;
            }
            else
            {
                // GCC-only
                m_of
                    << "typedef unsigned __int128 uint128_t;\n"
                    << "typedef signed __int128 int128_t;\n"
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
                << "static inline void noop_drop(tUNIT *p) { }\n"
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
                // Map of reversed nibbles                       0  1  2  3  4  5  6  7   8  9 10 11 12 14 15
                << "static const uint8_t __mrustc_revmap[16] = { 0, 8, 4,12, 2,10, 6,14,  1, 9, 5,13, 3, 7,15};\n"
                << "static inline uint8_t __mrustc_bitrev8(uint8_t v) { if(v==0||v==0xFF) return v; return __mrustc_revmap[v>>4]|(__mrustc_revmap[v&15]<<4); }\n"
                << "static inline uint16_t __mrustc_bitrev16(uint16_t v) { if(v==0) return 0; return ((uint16_t)__mrustc_bitrev8(v>>8))|((uint16_t)__mrustc_bitrev8(v)<<8); }\n"
                << "static inline uint32_t __mrustc_bitrev32(uint32_t v) { if(v==0) return 0; return ((uint32_t)__mrustc_bitrev16(v>>16))|((uint32_t)__mrustc_bitrev16(v)<<16); }\n"
                << "static inline uint64_t __mrustc_bitrev64(uint64_t v) { if(v==0) return 0; return ((uint64_t)__mrustc_bitrev32(v>>32))|((uint64_t)__mrustc_bitrev32(v)<<32); }\n"
                // TODO: 128
                ;
            if( m_options.emulated_i128 )
            {
                m_of << "static inline uint128_t __mrustc_bitrev128(uint128_t v) { uint128_t rv = { __mrustc_bitrev64(v.hi), __mrustc_bitrev64(v.lo) }; return rv; }\n";
            }
            else
            {
                m_of << "static inline uint128_t __mrustc_bitrev128(uint128_t v) {"
                    << " if(v==0) return 0;"
                    << " uint128_t rv = ((uint128_t)__mrustc_bitrev64(v>>64))|((uint128_t)__mrustc_bitrev64(v)<<64);"
                    << " return rv;"
                    << " }\n"
                    ;
            }
            for(int sz = 8; sz <= 64; sz *= 2)
            {
                m_of
                    << "static inline uint"<<sz<<"_t __mrustc_op_umax"<<sz<<"(uint"<<sz<<"_t a, uint"<<sz<<"_t b) { return (a > b ? a : b); }\n"
                    << "static inline uint"<<sz<<"_t __mrustc_op_umin"<<sz<<"(uint"<<sz<<"_t a, uint"<<sz<<"_t b) { return (a < b ? a : b); }\n"
                    << "static inline uint"<<sz<<"_t __mrustc_op_imax"<<sz<<"(uint"<<sz<<"_t a, uint"<<sz<<"_t b) { return ((int"<<sz<<"_t)a > (int"<<sz<<"_t)b ? a : b); }\n"
                    << "static inline uint"<<sz<<"_t __mrustc_op_imin"<<sz<<"(uint"<<sz<<"_t a, uint"<<sz<<"_t b) { return ((int"<<sz<<"_t)a < (int"<<sz<<"_t)b ? a : b); }\n"
                    << "static inline uint"<<sz<<"_t __mrustc_op_and_not"<<sz<<"(uint"<<sz<<"_t a, uint"<<sz<<"_t b) { return ~(a & b); }\n"
                    ;
            }
        }

        ~CodeGenerator_C() {}

        void finalise(const TransOptions& opt, CodegenOutput out_ty, const ::std::string& hir_file) override
        {
            const bool create_shims = (out_ty == CodegenOutput::Executable);

            // TODO: Support dynamic libraries too
            // - No main, but has the rest.
            // - Well... for cdylibs that's the case, for rdylibs it's not
            if( out_ty == CodegenOutput::Executable )
            {
                // TODO: Define this function in MIR?
                m_of << "int main(int argc, const char* argv[]) {\n";
                auto c_start_path = m_resolve.m_crate.get_lang_item_path_opt("mrustc-start");
                if( c_start_path == ::HIR::SimplePath() )
                {
                    m_of << "\treturn " << Trans_Mangle( ::HIR::GenericPath(m_resolve.m_crate.get_lang_item_path(Span(), "start")) ) << "("
                            << Trans_Mangle( ::HIR::GenericPath(m_resolve.m_crate.get_lang_item_path(Span(), "mrustc-main")) ) << ", argc, (uint8_t**)argv"
                            << ");\n";
                }
                else
                {
                    m_of << "\treturn " << Trans_Mangle(::HIR::GenericPath(c_start_path)) << "(argc, argv);\n";
                }
                m_of << "}\n";
            }

            // Auto-generated code/items for the "root" rust binary (cdylib or executable)
            if( create_shims )
            {
                if( m_compiler == Compiler::Gcc )
                {
                    m_of
                        << "__thread jmp_buf* mrustc_panic_target;\n"
                        << "__thread void* mrustc_panic_value;\n"
                        ;
                }

                // Allocator/panic shims
                if( TARGETVER_LEAST_1_29 )
                {
                    // If #[global_allocator]  present, use `__rg_`
                    const char* alloc_prefix = "__rdl_";
                    for(size_t i = 0; i < NUM_ALLOCATOR_METHODS; i++)
                    {
                        struct H {
                            static void ty_args(::std::vector<const char*>& out, AllocatorDataTy t) {
                                switch(t)
                                {
                                case AllocatorDataTy::Unit:
                                case AllocatorDataTy::ResultPtr:  // (..., *mut i8) + *mut u8
                                    throw "";
                                // - Args
                                case AllocatorDataTy::Layout: // usize, usize
                                    out.push_back("uintptr_t");
                                    out.push_back("uintptr_t");
                                    break;
                                case AllocatorDataTy::Ptr:    // *mut u8
                                    out.push_back("int8_t*");
                                    break;
                                case AllocatorDataTy::Usize:
                                    out.push_back("uintptr_t");
                                    break;
                                }
                            }
                            static const char* ty_ret(AllocatorDataTy t) {
                                switch(t)
                                {
                                case AllocatorDataTy::Unit:
                                    return "void";
                                case AllocatorDataTy::ResultPtr:  // (..., *mut i8) + *mut u8
                                    return "int8_t*";
                                // - Args
                                case AllocatorDataTy::Layout: // usize, usize
                                case AllocatorDataTy::Ptr:    // *mut u8
                                case AllocatorDataTy::Usize:
                                    throw "";
                                }
                                throw "";
                            }
                            static void emit_proto(::std::ostream& os, const AllocatorMethod& method, const char* name_prefix, const ::std::vector<const char*>& args) {
                                os << H::ty_ret(method.ret) << " " << name_prefix << method.name << "(";
                                for(size_t j = 0; j < args.size(); j ++)
                                {
                                    if( j != 0 )
                                        os << ", ";
                                    os << args[j] << " a" << j;
                                }
                                os << ")";
                            }
                        };
                        const auto& method = ALLOCATOR_METHODS[i];
                        ::std::vector<const char*>  args;
                        for(size_t j = 0; j < method.n_args; j ++)
                            H::ty_args(args, method.args[j]);
                        H::emit_proto(m_of, method, "__rust_", args); m_of << " {\n";
                        m_of << "\textern "; H::emit_proto(m_of, method, alloc_prefix, args); m_of << ";\n";
                        m_of << "\t";
                        if(method.ret != AllocatorDataTy::Unit)
                            m_of << "return ";
                        m_of << alloc_prefix << method.name << "(";
                        for(size_t j = 0; j < args.size(); j ++)
                        {
                            if( j != 0 )
                                m_of << ", ";
                            m_of << "a" << j;
                        }
                        m_of << ");\n";
                        m_of << "}\n";
                    }

                    if( TARGETVER_LEAST_1_54 )
                    {
                        auto oom_method = m_crate.get_lang_item_path_opt("mrustc-alloc_error_handler");
                        m_of << "void __rust_alloc_error_handler(uintptr_t s, uintptr_t a) {\n";
                        if(oom_method == HIR::SimplePath()) {
                            m_of << "\tvoid __rdl_oom(uintptr_t, uintptr_t);\n";
                            m_of << "\t__rdl_oom(s,a);\n";
                        }
                        else {
                            m_of << "\tvoid __rg_oom(uintptr_t, uintptr_t);\n";
                            m_of << "\t__rg_oom(s,a);\n";
                        }
                        m_of << "}\n";

                        if(oom_method != HIR::SimplePath()) {
                            auto layout_path = ::HIR::SimplePath("core", {"alloc", "Layout"});
                            m_of << "struct s_" << Trans_Mangle(layout_path) << "_A { uintptr_t a, b; };\n";
                            m_of << "void oom_impl(struct s_" << Trans_Mangle(layout_path) << "_A l) {"
                                << " extern void " << Trans_Mangle(oom_method) << "(struct s_" << Trans_Mangle(layout_path) << "_A l);"
                                << " " << Trans_Mangle(oom_method) << "(l);"
                                << " }\n"
                                ;
                        }
                    }
                    else
                    {
                        // TODO: Bind `oom` lang item to the item tagged with `alloc_error_handler`
                        // - Can do this in enumerate/auto_impls instead, for better iteraction with enum
                        // XXX: HACK HACK HACK - This only works with libcore/libstd's current layout
                        auto layout_path = ::HIR::SimplePath("core", {"alloc", "Layout"});
                        //auto oom_method = ::HIR::SimplePath("std", {"alloc", "rust_oom"});
                        auto oom_method = m_crate.get_lang_item_path(Span(), "mrustc-alloc_error_handler");
                        m_of << "struct s_" << Trans_Mangle(layout_path) << "_A { uintptr_t a, b; };\n";
                        m_of << "void oom_impl(struct s_" << Trans_Mangle(layout_path) << "_A l) {"
                            << " extern void " << Trans_Mangle(oom_method) << "(struct s_" << Trans_Mangle(layout_path) << "_A l);"
                            << " " << Trans_Mangle(oom_method) << "(l);"
                            << " }\n"
                            ;
                    }
                }


                if(TARGETVER_LEAST_1_29)
                {
                    // Bind `panic_impl` lang item to the item tagged with `panic_implementation`
                    m_of << "uint32_t panic_impl(uintptr_t payload) {";
                    const auto& panic_impl_path = m_crate.get_lang_item_path(Span(), "mrustc-panic_implementation");
                    m_of << "extern uint32_t " << Trans_Mangle(panic_impl_path) << "(uintptr_t payload);";
                    m_of << "return " << Trans_Mangle(panic_impl_path) << "(payload);";
                    m_of << "}\n";
                }
            }

            m_of.flush();
            m_of.close();
            ASSERT_BUG(Span(), !m_of.bad(), "Error set on output stream for: " << m_outfile_path_c);

            class LinkList: private StringList
            {
            public:
                enum class Ty {
                    //Border,   // --{push,pop}-state
                    Directory,  // -L <value>
                    Explicit,   // <value>
                    Implicit,   // -l <value>
                };
            private:
                std::vector<Ty> m_ty;
            public:
                void push_dir(const char* s) {
                    #if 1
                    // Don't de-dup since there's the push/pop rules
                    auto it = ::std::find_if(StringList::begin(), StringList::end(), [&](const char* es){ return ::std::strcmp(es, s) == 0; });
                    if(it != StringList::end())
                        return ;
                    #endif
                    m_ty.push_back(Ty::Directory);
                    this->push_back(s);
                }
                void push_explicit(std::string s) {
                    m_ty.push_back(Ty::Explicit);
                    this->push_back(std::move(s));
                }
                void push_lib(const char* s) {
                    m_ty.push_back(Ty::Implicit);
                    this->push_back(s);
                }
                void push_lib(std::string s) {
                    m_ty.push_back(Ty::Implicit);
                    this->push_back(std::move(s));
                }
                void push_border() {
                    // If the previous is also a marker, don't push
                    #if 0
                    if( this->get_vec().size() == 0 || this->get_vec().back()[0] == '\0' )
                        return ;
                    m_ty.push_back(Ty::Border);
                    this->push_back("");
                    #endif
                }

                class iterator {
                    const LinkList& parent;
                    size_t idx;
                public:
                    iterator(const LinkList& parent, size_t idx): parent(parent), idx(idx) {}
                    void operator++() {
                        this->idx ++;
                    }
                    bool operator!=(const iterator& x) { return this->idx != x.idx; }
                    std::pair<Ty, const char*> operator*() const {
                        return std::make_pair(parent.m_ty[idx], parent.get_vec()[idx]);
                    }
                };
                iterator begin() const {
                    return iterator(*this, 0);
                }
                iterator end() const {
                    return iterator(*this, this->get_vec().size());
                }
            };
            // Combined list to ensure a sane resolution order?
            LinkList    libraries_and_dirs;

            StringList  ext_crates;
            StringList  ext_crates_dylib;
            switch(out_ty)
            {
            case CodegenOutput::Executable:
            case CodegenOutput::DynamicLibrary:
                for( const auto& crate_name : m_crate.m_ext_crates_ordered )
                {
                    const auto& crate = m_crate.m_ext_crates.at(crate_name);
                    auto is_dylib = [](const ::HIR::ExternCrate& c) {
                        bool rv = false;
                        // TODO: Better rule than this
                        rv |= (c.m_path.compare(c.m_path.size() - 3, 3, ".so") == 0);
                        rv |= (c.m_path.compare(c.m_path.size() - 4, 4, ".dll") == 0);
                        return rv;
                        };
                    // If this crate is included in a dylib crate, ignore it
                    bool is_in_dylib = false;
                    for( const auto& crate2 : m_crate.m_ext_crates )
                    {
                        if( is_dylib(crate2.second) )
                        {
                            for(const auto& subcrate : crate2.second.m_data->m_ext_crates)
                            {
                                if( subcrate.second.m_path == crate.m_path ) {
                                    DEBUG(crate_name << " referenced by dylib " << crate2.first);
                                    is_in_dylib = true;
                                }
                            }
                        }
                        if( is_in_dylib )
                            break;
                    }
                    // NOTE: Only exclude non-dylibs referenced by other dylibs
                    if( is_in_dylib && !is_dylib(crate) )
                        continue ;

                    // Ignore panic crates unless they're the selected crate (and add in the selected panic crate)
                    if( crate.m_data->m_lang_items.count("mrustc-panic_runtime") )
                    {
                        // Check if this is the requested panic crate
                        if( strncmp(crate_name.c_str(), opt.panic_crate.c_str(), opt.panic_crate.size()) != 0 )
                        {
                            DEBUG("Ignore not-selected panic crate: " << crate_name);
                            continue ;
                        }
                        else
                        {
                            DEBUG("Keep panic crate: " << crate_name);
                        }
                    }

                    if( crate.m_path.compare(crate.m_path.size() - 5, 5, ".rlib") == 0)
                    {
                        ext_crates.push_back(crate.m_path.c_str());
                    }
                    else if( is_dylib(crate) )
                    {
                        ext_crates_dylib.push_back(crate.m_path.c_str());
                    }
                    else
                    {
                        // Probably a procedural macro, ignore it
                    }
                }

                struct H {
                    static bool file_exists(const std::string& path)
                    {
                        return std::ifstream(path).is_open();
                    }
                    static std::string find_library_one(const std::string& path, const std::string& name, bool is_windows)
                    {
                        std::string lib_path;
                        if( !is_windows ) {
                            lib_path = FMT(path << "/lib" << name << ".so");
                            if( file_exists(lib_path) )
                                return lib_path;
                            lib_path = FMT(path << "/lib" << name << ".a");
                            if( file_exists(lib_path) )
                                return lib_path;
                        }
                        else {
                            lib_path = FMT(path << "/" << name << ".lib");
                            if( file_exists(lib_path) )
                                return lib_path;
                        }
                        return "";
                    }
                    static std::string find_library(const std::vector<std::string>& paths1, const std::vector<std::string>& paths2, const std::string& name, bool is_windows) {
                        std::string rv;
                        for(const auto& p : paths1)
                        {
                            if( (rv = find_library_one(p, name, is_windows)) != "" )
                                return rv;
                        }
                        for(const auto& p : paths2)
                        {
                            if( (rv = find_library_one(p, name, is_windows)) != "" )
                                return rv;
                        }
                        return "";
                    }
                };

                for(const auto& path : opt.library_search_dirs ) {
                    libraries_and_dirs.push_dir(path.c_str());
                }
                for(const auto& path : opt.libraries ) {
                    libraries_and_dirs.push_lib(path.c_str());
                }
                libraries_and_dirs.push_border();

                for(const auto& path : m_crate.m_link_paths ) {
                    libraries_and_dirs.push_dir(path.c_str());
                }
                for(const auto& lib : m_crate.m_ext_libs) {
                    ASSERT_BUG(Span(), lib.name != "", "");
                    libraries_and_dirs.push_lib(lib.name.c_str());
                }

                for( const auto& crate_name : m_crate.m_ext_crates_ordered )
                {
                    const auto& crate = m_crate.m_ext_crates.at(crate_name);
                    if( !crate.m_data->m_ext_libs.empty() )
                    {
                        if( !crate.m_data->m_link_paths.empty() ) {
                            libraries_and_dirs.push_border();
                        }
                        for(const auto& path : crate.m_data->m_link_paths ) {
                            libraries_and_dirs.push_dir(path.c_str());
                        }
                        // NOTE: Does explicit lookup, to provide scoped search directories
                        // - Needed for 1.39 cargo on linux when libgit2 and libz exist on the system, butsystem libgit2 isn't new enough
                        for(const auto& lib : crate.m_data->m_ext_libs) {
                            ASSERT_BUG(Span(), lib.name != "", "Empty lib from " << crate_name);
                            auto path = H::find_library(crate.m_data->m_link_paths, opt.library_search_dirs, lib.name, m_compiler == Compiler::Msvc);
                            if( path != "" )
                            {
                                libraries_and_dirs.push_explicit(std::move(path));
                            }
                            else
                            {
                                libraries_and_dirs.push_lib(lib.name.c_str());
                            }
                        }
                    }
                }
                break;
            case CodegenOutput::Object:
            case CodegenOutput::StaticLibrary:
                break;
            }

            // Execute $CC with the required libraries
            StringList  args;
#ifdef _WIN32
            bool is_windows = true;
#else
            bool is_windows = false;
#endif
            size_t  arg_file_start = 0;
            switch( m_compiler )
            {
            case Compiler::Gcc:
                // Pick the compiler
                // - from `CC_${TRIPLE}` environment variable, with all '-' in TRIPLE replaced by '_'
                // - from the `CC` environment variable
                // - `${TRIPLE}-gcc` (if available)
                // - `gcc` as fallback
                {
                    std::string varname = "CC_" +  Target_GetCurSpec().m_backend_c.m_c_compiler;
                    std::replace(varname.begin(), varname.end(), '-', '_');

                    if( getenv(varname.c_str()) ) {
                        args.push_back( getenv(varname.c_str()) );
                    }
                    else if( getenv("CC") ) {
                            args.push_back( getenv("CC") );
                    }
                    else if (system(("command -v " + Target_GetCurSpec().m_backend_c.m_c_compiler + "-gcc" + " >/dev/null 2>&1").c_str()) == 0) {
                        args.push_back( Target_GetCurSpec().m_backend_c.m_c_compiler + "-gcc" );
                    }
                    else {
                        args.push_back("gcc");
                    }
                }
                arg_file_start = args.get_vec().size();
                for( const auto& a : Target_GetCurSpec().m_backend_c.m_compiler_opts )
                {
                    args.push_back( a.c_str() );
                }
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
                args.push_back("-fPIC");
                args.push_back("-o");
                switch(out_ty)
                {
                case CodegenOutput::DynamicLibrary:
                case CodegenOutput::Executable:
                case CodegenOutput::Object:
                    args.push_back(m_outfile_path  .c_str());
                    break;
                case CodegenOutput::StaticLibrary:
                    args.push_back(m_outfile_path+".o");
                    break;
                }
                args.push_back(m_outfile_path_c.c_str());
                switch(out_ty)
                {
                case CodegenOutput::DynamicLibrary:
                    args.push_back("-shared");
                case CodegenOutput::Executable:
                    for(const auto& c : ext_crates)
                    {
                        args.push_back(std::string(c) + ".o");
                    }
                    for(const auto& c : ext_crates_dylib)
                    {
                        args.push_back(c);
                    }
                    args.push_back("-Wl,--start-group");    // Group to avoid linking ordering
                    //args.push_back("-Wl,--push-state");
                    for(auto l_d : libraries_and_dirs)
                    {
                        switch(l_d.first)
                        {
                        //case LinkList::Ty::Border:
                        //    args.push_back("-Wl,--pop-state");
                        //    args.push_back("-Wl,--push-state");
                        //    break;
                        case LinkList::Ty::Directory:
                            args.push_back("-L");
                            args.push_back(l_d.second);
                            break;
                        case LinkList::Ty::Implicit:
                            if (!strncmp(l_d.second, "framework=", strlen("framework="))) {
                                args.push_back("-framework");
                                args.push_back(l_d.second + strlen("framework="));
                            }
                            else {
                                args.push_back("-l");
                                args.push_back(l_d.second);
                            }
                            break;
                        case LinkList::Ty::Explicit:
                            args.push_back(l_d.second);
                            break;
                        }
                    }
                    //args.push_back("-Wl,--pop-state");
                    args.push_back("-Wl,--end-group");    // Group to avoid linking ordering
                    for( const auto& a : Target_GetCurSpec().m_backend_c.m_linker_opts )
                    {
                        args.push_back( a.c_str() );
                    }
                    // TODO: Include the HIR file as a magic object?
                    break;
                case CodegenOutput::StaticLibrary:
                case CodegenOutput::Object:
                    args.push_back("-c");
                    break;
                }
                break;
            case Compiler::Msvc:
                // TODO: Look up these paths in the registry and use CreateProcess instead of system
                // - OR, run `vcvarsall` and get the required environment variables and PATH from it?
                args.push_back(detect_msvc().path_vcvarsall);
                args.push_back( Target_GetCurSpec().m_backend_c.m_c_compiler );
                args.push_back("&");
                args.push_back("cl.exe");
                args.push_back("/nologo");
                args.push_back(m_outfile_path_c.c_str());
                arg_file_start = args.get_vec().size(); // Must be after the source file

                args.push_back("/wd4700");  // Ignore C4700 ("uninitialized local variable 'var14' used")
                args.push_back("/F8388608"); // Set max stack size to 8 MB.
                switch(opt.opt_level)
                {
                case 0: break;
                case 1:
                    args.push_back("/O1");
                    break;
                case 2:
                    //args.push_back("/O2");
                    break;
                }
                if( opt.emit_debug_info )
                {
                    args.push_back("/DEBUG");
                    switch(out_ty)
                    {
                    case CodegenOutput::Executable:
                    case CodegenOutput::DynamicLibrary:
                        args.push_back("/Zi");  // Emit a PDB
                        args.push_back(FMT("/Fd" << m_outfile_path << ".pdb")); // Set the PDB path
                        break;
                    case CodegenOutput::StaticLibrary:
                    case CodegenOutput::Object:
                        args.push_back("/Z7");  // Store the debug data in the .obj
                        break;
                    }
                }
                switch(out_ty)
                {
                case CodegenOutput::Executable:
                case CodegenOutput::DynamicLibrary:
                    args.push_back(FMT("/Fe" << m_outfile_path));
                    args.push_back(FMT("/Fo" << m_outfile_path << ".obj"));

                    switch(out_ty)
                    {
                    case CodegenOutput::Executable:
                        args.push_back("/link");
                        break;
                    case CodegenOutput::DynamicLibrary:
                        args.push_back("/LD");
                        break;
                    default:
                        throw "bug";
                    }

                    for(const auto& c : ext_crates)
                    {
                        args.push_back(std::string(c) + ".obj");
                    }
                    for(const auto& c : ext_crates_dylib)
                    {
                        TODO(Span(), "Windows dylibs: " << c);
                    }
                    args.push_back("kernel32.lib"); // Needed for Interlocked*
                    // Crate-specified libraries
                    for(auto l_d : libraries_and_dirs)
                    {
                        switch(l_d.first)
                        {
                        //case LinkList::Ty::Border:      /* TODO: pop/push search path state */  break;
                        case LinkList::Ty::Directory:   args.push_back(FMT("/LIBPATH:" << l_d.second));     break;
                        case LinkList::Ty::Implicit:    args.push_back(std::string(l_d.second) + ".lib");   break;
                        case LinkList::Ty::Explicit:    args.push_back(l_d.second); break;
                        }
                    }
                    break;
                case CodegenOutput::StaticLibrary:
                    args.push_back("/c");
                    args.push_back(FMT("/Fo" << m_outfile_path << ".obj"));
                    break;
                case CodegenOutput::Object:
                    args.push_back("/c");
                    args.push_back(FMT("/Fo" << m_outfile_path));
                    break;
                }
                break;
            }

            ::std::stringstream cmd_ss;
            if (is_windows)
            {
                cmd_ss << "echo \"\" & ";
            }
            std::string command_file = m_outfile_path + "_cmd.txt";
            std::ofstream   command_file_stream;
            bool use_arg_file = arg_file_start > 0;
            if(use_arg_file) {
                command_file_stream.open(command_file);
                ASSERT_BUG(Span(), command_file_stream.is_open(), "Failed to open command file `" << command_file << "` for writing");
            }
            size_t i = -1;
            for(const auto& arg : args.get_vec())
            {
                i ++;
                auto& out_ss = (use_arg_file && i >= arg_file_start ? static_cast<::std::ostream&>(command_file_stream) : cmd_ss);
                if(strcmp(arg, "&") == 0 && is_windows) {
                    out_ss << "&";
                }
                else {
                    if( is_windows && strchr(arg, ' ') == nullptr ) {
                        out_ss << arg << " ";
                    }
                    else {
                        out_ss << "\"" << FmtShell(arg, is_windows) << "\" ";
                    }
                }
            }
            if(use_arg_file) {
                cmd_ss << "@\"" << FmtShell(command_file, is_windows) << "\"";
                command_file_stream.close();
                ASSERT_BUG(Span(), !command_file_stream.bad(), "Error set on output stream for: " << m_outfile_path_c);
            }
            //DEBUG("- " << cmd_ss.str());
            ::std::cout << "Running command - " << cmd_ss.str() << ::std::endl;
            if( opt.build_command_file != "" )
            {
                ::std::cerr << "INVOKE CC: " << cmd_ss.str() << ::std::endl;
                ::std::ofstream(opt.build_command_file) << cmd_ss.str() << ::std::endl;
            }
            else
            {
                int ec = system(cmd_ss.str().c_str());
                if( ec == -1 )
                {
                    ::std::cerr << "C Compiler failed to execute (system returned -1)" << ::std::endl;
                    perror("system");
                    exit(1);
                }
                else if( ec != 0 )
                {
                    ::std::cerr << "C Compiler failed to execute - error code " << ec << ::std::endl;
                    exit(1);
                }
            }

            // HACK! Static libraries aren't implemented properly yet, just touch the output file
            if( out_ty == CodegenOutput::StaticLibrary )
            {
                ::std::ofstream of( m_outfile_path );
                if( !of.good() )
                {
                    // TODO: Error?
                }
            }
        }

        void emit_box_drop(unsigned indent_level, const ::HIR::TypeRef& inner_type, const ::HIR::TypeRef& box_type, const ::MIR::LValue& slot, bool run_destructor)
        {
            auto indent = RepeatLitStr { "\t", static_cast<int>(indent_level) };
            // Emit a call to box_free for the type
            if( run_destructor )
            {
                auto inner_ptr =
                    ::MIR::LValue::new_Field(
                        ::MIR::LValue::new_Field(
                            ::MIR::LValue::new_Field(
                                slot.clone()
                                ,0)
                            ,0)
                        ,0)
                    ;
                emit_destructor_call( ::MIR::LValue::new_Deref(mv$(inner_ptr)), inner_type, /*unsized_valid=*/true, indent_level );
            }

            // NOTE: This is specific to the official liballoc's owned_box
            const auto& p = box_type.data().as_Path().path.m_data.as_Generic().m_params;
            ::HIR::GenericPath  box_free { m_crate.get_lang_item_path(sp, "box_free"), p.clone() };

            // If the allocator is a ZST, it won't exist in the type (need to create a dummy instance for the argument)
            bool alloc_is_zst = false;
            if( TARGETVER_LEAST_1_54 ) {
                ::HIR::TypeRef  tmp;
                const auto& ty = m_mir_res->get_lvalue_type(tmp, MIR::LValue::new_Field(slot.clone(), 1));
                if( type_is_bad_zst(ty) ) {
                    alloc_is_zst = true;
                    m_of << indent << "{ ";
                    emit_ctype(ty); m_of << " zst_alloc = {0};";
                }
            }

            m_of << indent << Trans_Mangle(box_free) << "("; 
            if( TARGETVER_LEAST_1_29 ) {
                // In 1.29, `box_free` takes Unique, so pass the Unique within the Box
                emit_lvalue(slot); m_of << "._0";
            }
            else {
                emit_lvalue(slot); m_of << "._0._0._0";
            }
            // With 1.54, also need to pass the allocator
            if( TARGETVER_LEAST_1_54 ) {
                m_of << ", ";
                if(alloc_is_zst) {
                    m_of << "zst_alloc";
                } else {
                    emit_lvalue(slot); m_of << "._1";
                }
            }
            m_of << ");";
            if(alloc_is_zst) {
                m_of << " }";
            }
            m_of << "\n";
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
            TU_MATCH_HDRA( (ty.data()), {)
            default:
                // No prototype required
            TU_ARMA(Tuple, te) {
                if( te.size() > 0 )
                {
                    m_of << "typedef struct "; emit_ctype(ty); m_of << " "; emit_ctype(ty); m_of << ";\n";
                }
                }
            TU_ARMA(Function, te) {
                emit_type_fn(ty); m_of << "\n";
                }
            TU_ARMA(Array, te) {
                m_of << "typedef struct "; emit_ctype(ty); m_of << " "; emit_ctype(ty); m_of << ";\n";
                }
            TU_ARMA(Path, te) {
                TU_MATCH_HDRA( (te.binding), {)
                TU_ARMA(Unbound, tpb) throw "";
                TU_ARMA(Opaque,  tpb) throw "";
                TU_ARMA(Struct, tpb) {
                    m_of << "struct s_" << Trans_Mangle(te.path) << ";\n";
                    }
                TU_ARMA(ExternType, tpb) {
                    m_of << "struct x_" << Trans_Mangle(te.path) << ";\n";
                    }
                TU_ARMA(Union, tpb) {
                    m_of << "union u_" << Trans_Mangle(te.path) << ";\n";
                    }
                TU_ARMA(Enum, tpb) {
                    m_of << "struct e_" << Trans_Mangle(te.path) << ";\n";
                    }
                }
                }
            TU_ARMA(ErasedType, te) {
                // TODO: Is this actually a bug?
                return ;
                }
            }
        }
        void emit_type_fn(const ::HIR::TypeRef& ty)
        {
            if( m_emitted_fn_types.count(ty) ) {
                return ;
            }
            m_emitted_fn_types.insert(ty.clone());

            const auto& te = ty.data().as_Function();
            m_of << "typedef ";
            // TODO: ABI marker, need an ABI enum?
            if( te.m_rettype == ::HIR::TypeRef::new_unit() )
                m_of << "void";
            else
                // TODO: Better emit_ctype call for return type?
                emit_ctype(te.m_rettype);
            m_of << " (";
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
                    this->emit_ctype( te.m_arg_types[i], FMT_CB(os, os << (this->type_is_high_align(te.m_arg_types[i]) ? "*":"");) );
                }
                m_of << " )";
            }
            m_of << ";";
        }

        // Shared logic between `emit_struct` and `emit_type` (w/ Tuple)
        void emit_struct_inner(const ::HIR::TypeRef& ty, const TypeRepr* repr, unsigned packing_max_align)
        {
            // Fill `fields` with ascending indexes (for sorting)
            // AND: Determine if the type has a a zero-sized item that has an alignment equal to the structure's alignment
            ::std::vector<unsigned> fields;
            size_t max_align = 0;
            bool has_manual_align = false;
            for(const auto& ent : repr->fields)
            {
                fields.push_back(fields.size());

                const auto& ty = ent.ty;

                size_t sz = -1, al = 0;
                Target_GetSizeAndAlignOf(sp, m_resolve, ty, sz, al);
                if( sz == 0 && al == repr->align && al > 0 ) {
                    has_manual_align = true;
                }
                max_align = std::max(max_align, al);
            }
            if(packing_max_align == 0 && max_align != repr->align /*&& repr->size > 0*/) {
                has_manual_align = true;
            }
            // - Sort the fields by offset
            ::std::sort(fields.begin(), fields.end(), [&](auto a, auto b){ return repr->fields[a].offset < repr->fields[b].offset; });

            // For repr(packed), mark as packed
            if(packing_max_align)
            {
                m_of << "#pragma pack(push, " << packing_max_align << ")\n";
            }
            if(has_manual_align)
            {
                switch(m_compiler)
                {
                case Compiler::Msvc:
                    m_of << "__declspec(align(" << repr->align << "))\n";
                    break;
                case Compiler::Gcc:
                    break;
                }
            }
            if( ty.data().is_Tuple() )
            {
                m_of << "typedef ";
                m_of << "struct ";
            }
            emit_ctype(ty); m_of << " {\n";

            bool has_unsized = false;
            size_t sized_fields = 0;
            size_t  cur_ofs = 0;
            for(unsigned fld : fields)
            {
                const auto& ty = repr->fields[fld].ty;
                const auto offset = repr->fields[fld].offset;
                size_t s = 0, a;
                Target_GetSizeAndAlignOf(sp, m_resolve, ty, s, a);
                DEBUG("@" << offset << ": " << ty << " " << s << "," << a);

                // Check offset/alignment
                if( s == SIZE_MAX )
                {
                }
                else if( s == 0 )
                {
                }
                else
                {
                    MIR_ASSERT(*m_mir_res, cur_ofs <= offset, "Current offset is already past expected (#" << fld << "): " << cur_ofs << " > " << offset);
                    a = packing_max_align > 0 ? std::min<size_t>(packing_max_align, a) : a;
                    DEBUG("a = " << a);
                    while(cur_ofs % a != 0)
                        cur_ofs ++;
                    MIR_ASSERT(*m_mir_res, cur_ofs == offset, "Current offset doesn't match expected (#" << fld << "): " << cur_ofs << " != " << offset);

                    cur_ofs += s;
                }

                m_of << "\t";
                if( const auto* te = ty.data().opt_Slice() ) {
                    emit_ctype( te->inner, FMT_CB(ss, ss << "_" << fld << "[0]";) );
                    has_unsized = true;
                }
                else if( ty.data().is_TraitObject() ) {
                    m_of << "unsigned char _" << fld << "[0]";
                    has_unsized = true;
                }
                else if( ty == ::HIR::CoreType::Str ) {
                    m_of << "uint8_t _" << fld << "[0]";
                    has_unsized = true;
                }
                else if( TU_TEST1(ty.data(), Path, .binding.is_ExternType()) ) {
                    m_of << "// External";
                    has_unsized = true;
                }
                else {
                    if( s == 0 && m_options.disallow_empty_structs ) {
                        m_of << "// ZST";
                    }
                    else {

                        // TODO: Nested unsized?
                        emit_ctype( ty, FMT_CB(ss, ss << "_" << fld) );
                        sized_fields ++;

                        has_unsized |= (s == SIZE_MAX);
                    }
                }
                m_of << "; // " << ty << "\n";
            }
            if( sized_fields == 0 && !has_unsized && m_options.disallow_empty_structs )
            {
                m_of << "\tchar _d;\n";
            }
            m_of << "}";
            if(has_manual_align)
            {
                switch(m_compiler)
                {
                case Compiler::Msvc:
                    m_of << " ";
                    if( ty.data().is_Tuple() )
                    {
                        emit_ctype(ty);
                    }
                    m_of << ";";
                    m_of << "\n";
                    break;
                case Compiler::Gcc:
                    m_of << " __attribute__((";
                    if( has_manual_align )
                        m_of << "__aligned__(" << repr->align << "),";
                    m_of << "))";
                    m_of << " ";
                    if( ty.data().is_Tuple() )
                    {
                        emit_ctype(ty);
                    }
                    m_of << ";\n";
                    break;
                }
            }
            else
            {
                m_of << " ";
                if( ty.data().is_Tuple() )
                {
                    emit_ctype(ty);
                }
                m_of << ";\n";
            }
            if( packing_max_align != 0 )
            {
                m_of << "#pragma pack(pop)\n";
            }
        }

        void emit_type(const ::HIR::TypeRef& ty) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "type " << ty;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(ty);
            TU_MATCH_HDRA( (ty.data()), { )
            default:
                // Nothing to emit
                break;
            TU_ARMA(Tuple, te) {
                if( te.size() > 0 )
                {
                    m_of << " // " << ty << "\n";
                    const auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);

                    emit_struct_inner(ty, repr, /*packing_max_align=*/0);

                    if( repr->size > 0 )
                    {
                        m_of << "typedef char sizeof_assert_"; emit_ctype(ty); m_of << "[ (sizeof("; emit_ctype(ty); m_of << ") == " << repr->size << ") ? 1 : -1 ];\n";
                    }
                }
                }
            TU_ARMA(Function, te) {
                emit_type_fn(ty);
                m_of << " // " << ty << "\n";
                }
            TU_ARMA(Array, te) {
                m_of << "typedef ";
                size_t align;
                if( te.size.as_Known() == 0 ) {
                    Target_GetAlignOf(sp, m_resolve, ty, align);
                    switch(m_compiler)
                    {
                    case Compiler::Msvc:
                        m_of << "__declspec(align(" << align << "))\n";
                        break;
                    case Compiler::Gcc:
                        break;
                    }
                }
                m_of << "struct "; emit_ctype(ty); m_of << " { ";
                if( te.size.as_Known() == 0 && m_options.disallow_empty_structs )
                {
                    m_of << "char _d;";
                }
                else
                {
                    emit_ctype(te.inner); m_of << " DATA[" << te.size.as_Known() << "];";
                }
                m_of << " } ";
                if( te.size.as_Known() == 0 ) {
                    switch(m_compiler)
                    {
                    case Compiler::Msvc:
                        break;
                    case Compiler::Gcc:
                        m_of << " __attribute__((";
                        m_of << "__aligned__(" << align << "),";
                        m_of << "))";
                        break;
                    }
                }
                emit_ctype(ty); m_of << ";";
                m_of << " // " << ty << "\n";
                }
            TU_ARMA(ErasedType, te) {
                // TODO: Is this actually a bug?
                return ;
                }
            }

            m_mir_res = nullptr;
        }

        void emit_struct(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Struct& item) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "struct " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;
            // TODO: repr(transparent) and repr(align(foo))

            TRACE_FUNCTION_F(p);
            auto item_ty = ::HIR::TypeRef::new_path(p.clone(), &item);
            const auto* repr = Target_GetTypeRepr(sp, m_resolve, item_ty);
            MIR_ASSERT(*m_mir_res, repr, "No repr for struct " << p);

            m_of << "// struct " << p << "\n";

            emit_struct_inner(item_ty, repr, item.m_max_field_alignment);

            if(repr->size > 0 && repr->size != SIZE_MAX )
            {
                // TODO: Handle unsized (should check the size of the fixed-size region)
                m_of << "typedef char sizeof_assert_" << Trans_Mangle(p) << "[ (sizeof(struct s_" << Trans_Mangle(p) << ") == " << repr->size << ") ? 1 : -1 ];\n";
            }
            m_of << "typedef char alignof_assert_" << Trans_Mangle(p) << "[ (ALIGNOF(struct s_" << Trans_Mangle(p) << ") == " << repr->align << ") ? 1 : -1 ];\n";

            m_mir_res = nullptr;
        }
        void emit_union(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Union& item) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "union " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);
            auto item_ty = ::HIR::TypeRef::new_path(p.clone(), &item);
            const auto* repr = Target_GetTypeRepr(sp, m_resolve, item_ty);
            MIR_ASSERT(*m_mir_res, repr != nullptr, "No repr for union " << item_ty);

            m_of << "union u_" << Trans_Mangle(p) << " {\n";
            for(unsigned int i = 0; i < repr->fields.size(); i ++)
            {
                assert(repr->fields[i].offset == 0);
                m_of << "\t"; emit_ctype( repr->fields[i].ty, FMT_CB(ss, ss << "var_" << i;) ); m_of << ";\n";
            }
            m_of << "};\n";
            if( true && repr->size > 0 )
            {
                m_of << "typedef char sizeof_assert_" << Trans_Mangle(p) << "[ (sizeof(union u_" << Trans_Mangle(p) << ") == " << repr->size << ") ? 1 : -1 ];\n";
            }

            m_mir_res = nullptr;
        }

        bool is_enum_tag(const TypeRepr* repr, size_t idx)
        {
            if( const auto* ve = repr->variants.opt_Values() ) {
                return ve->is_tag(idx);
            }
            if( const auto* ve = repr->variants.opt_Linear() ) {
                return ve->is_tag(idx);
            }
            return false;
        }

        const HIR::TypeRef& emit_enum_path(const TypeRepr* repr, const TypeRepr::FieldPath& path)
        {
            if( is_enum_tag(repr, path.index) )
            {
                // Some enums have the tag outside, some inside
                if( m_embedded_tags.count(repr) ) {
                    m_of << ".DATA";
                }
                m_of << ".TAG";
                assert(path.sub_fields.empty());
            }
            else
            {
                m_of << ".DATA.var_" << path.index;
            }
            const auto* ty = &repr->fields[path.index].ty;
            for(const auto& fld : path.sub_fields)
            {
                repr = Target_GetTypeRepr(sp, m_resolve, *ty);
                if( is_enum_tag(repr, fld) ) {
                    if( m_embedded_tags.count(repr) ) {
                        m_of << ".DATA";
                    }
                    m_of << ".TAG";
                    assert(&fld == &path.sub_fields.back());
                }
                else if( /*!repr->variants.is_None() ||*/ TU_TEST1(ty->data(), Path, .binding.is_Enum()) ) {
                    m_of << ".DATA.var_" << fld;
                }
                else {
                    m_of << "._" << fld;
                }

                ty = &repr->fields[fld].ty;
            }
            if( const auto* te = ty->data().opt_Borrow() )
            {
                if( is_dst(te->inner) ) {
                    m_of << ".PTR";
                }
            }
            else if( const auto* te = ty->data().opt_Pointer() )
            {
                if( is_dst(te->inner) ) {
                    m_of << ".PTR";
                }
            }
            return *ty;
        }

        void emit_enum(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Enum& item) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "enum " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);
            auto item_ty = ::HIR::TypeRef::new_path(p.clone(), &item);
            const auto* repr = Target_GetTypeRepr(sp, m_resolve, item_ty);

            // 1. Enumerate fields with the same offset as the first (these go into a union)
            // TODO: What if all data variants are zero-sized?
            ::std::vector<unsigned> union_fields;
            for(size_t i = 1; i < repr->fields.size(); i ++)
            {
                if( repr->fields[i].offset == repr->fields[0].offset )
                {
                    union_fields.push_back(i);
                }
            }
            if(union_fields.size() > 0 )
            {
                union_fields.insert( union_fields.begin(), 0 );
            }

            m_of << "// enum " << p << "\n";
            m_of << "struct e_" << Trans_Mangle(p) << " {\n";

            // HACK: For NonZero optimised enums, emit a struct with a single field
            // - This avoids a bug in GCC5 where it would generate incorrect code if there's a union here.
            if( const auto* ve = repr->variants.opt_NonZero() )
            {
                m_of << "\tstruct {\n";
                m_of << "\t\t";
                unsigned idx = 1 - ve->zero_variant;
                emit_ctype(repr->fields.at(idx).ty, FMT_CB(os, os << "var_" << idx));
                m_of << ";\n";
                m_of << "\t} DATA;";
            }
            // If there's only one field - it's either a single variant, or a value enum
            else if( repr->fields.size() == 1 )
            {
                if( repr->variants.is_Values() )
                {
                    // Tag only.
                    // - A value-only enum.
                    m_of << "\t";
                    emit_ctype(repr->fields.back().ty, FMT_CB(os, os << "TAG"));
                    m_of << ";\n";
                }
                else
                {
                    m_of << "\tunion {\n";
                    m_of << "\t\t";
                    emit_ctype(repr->fields.back().ty, FMT_CB(os, os << "var_0"));
                    m_of << ";\n";
                    m_of << "\t} DATA;\n";
                    // No tag
                }
            }
            // If there multiple fields with the same offset, they're the data variants
            else if( union_fields.size() > 0 )
            {
                if( union_fields.size() == repr->fields.size() )
                {
                    // Embedded tag
                    DEBUG("Untagged, nonzero or other");
                }
                else
                {
                    // Leading & external tag: repr(C)
                    assert(union_fields.size() + 1 == repr->fields.size());
                    assert( is_enum_tag(repr, repr->fields.size()-1) );
                    
                    assert( repr->fields.back().offset == 0 );
                    DEBUG("Tag present at offset " << repr->fields.back().offset << " - " << repr->fields.back().ty);

                    m_of << "\t";
                    emit_ctype(repr->fields.back().ty, FMT_CB(os, os << "TAG"));
                    m_of << ";\n";
                }

                // Options:
                // - Leading tag (union fields have a non-zero offset, tag has zero)
                // - Embedded (tag field shares offset with union fields, or there's no tag field)

                // Make the union!
                // NOTE: The way the structure generation works is that enum variants are always first, so the field index = the variant index
                // NOTE: Only emit if there are non-empty fields
                if( ::std::any_of(union_fields.begin(), union_fields.end(), [this,repr](auto x){ return !this->type_is_bad_zst(repr->fields[x].ty); }) )
                {
                    m_of << "\tunion {\n";
                    for(auto idx : union_fields)
                    {
                        m_of << "\t\t";

                        const auto& ty = repr->fields[idx].ty;
                        if( this->type_is_bad_zst(ty) ) {
                            m_of << "// ZST: " << ty << "\n";
                        }
                        else {
                            if( is_enum_tag(repr, idx) ) {
                                emit_ctype( ty, FMT_CB(ss, ss << "TAG") );
                                m_embedded_tags.insert(repr);
                            }
                            else {
                                emit_ctype( ty, FMT_CB(ss, ss << "var_" << idx) );
                            }
                            m_of << ";\n";
                            //sized_fields ++;
                        }
                    }
                    m_of << "\t} DATA;\n";
                }
            }
            else if( repr->fields.size() == 0 )
            {
                // Empty/un-constructable
                // - Shouldn't be emitted really?
                if( m_options.disallow_empty_structs )
                {
                    m_of << "\tchar _d;\n";
                }
            }
            else
            {
                // One data field and a tag (or all different offsets)
                TODO(sp, "No common offsets and more than one field, is this possible? - " << item_ty);
            }

            m_of << "};\n";

            size_t exp_size = (repr->size > 0 ? repr->size : (m_options.disallow_empty_structs ? 1 : 0));
            m_of << "typedef char sizeof_assert_" << Trans_Mangle(p) << "[ (sizeof(struct e_" << Trans_Mangle(p) << ") == " << exp_size << ") ? 1 : -1 ];\n";

            m_mir_res = nullptr;
        }

        void emit_constructor_enum(const Span& sp, const ::HIR::GenericPath& path, const ::HIR::Enum& item, size_t var_idx) override
        {
            TRACE_FUNCTION_F(path << " var_idx=" << var_idx);

            auto p = path.clone();
            p.m_path.m_components.pop_back();
            auto ty = ::HIR::TypeRef::new_path(p.clone(), &item);

            MonomorphStatePtr   ms(nullptr, &path.m_params, nullptr);
            ::HIR::TypeRef  tmp;
            auto monomorph = [&](const auto& x)->const auto& { return m_resolve.monomorph_expand_opt(sp, tmp, x, ms); };


            ASSERT_BUG(sp, item.m_data.is_Data(), "");
            const auto& var = item.m_data.as_Data().at(var_idx);
            ASSERT_BUG(sp, var.type.data().is_Path(), "");
            const auto& str = *var.type.data().as_Path().binding.as_Struct();
            ASSERT_BUG(sp, str.m_data.is_Tuple(), "");
            const auto& e = str.m_data.as_Tuple();

            HIR::Function::args_t   args;
            for(unsigned int i = 0; i < e.size(); i ++)
            {
                args.push_back(::std::make_pair(HIR::Pattern(), monomorph(e[i].ent)) );
            }

            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "enum cons " << path;), ty, args, empty_fcn };
            m_mir_res = &top_mir_res;

            m_of << "static struct e_" << Trans_Mangle(p) << " " << Trans_Mangle(path) << "(";
            for(unsigned int i = 0; i < e.size(); i ++)
            {
                if(i != 0)
                    m_of << ", ";
                emit_ctype( args[i].second, FMT_CB(ss, ss << "arg" << i;) );
            }
            m_of << ") {\n";

            m_of << "\tstruct e_" << Trans_Mangle(p) << " rv;\n";

            std::vector<MIR::Param> vals;
            for(unsigned int i = 0; i < e.size(); i ++)
            {
                vals.push_back(MIR::LValue::new_Argument(i));
            }

            // Create the variant
            // - Use `emit_statement` to avoid re-writing the enum tag handling
            emit_statement(*m_mir_res, ::MIR::Statement::make_Assign({
                ::MIR::LValue::new_Return(),
                ::MIR::RValue::make_EnumVariant({
                    p.clone(),
                    static_cast<unsigned>(var_idx),
                    mv$(vals)
                    })
                }));
            m_of << "\treturn rv;\n";
            m_of << "}\n";
            m_mir_res = nullptr;
        }
        void emit_constructor_struct(const Span& sp, const ::HIR::GenericPath& p, const ::HIR::Struct& item) override
        {
            TRACE_FUNCTION_F(p);
            ::HIR::TypeRef  tmp;
            MonomorphStatePtr   ms(nullptr, &p.m_params, nullptr);
            auto monomorph = [&](const auto& x)->const auto& { return m_resolve.monomorph_expand_opt(sp, tmp, x, ms); };

            // Crate constructor function
            const auto& e = item.m_data.as_Tuple();
            m_of << "static struct s_" << Trans_Mangle(p) << " " << Trans_Mangle(p) << "(";
            for(unsigned int i = 0; i < e.size(); i ++)
            {
                if(i != 0)
                    m_of << ", ";
                emit_ctype( monomorph(e[i].ent), FMT_CB(ss, ss << "_" << i;) );
            }
            m_of << ") {\n";
            m_of << "\tstruct s_" << Trans_Mangle(p) << " rv = {";
            bool emitted = false;
            for(unsigned int i = 0; i < e.size(); i ++)
            {
                if( this->type_is_bad_zst(monomorph(e[i].ent)) )
                    continue ;
                if(emitted)
                    m_of << ",";
                emitted = true;
                m_of << "\n\t\t_" << i;
            }
            if( !emitted )
            {
                m_of << "\n\t\t0";
            }
            m_of << "\n";
            m_of << "\t\t};\n";
            m_of << "\treturn rv;\n";
            m_of << "}\n";
        }

        // Returns `true` if the type is pointer-aligned (i.e. it could contain a pointer)
        bool emit_static_ty(const HIR::TypeRef& type, const ::HIR::Path& p, bool is_proto)
        {
            size_t size = 0, align = 0;
            Target_GetSizeAndAlignOf(sp, m_resolve, type, size, align);
            bool rv = ( align * 8 >= Target_GetCurSpec().m_arch.m_pointer_bits );
            m_of << "union u_static_" << Trans_Mangle(p);
            if(is_proto) {
                m_of << "{ "; emit_ctype( type, FMT_CB(ss, ss << "val";) ); m_of << "; ";
                if( rv ) {
                    m_of << "uintptr_t raw[" << (size / (Target_GetCurSpec().m_arch.m_pointer_bits / 8)) << "];";
                }
                else {
                    m_of << "uint8_t raw[" << size << "];";
                }
                m_of << " }";
            }
            m_of << " " << Trans_Mangle(p);
            return rv;
        }

        void emit_static_ext(const ::HIR::Path& p, const ::HIR::Static& item, const Trans_Params& params) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "extern static " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;
            TRACE_FUNCTION_F(p);

            // LLVM supports prepending a symbol name with \1 to prevent further mangling.
            // Since we're targeting C, not LLVM, strip off this prefix.
            std::string linkage_name = item.m_linkage.name;
            if( !linkage_name.empty() && linkage_name[0] == '\1' ) {
                linkage_name = linkage_name.substr(1);
            }

            if( linkage_name != "" )
            {
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    // Handled with asm() later
                    break;
                case Compiler::Msvc:
                    m_of << "#pragma comment(linker, \"/alternatename:" << Trans_Mangle(p) << "=" << linkage_name << "\")\n";
                    break;
                //case Compiler::Std11:
                //    m_of << "#define " << Trans_Mangle(p) << " " << linkage_name << "\n";
                //    break;
                }
            }

            auto type = params.monomorph(m_resolve, item.m_type);
            m_of << "extern ";
            emit_static_ty(type, p, /*is_proto=*/true);
            if( linkage_name != "" && m_compiler == Compiler::Gcc)
            {
                if (Target_GetCurSpec().m_os_name == "macos") // Not macOS only, but all Apple platforms.
                    m_of << " asm(\"_" << linkage_name << "\")";
                else
                    m_of << " asm(\"" << linkage_name << "\")";
            }
            m_of << ";";
            m_of << "\t// static " << p << " : " << type;
            m_of << "\n";

            m_mir_res = nullptr;
        }
        void emit_static_proto(const ::HIR::Path& p, const ::HIR::Static& item, const Trans_Params& params) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "static " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);
            auto type = params.monomorph(m_resolve, item.m_type);
            switch(item.m_linkage.type)
            {
            case HIR::Linkage::Type::External:
                break;
            case HIR::Linkage::Type::Auto:
                break;
            case HIR::Linkage::Type::Weak:
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    m_of << "__attribute__((weak)) ";
                    break;
                case Compiler::Msvc:
                    m_of << "__declspec(selectany) ";
                    break;
                }
                break;
            }
            if(item.m_linkage.section != "")
            {
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    m_of << "__attribute__((section(\"" << item.m_linkage.section << "\"))) ";
                    break;
                case Compiler::Msvc:
                    // Ignore section on MSVC
                    break;
                }
            }
            emit_static_ty(type, p, /*is_proto=*/true);
            m_of << ";";
            m_of << "\t// static " << p << " : " << type;
            m_of << "\n";

            m_mir_res = nullptr;
        }
        void emit_static_local(const ::HIR::Path& p, const ::HIR::Static& item, const Trans_Params& params) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "static " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);

            auto type = params.monomorph(m_resolve, item.m_type);
            // statics that are zero do not require initializers, since they will be initialized to zero on program startup.
            if( !is_zero_literal(type, item.m_value_res, params)) {
                bool is_packed = emit_static_ty(type, p, /*is_proto=*/false);
                m_of << " = ";

                //auto encoded = Trans_EncodeLiteralAsBytes(sp, m_resolve, item.m_value_res, type);
                const auto& encoded = item.m_value_res;
                m_of << "{ .raw = {";
                if( is_packed ) {
                    DEBUG("encoded.bytes = `" << FMT_CB(ss, for(auto& b: encoded.bytes) ss << std::setw(2) << std::setfill('0') << std::hex << unsigned(b) << (int(&b - encoded.bytes.data()) % 8 == 7 ? " " : "");) << "`");
                    DEBUG("encoded.relocations = " << encoded.relocations);
                    auto reloc_it = encoded.relocations.begin();
                    auto ptr_size = Target_GetCurSpec().m_arch.m_pointer_bits / 8;
                    for(size_t i = 0; i < encoded.bytes.size(); i += ptr_size)
                    {
                        uint64_t v = 0;
                        if(Target_GetCurSpec().m_arch.m_big_endian) {
                            for(size_t o = 0, j = ptr_size; j--; o++)
                                v |= static_cast<uint64_t>(encoded.bytes[i+o]) << (j*8);
                        }
                        else {
                            for(size_t o = 0, j = 0; j < ptr_size; j++, o++)
                                v |= static_cast<uint64_t>(encoded.bytes[i+o]) << (j*8);
                        }

                        if(i > 0) {
                            m_of << ",";
                        }

                        if( reloc_it != encoded.relocations.end() && reloc_it->ofs <= i ) {
                            MIR_ASSERT(*m_mir_res, reloc_it->ofs == i, "Relocation not aligned to a pointer - " << reloc_it->ofs << " != " << i);
                            MIR_ASSERT(*m_mir_res, reloc_it->len == ptr_size, "Relocation size not pointer size - " << reloc_it->len << " != " << ptr_size);
                            v -= EncodedLiteral::PTR_BASE;

                            MIR_ASSERT(*m_mir_res, v == 0, "TODO: Relocation with non-zero offset " << i << ": v=0x" << std::hex << v << std::dec << " Literal=" << item.m_value_res << " Reloc=" << *reloc_it);
                            m_of << "(uintptr_t)";
                            if( reloc_it->p ) {
                                m_of << "&" << Trans_Mangle(*reloc_it->p);
                            }
                            else {
                                this->print_escaped_string(reloc_it->bytes);
                            }

                            ++ reloc_it;
                        }
                        else {
                            m_of << "0x" << std::hex << v << "ull" << std::dec;
                        }
                    }
                }
                else {
                    MIR_ASSERT(*m_mir_res, encoded.relocations.empty(), "Non-pointer-aligned data with relocations");
                    bool e = false;
                    m_of << std::dec;
                    for(auto b : encoded.bytes) {
                        if(e)
                            m_of << ",";
                        m_of << int(b); // Just leave it as decimal
                        e = true;
                    }
                }
                m_of << "} }";
                m_of << ";";
                m_of << "\t// static " << p << " : " << type << " = " << item.m_value_res;
                m_of << "\n";
            }

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
                        // If the next character is a hex digit, close/reopen the string.
                        if( &v < &s.back() && isxdigit(*(&v+1)) )
                            m_of << "\"\"";
                    }
                }
            }
            m_of << "\"" << ::std::dec;
        }

        void emit_function_ext(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "extern fn " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;
            TRACE_FUNCTION_F(p);

            m_of << "// EXTERN extern \"" << item.m_abi << "\" " << p << "\n";
            // For MSVC, make a static wrapper that goes and calls the actual function
            if( item.m_linkage.name.rfind("llvm.", 0) == 0 )
            {
                m_of << "static ";
                emit_function_header(p, item, params);
                m_of
                    << "{\n"
                    ;
                m_of << "\t"; emit_ctype(item.m_return); m_of << " rv;\n";

                // MSVC needs suffixed `__builtin_{add,sub}_overflow` calls
                const char* msvc_suffix_u32 = "";
                if( m_compiler == Compiler::Msvc )
                {
                    msvc_suffix_u32 = "_u32";
                }

                // pshufb instruction w/ 128 bit operands
                if( item.m_linkage.name == "llvm.x86.ssse3.pshuf.b.128" ) {
                    m_of
                        << "\tconst uint8_t* src = (const uint8_t*)&arg0;\n"
                        << "\tconst uint8_t* mask = (const uint8_t*)&arg1;\n"
                        << "\tuint8_t* dst = (uint8_t*)&rv;\n"
                        << "\tfor(int i = 0; i < " << 128/8 << "; i ++) dst[i] = (mask[i] < 0x80 ? src[i] : 0);\n"
                        << "\treturn rv;\n"
                        ;
                }
                else if( item.m_linkage.name == "llvm.x86.sse2.psrli.d") {
                    m_of
                        << "\tconst uint32_t* src = (const uint32_t*)&arg0;\n"
                        << "\tuint32_t* dst = (uint32_t*)&rv;\n"
                        << "\tfor(int i = 0; i < " << 128/32 << "; i ++) dst[i] = src[i] >> arg1;\n"
                        << "\treturn rv;\n"
                        ;
                }
                else if( item.m_linkage.name == "llvm.x86.sse2.pslli.d") {
                    m_of
                        << "\tconst uint32_t* src = (const uint32_t*)&arg0;\n"
                        << "\tuint32_t* dst = (uint32_t*)&rv;\n"
                        << "\tfor(int i = 0; i < " << 128/32 << "; i ++) dst[i] = src[i] << arg1;\n"
                        << "\treturn rv;\n"
                        ;
                }
                else if( item.m_linkage.name == "llvm.x86.sse2.pmovmskb.128") {
                    m_of
                        << "\tconst uint8_t* src = (const uint8_t*)&arg0;\n"
                        << "\tuint8_t* dst = (uint8_t*)&rv; *dst = 0;\n"
                        << "\tfor(int i = 0; i < " << 128/8 << "; i ++) *dst |= (src[i] >> 7) << i;\n"
                        << "\treturn rv;\n"
                        ;
                }
                else if( item.m_linkage.name == "llvm.x86.sse2.storeu.dq" ) {
                    m_of << "\tmemcpy(arg0, &arg1, sizeof(arg1));\n";
                }
                // Add with carry
                // `fn llvm_addcarry_u32(a: u8, b: u32, c: u32) -> (u8, u32)`
                else if( item.m_linkage.name == "llvm.x86.addcarry.32") {
                    m_of << "\trv._0 = __builtin_add_overflow" << msvc_suffix_u32 << "(arg1, arg2, &rv._1);\n";
                    m_of << "\tif(arg0) rv._0 |= __builtin_add_overflow" << msvc_suffix_u32 << "(rv._1, 1, &rv._1);\n";
                    m_of << "\treturn rv;\n";
                }
                // `fn llvm_addcarryx_u32(a: u8, b: u32, c: u32, d: *mut u8) -> u32`
                else if( item.m_linkage.name == "llvm.x86.addcarryx.u32") {
                    m_of << "\t*arg3 = __builtin_add_overflow" << msvc_suffix_u32 << "(arg1, arg2, &rv);\n";
                    m_of << "\tif(*arg3) *arg3 |= __builtin_add_overflow" << msvc_suffix_u32 << "(rv, 1, &rv);\n";
                    m_of << "\treturn rv;\n";
                }
                // `fn llvm_subborrow" << msvc_suffix_u32 << "(a: u8, b: u32, c: u32) -> (u8, u32);`
                else if( item.m_linkage.name == "llvm.x86.subborrow.32") {
                    m_of << "\trv._0 = __builtin_sub_overflow" << msvc_suffix_u32 << "(arg1, arg2, &rv._1);\n";
                    m_of << "\tif(arg0) rv._0 |= __builtin_sub_overflow" << msvc_suffix_u32 << "(rv._1, 1, &rv._1);\n";
                    m_of << "\treturn rv;\n";
                }
                // AES functions
                else if( item.m_linkage.name.rfind("llvm.x86.aesni.", 0) == 0 )
                {
                    m_of << "\tassert(!\"Unsupprorted LLVM x86 intrinsic: " << item.m_linkage.name << "\"); abort();\n";
                }
                else {
                    // TODO: Hand off to compiler-specific intrinsics
                    //MIR_TODO(*m_mir_res, "LLVM extern linkage: " << item.m_linkage.name);
                    m_of << "\tassert(!\"Extern LLVM: " << item.m_linkage.name << "\"); abort();\n";
                }
                m_of << "}\n";
                m_mir_res = nullptr;
                return ;
            }
            else if( item.m_linkage.name != "" && m_compiler == Compiler::Msvc )
            {
                m_of << "#pragma comment(linker, \"/alternatename:" << Trans_Mangle(p) << "=" << item.m_linkage.name << "\")\n";
                m_of << "extern ";
            }
            else if( item.m_linkage.name == "_Unwind_RaiseException" )
            {
                MIR_ASSERT(*m_mir_res, m_compiler == Compiler::Gcc, item.m_linkage.name << " in non-GCC mode");
                m_of << "// - Magic compiler impl\n";
                m_of << "static ";
                emit_function_header(p, item, params);
                m_of << " {\n";
                m_of << "\tif( !mrustc_panic_target ) abort();\n";
                m_of << "\tmrustc_panic_value = arg0;\n";
                m_of << "\tlongjmp(*mrustc_panic_target, 1);\n";
                m_of << "}\n";
                return;
            }
            else
            {
                m_of << "extern ";
            }
            emit_function_header(p, item, params);
            if( item.m_linkage.name != "" )
            {
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    if (Target_GetCurSpec().m_os_name == "macos") // Not macOS only, but all Apple platforms.
                        m_of << " asm(\"_" << item.m_linkage.name << "\")";
                    else
                        m_of << " asm(\"" << item.m_linkage.name << "\")";
                    break;
                case Compiler::Msvc:
                    break;
                }
            }
            m_of << ";\n";

            m_mir_res = nullptr;
        }
        void emit_function_proto(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params, bool is_extern_def) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "/*proto*/ fn " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(p);
            m_of << "// PROTO extern \"" << item.m_abi << "\" " << p << "\n";
            if( item.m_linkage.name != "" )
            {
                if( item.m_linkage.type == ::HIR::Linkage::Type::Weak && m_compiler == Compiler::Msvc )
                {
                    // If this function is implementing an external ABI, just rename it (don't bother with per-compiler trickery).
                    m_of << "#pragma comment(linker, \"/alternatename:" << item.m_linkage.name << "=" << Trans_Mangle(p) << "\")\n";
                }
                else
                {
                    // If this function is implementing an external ABI, just rename it (don't bother with per-compiler trickery).
                    m_of << "#define " << Trans_Mangle(p) << " " << item.m_linkage.name << "\n";
                }
            }
            if( is_extern_def )
            {
                m_of << "static ";
            }
            switch(item.m_linkage.type)
            {
            case HIR::Linkage::Type::External:
                break;
            case HIR::Linkage::Type::Auto:
                break;
            case HIR::Linkage::Type::Weak:
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    m_of << "__attribute__((weak)) ";
                    break;
                case Compiler::Msvc:
                    // handled above
                    break;
                }
                break;
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
                m_of << "\t"; emit_ctype(code->locals[i], FMT_CB(ss, ss << "var" << i;));
                // If the type is a ZST, initialise it (to avoid warnings)
                if( this->type_is_bad_zst(code->locals[i]) ) {
                    m_of << " = {0}";
                }
                m_of << ";";
                m_of << "\t// " << code->locals[i];
                m_of << "\n";
            }
            for(unsigned int i = 0; i < code->drop_flags.size(); i ++) {
                m_of << "\tbool df" << i << " = " << code->drop_flags[i] << ";\n";
            }

            ::std::vector<unsigned> bb_use_counts( code->blocks.size() );
            for(const auto& blk : code->blocks)
            {
                MIR::visit::visit_terminator_target(blk.terminator, [&](const auto& tgt){ bb_use_counts[tgt] ++; });
                // Ignore the panic arm. (TODO: is this correct?)
                if( const auto* te = blk.terminator.opt_Call() )
                {
                    bb_use_counts[te->panic_block] --;
                }
            }

            const bool EMIT_STRUCTURED = (nullptr != getenv("MRUSTC_STRUCTURED_C")); // Saves time.
            const bool USE_STRUCTURED = EMIT_STRUCTURED && (0 == strcmp("1", getenv("MRUSTC_STRUCTURED_C")));  // Still not correct.
            if( EMIT_STRUCTURED )
            {
                m_of << "#if " << USE_STRUCTURED << "\n";
                auto nodes = MIR_To_Structured(*code);

                ::std::set<unsigned> goto_targets;
                struct H {
                    static void find_goto_targets(const Node& n, ::std::set<unsigned>& goto_targets) {
                        switch(n.tag())
                        {
                        case Node::TAGDEAD: throw "";
                        TU_ARM(n, Block, ne) {
                            for(const auto& sn : ne.nodes)
                            {
                                if(sn.node)
                                    find_goto_targets(*sn.node, goto_targets);
                            }
                            if(ne.next_bb != SIZE_MAX)
                                goto_targets.insert(ne.next_bb);
                            } break;
                        TU_ARM(n, If, ne) {
                            find_goto_targets_ref(ne.arm_true, goto_targets);
                            find_goto_targets_ref(ne.arm_false, goto_targets);
                            if(ne.next_bb != SIZE_MAX)
                                goto_targets.insert(ne.next_bb);
                            } break;
                        TU_ARM(n, Switch, ne) {
                            for(const auto& sn : ne.arms) {
                                find_goto_targets_ref(sn, goto_targets);
                                if( sn.has_target() && sn.target() != ne.next_bb )
                                {
                                    goto_targets.insert(sn.target());
                                }
                            }
                            if(ne.next_bb != SIZE_MAX)
                                goto_targets.insert(ne.next_bb);
                            } break;
                        TU_ARM(n, SwitchValue, ne) {
                            for(const auto& sn : ne.arms)
                            {
                                find_goto_targets_ref(sn, goto_targets);
                                if( sn.has_target() && sn.target() != ne.next_bb )
                                {
                                    goto_targets.insert(sn.target());
                                }
                            }
                            find_goto_targets_ref(ne.def_arm, goto_targets);
                            if( ne.def_arm.has_target() && ne.def_arm.target() != ne.next_bb )
                            {
                                goto_targets.insert(ne.def_arm.target());
                            }
                            if(ne.next_bb != SIZE_MAX)
                                goto_targets.insert(ne.next_bb);
                            } break;
                        TU_ARM(n, Loop, ne) {
                            assert(ne.code.node);
                            find_goto_targets(*ne.code.node, goto_targets);
                            if(ne.next_bb != SIZE_MAX)
                                goto_targets.insert(ne.next_bb);
                            } break;
                        }
                    }
                    static void find_goto_targets_ref(const NodeRef& r, ::std::set<unsigned>& goto_targets) {
                        if(r.node)
                            find_goto_targets(*r.node, goto_targets);
                        else
                            goto_targets.insert(r.bb_idx);
                    }
                };
                for(const auto& node : nodes)
                {
                    H::find_goto_targets(node, goto_targets);
                }

                for(const auto& node : nodes)
                {
                    m_of << "\t" << "// Node\n";
                    emit_fcn_node(mir_res, node, 1,  goto_targets);

                    switch(node.tag())
                    {
                    case Node::TAGDEAD: throw "";
                    TU_ARM(node, Block, e)
                        if( e.next_bb != SIZE_MAX )
                            m_of << "\t""goto bb" << e.next_bb << ";\n";
                        break;
                    TU_ARM(node, If, e)
                        if( e.next_bb != SIZE_MAX )
                            m_of << "\t""goto bb" << e.next_bb << ";\n";
                        break;
                    TU_ARM(node, Switch, e)
                        if( e.next_bb != SIZE_MAX )
                            m_of << "\t""goto bb" << e.next_bb << ";\n";
                        break;
                    TU_ARM(node, SwitchValue, e)
                        if( e.next_bb != SIZE_MAX )
                            m_of << "\t""goto bb" << e.next_bb << ";\n";
                        break;
                    TU_ARM(node, Loop, e)
                        if( e.next_bb != SIZE_MAX )
                            m_of << "\t""goto bb" << e.next_bb << ";\n";
                        break;
                    }
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
                TU_MATCH_HDRA( (code->blocks[i].terminator), {)
                TU_ARMA(Incomplete, e) {
                    m_of << "\tfor(;;);\n";
                    }
                TU_ARMA(Return, e) {
                    // If the return type is (), don't return a value.
                    if( ret_type == ::HIR::TypeRef::new_unit() )
                        m_of << "\treturn ;\n";
                    else
                        m_of << "\treturn rv;\n";
                    }
                TU_ARMA(Diverge, e) {
                    m_of << "\t_Unwind_Resume();\n";
                    }
                TU_ARMA(Goto, e) {
                    if( e == i+1 )
                    {
                        // Let it flow on to the next block
                    }
                    else
                    {
                        m_of << "\tgoto bb" << e << ";\n";
                    }
                    }
                TU_ARMA(Panic, e) {
                    m_of << "\tgoto bb" << e << "; /* panic */\n";
                    }
                TU_ARMA(If, e) {
                    m_of << "\tif("; emit_lvalue(e.cond); m_of << ") goto bb" << e.bb0 << "; else goto bb" << e.bb1 << ";\n";
                    }
                TU_ARMA(Switch, e) {

                    // If all arms except one are the same, then emit an `if` instead
                    size_t odd_arm = -1;
                    if( e.targets.size() >= 2 )
                    {
                        int n_unique = 0;
                        struct {
                            size_t  first_idx;
                            MIR::BasicBlockId   id;
                            unsigned    count;
                            bool operator==(MIR::BasicBlockId x) const { return id == x; }
                        } uniques[2];
                        for(size_t i = 0; i < e.targets.size(); i ++)
                        {
                            auto t = e.targets[i];
                            auto it = std::find(uniques, uniques+n_unique, t);
                            if( it != uniques+n_unique ) {
                                it->count += 1;
                                continue ;
                            }
                            n_unique += 1;
                            if( n_unique > 2 ) {
                                break;
                            }
                            uniques[n_unique-1].first_idx = i;
                            uniques[n_unique-1].id = t;
                            uniques[n_unique-1].count = 1;
                        }
                        if( n_unique == 2 && (uniques[0].count == 1 || uniques[1].count == 1) )
                        {
                            odd_arm = uniques[(uniques[0].count == 1 ? 0 : 1)].first_idx;
                            DEBUG("Odd arm " << odd_arm);
                        }
                    }
                    emit_term_switch(mir_res, e.val, e.targets.size(), 1, [&](size_t idx) {
                        m_of << "goto bb" << e.targets[idx] << ";";
                        }, odd_arm);
                    }
                TU_ARMA(SwitchValue, e) {
                    emit_term_switchvalue(mir_res, e.val, e.values, 1, [&](size_t idx) {
                        m_of << "goto bb" << (idx == SIZE_MAX ? e.def_target : e.targets[idx]) << ";";
                        });
                    }
                TU_ARMA(Call, e) {
                    emit_term_call(mir_res, e, 1);
                    if( e.ret_block == i+1 )
                    {
                        // Let it flow on to the next block
                    }
                    else
                    {
                        m_of << "\tgoto bb" << e.ret_block << ";\n";
                    }
                    }
                }
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

        void emit_fcn_node(::MIR::TypeResolve& mir_res, const Node& node, unsigned indent_level,  const ::std::set<unsigned>& goto_targets)
        {
            TRACE_FUNCTION_F(node.tag_str());
            auto indent = RepeatLitStr { "\t", static_cast<int>(indent_level) };
            switch(node.tag())
            {
            case Node::TAGDEAD: throw "";
            TU_ARM(node, Block, e) {
                for(size_t i = 0; i < e.nodes.size(); i ++)
                {
                    const auto& snr = e.nodes[i];
                    if( snr.node ) {
                        emit_fcn_node(mir_res, *snr.node, indent_level,  goto_targets);
                    }
                    else {
                        DEBUG(mir_res << "Block BB" << snr.bb_idx);
                        // TODO: Only emit the label if it's ever used as a goto target
                        if( goto_targets.count(snr.bb_idx) )
                            m_of << indent << "bb" << snr.bb_idx << ": (void)0;\n";
                        const auto& bb = mir_res.m_fcn.blocks.at(snr.bb_idx);
                        for(const auto& stmt : bb.statements)
                        {
                            mir_res.set_cur_stmt(snr.bb_idx, (&stmt - &bb.statements.front()));
                            this->emit_statement(mir_res, stmt, indent_level);
                        }

                        TU_MATCH_HDRA( (bb.terminator), {)
                        TU_ARMA(Incomplete, te) {}
                        TU_ARMA(Return, te) {
                            // TODO: If the return type is (), just emit "return"
                            assert(i == e.nodes.size()-1 && "Return");
                            m_of << indent << "return rv;\n";
                            }
                        TU_ARMA(Goto, te) {
                            // Ignore (handled by caller)
                            }
                        TU_ARMA(Diverge, te) {
                            m_of << indent << "_Unwind_Resume();\n";
                            }
                        TU_ARMA(Panic, te) {
                            }
                        TU_ARMA(If, te) {
                            //assert(i == e.nodes.size()-1 && "If");
                            // - This is valid, the next node should be a If (but could be a loop)
                            }
                        TU_ARMA(Call, te) {
                            emit_term_call(mir_res, te, indent_level);
                            }
                        TU_ARMA(Switch, te) {
                            //assert(i == e.nodes.size()-1 && "Switch");
                            }
                        TU_ARMA(SwitchValue, te) {
                            //assert(i == e.nodes.size()-1 && "Switch");
                            }
                        }
                    }
                }
                } break;
            TU_ARM(node, If, e) {
                m_of << indent << "if("; emit_lvalue(*e.val); m_of << ") {\n";
                if( e.arm_true.node ) {
                    emit_fcn_node(mir_res, *e.arm_true.node, indent_level+1,  goto_targets);
                    if( e.arm_true.has_target() && e.arm_true.target() != e.next_bb )
                        m_of << indent << "\tgoto bb" << e.arm_true.target() << ";\n";
                }
                else {
                    m_of << indent << "\tgoto bb" << e.arm_true.bb_idx << ";\n";
                }
                m_of << indent << "}\n";
                m_of << indent << "else {\n";
                if( e.arm_false.node ) {
                    emit_fcn_node(mir_res, *e.arm_false.node, indent_level+1,  goto_targets);
                    if( e.arm_false.has_target() && e.arm_false.target() != e.next_bb )
                        m_of << indent << "\tgoto bb" << e.arm_false.target() << ";\n";
                }
                else {
                    m_of << indent << "\tgoto bb" << e.arm_false.bb_idx << ";\n";
                }
                m_of << indent << "}\n";
                } break;
            TU_ARM(node, Switch, e) {
                this->emit_term_switch(mir_res, *e.val, e.arms.size(), indent_level, [&](auto idx) {
                    const auto& arm = e.arms.at(idx);
                    if( arm.node ) {
                        m_of << "{\n";
                        this->emit_fcn_node(mir_res, *arm.node, indent_level+1,  goto_targets);
                        if( arm.has_target() && arm.target() != e.next_bb ) {
                            m_of << indent << "\t" << "goto bb" << arm.target() << ";\n";
                        }
                        else {
                            //m_of << "break;";
                        }
                        m_of << indent  << "\t}";
                    }
                    else {
                        m_of << "goto bb" << arm.bb_idx << ";";
                    }
                    });
                } break;
            TU_ARM(node, SwitchValue, e) {
                this->emit_term_switchvalue(mir_res, *e.val, *e.vals, indent_level, [&](auto idx) {
                    const auto& arm = (idx == SIZE_MAX ? e.def_arm : e.arms.at(idx));
                    if( arm.node ) {
                        m_of << "{\n";
                        this->emit_fcn_node(mir_res, *arm.node, indent_level+1,  goto_targets);
                        m_of << indent  << "\t} ";
                        if( arm.has_target() && arm.target() != e.next_bb ) {
                            m_of << "goto bb" << arm.target() << ";";
                        }
                    }
                    else {
                        m_of << "goto bb" << arm.bb_idx << ";";
                    }
                    });
                } break;
            TU_ARM(node, Loop, e) {
                m_of << indent << "for(;;) {\n";
                assert(e.code.node);
                assert(e.code.node->is_Block());
                this->emit_fcn_node(mir_res, *e.code.node, indent_level+1,  goto_targets);
                m_of << indent << "}\n";
                } break;
            }
        }

        bool type_is_emulated_i128(const ::HIR::TypeRef& ty) const
        {
            return m_options.emulated_i128 && (ty == ::HIR::CoreType::I128 || ty == ::HIR::CoreType::U128);
        }
        // Returns true if the input type is a ZST and ZSTs are not being emitted
        bool type_is_bad_zst(const ::HIR::TypeRef& ty) const
        {
            if( m_options.disallow_empty_structs )
            {
                // TODO: Extern types are also ZSTs?
                size_t  size, align;
                // NOTE: Uses the Size+Align version because that doesn't panic on unsized
                MIR_ASSERT(*m_mir_res, Target_GetSizeAndAlignOf(sp, m_resolve, ty, size, align), "Unexpected generic? " << ty);
                return size == 0;
            }
            else
            {
                return false;
            }
        }
        bool type_is_high_align(const ::HIR::TypeRef& ty) const
        {
            size_t  size, align;
            // NOTE: Uses the Size+Align version because that doesn't panic on unsized
            MIR_ASSERT(*m_mir_res, Target_GetSizeAndAlignOf(sp, m_resolve, ty, size, align), "Unexpected generic? " << ty);
            return align >Target_GetPointerBits() / 8;
        }

        void emit_borrow(const ::MIR::TypeResolve& mir_res, HIR::BorrowType bt, const MIR::LValue& val)
        {
            ::HIR::TypeRef  tmp;
            const auto& ty = mir_res.get_lvalue_type(tmp, val);
            bool special = false;
            // If the inner value was a deref, just copy the pointer verbatim
            if( val.is_Deref() )
            {
                emit_lvalue( ::MIR::LValue::CRef(val).inner_ref() );
                special = true;
            }
            // Magic for taking a &-ptr to unsized field of a struct.
            // - Needs to get metadata from bottom-level pointer.
            else if( val.is_Field() )
            {
                auto meta_ty = metadata_type(ty);
                if( meta_ty != MetadataType::None ) {
                    auto base_val = ::MIR::LValue::CRef(val).inner_ref();
                    while(base_val.is_Field())
                        base_val.try_unwrap();
                    MIR_ASSERT(mir_res, base_val.is_Deref(), "DST access must be via a deref");
                    const auto base_ptr = base_val.inner_ref();

                    // Construct the new DST
                    switch(meta_ty)
                    {
                    case MetadataType::None:
                        throw "";
                    case MetadataType::Unknown:
                        MIR_BUG(mir_res, "");
                    case MetadataType::Zero:
                        MIR_BUG(mir_res, "");
                    case MetadataType::Slice:
                        m_of << "make_sliceptr";
                        break;
                    case MetadataType::TraitObject:
                        m_of << "make_traitobjptr";
                        break;
                    }
                    m_of << "(&"; emit_lvalue(val); m_of << ", "; emit_lvalue(base_ptr); m_of << ".META)";
                    special = true;
                }
            }
            else {
            }

            // NOTE: If disallow_empty_structs is set, structs don't include ZST fields
            // In this case, we need to avoid mentioning the removed fields
            if( !special && m_options.disallow_empty_structs && val.is_Field() && this->type_is_bad_zst(ty) )
            {
                // Work backwards to the first non-ZST field
                auto val_fp = ::MIR::LValue::CRef(val);
                assert(val_fp.is_Field());
                while( val_fp.inner_ref().is_Field() )
                {
                    ::HIR::TypeRef  tmp;
                    const auto& ty = mir_res.get_lvalue_type(tmp, val_fp.inner_ref());
                    if( !this->type_is_bad_zst(ty) )
                        break;
                    val_fp.try_unwrap();
                }
                assert(val_fp.is_Field());
                // Here, we have `val_fp` be a LValue::Field that refers to a ZST, but the inner of the field points to a non-ZST or a local

                // If the index is zero, then the best option is to borrow the source
                auto field_inner = val_fp.inner_ref();
                if( field_inner.is_Downcast() )
                {
                    m_of << "(void*)& "; emit_lvalue(field_inner.inner_ref());
                }
                else if( val_fp.as_Field() == 0 )
                {
                    m_of << "(void*)& "; emit_lvalue(field_inner);
                }
                else
                {
                    ::HIR::TypeRef  tmp;
                    struct H {
                        static size_t get_field_count(const ::MIR::TypeResolve& mir_res, const HIR::TypeRef& ty) {
                            TU_MATCH_HDRA( (ty.data()), { )
                            default:
                                break;
                            TU_ARMA(Path, te) {
                                TU_MATCH_HDRA( (te.binding), {)
                                default:
                                    break;
                                TU_ARMA(Struct, pbe) {
                                    TU_MATCH_HDRA( (pbe->m_data), {)
                                    TU_ARMA(Unit, sd)
                                        return 0;
                                    TU_ARMA(Tuple, sd)
                                        return sd.size();
                                    TU_ARMA(Named, sd)
                                        return sd.size();
                                    }
                                    }
                                }
                                }
                            TU_ARMA(Tuple, te)
                                return te.size();
                            TU_ARMA(Array, te)
                                return 0;
                            TU_ARMA(Slice, te)
                                return 0;
                            }
                            MIR_BUG(mir_res, "Field access on unexpected type: " << ty);
                        }
                    };
                    // Get the number of fields in parent
                    auto* repr = Target_GetTypeRepr(sp, m_resolve, mir_res.get_lvalue_type(tmp, field_inner));
                    assert(repr);
                    size_t n_parent_fields = repr->fields.size();
                    // Find next non-zero field
                    auto tmp_lv = ::MIR::LValue::new_Field( field_inner.clone(), val_fp.as_Field() + 1 );
                    bool found = false;
                    while(tmp_lv.as_Field() < n_parent_fields)
                    {
                        auto idx = tmp_lv.as_Field();
                        const auto& ty = repr->fields[idx].ty;
                        if( ty.data().is_Path() && ty.data().as_Path().binding.is_ExternType() ) {
                            // Extern types aren't emitted
                        }
                        else if( this->type_is_bad_zst(ty) ) {
                            // ZSTs are't either
                        }
                        else {
                            found = true;
                            break;
                        }
                        tmp_lv.m_wrappers.back() = ::MIR::LValue::Wrapper::new_Field(idx + 1);
                    }

                    // If no non-zero fields were found before the end, then do pointer manipulation using the repr
                    if( !found )
                    {
                        m_of << "(void*)( (uint8_t*)& "; emit_lvalue(field_inner); m_of << " + " << repr->fields[val_fp.as_Field()].offset << ") /*ZST*/";
                    }
                    // Otherwise, use the next non-zero field
                    else
                    {
                        m_of << "(void*)( &"; emit_lvalue(tmp_lv); m_of << ") /*ZST*/";
                    }
                }
                special = true;
            }

            if( !special )
            {
                m_of << "& "; emit_lvalue(val);
            }
        }

        void emit_composite_assign(
            const ::MIR::TypeResolve& mir_res, ::std::function<void()> emit_slot,
            const ::std::vector<::MIR::Param>& vals,
            unsigned indent_level, bool prepend_newline=true
            )
        {
            auto indent = RepeatLitStr { "\t", static_cast<int>(indent_level) };
            bool has_emitted = prepend_newline;
            for(unsigned int j = 0; j < vals.size(); j ++)
            {
                if( m_options.disallow_empty_structs )
                {
                    ::HIR::TypeRef  tmp;
                    const auto& ty = mir_res.get_param_type(tmp, vals[j]);

                    // Don't emit assignment of PhantomData
                    if( vals[j].is_LValue() && m_resolve.is_type_phantom_data(ty) )
                    {
                        continue ;
                    }

                    // Or ZSTs
                    if( this->type_is_bad_zst(ty) )
                    {
                        continue ;
                    }
                }

                if(has_emitted) {
                    m_of << ";\n" << indent;
                }
                has_emitted = true;

                emit_slot();
                m_of << "._" << j << " = ";
                emit_param(vals[j]);
            }
        }

        void emit_statement(const ::MIR::TypeResolve& mir_res, const ::MIR::Statement& stmt, unsigned indent_level=1)
        {
            DEBUG(stmt);
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
                        emit_box_drop(1, *ity, ty, e.slot, /*run_destructor=*/false);
                    }
                    else
                    {
                        MIR_BUG(mir_res, "Shallow drop on non-Box - " << ty);
                    }
                    break;
                case ::MIR::eDropKind::DEEP: {
                    // TODO: Determine if the lvalue is an owned pointer (i.e. it's via a `&move`)
                    bool unsized_valid = false;
                    unsized_valid = true;
                    emit_destructor_call(e.slot, ty, unsized_valid, indent_level + (e.flag_idx != ~0u ? 1 : 0));
                    break; }
                }
                if( e.flag_idx != ~0u )
                    m_of << indent << "}\n";
                m_of << indent << "// ^ " << stmt << "\n";
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
            case ::MIR::Statement::TAG_Asm2:
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    this->emit_asm2_gcc(mir_res, stmt, indent_level);
                    break;
                case Compiler::Msvc:
                    this->emit_asm2_msvc(mir_res, stmt, indent_level);
                    break;
                }
                break;
            case ::MIR::Statement::TAG_Assign: {
                const auto& e = stmt.as_Assign();
                DEBUG("- " << e.dst << " = " << e.src);
                m_of << indent;

                ::HIR::TypeRef  tmp;
                const auto& ty = mir_res.get_lvalue_type(tmp, e.dst);
                if( /*(e.dst.is_Deref() || e.dst.is_Field()) &&*/ this->type_is_bad_zst(ty) )
                {
                    m_of << "/* ZST assign */\n";
                    break;
                }

                TU_MATCH_HDRA( (e.src), {)
                TU_ARMA(Use, ve) {
                    ::HIR::TypeRef  tmp;
                    const auto& ty = mir_res.get_lvalue_type(tmp, ve);
                    if( ty == ::HIR::TypeRef::new_diverge() ) {
                        m_of << "abort()";
                        break;
                    }

                    if( ve.is_Field() && this->type_is_bad_zst(ty) )
                    {
                        m_of << "/* ZST field */";
                        break;
                    }

                    emit_lvalue(e.dst);
                    m_of << " = ";
                    emit_lvalue(ve);
                    }
                TU_ARMA(Constant, ve) {
                    emit_lvalue(e.dst);
                    m_of << " = ";
                    emit_constant(ve, &e.dst);
                    }
                TU_ARMA(SizedArray, ve) {
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
                    }
                TU_ARMA(Borrow, ve) {
                    emit_lvalue(e.dst);
                    m_of << " = ";
                    emit_borrow(mir_res, ve.type, ve.val);
                    }
                TU_ARMA(Cast, ve) {
                    emit_rvalue_cast(mir_res, e.dst, ve);
                    }
                TU_ARMA(BinOp, ve) {
                    emit_lvalue(e.dst);
                    m_of << " = ";
                    ::HIR::TypeRef  tmp, tmp_r;
                    const auto& ty = mir_res.get_param_type(tmp, ve.val_l);
                    const auto& ty_r = mir_res.get_param_type(tmp_r, ve.val_r);
                    if( ty.data().is_Borrow() ) {
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
                    else if( const auto* te = ty.data().opt_Pointer() ) {
                        if( is_dst(te->inner) )
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
                    }
                TU_ARMA(UniOp, ve) {
                    ::HIR::TypeRef  tmp;
                    const auto& ty = mir_res.get_lvalue_type(tmp, e.dst);

                    if( type_is_emulated_i128(ty) )
                    {
                        switch (ve.op)
                        {
                        case ::MIR::eUniOp::NEG:
                            emit_lvalue(e.dst); m_of << " = neg128s("; emit_lvalue(ve.val); m_of << ")";
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
                    }
                TU_ARMA(DstMeta, ve) {
                    emit_lvalue(e.dst);
                    m_of << " = ";
                    emit_lvalue(ve.val);
                    m_of << ".META";
                    }
                TU_ARMA(DstPtr, ve) {
                    emit_lvalue(e.dst);
                    m_of << " = ";
                    emit_lvalue(ve.val);
                    m_of << ".PTR";
                    }
                TU_ARMA(MakeDst, ve) {
                    emit_lvalue(e.dst);
                    m_of << " = ";
                    auto meta = metadata_type(ty.data().is_Pointer() ? ty.data().as_Pointer().inner : ty.data().as_Borrow().inner);
                    switch(meta)
                    {
                    case MetadataType::Slice:
                        m_of << "make_sliceptr";
                        break;
                    case MetadataType::TraitObject:
                        m_of << "make_traitobjptr";
                        break;
                    case MetadataType::Zero:
                    case MetadataType::Unknown:
                    case MetadataType::None:
                        MIR_BUG(mir_res, "MakeDst on type without metadata");
                    }
                    m_of << "("; emit_param(ve.ptr_val); m_of << ", "; emit_param(ve.meta_val); m_of << ")";
                    }
                TU_ARMA(Tuple, ve) {
                    emit_composite_assign(mir_res, [&](){  emit_lvalue(e.dst); }, ve.vals, indent_level);
                    }
                TU_ARMA(Array, ve) {
                    for(unsigned int j = 0; j < ve.vals.size(); j ++) {
                        if( j != 0 )    m_of << ";\n" << indent;
                        emit_lvalue(e.dst); m_of << ".DATA[" << j << "] = ";
                        emit_param(ve.vals[j]);
                    }
                    }
                TU_ARMA(UnionVariant, ve) {
                    MIR_ASSERT(mir_res, m_crate.get_typeitem_by_path(sp, ve.path.m_path).is_Union(), "");
                    emit_lvalue(e.dst);
                    m_of << ".var_" << ve.index << " = "; emit_param(ve.val);
                    }
                TU_ARMA(EnumVariant, ve) {
                    const auto& tyi = m_crate.get_typeitem_by_path(sp, ve.path.m_path);
                    MIR_ASSERT(mir_res, tyi.is_Enum(), "");
                    const auto* enm_p = &tyi.as_Enum();

                    ::HIR::TypeRef  tmp;
                    const auto& ty = mir_res.get_lvalue_type(tmp, e.dst);
                    auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);

                    TU_MATCH_HDRA( (repr->variants), {)
                    TU_ARMA(None, re) {
                        emit_composite_assign(mir_res, [&](){ emit_lvalue(e.dst); m_of << ".DATA.var_0"; }, /*repr->fields[0].ty,*/ ve.vals, indent_level);
                        }
                    TU_ARMA(NonZero, re) {
                        MIR_ASSERT(*m_mir_res, ve.index < 2, "");
                        if( ve.index == re.zero_variant ) {
                            // TODO: Use nonzero_path
                            m_of << "memset(&"; emit_lvalue(e.dst); m_of << ", 0, sizeof("; emit_ctype(ty); m_of << "))";
                        }
                        else {
                            emit_composite_assign(mir_res, [&](){ emit_lvalue(e.dst); m_of << ".DATA.var_" << ve.index; }, /*repr->fields[0].ty,*/ ve.vals, indent_level, /*prepend_newline=*/false);
                        }
                        }
                    TU_ARMA(Linear, re) {
                        bool emit_newline = false;
                        if( !re.is_niche(ve.index) )
                        {
                            emit_lvalue(e.dst); emit_enum_path(repr, re.field); m_of << " = " << (re.offset + ve.index);
                            emit_newline = true;
                        }
                        else {
                            m_of << "/* Niche tag */";
                        }
                        if( enm_p->is_value() )
                        {
                            // Value enums have no data fields
                        }
                        else
                        {
                            emit_composite_assign(mir_res, [&](){ emit_lvalue(e.dst); m_of << ".DATA.var_" << ve.index; }, ve.vals, indent_level, emit_newline);
                        }
                        }
                    TU_ARMA(Values, re) {
                        emit_lvalue(e.dst); m_of << ".TAG = "; emit_enum_variant_val(repr, ve.index);
                        if( !enm_p->is_value() )
                        {
                            emit_composite_assign(mir_res, [&](){ emit_lvalue(e.dst); m_of << ".DATA.var_" << ve.index; }, ve.vals, indent_level, true);
                        }
                        }
                    }
                    }
                TU_ARMA(Struct, ve) {
                    if( ve.vals.empty() )
                    {
                        if( m_options.disallow_empty_structs )
                        {
                            emit_lvalue(e.dst);
                            m_of << "._d = 0";
                        }
                    }
                    else
                    {
                        emit_composite_assign(mir_res, [&](){ emit_lvalue(e.dst); }, ve.vals, indent_level, /*emit_newline=*/false);
                    }
                    }
                }
                m_of << ";";
                m_of << "\t// " << e.dst << " = " << e.src;
                m_of << "\n";
                break; }
            }
        }
        void emit_rvalue_cast(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& dst, const ::MIR::RValue::Data_Cast& ve)
        {
            if (m_resolve.is_type_phantom_data(ve.type)) {
                m_of << "/* PhantomData cast */\n";
                return;
            }

            ::HIR::TypeRef  tmp;
            const auto& ty = mir_res.get_lvalue_type(tmp, ve.val);

            // A cast to a fat pointer doesn't actually change the C type.
            if ((ve.type.data().is_Pointer() && is_dst(ve.type.data().as_Pointer().inner))
                || (ve.type.data().is_Borrow() && is_dst(ve.type.data().as_Borrow().inner))
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
                MIR_ASSERT(mir_res, ve.type.data().is_Primitive(), "i128/u128 cast to non-primitive");
                MIR_ASSERT(mir_res, ty.data().is_Primitive(), "i128/u128 cast from non-primitive");
                switch (ve.type.data().as_Primitive())
                {
                case ::HIR::CoreType::I128:
                case ::HIR::CoreType::U128:
                    if (ty == ::HIR::CoreType::I128 || ty == ::HIR::CoreType::U128) {
                        // Cast between i128 and u128
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
                        // Cast from small to i128/u128
                        emit_lvalue(dst);
                        m_of << ".lo = ";
                        emit_lvalue(ve.val);
                        m_of << "; ";
                        emit_lvalue(dst);
                        m_of << ".hi = ";
                        emit_lvalue(ve.val);
                        m_of << " < 0 ? -1 : 0";
                    }
                    break;
                case ::HIR::CoreType::I8:
                case ::HIR::CoreType::I16:
                case ::HIR::CoreType::I32:
                case ::HIR::CoreType::I64:
                case ::HIR::CoreType::Isize:
                case ::HIR::CoreType::U8:
                case ::HIR::CoreType::U16:
                case ::HIR::CoreType::U32:
                case ::HIR::CoreType::U64:
                case ::HIR::CoreType::Usize:
                    emit_lvalue(dst);
                    m_of << " = ";
                    switch (ty.data().as_Primitive())
                    {
                    case ::HIR::CoreType::U128:
                    case ::HIR::CoreType::I128:
                        emit_lvalue(ve.val);
                        m_of << ".lo";
                        break;
                    default:
                        MIR_BUG(mir_res, "Unreachable");
                    }
                    break;
                case ::HIR::CoreType::F32:
                    emit_lvalue(dst);
                    m_of << " = ";
                    switch (ty.data().as_Primitive())
                    {
                    case ::HIR::CoreType::U128:
                        m_of << "cast128_float("; emit_lvalue(ve.val); m_of << ")";
                        break;
                    case ::HIR::CoreType::I128:
                        m_of << "cast128s_float("; emit_lvalue(ve.val); m_of << ")";
                        break;
                    default:
                        MIR_BUG(mir_res, "Unreachable");
                    }
                    break;
                case ::HIR::CoreType::F64:
                    emit_lvalue(dst);
                    m_of << " = ";
                    switch (ty.data().as_Primitive())
                    {
                    case ::HIR::CoreType::U128:
                        m_of << "cast128_double("; emit_lvalue(ve.val); m_of << ")";
                        break;
                    case ::HIR::CoreType::I128:
                        m_of << "cast128s_double("; emit_lvalue(ve.val); m_of << ")";
                        break;
                    default:
                        MIR_BUG(mir_res, "Unreachable");
                    }
                    break;
                default:
                    MIR_BUG(mir_res, "Bad i128/u128 cast - " << ty << " to " << ve.type);
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
            if (ve.type.data().is_Pointer() && !is_dst(ve.type.data().as_Pointer().inner))
            {
                // NOTE: Checks the result of the deref
                if ((ty.data().is_Borrow() && is_dst(ty.data().as_Borrow().inner))
                    || (ty.data().is_Pointer() && is_dst(ty.data().as_Pointer().inner))
                    )
                {
                    emit_lvalue(ve.val);
                    m_of << ".PTR";
                    special = true;
                }
            }
            if (ve.type.data().is_Primitive() && ty.data().is_Path() && ty.data().as_Path().binding.is_Enum())
            {
                emit_lvalue(ve.val);
                // NOTE: Embedded tag enums can't be cast
                m_of << ".TAG";
                special = true;
            }
            if (!special)
            {
                emit_lvalue(ve.val);
            }
        }
        void emit_term_switch(const ::MIR::TypeResolve& mir_res, const ::MIR::LValue& val, size_t n_arms, unsigned indent_level, ::std::function<void(size_t)> cb, size_t odd_arm=-1)
        {
            auto indent = RepeatLitStr { "\t", static_cast<int>(indent_level) };

            ::HIR::TypeRef  tmp;
            const auto& ty = mir_res.get_lvalue_type(tmp, val);
            MIR_ASSERT(mir_res, ty.data().is_Path(), "Switch over non-Path type");
            MIR_ASSERT(mir_res, ty.data().as_Path().binding.is_Enum(), "Switch over non-enum");
            const auto* repr = Target_GetTypeRepr(mir_res.sp, m_resolve, ty);
            MIR_ASSERT(mir_res, repr, "No repr for " << ty);

            struct MaybeSigned64 {
                bool    is_signed;
                uint64_t    v;

                MaybeSigned64(bool is_signed, uint64_t v)
                    :is_signed(is_signed)
                    ,v(v)
                {
                }

                void fmt(std::ostream& os) const {
                    if( is_signed ) {
                        os << static_cast<int64_t>(v);
                    }
                    else {
                        os << v;
                    }
                }
                //friend std::ostream& operator<<(std::ostream& os, const MaybeSigned64& x) {
                //    x.fmt(os);
                //    return os;
                //}
            };

            TU_MATCH_HDRA( (repr->variants), {)
            TU_ARMA(NonZero, e) {
                MIR_ASSERT(mir_res, n_arms == 2, "NonZero optimised switch without two arms");
                // If this is an emulated i128, check both fields
                m_of << indent << "if( "; emit_lvalue(val);
                const auto& slot_ty = emit_enum_path(repr, e.field);
                if(type_is_emulated_i128(slot_ty)) {
                    m_of << ".lo == 0 && ";
                    emit_lvalue(val); emit_enum_path(repr, e.field);
                    m_of << ".hi";
                }
                m_of << " != 0 )\n";
                m_of << indent << "\t";
                cb(1 - e.zero_variant);
                m_of << "\n";
                m_of << indent << "else\n";
                m_of << indent << "\t";
                cb(e.zero_variant);
                m_of << "\n";
                }
            TU_ARMA(Linear, e) {
                const auto& tag_ty = Target_GetInnerType(sp, m_resolve, *repr, e.field.index, e.field.sub_fields);
                switch(tag_ty.data().as_Primitive())
                {
                case ::HIR::CoreType::Bool:
                case ::HIR::CoreType::U8:   case ::HIR::CoreType::I8:
                case ::HIR::CoreType::U16:  case ::HIR::CoreType::I16:
                case ::HIR::CoreType::U32:  case ::HIR::CoreType::I32:
                case ::HIR::CoreType::U64:  case ::HIR::CoreType::I64:
                case ::HIR::CoreType::Usize:case ::HIR::CoreType::Isize:
                case ::HIR::CoreType::Char:
                    break;
                default:
                    MIR_BUG(mir_res, "Invalid tag type?! " << tag_ty);
                }


                // Optimisation: If there's only one arm with a different value, then emit an `if` isntead of a `switch`
                if( odd_arm != static_cast<size_t>(-1) )
                {
                    m_of << indent << "if( "; emit_lvalue(val); emit_enum_path(repr, e.field);
                    if( e.is_niche(odd_arm) ) {
                        m_of << " < " << e.offset;
                    }
                    else {
                        m_of << " == " << (e.offset + odd_arm);
                    }
                    m_of << ") {"; cb(odd_arm); m_of << "} else {"; cb(odd_arm == 0 ? 1 : 0); m_of << "}\n";
                }
                else
                {
                    m_of << indent << "switch("; emit_lvalue(val); emit_enum_path(repr, e.field); m_of << ") {\n";
                    for(size_t j = 0; j < n_arms; j ++)
                    {
                        if( e.is_niche(j) ) {
                            continue ;
                        }
                        // Handle signed values
                        m_of << indent << "case " << (e.offset + j) << ": ";
                        cb(j);
                        m_of << "break;\n";
                    }
                    m_of << indent << "default: ";
                    if( e.uses_niche() ) {
                        cb( e.field.index );
                        m_of << "break;";
                    }
                    else {
                        m_of << "abort();";
                    }
                    m_of << "\n";
                    m_of << indent << "}\n";
                }
                }
            TU_ARMA(Values, e) {
                const auto& tag_ty = Target_GetInnerType(sp, m_resolve, *repr, e.field.index, e.field.sub_fields);
                bool is_signed = false;
                switch(tag_ty.data().as_Primitive())
                {
                case ::HIR::CoreType::I8:
                case ::HIR::CoreType::I16:
                case ::HIR::CoreType::I32:
                case ::HIR::CoreType::I64:
                case ::HIR::CoreType::Isize:
                    is_signed = true;
                    break;
                case ::HIR::CoreType::Bool:
                case ::HIR::CoreType::U8:
                case ::HIR::CoreType::U16:
                case ::HIR::CoreType::U32:
                case ::HIR::CoreType::U64:
                case ::HIR::CoreType::Usize:
                case ::HIR::CoreType::Char:
                    is_signed = false;
                    break;
                case ::HIR::CoreType::I128: // TODO: Emulation
                case ::HIR::CoreType::U128: // TODO: Emulation
                    break;
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    MIR_TODO(mir_res, "Floating point enum tag.");
                    break;
                case ::HIR::CoreType::Str:
                    MIR_BUG(mir_res, "Unsized tag?!");
                }

                // Optimisation: If there's only one arm with a different value, then emit an `if` isntead of a `switch`
                if( odd_arm != static_cast<size_t>(-1) )
                {
                    m_of << indent << "if("; emit_lvalue(val); m_of << ".TAG == ";
                    // Handle signed values
                    if( is_signed ) {
                        m_of << static_cast<int64_t>(e.values[odd_arm]);
                    }
                    else {
                        m_of << e.values[odd_arm];
                    }
                    m_of << ") {"; cb(odd_arm); m_of << "} else {"; cb(odd_arm == 0 ? 1 : 0); m_of << "}\n";
                    return ;
                }

                m_of << indent << "switch("; emit_lvalue(val); m_of << ".TAG) {\n";
                for(size_t j = 0; j < n_arms; j ++)
                {
                    // Handle signed values
                    if( is_signed ) {
                        m_of << indent << "case " << static_cast<int64_t>(e.values[j]) << ": ";
                    }
                    else {
                        m_of << indent << "case " << e.values[j] << ": ";
                    }
                    cb(j);
                    m_of << "break;\n";
                }
                m_of << indent << "default: abort();\n";
                m_of << indent << "}\n";
                }
            TU_ARMA(None, e) {
                m_of << indent; cb(0); m_of << "\n";
                }
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

            bool has_zst = false;
            for(unsigned int j = 0; j < e.args.size(); j ++) {
                ::HIR::TypeRef tmp;
                const auto& ty = m_mir_res->get_param_type(tmp, e.args[j]);
                if( m_options.disallow_empty_structs /*&& TU_TEST1(e.args[j], LValue, .is_Field())*/ )
                {
                    if( this->type_is_bad_zst(ty) )
                    {
                        if(!has_zst) {
                            m_of << "{\n";
                            indent.n ++ ;
                            m_of << indent;
                            has_zst = true;
                        }
                        emit_ctype(ty, FMT_CB(ss, ss << "zarg" << j;));
                        m_of << " = {0};\n";
                        m_of << indent;
                        continue;
                    }
                }
                if( e.args[j].is_Constant() && this->type_is_high_align(ty) )
                {
                    if(!has_zst) {
                        m_of << "{\n";
                        indent.n ++ ;
                        m_of << indent;
                        has_zst = true;
                    }

                    emit_ctype(ty, FMT_CB(ss, ss << "haarg" << j;));
                    m_of << " = ";
                    emit_param(e.args[j]);
                    m_of << ";\n";
                    m_of << indent;
                    continue;
                }
            }

            bool omit_assign = false;

            // If the return type is `()`, omit the assignment (all `()` returning functions are marked as returning
            // void)
            {
                ::HIR::TypeRef  tmp;
                if( m_mir_res->get_lvalue_type(tmp, e.ret_val) == ::HIR::TypeRef::new_unit() )
                {
                    omit_assign = true;
                }

                if( this->type_is_bad_zst( m_mir_res->get_lvalue_type(tmp, e.ret_val) ) )
                {
                    omit_assign = true;
                }
            }

            TU_MATCH_HDRA( (e.fcn), {)
            TU_ARMA(Value, e2) {
                {
                    ::HIR::TypeRef  tmp;
                    const auto& ty = mir_res.get_lvalue_type(tmp, e2);
                    MIR_ASSERT(mir_res, ty.data().is_Function(), "Call::Value on non-function - " << ty);

                    const auto& ret_ty = ty.data().as_Function().m_rettype;
                    omit_assign |= ret_ty.data().is_Diverge();
                    if( !omit_assign )
                    {
                        emit_lvalue(e.ret_val); m_of << " = ";
                    }
                }
                m_of << "("; emit_lvalue(e2); m_of << ")";
                }
            TU_ARMA(Path, e2) {
                {
                    TU_MATCH_HDRA( (e2.m_data), {)
                    TU_ARMA(Generic, pe) {
                        const auto& fcn = m_crate.get_function_by_path(sp, pe.m_path);
                        omit_assign |= fcn.m_return.data().is_Diverge();
                        // TODO: Monomorph.
                        }
                    TU_ARMA(UfcsUnknown, pe) {
                        }
                    TU_ARMA(UfcsInherent, pe) {
                        // Check if the return type is !
                        omit_assign |= m_resolve.m_crate.find_type_impls(pe.type, [&](const auto& ty)->const auto& { return ty; },
                            [&](const auto& impl) {
                                // Associated functions
                                {
                                    auto it = impl.m_methods.find(pe.item);
                                    if( it != impl.m_methods.end() ) {
                                        return it->second.data.m_return.data().is_Diverge();
                                    }
                                }
                                // Associated static (undef)
                                return false;
                            });
                        }
                    TU_ARMA(UfcsKnown, pe) {
                        // Check if the return type is !
                        const auto& tr = m_resolve.m_crate.get_trait_by_path(sp, pe.trait.m_path);
                        const auto& fcn = tr.m_values.find(pe.item)->second.as_Function();
                        const auto& rv_tpl = fcn.m_return;
                        if( rv_tpl.data().is_Diverge() || rv_tpl == ::HIR::TypeRef::new_unit() )
                        {
                            omit_assign |= true;
                        }
                        else if( const auto* te = rv_tpl.data().opt_Generic() )
                        {
                            (void)te;
                            // TODO: Generic lookup
                        }
                        else if( const auto* te = rv_tpl.data().opt_Path() )
                        {
                            if( te->binding.is_Opaque() ) {
                                // TODO: Associated type lookup
                            }
                        }
                        else
                        {
                            // Not a ! type
                        }
                        }
                    }
                    if(!omit_assign)
                    {
                        emit_lvalue(e.ret_val); m_of << " = ";
                    }
                }
                m_of << Trans_Mangle(e2);
                }
            TU_ARMA(Intrinsic, e2) {
                const auto& name = e2.name;
                const auto& params = e2.params;
                emit_intrinsic_call(name, params, e);
                if( has_zst )
                {
                    indent.n --;
                    m_of << indent << "}\n";
                }
                return ;
                }
            }
            m_of << "(";
            for(unsigned int j = 0; j < e.args.size(); j ++) {
                if(j != 0)  m_of << ",";
                m_of << " ";
                ::HIR::TypeRef tmp;
                const auto& ty = m_mir_res->get_param_type(tmp, e.args[j]);

                if( this->type_is_high_align(ty) )
                {
                    m_of << "&";
                    if( e.args[j].is_Constant() ) {
                        m_of << "haarg" << j;
                        continue;
                    }
                }
                if( this->type_is_bad_zst(ty) )
                {
                    m_of << "zarg" << j;
                    continue;
                }
                emit_param(e.args[j]);
            }
            m_of << " );\n";

            if( has_zst )
            {
                indent.n --;
                m_of << indent << "}\n";
            }
        }

        bool asm_matches_template(const ::MIR::Statement::Data_Asm& e, const char* tpl, ::std::initializer_list<const char*> inputs, ::std::initializer_list<const char*> outputs)
        {
            struct H {
                static bool check_list(const std::vector<std::pair<std::string, MIR::LValue>>& have, const ::std::initializer_list<const char*>& exp)
                {
                    if( have.size() != exp.size() )
                        return false;
                    auto h_it = have.begin();
                    auto e_it = exp.begin();
                    for(; h_it != have.end(); ++ h_it, ++e_it)
                    {
                        if( h_it->first != *e_it )
                        {
                            return false;
                        }
                    }
                    return true;
                }
            };

            if( e.tpl == tpl )
            {
                if( !H::check_list(e.inputs, inputs) || !H::check_list(e.outputs, outputs) )
                {
                    MIR_BUG(*m_mir_res, "Hard-coded asm translation doesn't apply - `" << e.tpl << "` inputs=" << e.inputs << " outputs=" << e.outputs);
                }
                return true;
            }
            return false;
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
                    else if (::std::strcmp(r, "{ebx}") == 0 || ::std::strcmp(r, "{rbx}") == 0) {
                        return "b";
                    }
                    else if (::std::strcmp(r, "{ecx}") == 0 || ::std::strcmp(r, "{rcx}") == 0) {
                        return "c";
                    }
                    else if (::std::strcmp(r, "{edx}") == 0 || ::std::strcmp(r, "{rdx}") == 0) {
                        return "d";
                    }
                    else {
                        return r;
                    }
                }
            };
            bool is_volatile = H::has_flag(e.flags, "volatile");
            bool is_intel = H::has_flag(e.flags, "intel");

            // The following clobber overlaps with an output
            // __asm__ ("cpuid": "=a" (var0), "=b" (var1), "=c" (var2), "=d" (var3): "a" (arg0), "c" (var4): "rbx");
            if( asm_matches_template(e, "cpuid", {"{eax}","{ecx}"}, {"={eax}", "={ebx}", "={ecx}", "={edx}"}) )
            {
                if( e.clobbers.size() == 1 && e.clobbers[0] == "rbx" ) {
                    m_of << indent << "__asm__(\"cpuid\"";
                    m_of << " : ";
                    m_of << "\"=a\" ("; emit_lvalue(e.outputs[0].second); m_of << "), ";
                    m_of << "\"=b\" ("; emit_lvalue(e.outputs[1].second); m_of << "), ";
                    m_of << "\"=c\" ("; emit_lvalue(e.outputs[2].second); m_of << "), ";
                    m_of << "\"=d\" ("; emit_lvalue(e.outputs[3].second); m_of << ")";
                    m_of << " : ";
                    m_of << "\"a\" ("; emit_lvalue(e.inputs[0].second); m_of << "), ";
                    m_of << "\"c\" ("; emit_lvalue(e.inputs[1].second); m_of << ")";
                    m_of << " );\n";
                    return ;
                }
            }
            if(asm_matches_template(e, "pushfd; popl $0", {}, {"=r"}))
            {
                m_of << indent << "__asm__ __volatile__ (\"pushfl; popl %0\" : \"=r\" ("; emit_lvalue(e.outputs[0].second); m_of << ") : : );\n";
                return;
            }
            if(asm_matches_template(e, "pushl $0; popfd", {"r"}, {}))
            {
                m_of << indent << "__asm__ __volatile__ (\"pushl %0; popfl\" : : \"r\" ("; emit_lvalue(e.inputs[0].second); m_of << ") : );\n";
                return;
            }

            m_of << indent << "__asm__ ";
            if (is_volatile) m_of << "__volatile__";
            m_of << "(\"" << (is_intel ? ".intel_syntax; " : "");
            // TODO: Use a more powerful parser
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
                else if (*it == '$' && isdigit(*(it + 1)) && *(it + 2) != 'x')
                    m_of << "%";
                // Hack for `${0:b}` seen with `setc`, just emit as `%0`
                else if( *it == '$' && *(it + 1) == '{') {
                    m_of << "%" << *(it + 2);
                    while(it != e.tpl.end() && *it != '}')
                        it ++;
                }
                else
                    m_of << *it;
            }
            m_of << (is_intel ? ".att_syntax; " : "") << "\"";
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
                m_of << "\" ("; emit_lvalue(v.second); m_of << ")";
            }
            m_of << ": ";
            for (unsigned int i = 0; i < e.inputs.size(); i++)
            {
                const auto& v = e.inputs[i];
                if (i != 0)    m_of << ", ";
                // TODO: If this is the same reg as an output, use the output index
                m_of << "\"" << H::convert_reg(v.first.c_str()) << "\" ("; emit_lvalue(v.second); m_of << ")";
            }
            m_of << ": ";
            for (unsigned int i = 0; i < e.clobbers.size(); i++)
            {
                if (i != 0)    m_of << ", ";
                if( e.tpl == "cpuid\n" && e.clobbers[i] == "rbx" ) {
                    continue;
                }
                m_of << "\"" << e.clobbers[i] << "\"";
            }
            m_of << ");\n";
        }
        void emit_asm_msvc(const ::MIR::TypeResolve& mir_res, const ::MIR::Statement::Data_Asm& e, unsigned indent_level)
        {
            auto indent = RepeatLitStr{ "\t", static_cast<int>(indent_level) };

            auto matches_template = [this,&e](const char* tpl, ::std::initializer_list<const char*> inputs, ::std::initializer_list<const char*> outputs)->bool {
                return this->asm_matches_template(e, tpl, inputs, outputs);
                };

            if( e.tpl == "" )
            {
                // Empty template, so nothing to do
                return ;
            }
            else if( matches_template("fnstcw $0", /*input=*/{}, /*output=*/{"*m"}) )
            {
                // HARD CODE: `fnstcw` -> _control87
                m_of << indent << "*("; emit_lvalue(e.outputs[0].second); m_of << ") = _control87(0,0);\n";
                return ;
            }
            else if( matches_template("fldcw $0", /*input=*/{"m"}, /*output=*/{}) )
            {
                // HARD CODE: `fldcw` -> _control87
                m_of << indent << "_control87("; emit_lvalue(e.inputs[0].second); m_of << ", 0xFFFF);\n";
                return ;
            }
            else if( matches_template("int $$0x29", /*input=*/{"{ecx}"}, /*output=*/{}) )
            {
                m_of << indent << "__fastfail("; emit_lvalue(e.inputs[0].second); m_of << ");\n";
                return ;
            }
            else if( matches_template("pause", /*input=*/{}, /*output=*/{}) )
            {
                m_of << indent << "_mm_pause();\n";
                return ;
            }
            else if(
                matches_template("cpuid\n", /*input=*/{"{eax}", "{ecx}"}, /*output=*/{"={eax}", "={ebx}", "={ecx}", "={edx}"})
                || matches_template("cpuid", /*input=*/{"{eax}", "{ecx}"}, /*output=*/{"={eax}", "={ebx}", "={ecx}", "={edx}"})
                )
            {
                m_of << indent << "{";
                m_of << " int cpuid_out[4];";
                m_of << " __cpuidex(cpuid_out, "; emit_lvalue(e.inputs[0].second); m_of << ", "; emit_lvalue(e.inputs[1].second); m_of << ");";
                m_of << " "; emit_lvalue(e.outputs[0].second); m_of << " = cpuid_out[0];";
                m_of << " "; emit_lvalue(e.outputs[1].second); m_of << " = cpuid_out[1];";
                m_of << " "; emit_lvalue(e.outputs[2].second); m_of << " = cpuid_out[2];";
                m_of << " "; emit_lvalue(e.outputs[3].second); m_of << " = cpuid_out[3];";
                m_of << " }\n";
                return ;
            }
            else if( matches_template("pushfq; popq $0", /*input=*/{}, /*output=*/{"=r"}) )
            {
                m_of << indent; emit_lvalue(e.outputs[0].second); m_of << " = __readeflags();\n";
                return ;
            }
            else if( matches_template("pushq $0; popfq", /*input=*/{"r"}, /*output=*/{}) )
            {
                m_of << indent << "__writeeflags("; emit_lvalue(e.inputs[0].second); m_of << ");\n";
                return ;
            }
            else if( matches_template("xgetbv", /*input=*/{"{ecx}"}, /*output=*/{"={eax}", "={edx}"}) )
            {
                m_of << indent << "{";
                m_of << " unsigned __int64 v = _xgetbv("; emit_lvalue(e.inputs[0].second); m_of << ");";
                m_of << " "; emit_lvalue(e.outputs[0].second); m_of << " = (uint32_t)(v & 0xFFFFFFFF);";
                m_of << " "; emit_lvalue(e.outputs[1].second); m_of << " = (uint32_t)(v >> 32);";
                m_of << " }\n";
                return ;
            }
            // parking_lot src/elision.rs
            else if( matches_template("xacquire; lock; cmpxchgl $2, $1", /*input=*/{"r", "{rax}"}, /*output=*/{"={rax}", "+*m"}) )
            {
                m_of << indent << "InterlockedCompareExchangeAcquire(";
                emit_lvalue(e.outputs[1].second); m_of << ",";
                emit_lvalue(e.inputs[0].second); m_of << ",";
                emit_lvalue(e.inputs[1].second);
                m_of << ");\n";
                return ;
            }
            else if( matches_template("xrelease; lock; cmpxchgl $2, $1", /*input=*/{"r", "{rax}"}, /*output=*/{"={rax}", "+*m"}) )
            {
                m_of << indent << "InterlockedCompareExchangeRelease(";
                emit_lvalue(e.outputs[1].second); m_of << ",";
                emit_lvalue(e.inputs[0].second); m_of << ",";
                emit_lvalue(e.inputs[1].second);
                m_of << ");\n";
                return ;
            }
            else if( matches_template("xacquire; lock; cmpxchgq $2, $1", /*input=*/{"r", "{rax}"}, /*output=*/{"={rax}", "+*m"}) )
            {
                m_of << indent << "InterlockedCompareExchangeAcquire64(";
                emit_lvalue(e.outputs[1].second); m_of << ",";
                emit_lvalue(e.inputs[0].second); m_of << ",";
                emit_lvalue(e.inputs[1].second);
                m_of << ");\n";
                return ;
            }
            else if( matches_template("xrelease; lock; cmpxchgq $2, $1", /*input=*/{"r", "{rax}"}, /*output=*/{"={rax}", "+*m"}) )
            {
                m_of << indent << "InterlockedCompareExchangeRelease64(";
                emit_lvalue(e.outputs[1].second); m_of << ",";
                emit_lvalue(e.inputs[0].second); m_of << ",";
                emit_lvalue(e.inputs[1].second);
                m_of << ");\n";
                return ;
            }
            else if( matches_template("xrelease; lock; xaddq $2, $1", /*input=*/{"0"}, /*output=*/{"=r", "+*m"}) )
            {
                m_of << indent;
                emit_lvalue(e.outputs[0].second); m_of << " = "; 
                m_of << "InterlockedExchangeAddRelease64(";
                emit_lvalue(e.outputs[1].second); m_of << ",";
                emit_lvalue(e.inputs[0].second);
                m_of << ");\n";
                return ;
            }
            else if( matches_template("btl $2, $1\n\tsetc ${0:b}", /*input=*/{"*m", "r"}, /*output=*/{"=r"}) )
            {
                m_of << indent; emit_lvalue(e.outputs[0].second); m_of << " = _bittest(";
                emit_lvalue(e.inputs[0].second); m_of << ",";
                emit_lvalue(e.inputs[1].second);
                m_of << ");\n";
                return;
            }
            else if( matches_template("btq $2, $1\n\tsetc ${0:b}", /*input=*/{"*m", "r"}, /*output=*/{"=r"}) )
            {
                m_of << indent; emit_lvalue(e.outputs[0].second); m_of << " = _bittest64(";
                emit_lvalue(e.inputs[0].second); m_of << ",";
                emit_lvalue(e.inputs[1].second);
                m_of << ");\n";
                return;
            }
            else if( matches_template("btsl $2, $1\n\tsetc ${0:b}", /*input=*/{"r"}, /*output=*/{"=r", "+*m"}) )
            {
                m_of << indent; emit_lvalue(e.outputs[0].second); m_of << " = _bittestandset(";
                emit_lvalue(e.outputs[1].second); m_of << ",";
                emit_lvalue(e.inputs[0].second);
                m_of << ");\n";
                return;
            }
            else if( matches_template("btsq $2, $1\n\tsetc ${0:b}", /*input=*/{"r"}, /*output=*/{"=r", "+*m"}) )
            {
                m_of << indent; emit_lvalue(e.outputs[0].second); m_of << " = _bittestandset64(";
                emit_lvalue(e.outputs[1].second); m_of << ",";
                emit_lvalue(e.inputs[0].second);
                m_of << ");\n";
                return;
            }
            else if( matches_template("btrl $2, $1\n\tsetc ${0:b}", /*input=*/{"r"}, /*output=*/{"=r", "+*m"}) )
            {
                m_of << indent; emit_lvalue(e.outputs[0].second); m_of << " = _bittestandreset(";
                emit_lvalue(e.outputs[1].second); m_of << ",";
                emit_lvalue(e.inputs[0].second);
                m_of << ");\n";
                return;
            }
            else if( matches_template("btrq $2, $1\n\tsetc ${0:b}", /*input=*/{"r"}, /*output=*/{"=r", "+*m"}) )
            {
                m_of << indent; emit_lvalue(e.outputs[0].second); m_of << " = _bittestandreset64(";
                emit_lvalue(e.outputs[1].second); m_of << ",";
                emit_lvalue(e.inputs[0].second);
                m_of << ");\n";
                return;
            }
            else if( matches_template("btcl $2, $1\n\tsetc ${0:b}", /*input=*/{"r"}, /*output=*/{"=r", "+*m"}) )
            {
                m_of << indent; emit_lvalue(e.outputs[0].second); m_of << " = _bittestandcomplement(";
                emit_lvalue(e.outputs[1].second); m_of << ",";
                emit_lvalue(e.inputs[0].second);
                m_of << ");\n";
                return;
            }
            else if( matches_template("btcq $2, $1\n\tsetc ${0:b}", /*input=*/{"r"}, /*output=*/{"=r", "+*m"}) )
            {
                m_of << indent; emit_lvalue(e.outputs[0].second); m_of << " = _bittestandcomplement64(";
                emit_lvalue(e.outputs[1].second); m_of << ",";
                emit_lvalue(e.inputs[0].second);
                m_of << ");\n";
                return;
            }
            else
            {
                // No hard-coded translations.
            }

            if( Target_GetCurSpec().m_backend_c.m_c_compiler == "amd64" ) {
                MIR_TODO(mir_res, "MSVC amd64 doesn't support inline assembly, need to have a transform for \"" << FmtEscaped(e.tpl) << "\" inputs=" << e.inputs << " outputs=" << e.outputs);
            }
            if( !e.inputs.empty() || !e.outputs.empty() )
            {
                MIR_TODO(mir_res, "Inputs/outputs in msvc inline assembly - `" << FmtEscaped(e.tpl) << "` inputs=" << e.inputs << " outputs=" << e.outputs);
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

        struct Asm2TplMatch {
            const MIR::TypeResolve& m_mir_res;
            const ::MIR::Statement& stmt;
            const ::MIR::Statement::Data_Asm2& e;
            std::vector<std::string>    fmt_lines;
            std::vector<std::string>    fmt_params;

            Asm2TplMatch(const MIR::TypeResolve& mir_res, const ::MIR::Statement& stmt)
                : m_mir_res(mir_res)
                , stmt(stmt)
                , e(stmt.as_Asm2())
            {
                for(const auto& v : e.lines) {
                    fmt_lines.push_back(FMT(FMT_CB(os, v.fmt(os))));
                    fmt_lines.back().erase(fmt_lines.back().begin());
                    fmt_lines.back().pop_back();
                    DEBUG(fmt_lines.back());
                }

                for(const auto& p : e.params) {
                    fmt_params.push_back(get_param_text(p));
                }
            }
            bool matches_template(::std::initializer_list<const char*> lines, ::std::initializer_list<const char*> params) const
            {
                if( !check_list(fmt_lines, lines) )
                    return false;

                if( !check_list(fmt_params, params) ) {
                    MIR_BUG(m_mir_res, "Hard-coded asm translation doesn't apply - " << stmt << "\n"
                        << "[" << fmt_params << "] != \n[" << FMT_CB(os, for(auto it = params.begin(); it != params.end(); ++it) os << *it << ", ") << "]");
                }

                return true;
            }

            const MIR::AsmParam& p(size_t i) const {
                return e.params.at(i);
            }
            const MIR::Param& input(size_t i) const {
                MIR_ASSERT(m_mir_res, e.params.at(i).as_Reg().input, "Parameter " << i << " isn't a register input");
                return *e.params.at(i).as_Reg().input;
            }
            const MIR::LValue& output(size_t i) const {
                MIR_ASSERT(m_mir_res, e.params.at(i).as_Reg().output, "Parameter " << i << " isn't a register output");
                return *e.params.at(i).as_Reg().output;
            }

        private:
            /// Get a description of the parameter's important attributes
            static std::string get_param_text(const MIR::AsmParam& p) {
                TU_MATCH_HDRA( (p), {)
                TU_ARMA(Reg, e) {
                    TU_MATCH_HDRA( (e.spec), { )
                    TU_ARMA(Explicit, n) {
                        return FMT(get_dir_text(e.dir) << "=" << n);
                        }
                    TU_ARMA(Class, c) {
                        return FMT(get_dir_text(e.dir) << ":" << AsmCommon::to_string(c));
                        }
                    }
                    }
                TU_ARMA(Const, e)
                    return "const";
                TU_ARMA(Sym, e)
                    return "sym";
                }
                throw "";
            }
            static const char* get_dir_text(const AsmCommon::Direction& d) {
                switch(d)
                {
                case AsmCommon::Direction::In:  return "in";
                case AsmCommon::Direction::Out:  return "out";
                case AsmCommon::Direction::InOut:  return "inout";
                case AsmCommon::Direction::LateOut:  return "lateout";
                case AsmCommon::Direction::InLateOut:  return "inlateout";
                }
                throw "";
            }
            static bool check_list(const std::vector<std::string>& have, const ::std::initializer_list<const char*>& exp) {
                if( have.size() != exp.size() ) {
                    return false;
                }
                auto h_it = have.begin();
                auto e_it = exp.begin();
                for(; h_it != have.end(); ++h_it, ++e_it)
                {
                    if( *h_it != *e_it ) {
                        return false;
                    }
                }
                return true;
            }
        };
        void emit_asm2_msvc(const ::MIR::TypeResolve& mir_res, const ::MIR::Statement& stmt, unsigned indent_level)
        {
            auto indent = RepeatLitStr{ "\t", static_cast<int>(indent_level) };
            Asm2TplMatch    m { mir_res, stmt };

            if( stmt.as_Asm2().lines.empty() ) 
            {
                // Ignore?
            }
            // === x86 intrinsics ===
            // - CPUID
            else if( m.matches_template({"movq %rbx, {0:r}", "cpuid", "xchgq %rbx, {0:r}"}, {"lateout:reg","inlateout=eax","inlateout=ecx","lateout=edx"}) )
            {
                m_of << indent << "{";
                m_of << " int cpuid_out[4];";
                m_of << " __cpuidex(cpuid_out, "; emit_param(m.input(1)); m_of << ", "; emit_param(m.input(2)); m_of << ");";
                m_of << " "; emit_lvalue(m.output(1)); m_of << " = cpuid_out[0];";  // EAX
                m_of << " "; emit_lvalue(m.output(0)); m_of << " = cpuid_out[1];";  // EBX
                m_of << " "; emit_lvalue(m.output(2)); m_of << " = cpuid_out[2];";  // ECX
                m_of << " "; emit_lvalue(m.output(3)); m_of << " = cpuid_out[3];";  // EDX
                m_of << " }\n";
            }
            // - EFlags
            else if( m.matches_template({"pushfq", "pop {0}"}, {"out:reg"}) )
            {
                m_of << indent; emit_lvalue(m.output(0)); m_of << " = __readeflags();\n";
            }
            else if( m.matches_template({"push {0}", "popfq"}, {"in:reg"}) )
            {
                m_of << indent << "__writeeflags("; emit_param(m.input(0)); m_of << ");\n";
            }
            // - Bit test (and *)
            else if( m.matches_template({"btl {1:e}, ({0})", "setc {2}"}, {"in:reg", "in:reg", "out:reg_byte"}) )
            {
                m_of << indent; emit_lvalue(m.output(2));
                m_of << " = _bittest("; emit_param(m.input(0)); m_of << ","; emit_param(m.input(1)); m_of << ");\n";
            }
            else if( m.matches_template({"btq {1}, ({0})", "setc {2}"}, {"in:reg", "in:reg", "out:reg_byte"}) )
            {
                m_of << indent; emit_lvalue(m.output(2));
                m_of << " = _bittest64("; emit_param(m.input(0)); m_of << ","; emit_param(m.input(1)); m_of << ");\n";
            }
            else if( m.matches_template({"btcl {1:e}, ({0})", "setc {2}"}, {"in:reg", "in:reg", "out:reg_byte"}) )
            {
                m_of << indent; emit_lvalue(m.output(2));
                m_of << " = _bittestandcomplement("; emit_param(m.input(0)); m_of << ","; emit_param(m.input(1)); m_of << ");\n";
            }
            else if( m.matches_template({"btcq {1}, ({0})", "setc {2}"}, {"in:reg", "in:reg", "out:reg_byte"}) )
            {
                m_of << indent; emit_lvalue(m.output(2));
                m_of << " = _bittestandcomplement64("; emit_param(m.input(0)); m_of << ","; emit_param(m.input(1)); m_of << ");\n";
            }
            else if( m.matches_template({"btrl {1:e}, ({0})", "setc {2}"}, {"in:reg", "in:reg", "out:reg_byte"}) )
            {
                m_of << indent; emit_lvalue(m.output(2));
                m_of << " = _bittestandreset("; emit_param(m.input(0)); m_of << ","; emit_param(m.input(1)); m_of << ");\n";
            }
            else if( m.matches_template({"btrq {1}, ({0})", "setc {2}"}, {"in:reg", "in:reg", "out:reg_byte"}) )
            {
                m_of << indent; emit_lvalue(m.output(2));
                m_of << " = _bittestandreset64("; emit_param(m.input(0)); m_of << ","; emit_param(m.input(1)); m_of << ");\n";
            }
            else if( m.matches_template({"btsl {1:e}, ({0})", "setc {2}"}, {"in:reg", "in:reg", "out:reg_byte"}) )
            {
                m_of << indent; emit_lvalue(m.output(2));
                m_of << " = _bittestandset("; emit_param(m.input(0)); m_of << ","; emit_param(m.input(1)); m_of << ");\n";
            }
            else if( m.matches_template({"btsq {1}, ({0})", "setc {2}"}, {"in:reg", "in:reg", "out:reg_byte"}) )
            {
                m_of << indent; emit_lvalue(m.output(2));
                m_of << " = _bittestandset64("; emit_param(m.input(0)); m_of << ","; emit_param(m.input(1)); m_of << ");\n";
            }
            // -- Windows intrinsics --
            else if( m.matches_template({"int $$0x29"}, {"in=ecx"}) )
            {
                m_of << indent << "__fastfail("; emit_param(m.input(0)); m_of << ");\n";
            }
            // -- Unknown --
            else
            {
                if( Target_GetCurSpec().m_backend_c.m_c_compiler == "amd64" ) {
                    MIR_TODO(mir_res, "MSVC amd64 doesn't support inline assembly, need to have a transform for " << stmt);
                }
                MIR_TODO(mir_res, "Translate to MSVC");
            }
        }
        void emit_asm2_gcc(const ::MIR::TypeResolve& mir_res, const ::MIR::Statement& stmt, unsigned indent_level)
        {
            auto indent = RepeatLitStr{ "\t", static_cast<int>(indent_level) };
            Asm2TplMatch    m { mir_res, stmt };
            const auto& se = stmt.as_Asm2();


            // The following clobber overlaps with an output
            // __asm__ ("cpuid": "=a" (var0), "=b" (var1), "=c" (var2), "=d" (var3): "a" (arg0), "c" (var4): "rbx");
            if( m.matches_template({"movq %rbx, {0:r}", "cpuid", "xchgq %rbx, {0:r}"}, {"lateout:reg", "inlateout=eax", "inlateout=ecx", "lateout=edx"}) )
            {
                //if( e.clobbers.size() == 1 && e.clobbers[0] == "rbx" ) {
                    m_of << indent << "__asm__(\"cpuid\"";
                    m_of << " : ";
                    m_of << "\"=a\" ("; emit_lvalue(m.output(1)); m_of << "), ";
                    m_of << "\"=b\" ("; emit_lvalue(m.output(0)); m_of << "), ";
                    m_of << "\"=c\" ("; emit_lvalue(m.output(2)); m_of << "), ";
                    m_of << "\"=d\" ("; emit_lvalue(m.output(3)); m_of << ")";
                    m_of << " : ";
                    m_of << "\"a\" ("; emit_param(m.input(1)); m_of << "), ";
                    m_of << "\"c\" ("; emit_param(m.input(2)); m_of << ")";
                    m_of << " );\n";
                    return ;
                //}
            }
            else if( m.matches_template({"btl {1:e}, ({0})", "setc {2}"}, {"in:reg", "in:reg", "out:reg_byte"}) )
            {
                m_of << indent << "__asm__(\"bt %1, (%2); setc %0\"";
                m_of << " : \"=r\"("; emit_lvalue(m.output(2)); m_of << ")";
                m_of << " : \"r\"("; emit_param(m.input(0));  m_of << "), \"r\"("; emit_param(m.input(1));  m_of << ")";
                m_of << ");\n";
                return;
            }
            else if( m.matches_template({"btcl {1:e}, ({0})", "setc {2}"}, {"in:reg", "in:reg", "out:reg_byte"}) )
            {
                m_of << indent << "__asm__(\"btc %1, (%2); setc %0\"";
                m_of << " : \"=r\"("; emit_lvalue(m.output(2)); m_of << ")";
                m_of << " : \"r\"("; emit_param(m.input(0));  m_of << "), \"r\"("; emit_param(m.input(1));  m_of << ")";
                m_of << ");\n";
                return;
            }
            else if( m.matches_template({"btrl {1:e}, ({0})", "setc {2}"}, {"in:reg", "in:reg", "out:reg_byte"}) )
            {
                m_of << indent << "__asm__(\"btr %1, (%2); setc %0\"";
                m_of << " : \"=r\"("; emit_lvalue(m.output(2)); m_of << ")";
                m_of << " : \"r\"("; emit_param(m.input(0));  m_of << "), \"r\"("; emit_param(m.input(1));  m_of << ")";
                m_of << ");\n";
                return;
            }
            else if( m.matches_template({"btsl {1:e}, ({0})", "setc {2}"}, {"in:reg", "in:reg", "out:reg_byte"}) )
            {
                m_of << indent << "__asm__(\"bts %1, (%2); setc %0\"";
                m_of << " : \"=r\"("; emit_lvalue(m.output(2)); m_of << ")";
                m_of << " : \"r\"("; emit_param(m.input(0));  m_of << "), \"r\"("; emit_param(m.input(1));  m_of << ")";
                m_of << ");\n";
                return;
            }
            else
            {
                std::vector<unsigned>   arg_mappings(se.params.size(), UINT_MAX);
                std::vector<const MIR::AsmParam::Data_Reg*>   outputs;
                // Outputs
                for(size_t i = 0; i < se.params.size(); i ++)
                {
                    if( const auto* pe = se.params[i].opt_Reg() ) {
                        if( pe->output )
                        {
                            arg_mappings[i] = outputs.size();
                            outputs.push_back(pe);
                        }
                    }
                }
                // Inputs
                std::vector<const MIR::AsmParam*>   inputs;
                for(size_t i = 0; i < se.params.size(); i ++)
                {
                    if( const auto* pe = se.params[i].opt_Reg() ) {
                        if( pe->input )
                        {
                            arg_mappings[i] = outputs.size() + inputs.size();
                            inputs.push_back(&se.params[i]);
                        }
                    }
                }
                // Clobbers
                std::vector<const char*>   clobbers;
                for(size_t i = 0; i < se.params.size(); i ++)
                {
                    // An explicit register, not "In" and output parameter
                }

                m_of << indent << "__asm__ ";
                m_of << "__volatile__"; // Default everything to volatile
                m_of << "(\".intel_syntax; ";
                for(const auto& l : se.lines)
                {
                    for(const auto& f : l.frags)
                    {
                        m_of << FmtEscaped(f.before);
                        //if( f.modifier != '\0' )
                        //    MIR_TODO(mir_res, "Asm2 GCC: modifier - " << stmt);
                        MIR_ASSERT(mir_res, arg_mappings.at(f.index) != UINT_MAX, stmt);
                        m_of << "%" << arg_mappings.at(f.index);
                    }
                    m_of << FmtEscaped(l.trailing);
                    m_of << ";\\n ";
                }
                m_of << ".att_syntax; \"";
                m_of << " :";
                for(size_t i = 0; i < outputs.size(); i ++)
                {
                    const auto& p = *outputs[i];
                    if(i != 0)  m_of << ",";
                    m_of << " ";
                    m_of << "\"";
                    TU_MATCH_HDRA((p.spec), {)
                    TU_ARMA(Class, c)
                        switch(c)
                        {
                        case AsmCommon::RegisterClass::x86_reg: m_of << "=r";   break;
                        case AsmCommon::RegisterClass::x86_reg_abcd: m_of << "=Q";   break;
                        case AsmCommon::RegisterClass::x86_reg_byte: m_of << "=q";   break;
                        case AsmCommon::RegisterClass::x86_xmm: m_of << "=x";   break;
                        case AsmCommon::RegisterClass::x86_ymm: m_of << "=x";   break;
                        case AsmCommon::RegisterClass::x86_zmm: m_of << "=v";   break;
                        case AsmCommon::RegisterClass::x86_kreg: MIR_TODO(mir_res, "Asm2 GCC - x86_kreg: " << stmt);
                        }
                    TU_ARMA(Explicit, name) {
                        MIR_TODO(mir_res, "Asm2 GCC - Explicit output reg: " << stmt);
                        }
                    }
                    assert(p.output);
                    m_of << "\" ("; emit_lvalue(*p.output); m_of << ")";
                }
                m_of << " :";
                for(size_t i = 0; i < inputs.size(); i ++)
                {
                    const auto& p = *inputs[i];
                    if(i != 0)  m_of << ",";
                    m_of << " ";
                    TU_MATCH_HDRA((p), {)
                    TU_ARMA(Reg, r) {
                        m_of << "\"";
                        TU_MATCH_HDRA((r.spec), {)
                        TU_ARMA(Class, c)
                            switch(c)
                            {
                            case AsmCommon::RegisterClass::x86_reg: m_of << "r";   break;
                            case AsmCommon::RegisterClass::x86_reg_abcd: m_of << "Q";   break;
                            case AsmCommon::RegisterClass::x86_reg_byte: m_of << "q";   break;
                            case AsmCommon::RegisterClass::x86_xmm: m_of << "x";   break;
                            case AsmCommon::RegisterClass::x86_ymm: m_of << "x";   break;
                            case AsmCommon::RegisterClass::x86_zmm: m_of << "v";   break;
                            case AsmCommon::RegisterClass::x86_kreg: MIR_TODO(mir_res, "Asm2 GCC - x86_kreg: " << stmt);
                            }
                        TU_ARMA(Explicit, name) {
                            MIR_TODO(mir_res, "Asm2 GCC - Explicit output reg: " << stmt);
                            }
                        }
                        assert(r.input);
                        m_of << "\" ("; emit_param(*r.input); m_of << ")";
                        }
                    TU_ARMA(Const, c)   MIR_TODO(mir_res, "Asm2 GCC - Const: " << stmt);
                    TU_ARMA(Sym, c)   MIR_TODO(mir_res, "Asm2 GCC - Sym: " << stmt);
                    }
                }
                m_of << ");\n";
            }
        }
    private:
        const ::HIR::TypeRef& monomorphise_fcn_return(::HIR::TypeRef& tmp, const ::HIR::Function& item, const Trans_Params& params)
        {
            bool has_erased = visit_ty_with(item.m_return, [&](const auto& x) { return x.data().is_ErasedType(); });
            
            if( has_erased || monomorphise_type_needed(item.m_return) )
            {
                // If there's an erased type, make a copy with the erased type expanded
                if( has_erased )
                {
                    tmp = clone_ty_with(sp, item.m_return, [&](const auto& x, auto& out) {
                        if( const auto* te = x.data().opt_ErasedType() ) {
                            out = item.m_code.m_erased_types.at(te->m_index).clone();
                            return true;
                        }
                        else {
                            return false;
                        }
                        });
                    tmp = params.monomorph_type(Span(), tmp).clone();
                }
                else
                {
                    tmp = params.monomorph_type(Span(), item.m_return).clone();
                }
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
            auto cb = FMT_CB(ss,
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
                        // TODO: If the type has a high alignment, emit as a pointer
                        auto ty = params.monomorph(m_resolve, item.m_args[i].second);
                        this->emit_ctype( ty, FMT_CB(os, os << (this->type_is_high_align(ty) ? "*":"") << "arg" << i;) );
                    }

                    if( item.m_variadic )
                        m_of << ", ...";

                    ss << "\n\t\t)";
                }
                );
            if( ret_ty != ::HIR::TypeRef::new_unit() )
            {
                emit_ctype( ret_ty, cb );
            }
            else
            {
                m_of << "void " << cb;
            }
        }

        void emit_intrinsic_call(const RcString& name, const ::HIR::PathParams& params, const ::MIR::Terminator::Data_Call& e)
        {
            const auto& mir_res = *m_mir_res;
            enum class Ordering
            {
                SeqCst,
                Acquire,
                Release,
                Relaxed,
                AcqRel,
            };
            auto get_atomic_ty_gcc = [&](Ordering o)->const char* {
                switch(o)
                {
                case Ordering::SeqCst:  return "memory_order_seq_cst";
                case Ordering::Acquire: return "memory_order_acquire";
                case Ordering::Release: return "memory_order_release";
                case Ordering::Relaxed: return "memory_order_relaxed";
                case Ordering::AcqRel:  return "memory_order_acq_rel";
                }
                throw "";
                };
            auto get_atomic_suffix_msvc = [&](Ordering o)->const char* {
                switch(o)
                {
                case Ordering::SeqCst:  return "";
                case Ordering::Acquire: return "Acquire";
                case Ordering::Release: return "Release";
                case Ordering::Relaxed: return "NoFence";
                case Ordering::AcqRel:  return "";  // this is either Acquire or Release
                }
                throw "";
            };
            auto get_atomic_ordering = [&](const RcString& name, size_t prefix_len)->Ordering {
                    if( name.size() < prefix_len )
                    {
                        return Ordering::SeqCst;
                    }
                    const char* suffix = name.c_str() + prefix_len;
                    if( ::std::strcmp(suffix, "acq") == 0 ) {
                        return Ordering::Acquire;
                    }
                    else if( ::std::strcmp(suffix, "rel") == 0 ) {
                        return Ordering::Release;
                    }
                    else if( ::std::strcmp(suffix, "relaxed") == 0 ) {
                        return Ordering::Relaxed;
                    }
                    else if( ::std::strcmp(suffix, "acqrel") == 0 ) {
                        return Ordering::AcqRel;
                    }
                    // TODO: Is this correct?
                    else if( ::std::strcmp(suffix, "unordered") == 0 ) {
                        return Ordering::Relaxed;
                    }
                    else {
                        MIR_BUG(mir_res, "Unknown atomic ordering suffix - '" << suffix << "'");
                    }
                    throw "";
                };
            auto get_prim_size = [&mir_res](const ::HIR::TypeRef& ty)->unsigned {
                    if(ty.data().is_Pointer())
                        return Target_GetCurSpec().m_arch.m_pointer_bits;
                    if( !ty.data().is_Primitive() )
                        MIR_BUG(mir_res, "Unknown type for getting primitive size - " << ty);
                    switch( ty.data().as_Primitive() )
                    {
                    case ::HIR::CoreType::U8:
                    case ::HIR::CoreType::I8:
                        return 8;
                    case ::HIR::CoreType::U16:
                    case ::HIR::CoreType::I16:
                        return 16;
                    case ::HIR::CoreType::U32:
                    case ::HIR::CoreType::I32:
                        return 32;
                    case ::HIR::CoreType::U64:
                    case ::HIR::CoreType::I64:
                        return 64;
                    case ::HIR::CoreType::U128:
                    case ::HIR::CoreType::I128:
                        return 128;
                    case ::HIR::CoreType::Usize:
                    case ::HIR::CoreType::Isize:
                        // TODO: Is this a good idea?
                        return Target_GetCurSpec().m_arch.m_pointer_bits;
                    default:
                        MIR_BUG(mir_res, "Unknown primitive for getting size- " << ty);
                    }
                };
            auto get_real_prim_ty = [](HIR::CoreType ct)->HIR::CoreType {
                switch( ct )
                {
                case HIR::CoreType::Usize:
                    if( Target_GetCurSpec().m_arch.m_pointer_bits == 64 )
                        return ::HIR::CoreType::U64;
                    if( Target_GetCurSpec().m_arch.m_pointer_bits == 32 )
                        return ::HIR::CoreType::U32;
                    BUG(Span(), "");
                case HIR::CoreType::Isize:
                    if( Target_GetCurSpec().m_arch.m_pointer_bits == 64 )
                        return ::HIR::CoreType::I64;
                    if( Target_GetCurSpec().m_arch.m_pointer_bits == 32 )
                        return ::HIR::CoreType::I32;
                    BUG(Span(), "");
                default:
                    return ct;
                }
                };
            auto emit_msvc_atomic_op = [&](const char* name, Ordering ordering) {
                const auto& ty = params.m_types.at(0);
                if(ty.data().is_Pointer()) {
                    m_of << "("; emit_ctype(ty); m_of << ")";
                }
                m_of << name;
                switch( get_prim_size(params.m_types.at(0)) )
                {
                case 8:
                    m_of << "8";
                    break;
                case 16:
                    m_of << "16";
                    break;
                case 32:
                    break;
                case 64:
                    m_of << "64";
                    break;
                default:
                    MIR_BUG(mir_res, "Unsupported atomic type - " << params.m_types.at(0));
                }
                m_of << get_atomic_suffix_msvc(ordering);
                m_of << "(";
                if(ty.data().is_Pointer()) {
                    m_of << "(uintptr_t*)";
                }
                };
            auto emit_atomic_cast = [&]() {
                m_of << "("; emit_ctype(params.m_types.at(0)); m_of << "_Atomic *)";
                };
            auto emit_atomic_cxchg = [&](const auto& e, Ordering o_succ, Ordering o_fail, bool is_weak) {
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    emit_lvalue(e.ret_val); m_of << "._0 = "; emit_param(e.args.at(1)); m_of << ";\n\t";
                    emit_lvalue(e.ret_val); m_of << "._1 = atomic_compare_exchange_" << (is_weak ? "weak" : "strong") << "_explicit(";
                        emit_atomic_cast(); emit_param(e.args.at(0));
                        m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0";   // Expected (i.e. the check value)
                        m_of << ", "; emit_param(e.args.at(2)); // `desired` (the new value for the slot if equal)
                        m_of << ", " << get_atomic_ty_gcc(o_succ) << ", " << get_atomic_ty_gcc(o_fail) << ")";
                    break;
                case Compiler::Msvc:
                    if( params.m_types.at(0) == ::HIR::CoreType::U128 || params.m_types.at(0) == ::HIR::CoreType::I128 )
                    {
                        emit_lvalue(e.ret_val); m_of << "._0 = "; emit_param(e.args.at(1)); m_of << ";\n\t";
                        emit_lvalue(e.ret_val); m_of << "._1 = InterlockedCompareExchange128(";
                        m_of << "(volatile uint64_t*)"; emit_param(e.args.at(0)); m_of << ", ";
                        emit_param(e.args.at(2)); m_of << ".hi, ";
                        emit_param(e.args.at(2)); m_of << ".lo, ";
                        m_of << "(uint64_t*)"; m_of << "&"; emit_lvalue(e.ret_val); m_of << "._0";
                        m_of << ")";
                        break;
                    }
                    emit_lvalue(e.ret_val); m_of << "._0 = ";
                    emit_msvc_atomic_op("InterlockedCompareExchange", Ordering::SeqCst);  // TODO: Use ordering, but which one?
                    // Slot, Exchange (new value), Comparand (expected value) - Note different order to the gcc/stdc version
                    emit_param(e.args.at(0)); m_of << ", ";
                    if(params.m_types.at(0).data().is_Pointer()) { m_of << "(uintptr_t)"; } emit_param(e.args.at(2)); m_of << ", ";
                    if(params.m_types.at(0).data().is_Pointer()) { m_of << "(uintptr_t)"; } emit_param(e.args.at(1)); m_of << ")";
                    m_of << ";\n\t";
                    // If the result equals the expected value, return true
                    emit_lvalue(e.ret_val); m_of << "._1 = ("; emit_lvalue(e.ret_val); m_of << "._0 == "; emit_param(e.args.at(1)); m_of << ")";
                    break;
                }
                };
            auto emit_atomic_arith = [&](AtomicOp op, Ordering ordering) {
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
                    m_of << "("; emit_atomic_cast(); emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", " << get_atomic_ty_gcc(ordering) << ")";
                    break;
                case Compiler::Msvc:
                    switch(op)
                    {
                    case AtomicOp::Add: emit_msvc_atomic_op("InterlockedExchangeAdd", ordering);    break;
                    case AtomicOp::Sub:
                        emit_msvc_atomic_op("InterlockedExchangeAdd", ordering);
                        emit_param(e.args.at(0)); m_of << ", ~(";
                        emit_param(e.args.at(1)); m_of << ")+1)";
                        return ;
                    case AtomicOp::And: emit_msvc_atomic_op("InterlockedAnd", ordering);    break;
                    case AtomicOp::Or:  emit_msvc_atomic_op("InterlockedOr", ordering);    break;
                    case AtomicOp::Xor: emit_msvc_atomic_op("InterlockedXor", ordering);    break;
                    }
                    emit_param(e.args.at(0)); m_of << ", ";
                    emit_param(e.args.at(1)); m_of << ")";
                    break;
                }
                };
            if( name == "size_of" ) {
                size_t size = 0;
                MIR_ASSERT(mir_res, Target_GetSizeOf(sp, m_resolve, params.m_types.at(0), size), "Can't get size of " << params.m_types.at(0));
                emit_lvalue(e.ret_val); m_of << " = " << size;
            }
            else if( name == "min_align_of" || name == "align_of" ) {
                size_t align = 0;
                MIR_ASSERT(mir_res, Target_GetAlignOf(sp, m_resolve, params.m_types.at(0), align), "Can't get alignment of " << params.m_types.at(0));
                emit_lvalue(e.ret_val); m_of << " = " << align;
            }
            else if( name == "size_of_val" ) {
                emit_lvalue(e.ret_val); m_of << " = ";
                const auto& ty = params.m_types.at(0);
                // Get the unsized type and use that in place of MetadataType
                auto inner_ty = get_inner_unsized_type(ty);
                if( inner_ty == ::HIR::TypeRef() ) {
                    size_t size = 0;
                    MIR_ASSERT(mir_res, Target_GetSizeOf(sp, m_resolve, ty, size), "Can't get size of " << ty);
                    m_of << size;
                }
                // slice metadata (`[T]` and `str`)
                else if( inner_ty.data().is_Slice() || inner_ty == ::HIR::CoreType::Str ) {
                    bool align_needed = false;
                    size_t item_size = 0;
                    size_t item_align = 0;
                    if(const auto* te = inner_ty.data().opt_Slice() ) {
                        MIR_ASSERT(mir_res, Target_GetSizeAndAlignOf(sp, m_resolve, te->inner, item_size, item_align), "Can't get size of " << te->inner);
                    }
                    else {
                        assert(inner_ty == ::HIR::CoreType::Str);
                        item_size = 1;
                        item_align = 1;
                    }
                    if( ! ty.data().is_Slice() && !ty.data().is_Primitive() ) {
                        // TODO: What if the wrapper has no other fields?
                        // Get the alignment and check if it's higher than the item alignment
                        size_t wrapper_align = 0, wrapper_size_ignore = 0;
                        MIR_ASSERT(mir_res, Target_GetSizeAndAlignOf(sp, m_resolve, ty, wrapper_size_ignore, wrapper_align), "Can't get align of " << ty);
                        if(wrapper_align > item_align) {
                            item_align = wrapper_align;
                            align_needed = true;
                            m_of << "ALIGN_TO(";
                        }
                        m_of << "sizeof("; emit_ctype(ty); m_of << ") + ";
                    }
                    emit_param(e.args.at(0)); m_of << ".META * " << item_size;
                    if(align_needed) {
                        m_of << ", " << item_align << ")";
                    }
                }
                // Trait object metadata.
                else if( inner_ty.data().is_TraitObject() ) {
                    // TODO: Handle aligning the size if the wrapper's alignment is greater than the object
                    // - Also, how is the final field aligned?
                    if( ! ty.data().is_TraitObject() ) {
                        m_of << "sizeof("; emit_ctype(ty); m_of << ") + ";
                    }
                    //auto vtable_path = inner_ty.data().as_TraitObject().m_trait.m_path.clone();
                    //vtable_path.m_path.m_components.back() += "#vtable";
                    //auto vtable_ty = ::HIR::TypeRef
                    m_of << "((VTABLE_HDR*)"; emit_param(e.args.at(0)); m_of << ".META)->size";
                }
                else {
                    MIR_BUG(mir_res, "Unknown inner unsized type " << inner_ty << " for " << ty);
                }
                // TODO: Align up
            }
            else if( name == "min_align_of_val" ) {
                emit_lvalue(e.ret_val); m_of << " = ";
                const auto& ty = params.m_types.at(0);
                #if 1
                auto inner_ty = get_inner_unsized_type(ty);
                if( inner_ty == ::HIR::TypeRef() ) {
                    m_of << "ALIGNOF("; emit_ctype(ty); m_of << ")";
                }
                else if( const auto* te = inner_ty.data().opt_Slice() ) {
                    if( ! ty.data().is_Slice() ) {
                        m_of << "mrustc_max( ALIGNOF("; emit_ctype(ty); m_of << "), ";
                    }
                    m_of << "ALIGNOF("; emit_ctype(te->inner); m_of << ")";
                    if( ! ty.data().is_Slice() ) {
                        m_of << " )";
                    }
                }
                else if( inner_ty == ::HIR::CoreType::Str ) {
                    if( ! ty.data().is_Primitive() ) {
                        m_of << "ALIGNOF("; emit_ctype(ty); m_of << ")";
                    }
                    else {
                        m_of << "1";
                    }
                }
                else if( inner_ty.data().is_TraitObject() ) {
                    if( ! ty.data().is_TraitObject() ) {
                        m_of << "mrustc_max( ALIGNOF("; emit_ctype(ty); m_of << "), ";
                    }
                    m_of << "((VTABLE_HDR*)"; emit_param(e.args.at(0)); m_of << ".META)->align";
                    if( ! ty.data().is_TraitObject() ) {
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
                    const auto& ity = *ty.data().as_Slice().inner;
                    m_of << "ALIGNOF("; emit_ctype(ity); m_of << ")";
                    break; }
                case MetadataType::TraitObject:
                    m_of << "((VTABLE_HDR*)"; emit_param(e.args.at(0)); m_of << ".META)->align";
                    break;
                }
                #endif
            }
            // --- Type assertions ---
            else if( name == "panic_if_uninhabited" || name == "assert_inhabited" ) {
                // TODO: Detect uninhabited (empty enum or `!` - potentially via nested types)
            }
            else if( name == "assert_zero_valid" ) {
                // TODO: Detect nonzero within
            }
            else if( name == "assert_uninit_valid" ) {
                // TODO: Detect nonzero or enum within
            }
            // --- Type identity ---
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
                const auto& ty_src = params.m_types.at(0);
                const auto& ty_dst = params.m_types.at(1);
                auto is_ptr = [](const ::HIR::TypeRef& ty){ return ty.data().is_Borrow() || ty.data().is_Pointer(); };
                if( this->type_is_bad_zst(ty_dst) )
                {
                    m_of << "/* zst */";
                }
                else if( e.args.at(0).is_Constant() )
                {
                    m_of << "{ "; emit_ctype(ty_src, FMT_CB(s, s << "v";)); m_of << " = "; emit_param(e.args.at(0)); m_of << "; ";
                    m_of << "memcpy( &"; emit_lvalue(e.ret_val); m_of << ", &v, sizeof("; emit_ctype(ty_dst); m_of << ")); ";
                    m_of << "}";
                }
                else if( is_ptr(ty_dst) && is_ptr(ty_src) )
                {
                    auto src_meta = metadata_type(ty_src.data().is_Pointer() ? ty_src.data().as_Pointer().inner : ty_src.data().as_Borrow().inner);
                    auto dst_meta = metadata_type(ty_dst.data().is_Pointer() ? ty_dst.data().as_Pointer().inner : ty_dst.data().as_Borrow().inner);
                    if( src_meta == MetadataType::None || src_meta == MetadataType::Zero )
                    {
                        MIR_ASSERT(*m_mir_res, dst_meta == MetadataType::None || dst_meta == MetadataType::Zero, "Transmuting to fat pointer from thin: " << ty_src << " -> " << ty_dst);
                        emit_lvalue(e.ret_val); m_of << " = (void*)"; emit_param(e.args.at(0));
                    }
                    else if(dst_meta == MetadataType::None || dst_meta == MetadataType::Zero)
                    {
                        MIR_BUG(*m_mir_res, "Transmuting from fat pointer to thin: (" << src_meta << "->" << dst_meta << ") " << ty_src << " -> " << ty_dst);
                    }
                    else if( src_meta != dst_meta )
                    {
                        emit_lvalue(e.ret_val); m_of << ".PTR = "; emit_param(e.args.at(0)); m_of << ".PTR; ";
                        emit_lvalue(e.ret_val); m_of << ".META = ";
                        switch(dst_meta)
                        {
                        case MetadataType::Unknown: assert(!"Impossible");
                        case MetadataType::None: assert(!"Impossible");
                        case MetadataType::Zero: assert(!"Impossible");
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
                    m_of << "memcpy( &"; emit_lvalue(e.ret_val); m_of << ", &"; emit_param(e.args.at(0)); m_of << ", sizeof("; emit_ctype(ty_src); m_of << "))";
                }
            }
            else if( name == "float_to_int_unchecked" ) {
                const auto& dst_ty = params.m_types.at(1);
                // Unchecked (can return `undef`) cast from a float to an integer
                if( this->type_is_emulated_i128(dst_ty) ) {
                    m_of << "abort()";
                    //emit_lvalue(e.ret_val); m_of << " = ("; emit_ctype(dst_ty); m_of << ")"; emit_param(e.args.at(0));
                }
                else {
                    emit_lvalue(e.ret_val); m_of << " = ("; emit_ctype(dst_ty); m_of << ")"; emit_param(e.args.at(0));
                }
            }
            else if( name == "copy_nonoverlapping" || name == "copy" ) {
                if( this->type_is_bad_zst(params.m_types.at(0)) ) {
                    m_of << "/* zst */";
                    return ;
                }
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
            // NOTE: This is generic, and fills count*sizeof(T) (unlike memset)
            else if( name == "write_bytes" ) {
                if( this->type_is_bad_zst(params.m_types.at(0)) ) {
                    m_of << "/* zst */";
                    return ;
                }
                // 0: Destination, 1: Value, 2: Count
                m_of << "if( "; emit_param(e.args.at(2)); m_of << " > 0) memset( "; emit_param(e.args.at(0));
                    m_of << ", "; emit_param(e.args.at(1));
                    m_of << ", "; emit_param(e.args.at(2)); m_of << " * sizeof("; emit_ctype(params.m_types.at(0)); m_of << ")";
                    m_of << ")";
            }
            else if( name == "forget" ) {
                // Nothing needs to be done, this just stops the destructor from running.
            }
            else if( name == "drop_in_place" ) {
                emit_destructor_call( ::MIR::LValue::new_Deref(e.args.at(0).as_LValue().clone()), params.m_types.at(0), true, /*indent_level=*/1 /* TODO: get from caller */ );
            }
            // --- Type traits
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
            // --- Initialisation (or lack thereof)
            else if( name == "uninit" ) {
                // Do nothing, leaves the destination undefined
                // TODO: This makes the C compiler warn
            }
            else if( name == "init" ) {
                m_of << "memset( &"; emit_lvalue(e.ret_val); m_of << ", 0, sizeof("; emit_ctype(params.m_types.at(0)); m_of << "))";
            }
            else if( name == "move_val_init" ) {
                if( !this->type_is_bad_zst(params.m_types.at(0)) )
                {
                    m_of << "*"; emit_param(e.args.at(0)); m_of << " = "; emit_param(e.args.at(1));
                }
            }
            else if( name == "abort" ) {
                m_of << "abort()";
            }
            else if( name == "try" ) {
                // Register thread-local setjmp
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    m_of << "{ ";
                    m_of << " jmp_buf jmpbuf, *old = mrustc_panic_target; mrustc_panic_target = &jmpbuf;";
                    m_of << " if(setjmp(jmpbuf)) {";
                    // NOTE: gcc unwind has a pointer as its `local_ptr` parameter
                    if(TARGETVER_MOST_1_39) {
                        m_of << " *(void**)("; emit_param(e.args.at(2)); m_of << ") = mrustc_panic_value;";
                    }
                    else {
                        m_of << "("; emit_param(e.args.at(2)); m_of << ")("; emit_param(e.args.at(1)); m_of << ", mrustc_panic_value);";
                    }
                    m_of << " "; emit_lvalue(e.ret_val); m_of << " = 1;";   // Return value non-zero when panic happens
                    m_of << " } else {";
                    m_of << " ";
                    break;
                default:
                    break;
                }
                emit_param(e.args.at(0)); m_of << "("; emit_param(e.args.at(1)); m_of << "); ";
                emit_lvalue(e.ret_val); m_of << " = 0";
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    m_of << ";";
                    m_of << " }";
                    m_of << " if(mrustc_panic_target != &jmpbuf) { abort(); }";
                    m_of << " mrustc_panic_target = old;";
                    m_of << " }";
                    break;
                default:
                    break;
                }
            }
            // --- #[track_caller]
            else if( name == "caller_location" ) {
                //m_of << "abort()";
                m_of << "static struct s_ZRG2cE9core0_0_05panic8Location0g mrustc_empty_caller_location = {0,0,{\"\",0}};";
                emit_lvalue(e.ret_val); m_of << " = &mrustc_empty_caller_location"; // TODO: Hidden ABI for caller location
            }
            // --- Pointer manipulation
            else if( name == "offset" ) {   // addition, with the reqirement that the resultant pointer be in bounds
                emit_lvalue(e.ret_val); m_of << " = "; emit_param(e.args.at(0)); m_of << " + "; emit_param(e.args.at(1));
            }
            else if( name == "arith_offset" ) { // addition, with no requirements
                emit_lvalue(e.ret_val); m_of << " = "; emit_param(e.args.at(0)); m_of << " + "; emit_param(e.args.at(1));
            }
            else if( name == "ptr_offset_from" ) {  // effectively subtraction
                emit_lvalue(e.ret_val); m_of << " = "; emit_param(e.args.at(0)); m_of << " - "; emit_param(e.args.at(1));
            }
            else if( name == "ptr_guaranteed_eq" ) {
                emit_lvalue(e.ret_val); m_of << " = ("; emit_param(e.args.at(0)); m_of << " == "; emit_param(e.args.at(1)); m_of << ")";
            }
            else if( name == "ptr_guaranteed_ne" ) {
                emit_lvalue(e.ret_val); m_of << " = ("; emit_param(e.args.at(0)); m_of << " != "; emit_param(e.args.at(1)); m_of << ")";
            }
            // ----
            else if( name == "bswap" ) {
                const auto& ty = params.m_types.at(0);
                MIR_ASSERT(mir_res, ty.data().is_Primitive(), "Invalid type passed to bwsap, must be a primitive, got " << ty);
                if( ty == ::HIR::CoreType::U8 || ty == ::HIR::CoreType::I8 ) {
                    // Nop.
                    emit_lvalue(e.ret_val); m_of << " = "; emit_param(e.args.at(0));
                }
                else {
                    emit_lvalue(e.ret_val); m_of << " = ";
                    switch(m_compiler)
                    {
                    case Compiler::Gcc:
                        switch(get_prim_size(ty))
                        {
                        case 16:
                            m_of << "__builtin_bswap16";
                            break;
                        case 32:
                            m_of << "__builtin_bswap32";
                            break;
                        case 64:
                            m_of << "__builtin_bswap64";
                            break;
                        case 128:
                            m_of << "__builtin_bswap128";
                            break;
                        default:
                            MIR_TODO(mir_res, "bswap<" << ty << ">");
                        }
                        break;
                    case Compiler::Msvc:
                        switch( ty.data().as_Primitive() )
                        {
                        case ::HIR::CoreType::U16:
                        case ::HIR::CoreType::I16:
                            m_of << "_byteswap_ushort";
                            break;
                        case ::HIR::CoreType::U32:
                        case ::HIR::CoreType::I32:
                            m_of << "_byteswap_ulong";
                            break;
                        case ::HIR::CoreType::U64:
                        case ::HIR::CoreType::I64:
                            m_of << "_byteswap_uint64";
                            break;
                        case ::HIR::CoreType::U128:
                        case ::HIR::CoreType::I128:
                            m_of << "__builtin_bswap128";
                            break;
                        default:
                            MIR_TODO(mir_res, "bswap<" << ty << ">");
                        }
                        break;
                    }
                    m_of << "("; emit_param(e.args.at(0)); m_of << ")";
                }
            }
            else if( name == "bitreverse" ) {
                const auto& ty = params.m_types.at(0);
                MIR_ASSERT(mir_res, ty.data().is_Primitive(), "Invalid type passed to bitreverse. Must be a primitive, got " << ty);
                emit_lvalue(e.ret_val); m_of << " = ";
                switch(get_prim_size(ty))
                {
                case  8: m_of << "__mrustc_bitrev8";    break;
                case 16: m_of << "__mrustc_bitrev16";    break;
                case 32: m_of << "__mrustc_bitrev32";    break;
                case 64: m_of << "__mrustc_bitrev64";    break;
                case 128: m_of << "__mrustc_bitrev128";    break;
                default:
                    MIR_TODO(mir_res, "bswap<" << ty << ">");
                }
                m_of << "("; emit_param(e.args.at(0)); m_of << ")";
            }
            // > Obtain the discriminane of a &T as u64
            else if( name == "discriminant_value" ) {
                const auto& ty = params.m_types.at(0);
                emit_lvalue(e.ret_val); m_of << " = ";
                const auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);
                if( !repr ) {
                    m_of << "0";
                }
                else {
                    switch(repr->variants.tag())
                    {
                    case TypeRepr::VariantMode::TAGDEAD:    throw "";
                    TU_ARM(repr->variants, None, _e)
                        m_of << "0";
                        break;
                    TU_ARM(repr->variants, Values, ve) {
                        m_of << "(*"; emit_param(e.args.at(0)); m_of << ")"; emit_enum_path(repr, ve.field);
                        } break;
                    TU_ARM(repr->variants, Linear, ve) {
                        if( ve.uses_niche() ) {
                            m_of << "( (*"; emit_param(e.args.at(0)); m_of << ")"; emit_enum_path(repr, ve.field); m_of << " < " << ve.offset;
                            m_of << " ? " << ve.field.index;
                            m_of << " : (*"; emit_param(e.args.at(0)); m_of << ")"; emit_enum_path(repr, ve.field);
                            m_of << " )";
                        }
                        else {
                            m_of << "(*"; emit_param(e.args.at(0)); m_of << ")"; emit_enum_path(repr, ve.field);
                        }
                        } break;
                    TU_ARM(repr->variants, NonZero, ve) {
                        m_of << "(*"; emit_param(e.args.at(0)); m_of << ")"; emit_enum_path(repr, ve.field); m_of << " ";
                        m_of << (ve.zero_variant ? "==" : "!=");
                        m_of << " 0";
                        } break;
                    }
                }
            }
            // Hints
            else if( name == "unreachable" ) {
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    m_of << "__builtin_unreachable()";
                    break;
                case Compiler::Msvc:
                    m_of << "for(;;)";
                    break;
                }
            }
            else if( name == "assume" ) {
                // I don't assume :)
            }
            else if( name == "likely" ) {
                emit_lvalue(e.ret_val); m_of << "= ("; emit_param(e.args.at(0)); m_of << ")";
            }
            else if( name == "unlikely" ) {
                emit_lvalue(e.ret_val); m_of << "= ("; emit_param(e.args.at(0)); m_of << ")";
            }
            // Overflowing Arithmetic
            // HACK: Uses GCC intrinsics
            else if( name == "add_with_overflow" ) {
                if(m_options.emulated_i128 && params.m_types.at(0) == ::HIR::CoreType::U128)
                {
                    emit_lvalue(e.ret_val); m_of << "._1 = add128_o";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
                }
                else if(m_options.emulated_i128 && params.m_types.at(0) == ::HIR::CoreType::I128)
                {
                    emit_lvalue(e.ret_val); m_of << "._1 = add128s_o";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
                }
                else

                {
                    switch(m_compiler)
                    {
                    case Compiler::Gcc:
                        emit_lvalue(e.ret_val); m_of << "._1 = __builtin_add_overflow";
                        m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
                        break;
                    case Compiler::Msvc:
                        emit_lvalue(e.ret_val); m_of << "._1 = __builtin_add_overflow_" << params.m_types.at(0).data().as_Primitive();
                        m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
                        break;
                    }
                }
            }
            else if( name == "sub_with_overflow" ) {
                if(m_options.emulated_i128 && params.m_types.at(0) == ::HIR::CoreType::U128)
                {
                    emit_lvalue(e.ret_val); m_of << "._1 = sub128_o";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
                }
                else if(m_options.emulated_i128 && params.m_types.at(0) == ::HIR::CoreType::I128)
                {
                    emit_lvalue(e.ret_val); m_of << "._1 = sub128s_o";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
                }
                else
                {
                    switch(m_compiler)
                    {
                    case Compiler::Gcc:
                        emit_lvalue(e.ret_val); m_of << "._1 = __builtin_sub_overflow";
                        m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
                        break;
                    case Compiler::Msvc:
                        emit_lvalue(e.ret_val); m_of << "._1 = __builtin_sub_overflow_" << params.m_types.at(0).data().as_Primitive();
                        m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
                        break;
                    }
                }
            }
            else if( name == "mul_with_overflow" ) {
                if(m_options.emulated_i128 && params.m_types.at(0) == ::HIR::CoreType::U128)
                {
                    emit_lvalue(e.ret_val); m_of << "._1 = mul128_o";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
                }
                else if(m_options.emulated_i128 && params.m_types.at(0) == ::HIR::CoreType::I128)
                {
                    emit_lvalue(e.ret_val); m_of << "._1 = mul128s_o";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
                }
                else
                {
                    switch(m_compiler)
                    {
                    case Compiler::Gcc:
                        emit_lvalue(e.ret_val); m_of << "._1 = __builtin_mul_overflow("; emit_param(e.args.at(0));
                            m_of << ", "; emit_param(e.args.at(1));
                            m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
                        break;
                    case Compiler::Msvc:
                        emit_lvalue(e.ret_val); m_of << "._1 = __builtin_mul_overflow_" << params.m_types.at(0).data().as_Primitive();
                        m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
                        break;
                    }
                }
            }
            else if(
                name == "overflowing_add" || name == "wrapping_add"    // Renamed in 1.39
                || name == "saturating_add"
                || name == "unchecked_add"
                )
            {
                const auto& ty = params.m_types.at(0);
                if( name == "saturating_add" )
                {
                    m_of << "if( ";
                }

                if(m_options.emulated_i128 && ty == ::HIR::CoreType::U128)
                {
                    m_of << "add128_o";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                }
                else if(m_options.emulated_i128 && ty == ::HIR::CoreType::I128)
                {
                    m_of << "add128s_o";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                }
                else
                {
                    switch(m_compiler)
                    {
                    case Compiler::Gcc:
                        m_of << "__builtin_add_overflow";
                        m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                        break;
                    case Compiler::Msvc:
                        m_of << "__builtin_add_overflow_" << ty.data().as_Primitive();
                        m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                        break;
                    }
                }

#if 1
                if( name == "saturating_add" )
                {
                    m_of << ") { ";
                    emit_lvalue(e.ret_val); m_of << " = ";
                    switch( get_real_prim_ty(ty.data().as_Primitive()) )
                    {
                    case ::HIR::CoreType::U8:
                    case ::HIR::CoreType::U16:
                    case ::HIR::CoreType::U32:
                    case ::HIR::CoreType::U64:
                        m_of << "-1";   // -1 should extend to MAX
                        break;
                    case ::HIR::CoreType::U128:
                        if( m_options.emulated_i128 )
                        {
                            m_of << "make128_raw(-1, -1)";
                        }
                        else
                        {
                            m_of << "-1";
                        }
                        break;
                    // If the LHS is negative, then the only way overflow can happen is if the RHS is also negative, so saturate at negative.
                    case ::HIR::CoreType::I8:
                        m_of << "("; emit_param(e.args.at(0)); m_of << " < 0 ? -0x80 : 0x7F)";
                        break;
                    case ::HIR::CoreType::I16:
                        m_of << "("; emit_param(e.args.at(0)); m_of << " < 0 ? -0x8000 : 0x7FFF)";
                        break;
                    case ::HIR::CoreType::I32:
                        m_of << "("; emit_param(e.args.at(0)); m_of << " < 0 ? -0x8000000l : 0x7FFFFFFFl)";
                        break;
                    case ::HIR::CoreType::I64:
                        m_of << "("; emit_param(e.args.at(0)); m_of << " < 0 ? -0x8000000""00000000ll : 0x7FFFFFFF""FFFFFFFFll)";
                        break;
                    case ::HIR::CoreType::I128:
                        if( m_options.emulated_i128 )
                        {
                            m_of << "( (int64_t)("; emit_param(e.args.at(0)); m_of << ".hi) < 0 ? make128s_raw(-0x8000000""00000000ll, 0) : make128s_raw(0x7FFFFFFF""FFFFFFFFll, -1))";
                        }
                        else
                        {
                            m_of << "("; emit_param(e.args.at(0)); m_of << " < 0 ? ((uint128_t)1 << 127) : (((uint128_t)1 << 127) - 1))";
                        }
                        break;
                    default:
                        MIR_TODO(mir_res, "saturating_add - " << ty);
                    }
                    m_of << "; }";
                }
#endif
            }
            else if( name == "overflowing_sub" || name == "wrapping_sub"
                || name == "saturating_sub"
                || name == "unchecked_sub"
                )
            {
                const auto& ty = params.m_types.at(0);
                if( name == "saturating_sub" )
                {
                    m_of << "if( ";
                }
                if(m_options.emulated_i128 && ty == ::HIR::CoreType::U128)
                {
                    m_of << "sub128_o";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                }
                else if(m_options.emulated_i128 && ty == ::HIR::CoreType::I128)
                {
                    m_of << "sub128s_o";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                }
                else
                {
                    switch(m_compiler)
                    {
                    case Compiler::Gcc:
                        m_of << "__builtin_sub_overflow";
                        m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                        break;
                    case Compiler::Msvc:
                        m_of << "__builtin_sub_overflow_" << ty.data().as_Primitive();
                        m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                        break;
                    }
                }


#if 1
                if( name == "saturating_sub" )
                {
                    m_of << ") { ";
                    emit_lvalue(e.ret_val); m_of << " = ";
                    switch( get_real_prim_ty(ty.data().as_Primitive()) )
                    {
                    case ::HIR::CoreType::U8:
                    case ::HIR::CoreType::U16:
                    case ::HIR::CoreType::U32:
                    case ::HIR::CoreType::U64:
                        m_of << "0";
                        break;
                    case ::HIR::CoreType::U128:
                        if( m_options.emulated_i128 )
                        {
                            m_of << "make128(0)";
                        }
                        else
                        {
                            m_of << "0";
                        }
                        break;
                    case ::HIR::CoreType::I8:
                        m_of << "("; emit_param(e.args.at(0)); m_of << " < 0 ? -0x80 : 0x7F)";
                        break;
                    case ::HIR::CoreType::I16:
                        m_of << "("; emit_param(e.args.at(0)); m_of << " < 0 ? -0x8000 : 0x7FFF)";
                        break;
                    case ::HIR::CoreType::I32:
                        m_of << "("; emit_param(e.args.at(0)); m_of << " < 0 ? -0x8000000l : 0x7FFFFFFFl)";
                        break;
                    case ::HIR::CoreType::I64:
                        m_of << "("; emit_param(e.args.at(0)); m_of << " < 0 ? -0x8000000""00000000ll : 0x7FFFFFFF""FFFFFFFFll)";
                        break;
                    case ::HIR::CoreType::I128:
                        if( m_options.emulated_i128 )
                        {
                            m_of << "( (int64_t)("; emit_param(e.args.at(0)); m_of << ".hi) < 0 ? make128s_raw(-0x8000000""00000000ll, 0) : make128s_raw(0x7FFFFFFF""FFFFFFFFll, -1))";
                        }
                        else
                        {
                            m_of << "("; emit_param(e.args.at(0)); m_of << " < 0 ? ((uint128_t)1 << 127) : (((uint128_t)1 << 127) - 1))";
                        }
                        break;
                    default:
                        MIR_TODO(mir_res, "saturating_sub - " << ty);
                    }
                    m_of << "; }";
                }
#endif
            }
            else if( name == "overflowing_mul" || name == "wrapping_mul" || name == "unchecked_mul" ) {
                if(m_options.emulated_i128 && params.m_types.at(0) == ::HIR::CoreType::U128)
                {
                    m_of << "mul128_o";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                }
                else if(m_options.emulated_i128 && params.m_types.at(0) == ::HIR::CoreType::I128)
                {
                    m_of << "mul128s_o";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                }
                else
                {
                    switch(m_compiler)
                    {
                    case Compiler::Gcc:
                        m_of << "__builtin_mul_overflow";
                        m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                        break;
                    case Compiler::Msvc:
                        m_of << "__builtin_mul_overflow_" << params.m_types.at(0).data().as_Primitive();
                        m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                        break;
                    }
                }
            }
            // Unchecked Arithmetic
            // - exact_div is UB to call on a non-multiple
            else if( name == "unchecked_div" || name == "exact_div") {
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
            // Rotate
            else if( name == "rotate_left" ) {
                const auto& ty = params.m_types.at(0);
                switch( get_real_prim_ty(ty.data().as_Primitive()) )
                {
                case ::HIR::CoreType::I8:
                case ::HIR::CoreType::U8:
                    m_of << "{";
                    m_of << " uint8_t v = "; emit_param(e.args.at(0)); m_of << ";";
                    m_of << " unsigned shift = "; emit_param(e.args.at(1)); m_of << ";";
                    m_of << " "; emit_lvalue(e.ret_val); m_of << " = (v << shift) | (v >> (8 - shift));";
                    m_of << "}";
                    break;
                case ::HIR::CoreType::I16:
                case ::HIR::CoreType::U16:
                    m_of << "{";
                    m_of << " uint16_t v = "; emit_param(e.args.at(0)); m_of << ";";
                    m_of << " unsigned shift = "; emit_param(e.args.at(1)); m_of << ";";
                    m_of << " "; emit_lvalue(e.ret_val); m_of << " = (v << shift) | (v >> (16 - shift));";
                    m_of << "}";
                    break;
                case ::HIR::CoreType::I32:
                case ::HIR::CoreType::U32:
                    m_of << "{";
                    m_of << " uint32_t v = "; emit_param(e.args.at(0)); m_of << ";";
                    m_of << " unsigned shift = "; emit_param(e.args.at(1)); m_of << ";";
                    m_of << " "; emit_lvalue(e.ret_val); m_of << " = (v << shift) | (v >> (32 - shift));";
                    m_of << "}";
                    break;
                case ::HIR::CoreType::I64:
                case ::HIR::CoreType::U64:
                    m_of << "{";
                    m_of << " uint64_t v = "; emit_param(e.args.at(0)); m_of << ";";
                    m_of << " unsigned shift = "; emit_param(e.args.at(1)); m_of << ";";
                    m_of << " "; emit_lvalue(e.ret_val); m_of << " = (v << shift) | (v >> (64 - shift));";
                    m_of << "}";
                    break;
                case ::HIR::CoreType::I128:
                case ::HIR::CoreType::U128:
                    m_of << "{";
                    m_of << " uint128_t v = "; emit_param(e.args.at(0)); m_of << ";";
                    m_of << " unsigned shift = "; emit_param(e.args.at(1)); m_of << (m_options.emulated_i128 ? ".lo" : "") << ";";
                    if( m_options.emulated_i128 )
                    {
                        m_of << " if(shift < 64) {";
                        m_of << " "; emit_lvalue(e.ret_val); m_of << ".lo = (v.lo << shift) | (v.hi >> (64 - shift));";
                        m_of << " "; emit_lvalue(e.ret_val); m_of << ".hi = (v.hi << shift) | (v.lo >> (64 - shift));";
                        m_of << " } else {";
                        m_of << " shift -= 64;";    // Swap order and reduce shift
                        m_of << " "; emit_lvalue(e.ret_val); m_of << ".lo = (v.hi << shift) | (v.lo >> (64 - shift));";
                        m_of << " "; emit_lvalue(e.ret_val); m_of << ".hi = (v.lo << shift) | (v.hi >> (64 - shift));";
                        m_of << " }";
                    }
                    else
                    {
                        m_of << " "; emit_lvalue(e.ret_val); m_of << " = (v << shift) | (v >> (128 - shift));";
                    }
                    m_of << "}";
                    break;
                default:
                    MIR_TODO(mir_res, "rotate_left - " << ty);
                }
            }
            else if( name == "rotate_right" ) {
                const auto& ty = params.m_types.at(0);
                switch( get_real_prim_ty(ty.data().as_Primitive()) )
                {
                case ::HIR::CoreType::I8:
                case ::HIR::CoreType::U8:
                    m_of << "{";
                    m_of << " uint8_t v = "; emit_param(e.args.at(0)); m_of << ";";
                    m_of << " unsigned shift = "; emit_param(e.args.at(1)); m_of << ";";
                    m_of << " uint8_t rv = (v >> shift) | (v << (8 - shift));";
                    m_of << " "; emit_lvalue(e.ret_val); m_of << " = rv;";
                    m_of << "}";
                    break;
                case ::HIR::CoreType::I16:
                case ::HIR::CoreType::U16:
                    m_of << "{";
                    m_of << " uint16_t v = "; emit_param(e.args.at(0)); m_of << ";";
                    m_of << " unsigned shift = "; emit_param(e.args.at(1)); m_of << ";";
                    m_of << " uint16_t rv = (v >> shift) | (v << (16 - shift));";
                    m_of << " "; emit_lvalue(e.ret_val); m_of << " = rv;";
                    m_of << "}";
                    break;
                case ::HIR::CoreType::I32:
                case ::HIR::CoreType::U32:
                    m_of << "{";
                    m_of << " uint32_t v = "; emit_param(e.args.at(0)); m_of << ";";
                    m_of << " unsigned shift = "; emit_param(e.args.at(1)); m_of << ";";
                    m_of << " uint32_t rv = (v >> shift) | (v << (32 - shift));";
                    m_of << " "; emit_lvalue(e.ret_val); m_of << " = rv;";
                    m_of << "}";
                    break;
                case ::HIR::CoreType::I64:
                case ::HIR::CoreType::U64:
                    m_of << "{";
                    m_of << " uint64_t v = "; emit_param(e.args.at(0)); m_of << ";";
                    m_of << " unsigned shift = "; emit_param(e.args.at(1)); m_of << ";";
                    m_of << " uint64_t rv = (v >> shift) | (v << (64 - shift));";
                    m_of << " "; emit_lvalue(e.ret_val); m_of << " = rv;";
                    m_of << "}";
                    break;
                case ::HIR::CoreType::I128:
                case ::HIR::CoreType::U128:
                    m_of << "{";
                    m_of << " uint128_t v = "; emit_param(e.args.at(0)); m_of << ";";
                    m_of << " unsigned shift = "; emit_param(e.args.at(1)); m_of << (m_options.emulated_i128 ? ".lo" : "") << ";";
                    if( m_options.emulated_i128 )
                    {
                        m_of << " if(shift < 64) {";
                        m_of << " "; emit_lvalue(e.ret_val); m_of << ".lo = (v.lo >> shift) | (v.hi << (64 - shift));";
                        m_of << " "; emit_lvalue(e.ret_val); m_of << ".hi = (v.hi >> shift) | (v.lo << (64 - shift));";
                        m_of << " } else {";
                        m_of << " shift -= 64;";    // Swap order and reduce shift
                        m_of << " "; emit_lvalue(e.ret_val); m_of << ".lo = (v.hi >> shift) | (v.lo << (64 - shift));";
                        m_of << " "; emit_lvalue(e.ret_val); m_of << ".hi = (v.lo >> shift) | (v.hi << (64 - shift));";
                        m_of << " }";
                    }
                    else
                    {
                        m_of << " "; emit_lvalue(e.ret_val); m_of << " = (v >> shift) | (v << (128 - shift));";
                    }
                    m_of << "}";
                    break;
                default:
                    MIR_TODO(mir_res, "rotate_right - " << ty);
                }
            }
            // Bit Twiddling
            // - CounT Leading Zeroes
            // - CounT Trailing Zeroes
            else if( name == "ctlz" || name == "ctlz_nonzero" || name == "cttz" || name == "cttz_nonzero" ) {
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
                else if( ty == ::HIR::CoreType::U64 || (ty == ::HIR::CoreType::Usize && Target_GetPointerBits() > 32) )
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
                }
                else
                {
                    m_of << "__builtin_popcountll";
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
            else if( name == "maxnumf32" || name == "maxnumf64" ) {
                emit_lvalue(e.ret_val); m_of << " = ("; emit_param(e.args.at(0)); m_of << " > "; emit_param(e.args.at(1)); m_of << ") ? "; emit_param(e.args.at(0)); m_of << " : "; emit_param(e.args.at(1));
            }
            else if( name == "minnumf32" || name == "minnumf64" ) {
                emit_lvalue(e.ret_val); m_of << " = ("; emit_param(e.args.at(0)); m_of << " < "; emit_param(e.args.at(1)); m_of << ") ? "; emit_param(e.args.at(0)); m_of << " : "; emit_param(e.args.at(1));
            }
            // --- Volatile Load/Store
            else if( name == "volatile_load" ) {
                emit_lvalue(e.ret_val); m_of << " = *(volatile "; emit_ctype(params.m_types.at(0)); m_of << "*)"; emit_param(e.args.at(0));
            }
            else if( name == "volatile_store" ) {
                m_of << "*(volatile "; emit_ctype(params.m_types.at(0)); m_of << "*)"; emit_param(e.args.at(0)); m_of << " = "; emit_param(e.args.at(1));
            }
            else if( name == "nontemporal_store" ) {
                // TODO: Actually do a non-temporal store
                // GCC: _mm_stream_* (depending on input type, which must be `repr(simd)`)
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
            else if( name == "atomic_nand" || name.compare(0, 7+4+1, "atomic_nand_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+4+1);
                const auto& ty = params.m_types.at(0);
                emit_lvalue(e.ret_val); m_of << " = __mrustc_atomicloop" << get_prim_size(ty) << "(";
                    m_of << "(volatile uint" << get_prim_size(ty) << "_t*)";
                    emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1));
                    if( m_compiler == Compiler::Gcc )
                    {
                        m_of << ", " << get_atomic_ty_gcc(ordering);
                    }
                    m_of << ", __mrustc_op_and_not" << get_prim_size(ty);
                    m_of << ")";
            }
            else if( name == "atomic_or" || name.compare(0, 7+2+1, "atomic_or_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+2+1);
                emit_atomic_arith(AtomicOp::Or, ordering);
            }
            else if( name == "atomic_xor" || name.compare(0, 7+3+1, "atomic_xor_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+3+1);
                emit_atomic_arith(AtomicOp::Xor, ordering);
            }
            else if( name == "atomic_max" || name.compare(0, 7+3+1, "atomic_max_") == 0
                  || name == "atomic_min" || name.compare(0, 7+3+1, "atomic_min_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+3+1);
                const auto& ty = params.m_types.at(0);
                const char* op = (name.c_str()[7+1] == 'a' ? "imax" : "imin");    // m'a'x vs m'i'n
                emit_lvalue(e.ret_val); m_of << " = __mrustc_atomicloop" << get_prim_size(ty) << "(";
                    m_of << "(volatile uint" << get_prim_size(ty) << "_t*)";
                    emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1));
                    if( m_compiler == Compiler::Gcc )
                    {
                        m_of << ", " << get_atomic_ty_gcc(ordering);
                    }
                    m_of << ", __mrustc_op_" << op << get_prim_size(ty);
                    m_of << ")";
            }
            else if( name == "atomic_umax" || name.compare(0, 7+4+1, "atomic_umax_") == 0
                  || name == "atomic_umin" || name.compare(0, 7+4+1, "atomic_umin_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+4+1);
                const auto& ty = params.m_types.at(0);
                const char* op = (name.c_str()[7+2] == 'a' ? "umax" : "umin");    // m'a'x vs m'i'n
                emit_lvalue(e.ret_val); m_of << " = __mrustc_atomicloop" << get_prim_size(ty) << "(";
                    m_of << "(volatile uint" << get_prim_size(ty) << "_t*)";
                    emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1));
                    if( m_compiler == Compiler::Gcc )
                    {
                        m_of << ", " << get_atomic_ty_gcc(ordering);
                    }
                    m_of << ", __mrustc_op_" << op << get_prim_size(ty);
                    m_of << ")";
            }
            else if( name == "atomic_load" || name.compare(0, 7+4+1, "atomic_load_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+4+1);
                emit_lvalue(e.ret_val); m_of << " = ";
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    m_of << "atomic_load_explicit("; emit_atomic_cast(); emit_param(e.args.at(0)); m_of << ", " << get_atomic_ty_gcc(ordering) << ")";
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
                    m_of << "atomic_store_explicit("; emit_atomic_cast(); emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", " << get_atomic_ty_gcc(ordering) << ")";
                    break;
                case Compiler::Msvc:
                    m_of << "*"; emit_param(e.args.at(0)); m_of << " = "; emit_param(e.args.at(1));
                    break;
                }
            }
            // Comare+Exchange (has two orderings)
            else if( name == "atomic_cxchg_acq_failrelaxed" ) {
                emit_atomic_cxchg(e, Ordering::Acquire, Ordering::Relaxed, false);
            }
            else if( name == "atomic_cxchg_acqrel_failrelaxed" ) {
                emit_atomic_cxchg(e, Ordering::AcqRel, Ordering::Relaxed, false);
            }
            // _rel = Release, Relaxed (not Release,Release)
            else if( name == "atomic_cxchg_rel" ) {
                emit_atomic_cxchg(e, Ordering::Release, Ordering::Relaxed, false);
            }
            // _acqrel = Release, Acquire (not AcqRel,AcqRel)
            else if( name == "atomic_cxchg_acqrel" ) {
                emit_atomic_cxchg(e, Ordering::AcqRel, Ordering::Acquire, false);
            }
            else if( name.compare(0, 7+6+4, "atomic_cxchg_fail") == 0 ) {
                auto fail_ordering = get_atomic_ordering(name, 7+6+4);
                emit_atomic_cxchg(e, Ordering::SeqCst, fail_ordering, false);
            }
            else if( name == "atomic_cxchg" || name.compare(0, 7+6, "atomic_cxchg_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+6);
                emit_atomic_cxchg(e, ordering, ordering, false);
            }
            else if( name == "atomic_cxchgweak_acq_failrelaxed" ) {
                emit_atomic_cxchg(e, Ordering::Acquire, Ordering::Relaxed, true);
            }
            else if( name == "atomic_cxchgweak_acqrel_failrelaxed" ) {
                emit_atomic_cxchg(e, Ordering::AcqRel , Ordering::Relaxed, true);
            }
            else if( name.compare(0, 7+10+4, "atomic_cxchgweak_fail") == 0 ) {
                auto fail_ordering = get_atomic_ordering(name, 7+10+4);
                emit_atomic_cxchg(e, Ordering::SeqCst , fail_ordering, true);
            }
            else if( name == "atomic_cxchgweak" ) {
                emit_atomic_cxchg(e, Ordering::SeqCst , Ordering::SeqCst , true);
            }
            else if( name == "atomic_cxchgweak_acq" ) {
                emit_atomic_cxchg(e, Ordering::Acquire, Ordering::Acquire, true);
            }
            else if( name == "atomic_cxchgweak_rel" ) {
                emit_atomic_cxchg(e, Ordering::Release, Ordering::Relaxed, true);
            }
            else if( name == "atomic_cxchgweak_acqrel" ) {
                emit_atomic_cxchg(e, Ordering::AcqRel , Ordering::Acquire, true);
            }
            else if( name == "atomic_cxchgweak_relaxed" ) {
                emit_atomic_cxchg(e, Ordering::Relaxed, Ordering::Relaxed, true);
            }
            else if( name == "atomic_xchg" || name.compare(0, 7+5, "atomic_xchg_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+5);
                emit_lvalue(e.ret_val); m_of << " = ";
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    m_of << "atomic_exchange_explicit("; emit_atomic_cast(); emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", " << get_atomic_ty_gcc(ordering) << ")";
                    break;
                case Compiler::Msvc:
                    if(ordering == Ordering::Release)
                        ordering = Ordering::SeqCst;
                    emit_msvc_atomic_op("InterlockedExchange", ordering);
                    emit_param(e.args.at(0)); m_of << ", ";
                    if(params.m_types.at(0).data().is_Pointer()) { m_of << "(uintptr_t)"; } emit_param(e.args.at(1)); m_of << ")";
                    break;
                }
            }
            else if( name == "atomic_fence" || name.compare(0, 7+6, "atomic_fence_") == 0 ) {
                auto ordering = get_atomic_ordering(name, 7+6);
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    m_of << "atomic_thread_fence(" << get_atomic_ty_gcc(ordering) << ")";
                    break;
                case Compiler::Msvc:
                    // TODO: MSVC atomic fence?
                    break;
                }
            }
            else if( name == "atomic_singlethreadfence" || name.compare(0, 7+18, "atomic_singlethreadfence_") == 0 ) {
                // TODO: Does this matter?
            }
            // -- stdarg --
            else if( name == "va_copy" ) {
                m_of << "va_copy( *(va_list*)&"; emit_param(e.args.at(0)); m_of << ", *(va_list*)&"; emit_param(e.args.at(1)); m_of << ")";
            }
            // -- Platform Intrinsics --
            else if( name.compare(0, 9, "platform:") == 0 ) {
                struct SimdInfo {
                    unsigned count;
                    unsigned item_size;
                    enum Ty {
                        Float,
                        Signed,
                        Unsigned,
                    } ty;

                    static SimdInfo for_ty(const CodeGenerator_C& self, const HIR::TypeRef& ty) {
                        size_t size_slot = 0, size_val = 0;;
                        Target_GetSizeOf(self.sp, self.m_resolve, ty, size_slot);
                        const auto& ty_val = ty.data().as_Path().binding.as_Struct()->m_data.as_Tuple().at(0).ent;
                        Target_GetSizeOf(self.sp, self.m_resolve, ty_val, size_val);

                        MIR_ASSERT(*self.m_mir_res, size_slot >= size_val, size_slot << " < " << size_val);
                        MIR_ASSERT(*self.m_mir_res, size_slot / size_val * size_val == size_slot, size_slot << " not a multiple of " << size_val);

                        SimdInfo    rv;
                        rv.item_size = size_val;
                        rv.count = size_slot / size_val;
                        switch(ty_val.data().as_Primitive())
                        {
                        case ::HIR::CoreType::I8:   rv.ty = Signed; break;
                        case ::HIR::CoreType::I16:  rv.ty = Signed; break;
                        case ::HIR::CoreType::I32:  rv.ty = Signed; break;
                        case ::HIR::CoreType::I64:  rv.ty = Signed; break;
                        //case ::HIR::CoreType::I128: rv.ty = Signed; break;
                        case ::HIR::CoreType::U8:   rv.ty = Unsigned; break;
                        case ::HIR::CoreType::U16:  rv.ty = Unsigned; break;
                        case ::HIR::CoreType::U32:  rv.ty = Unsigned; break;
                        case ::HIR::CoreType::U64:  rv.ty = Unsigned; break;
                        //case ::HIR::CoreType::U128: rv.ty = Unsigned; break;
                        case ::HIR::CoreType::F32:  rv.ty = Float;  break;
                        case ::HIR::CoreType::F64:  rv.ty = Float;  break;
                        default:
                            MIR_BUG(*self.m_mir_res, "Invalid SIMD type inner - " << ty_val);
                        }
                        return rv;
                    }
                    void emit_val_ty(CodeGenerator_C& self) {
                        switch(ty)
                        {
                        case Float: self.m_of << (item_size == 4 ? "float" : "double"); break;
                        case Signed:    self.m_of << "int" << (item_size*8) << "_t";    break;
                        case Unsigned:  self.m_of << "uint" << (item_size*8) << "_t";   break;
                        }
                    }
                };

                auto simd_cmp = [&](const char* op) {
                    auto src_info = SimdInfo::for_ty(*this, params.m_types.at(0));
                    auto dst_info = SimdInfo::for_ty(*this, params.m_types.at(1));
                    MIR_ASSERT(mir_res, src_info.count == dst_info.count, "Element counts must match for " << name);
                    m_of << "for(int i = 0; i < " << dst_info.count << "; i++)";
                    m_of << "(("; dst_info.emit_val_ty(*this); m_of << "*)&"; emit_lvalue(e.ret_val); m_of << ")[i] ";
                    m_of << "= (";
                    m_of << " (("; src_info.emit_val_ty(*this); m_of << "*)&"; emit_param(e.args.at(0)); m_of << ")[i]";
                    m_of << op;
                    m_of << " (("; src_info.emit_val_ty(*this); m_of << "*)&"; emit_param(e.args.at(1)); m_of << ")[i]";
                    m_of << " )";
                    };
                auto simd_arith = [&](const char* op) {
                    auto info = SimdInfo::for_ty(*this, params.m_types.at(0));
                    // Emulate!
                    emit_lvalue(e.ret_val); m_of << " = "; emit_param(e.args.at(0)); m_of << "; ";
                    m_of << "for(int i = 0; i < " << info.count << "; i++)";
                    m_of << "(("; info.emit_val_ty(*this); m_of << "*)&"; emit_lvalue(e.ret_val); m_of << ")[i] ";
                    m_of << op << "=";
                    m_of << " (("; info.emit_val_ty(*this); m_of << "*)&"; emit_param(e.args.at(1)); m_of << ")[i]";
                    };

                // dst: T, index: usize, val: U
                // Insert a value at position
                if( name == "platform:simd_insert" ) {
                    size_t size_slot = 0, size_val = 0;
                    Target_GetSizeOf(sp, m_resolve, params.m_types.at(0), size_slot);
                    Target_GetSizeOf(sp, m_resolve, params.m_types.at(1), size_val);
                    MIR_ASSERT(mir_res, size_slot >= size_val, size_slot << " < " << size_val);
                    MIR_ASSERT(mir_res, size_slot / size_val * size_val == size_slot, size_slot << " not a multiple of " << size_val);

                    // Emulate!
                    emit_lvalue(e.ret_val); m_of << " = "; emit_param(e.args.at(0)); m_of << "; ";
                    m_of << "(( "; emit_ctype(params.m_types.at(1)); m_of << "*)&"; emit_lvalue(e.ret_val); m_of << ")["; emit_param(e.args.at(1)); m_of << "] = "; emit_param(e.args.at(2));
                }
                else if( name == "platform:simd_extract" ) {
                    size_t size_slot = 0, size_val = 0;
                    Target_GetSizeOf(sp, m_resolve, params.m_types.at(0), size_slot);
                    Target_GetSizeOf(sp, m_resolve, params.m_types.at(1), size_val);
                    MIR_ASSERT(mir_res, size_slot >= size_val, size_slot << " < " << size_val);
                    MIR_ASSERT(mir_res, size_slot / size_val * size_val == size_slot, size_slot << " not a multiple of " << size_val);

                    // Emulate!
                    emit_lvalue(e.ret_val); m_of << " = (( "; emit_ctype(params.m_types.at(1)); m_of << "*)&"; emit_param(e.args.at(0)); m_of << ")["; emit_param(e.args.at(1)); m_of << "]";
                }
                else if(
                        name == "platform:simd_shuffle128" ||
                        name == "platform:simd_shuffle64" ||
                        name == "platform:simd_shuffle32" ||
                        name == "platform:simd_shuffle16" ||
                        name == "platform:simd_shuffle8" ||
                        name == "platform:simd_shuffle4" ||
                        name == "platform:simd_shuffle2"
                        ) {
                    // Shuffle in 8 entries
                    size_t size_slot = 0;
                    Target_GetSizeOf(sp, m_resolve, params.m_types.at(1), size_slot);
                    size_t div =
                        name == "platform:simd_shuffle128" ? 128 :
                        name == "platform:simd_shuffle64" ? 64 :
                        name == "platform:simd_shuffle32" ? 32 :
                        name == "platform:simd_shuffle16" ? 16 :
                        name == "platform:simd_shuffle8" ? 8 :
                        name == "platform:simd_shuffle4" ? 4 :
                        name == "platform:simd_shuffle2" ? 2 :
                        throw ""
                        ;
                    size_t size_val = size_slot / div;
                    MIR_ASSERT(mir_res, size_val > 0, size_slot << " / " << div << " == 0?");
                    MIR_ASSERT(mir_res, size_slot >= size_val, size_slot << " < " << size_val);
                    MIR_ASSERT(mir_res, size_slot / size_val * size_val == size_slot, size_slot << " not a multiple of " << size_val);
                    m_of << "for(int i = 0; i < " << div << "; i++) { int j = "; emit_param(e.args.at(2)); m_of << ".DATA[i];";
                    m_of << "((uint" << (size_val*8) << "_t*)&"; emit_lvalue(e.ret_val); m_of << ")[i]";
                    m_of << " = ((uint" << (size_val*8) << "_t*)(j < " << div << " ? &"; emit_param(e.args.at(1)); m_of << " : &"; emit_param(e.args.at(1)); m_of << "))[j % " << div << "];";
                    m_of << "}";
                }
                else if( name == "platform:simd_cast" ) {
                    auto src_info = SimdInfo::for_ty(*this, params.m_types.at(0));
                    auto dst_info = SimdInfo::for_ty(*this, params.m_types.at(1));
                    MIR_ASSERT(mir_res, src_info.count == dst_info.count, "Element counts must match for " << name);
                    m_of << "for(int i = 0; i < " << dst_info.count << "; i++) ";
                    m_of << "(("; dst_info.emit_val_ty(*this); m_of << "*)&"; emit_lvalue(e.ret_val); m_of << ")[i] ";
                    m_of << "= (("; src_info.emit_val_ty(*this); m_of << "*)&"; emit_param(e.args.at(0)); m_of << ")[i];";
                }
                // Select between two values
                else if(name == "platform:simd_select") {
                    auto mask_info = SimdInfo::for_ty(*this, params.m_types.at(0));
                    auto val_info = SimdInfo::for_ty(*this, params.m_types.at(1));
                    MIR_ASSERT(mir_res, mask_info.count == val_info.count, "Element counts must match for " << name);
                    m_of << "for(int i = 0; i < " << val_info.count << "; i++) ";
                    m_of << "(("; val_info.emit_val_ty(*this); m_of << "*)&"; emit_lvalue(e.ret_val); m_of << ")[i] ";
                    m_of << "= (("; mask_info.emit_val_ty(*this); m_of << "*)&"; emit_param(e.args.at(0)); m_of << ")[i]";
                    m_of << "? (("; val_info.emit_val_ty(*this); m_of << "*)&"; emit_param(e.args.at(1)); m_of << ")[i]";
                    m_of << ": (("; val_info.emit_val_ty(*this); m_of << "*)&"; emit_param(e.args.at(2)); m_of << ")[i]";
                    m_of << ";";
                }
                else if(name == "platform:simd_select_bitmask") {
                    auto val_info = SimdInfo::for_ty(*this, params.m_types.at(1));
                    m_of << "for(int i = 0; i < " << val_info.count << "; i++) ";
                    m_of << "(("; val_info.emit_val_ty(*this); m_of << "*)&"; emit_lvalue(e.ret_val); m_of << ")[i] ";
                    m_of << "= (("; emit_param(e.args.at(0)); m_of << ") >> i) != 0";
                    m_of << "? (("; val_info.emit_val_ty(*this); m_of << "*)&"; emit_param(e.args.at(1)); m_of << ")[i]";
                    m_of << ": (("; val_info.emit_val_ty(*this); m_of << "*)&"; emit_param(e.args.at(2)); m_of << ")[i]";
                    m_of << ";";
                }
                // Comparisons
                else if(name == "platform:simd_eq")   simd_cmp("==");
                else if(name == "platform:simd_ne")   simd_cmp("!=");
                else if(name == "platform:simd_lt")   simd_cmp("<" );
                else if(name == "platform:simd_le")   simd_cmp("<=");
                else if(name == "platform:simd_gt")   simd_cmp(">" );
                else if(name == "platform:simd_ge")   simd_cmp(">=");
                // Arithmetic
                else if(name == "platform:simd_add")    simd_arith("+");
                else if(name == "platform:simd_sub")    simd_arith("-");
                else if(name == "platform:simd_mul")    simd_arith("*");
                else if(name == "platform:simd_div")    simd_arith("/");
                else if(name == "platform:simd_and")    simd_arith("&");
                else if(name == "platform:simd_or" )    simd_arith("|");
                else if(name == "platform:simd_xor")    simd_arith("^");

                else {
                    // TODO: Platform intrinsics
                    m_of << "assert(!\"TODO: Platform intrinsic \\\"" << name << "\\\"\")";
                    //MIR_TODO(mir_res, "Platform intrinsic " << name);
                }
            }
            else {
                MIR_BUG(mir_res, "Unknown intrinsic '" << name << "'");
            }
            m_of << ";\n";
        }

        /// slot :: The value to drop
        /// ty :: Type of value to be dropped
        /// unsized_valid :: 
        /// indent_level :: (formatting) Current amount of indenting
        void emit_destructor_call(const ::MIR::LValue& slot, const ::HIR::TypeRef& ty, bool unsized_valid, unsigned indent_level)
        {
            // If the type doesn't need dropping, don't try.
            if( !m_resolve.type_needs_drop_glue(sp, ty) )
            {
                return ;
            }
            auto indent = RepeatLitStr { "\t", static_cast<int>(indent_level) };
            TU_MATCH_HDRA( (ty.data()), {)
            // Impossible
            TU_ARMA(Diverge, te) {}
            TU_ARMA(Infer, te) {}
            TU_ARMA(ErasedType, te) {}
            TU_ARMA(Closure, te) {}
            TU_ARMA(Generator, te) {}
            TU_ARMA(Generic, te) {}

            // Nothing
            TU_ARMA(Primitive, te) {
                }
            TU_ARMA(Pointer, te) {
                }
            TU_ARMA(Function, te) {
                }
            // Has drop glue/destructors
            TU_ARMA(Borrow, te) {
                if( te.type == ::HIR::BorrowType::Owned )
                {
                    // Call drop glue on inner.
                    emit_destructor_call( ::MIR::LValue::new_Deref(slot.clone()), te.inner, true, indent_level );
                }
                }
            TU_ARMA(Path, te) {
                // Call drop glue
                // - TODO: If the destructor is known to do nothing, don't call it.
                auto p = ::HIR::Path(ty.clone(), "#drop_glue");
                const char* make_fcn = nullptr;
                switch( metadata_type(ty) )
                {
                case MetadataType::Unknown:
                    MIR_BUG(*m_mir_res, ty << " unknown metadata");
                case MetadataType::None:
                case MetadataType::Zero:
                    if( this->type_is_bad_zst(ty) && (slot.is_Field() || slot.is_Downcast()) )
                    {
                        m_of << indent << Trans_Mangle(p) << "((void*)&";
                        emit_lvalue(::MIR::LValue::CRef(slot).inner_ref());
                        m_of << ");\n";
                    }
                    else
                    {
                        m_of << indent << Trans_Mangle(p) << "(&"; emit_lvalue(slot); m_of << ");\n";
                    }
                    break;
                case MetadataType::Slice:
                    make_fcn = "make_sliceptr"; if(0)
                case MetadataType::TraitObject:
                    make_fcn = "make_traitobjptr";
                    m_of << indent << Trans_Mangle(p) << "( " << make_fcn << "(";
                    if( slot.is_Deref() )
                    {
                        emit_lvalue( ::MIR::LValue::CRef(slot).inner_ref() );
                        m_of << ".PTR";
                    }
                    else
                    {
                        m_of << "&"; emit_lvalue(slot);
                    }
                    m_of << ", ";
                    auto lvr = ::MIR::LValue::CRef(slot);
                    while(lvr.is_Field())   lvr.try_unwrap();
                    MIR_ASSERT(*m_mir_res, lvr.is_Deref(), "Access to unized type without a deref - " << lvr << " (part of " << slot << ")");
                    emit_lvalue(lvr.inner_ref()); m_of << ".META";
                    m_of << ") );\n";
                    break;
                }
                }
            TU_ARMA(Array, te) {
                // Emit destructors for all entries
                if( te.size.as_Known() > 0 )
                {
                    m_of << indent << "for(unsigned i = 0; i < " << te.size.as_Known() << "; i++) {\n";
                    emit_destructor_call(::MIR::LValue::new_Index(slot.clone(), ::MIR::LValue::Storage::MAX_ARG), te.inner, false, indent_level+1);
                    m_of << "\n" << indent << "}";
                }
                }
            TU_ARMA(Tuple, te) {
                // Emit destructors for all entries
                if( te.size() > 0 )
                {
                    ::MIR::LValue   lv = ::MIR::LValue::new_Field(slot.clone(), 0);
                    for(unsigned int i = 0; i < te.size(); i ++)
                    {
                        emit_destructor_call(lv, te[i], unsized_valid && (i == te.size()-1), indent_level);
                        lv.inc_Field();
                    }
                }
                }
            TU_ARMA(TraitObject, te) {
                MIR_ASSERT(*m_mir_res, unsized_valid, "Dropping TraitObject without an owned pointer");
                // Call destructor in vtable
                auto lvr = ::MIR::LValue::CRef(slot);
                while(lvr.is_Field())   lvr.try_unwrap();
                MIR_ASSERT(*m_mir_res, lvr.is_Deref(), "Access to unized type without a deref - " << lvr << " (part of " << slot << ")");
                m_of << indent << "((VTABLE_HDR*)"; emit_lvalue(lvr.inner_ref()); m_of << ".META)->drop(";
                if( slot.is_Deref() )
                {
                    emit_lvalue(::MIR::LValue::CRef(slot).inner_ref()); m_of << ".PTR";
                }
                else
                {
                    m_of << "&"; emit_lvalue(slot);
                }
                m_of << ");";
                }
            TU_ARMA(Slice, te) {
                MIR_ASSERT(*m_mir_res, unsized_valid, "Dropping Slice without an owned pointer");
                auto lvr = ::MIR::LValue::CRef(slot);
                while(lvr.is_Field())   lvr.try_unwrap();
                MIR_ASSERT(*m_mir_res, lvr.is_Deref(), "Access to unized type without a deref - " << lvr << " (part of " << slot << ")");
                // Call destructor on all entries
                m_of << indent << "for(unsigned i = 0; i < "; emit_lvalue(lvr.inner_ref()); m_of << ".META; i++) {\n";
                emit_destructor_call(::MIR::LValue::new_Index(slot.clone(), ::MIR::LValue::Storage::MAX_ARG), te.inner, false, indent_level+1);
                m_of << "\n" << indent << "}";
                }
            }
        }

        void emit_enum_variant_val(const TypeRepr* repr, unsigned idx)
        {
            const auto& ve = repr->variants.as_Values();
            const auto& tag_ty = Target_GetInnerType(sp, m_resolve, *repr, ve.field.index, ve.field.sub_fields);
            switch(tag_ty.data().as_Primitive())
            {
            case ::HIR::CoreType::I8:
            case ::HIR::CoreType::I16:
            case ::HIR::CoreType::I32:
            case ::HIR::CoreType::I64:
            case ::HIR::CoreType::Isize:
                m_of << static_cast<int64_t>(ve.values[idx]);
                break;
            case ::HIR::CoreType::Bool:
            case ::HIR::CoreType::U8:
            case ::HIR::CoreType::U16:
            case ::HIR::CoreType::U32:
            case ::HIR::CoreType::U64:
            case ::HIR::CoreType::Usize:
            case ::HIR::CoreType::Char:
                m_of << ve.values[idx];
                break;
            case ::HIR::CoreType::I128: // TODO: Emulation
            case ::HIR::CoreType::U128: // TODO: Emulation
                MIR_TODO(*m_mir_res, "Emulated i128 tag");
                break;
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64:
                MIR_TODO(*m_mir_res, "Floating point enum tag.");
                break;
            case ::HIR::CoreType::Str:
                MIR_BUG(*m_mir_res, "Unsized tag?!");
            }
        }

        // returns whether a literal can be represented as zeroed memory.
        bool is_zero_literal(const ::HIR::TypeRef& ty, const EncodedLiteral& lit, const Trans_Params& params) {
            for(auto v: lit.bytes)
                if(v)
                    return false;
            if(!lit.relocations.empty())
                return false;
            return true;
        }
        void emit_lvalue(const ::MIR::LValue::CRef& val)
        {
            TU_MATCH_HDRA( (val), {)
            TU_ARMA(Return, _e) {
                m_of << "rv";
                }
            TU_ARMA(Argument, e) {
                if(this->type_is_high_align(m_mir_res->m_args[e].second)) {
                    m_of << "(*";
                    m_of << "arg" << e;
                    m_of << ")";
                }
                else {
                    m_of << "arg" << e;
                }
                }
            TU_ARMA(Local, e) {
                if( e == ::MIR::LValue::Storage::MAX_ARG )
                    m_of << "i";
                else
                    m_of << "var" << e;
                }
            TU_ARMA(Static, e) {
                m_of << Trans_Mangle(e);
                m_of << ".val";
                }
            TU_ARMA(Field, field_index) {
                ::HIR::TypeRef  tmp;
                auto inner = val.inner_ref();
                const auto& ty = m_mir_res->get_lvalue_type(tmp, inner);
                if( ty.data().is_Slice() )
                {
                    if( inner.is_Deref() )
                    {
                        m_of << "(("; emit_ctype(ty.data().as_Slice().inner); m_of << "*)";
                        emit_lvalue(inner.inner_ref());
                        m_of << ".PTR)";
                    }
                    else
                    {
                        emit_lvalue(inner);
                    }
                    m_of << "[" << field_index << "]";
                }
                else if( ty.data().is_Array() ) {
                    emit_lvalue(inner);
                    m_of << ".DATA[" << field_index << "]";
                }
                else if( inner.is_Deref() ) {
                    auto dst_type = metadata_type(ty);
                    if( dst_type != MetadataType::None )
                    {
                        m_of << "(("; emit_ctype(ty); m_of << "*)"; emit_lvalue(inner.inner_ref()); m_of << ".PTR)->_" << field_index;
                    }
                    else
                    {
                        emit_lvalue(inner.inner_ref());
                        m_of << "->_" << field_index;
                    }
                }
                else {
                    emit_lvalue(inner);
                    m_of << "._" << field_index;
                }
                }
            TU_ARMA(Deref, _e) {
                auto inner = val.inner_ref();
                ::HIR::TypeRef  tmp;
                const auto& ty = m_mir_res->get_lvalue_type(tmp, val);
                auto dst_type = metadata_type(ty);
                // If the type is unsized, then this pointer is a fat pointer, so we need to cast the data pointer.
                if( dst_type != MetadataType::None )
                {
                    m_of << "(*("; emit_ctype(ty); m_of << "*)";
                    emit_lvalue(inner);
                    m_of << ".PTR)";
                }
                else
                {
                    m_of << "(*";
                    emit_lvalue(inner);
                    m_of << ")";
                }
                }
            TU_ARMA(Index, index_local) {
                auto inner = val.inner_ref();
                ::HIR::TypeRef  tmp;
                const auto& ty = m_mir_res->get_lvalue_type(tmp, inner);
                m_of << "(";
                if( ty.data().is_Slice() ) {
                    if( inner.is_Deref() )
                    {
                        m_of << "("; emit_ctype(ty.data().as_Slice().inner); m_of << "*)";
                        emit_lvalue(inner.inner_ref());
                        m_of << ".PTR";
                    }
                    else {
                        emit_lvalue(inner);
                    }
                }
                else if( ty.data().is_Array() ) {
                    emit_lvalue(inner);
                    m_of << ".DATA";
                }
                else {
                    emit_lvalue(inner);
                }
                m_of << ")[";
                emit_lvalue(::MIR::LValue::new_Local(index_local));
                m_of << "]";
                }
            TU_ARMA(Downcast, variant_index) {
                auto inner = val.inner_ref();
                ::HIR::TypeRef  tmp;
                const auto& ty = m_mir_res->get_lvalue_type(tmp, inner);
                emit_lvalue(inner);
                MIR_ASSERT(*m_mir_res, ty.data().is_Path(), "Downcast on non-Path type - " << ty);
                if( ty.data().as_Path().binding.is_Enum() )
                {
                    m_of << ".DATA";
                }
                m_of << ".var_" << variant_index;
                }
            }
        }
        void emit_lvalue(const ::MIR::LValue& val) {
            emit_lvalue( ::MIR::LValue::CRef(val) );
        }
        void emit_constant(const ::MIR::Constant& ve, const ::MIR::LValue* dst_ptr=nullptr)
        {
            TU_MATCH_HDRA( (ve), {)
            TU_ARMA(Int, c) {
                if( c.v == INT64_MIN )
                {
                    if( m_options.emulated_i128 && c.t == ::HIR::CoreType::I128 )
                    {
                        m_of << "make128s(INT64_MIN)";
                    }
                    else
                    {
                        m_of << "INT64_MIN";
                    }
                }
                else
                {
                    switch(c.t)
                    {
                    // TODO: These should already have been truncated/reinterpreted, but just in case.
                    case ::HIR::CoreType::I8:
                        m_of << static_cast<int>( static_cast<int8_t>(c.v) );   // cast to int, because `int8_t` is printed as a `char`
                        break;
                    case ::HIR::CoreType::I16:
                        m_of << static_cast<int16_t>(c.v);
                        break;
                    case ::HIR::CoreType::I32:
                        m_of << static_cast<int32_t>(c.v);
                        break;
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
                            m_of << "(int128_t)";
                            m_of << c.v;
                            m_of << "ll";
                        }
                        break;
                    default:
                        m_of << c.v;
                        break;
                    }
                }
                }
            TU_ARMA(Uint, c) {
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
                        m_of << "(uint128_t)";
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
                }
            TU_ARMA(Float, c) {
                this->emit_float(c.v);
                }
            TU_ARMA(Bool, c) {
                m_of << (c.v ? "true" : "false");
                }
            TU_ARMA(Bytes, c) {
                // Array borrow : Cast the C string to the array
                // - Laziness
                m_of << "(void*)";
                this->print_escaped_string(c);
                }
            TU_ARMA(StaticString, c) {
                m_of << "make_sliceptr(";
                this->print_escaped_string(c);
                m_of << ", " << ::std::dec << c.size() << ")";
                }
            TU_ARMA(Const, c) {
                MIR_BUG(*m_mir_res, "Unexpected Constant::Const - " << ve);
                }
            TU_ARMA(Generic, c) {
                MIR_BUG(*m_mir_res, "Generic value present at codegen");
                }
            TU_ARMA(ItemAddr, c) {
                bool  is_fcn = false;
                MonomorphState  ms_tmp;
                auto v = m_resolve.get_value(sp, *c, ms_tmp, /*signature_only=*/true);
                is_fcn = v.is_Function() || v.is_EnumConstructor() || v.is_StructConstructor();
                if(!is_fcn) {
                    m_of << "&";
                }
                m_of << Trans_Mangle(*c);
                if(!is_fcn) {
                    m_of << ".val";
                }
                }
            }
        }
        void emit_param(const ::MIR::Param& p) {
            TU_MATCH_HDRA( (p), {)
            TU_ARMA(LValue, e) {
                emit_lvalue(e);
                }
            TU_ARMA(Borrow, e) {
                emit_borrow(*m_mir_res, e.type, e.val);
                }
            TU_ARMA(Constant, e) {
                emit_constant(e);
                }
            }
        }
        void emit_ctype(const ::HIR::TypeRef& ty) {
            emit_ctype(ty, FMT_CB(_,));
        }
        void emit_ctype(const ::HIR::TypeRef& ty, ::FmtLambda inner, bool is_extern_c=false) {
            TU_MATCH_HDRA( (ty.data()), {)
            TU_ARMA(Infer, te) {
                m_of << "@" << ty << "@" << inner;
                }
            TU_ARMA(Diverge, te) {
                m_of << "tBANG " << inner;
                }
            TU_ARMA(Primitive, te) {
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

                case ::HIR::CoreType::Bool: m_of << "RUST_BOOL"; break;
                case ::HIR::CoreType::Char: m_of << "RUST_CHAR";  break;
                case ::HIR::CoreType::Str:
                    MIR_BUG(*m_mir_res, "Raw str");
                }
                m_of << " " << inner;
                }
            TU_ARMA(Path, te) {
                //if( const auto* ity = m_resolve.is_type_owned_box(ty) ) {
                //    emit_ctype_ptr(*ity, inner);
                //    return ;
                //}
                TU_MATCH_HDRA( (te.binding), { )
                TU_ARMA(Struct, tpb) {
                    m_of << "struct s_" << Trans_Mangle(te.path);
                    }
                TU_ARMA(Union, tpb) {
                    m_of << "union u_" << Trans_Mangle(te.path);
                    }
                TU_ARMA(Enum, tpb) {
                    m_of << "struct e_" << Trans_Mangle(te.path);
                    }
                TU_ARMA(ExternType, tpb) {
                    m_of << "struct x_" << Trans_Mangle(te.path);
                    }
                TU_ARMA(Unbound, tpb) {
                    MIR_BUG(*m_mir_res, "Unbound type path in trans - " << ty);
                    }
                TU_ARMA(Opaque, tpb) {
                    MIR_BUG(*m_mir_res, "Opaque path in trans - " << ty);
                    }
                }
                m_of << " " << inner;
                }
            TU_ARMA(Generic, te) {
                MIR_BUG(*m_mir_res, "Generic in trans - " << ty);
                }
            TU_ARMA(TraitObject, te) {
                MIR_BUG(*m_mir_res, "Raw trait object - " << ty);
                }
            TU_ARMA(ErasedType, te) {
                MIR_BUG(*m_mir_res, "ErasedType in trans - " << ty);
                }
            TU_ARMA(Array, te) {
                m_of << "t_" << Trans_Mangle(ty) << " " << inner;
                //emit_ctype(te.inner, inner);
                //m_of << "[" << te.size.as_Known() << "]";
                }
            TU_ARMA(Slice, te) {
                MIR_BUG(*m_mir_res, "Raw slice object - " << ty);
                }
            TU_ARMA(Tuple, te) {
                if( te.size() == 0 )
                    m_of << "tUNIT";
                else {
                    m_of << "TUP_" << te.size();
                    for(const auto& t : te)
                    {
                        m_of << "_" << Trans_Mangle(t);
                    }
                }
                m_of << " " << inner;
                }
            TU_ARMA(Borrow, te) {
                emit_ctype_ptr(te.inner, inner);
                }
            TU_ARMA(Pointer, te) {
                emit_ctype_ptr(te.inner, inner);
                }
            TU_ARMA(Function, te) {
                m_of << "t_" << Trans_Mangle(ty) << " " << inner;
                }
                break;
            case ::HIR::TypeData::TAG_Closure:
            case ::HIR::TypeData::TAG_Generator:
                MIR_BUG(*m_mir_res, "Closure during trans - " << ty);
                break;
            }
        }

        ::HIR::TypeRef get_inner_unsized_type(const ::HIR::TypeRef& ty)
        {
            if( ty == ::HIR::CoreType::Str || ty.data().is_Slice() ) {
                return ty.clone();
            }
            else if( ty.data().is_TraitObject() ) {
                return ty.clone();
            }
            else if( ty.data().is_Path() )
            {
                TU_MATCH_HDRA( (ty.data().as_Path().binding), {)
                default:
                    MIR_BUG(*m_mir_res, "Unbound/opaque path in trans - " << ty);
                    throw "";
                TU_ARMA(Struct, tpb) {
                    switch( tpb->m_struct_markings.dst_type )
                    {
                    case ::HIR::StructMarkings::DstType::None:
                        return ::HIR::TypeRef();
                    case ::HIR::StructMarkings::DstType::Slice:
                    case ::HIR::StructMarkings::DstType::TraitObject:
                    case ::HIR::StructMarkings::DstType::Possible: {
                        // TODO: How to figure out? Lazy way is to check the monomorpised type of the last field (structs only)
                        const auto& path = ty.data().as_Path().path.m_data.as_Generic();
                        const auto& str = *ty.data().as_Path().binding.as_Struct();
                        auto monomorph = [&](const auto& tpl) {
                            return m_resolve.monomorph_expand(sp, tpl, MonomorphStatePtr(nullptr, &path.m_params, nullptr));
                            };
                        TU_MATCH_HDRA( (str.m_data), { )
                        TU_ARMA(Unit, se) MIR_BUG(*m_mir_res, "Unit-like struct with DstType::Possible");
                        TU_ARMA(Tuple,se) return get_inner_unsized_type( monomorph(se.back().ent) );
                        TU_ARMA(Named,se) return get_inner_unsized_type( monomorph(se.back().second.ent) );
                        }
                        throw "";
                        }
                    }
                    }
                TU_ARMA(Union, tpb) {
                    return ::HIR::TypeRef();
                    }
                TU_ARMA(Enum, tpb) {
                    return ::HIR::TypeRef();
                    }
                }
                throw "";
            }
            else
            {
                return ::HIR::TypeRef();
            }
        }

        MetadataType metadata_type(const ::HIR::TypeRef& ty) const {
            return m_resolve.metadata_type(m_mir_res ? m_mir_res->sp : sp, ty);
        }

        void emit_ctype_ptr(const ::HIR::TypeRef& inner_ty, ::FmtLambda inner) {
            //if( inner_ty.data().is_Array() ) {
            //    emit_ctype(inner_ty, FMT_CB(ss, ss << "(*" << inner << ")";));
            //}
            //else
            {
                switch( this->metadata_type(inner_ty) )
                {
                case MetadataType::Unknown:
                    BUG(sp, inner_ty << " unknown metadata type");
                case MetadataType::None:
                case MetadataType::Zero:
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
            switch(this->metadata_type(ty))
            {
            case MetadataType::Unknown:
                BUG(sp, ty << " unknown metadata type");
            case MetadataType::None:
            case MetadataType::Zero:
                return false;
            case MetadataType::Slice:
            case MetadataType::TraitObject:
                return true;
            }
            return false;
        }
    };
    Span CodeGenerator_C::sp;
}

::std::unique_ptr<CodeGenerator> Trans_Codegen_GetGeneratorC(const ::HIR::Crate& crate, const ::std::string& outfile)
{
    return ::std::unique_ptr<CodeGenerator>(new CodeGenerator_C(crate, outfile));
}
