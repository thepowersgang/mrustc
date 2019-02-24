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

        ::std::vector< ::std::pair< ::HIR::GenericPath, const ::HIR::Struct*> >   m_box_glue_todo;
        ::std::set< ::HIR::TypeRef> m_emitted_fn_types;
    public:
        CodeGenerator_C(const ::HIR::Crate& crate, const ::std::string& outfile):
            m_crate(crate),
            m_resolve(crate),
            m_outfile_path(outfile),
            m_outfile_path_c(outfile + ".c"),
            m_of(m_outfile_path_c)
        {
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
            //case Compilter::Std11:
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
                break;
            case Compiler::Msvc:
                m_of
                    << "static inline uint64_t __builtin_popcount(uint64_t v) {\n"
                    << "\treturn (v >> 32 != 0 ? __popcnt64(v>>32) : 32 + __popcnt64(v));\n"
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
                    << "\tif(a > UINT8_MAX/b)  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_i8(int8_t a, int8_t b, int8_t* out) {\n"
                    << "\t*out = a*b;\n"    // Wait, this isn't valid?
                    << "\tif(a > INT8_MAX/b)  return true;\n"
                    << "\tif(a < INT8_MIN/b)  return true;\n"
                    << "\tif( (a == -1) && (b == INT8_MIN))  return true;\n"
                    << "\tif( (b == -1) && (a == INT8_MIN))  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_u16(uint16_t a, uint16_t b, uint16_t* out) {\n"
                    << "\t*out = a*b;\n"
                    << "\tif(a > UINT16_MAX/b)  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_i16(int16_t a, int16_t b, int16_t* out) {\n"
                    << "\t*out = a*b;\n"    // Wait, this isn't valid?
                    << "\tif(a > INT16_MAX/b)  return true;\n"
                    << "\tif(a < INT16_MIN/b)  return true;\n"
                    << "\tif( (a == -1) && (b == INT16_MIN))  return true;\n"
                    << "\tif( (b == -1) && (a == INT16_MIN))  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_u32(uint32_t a, uint32_t b, uint32_t* out) {\n"
                    << "\t*out = a*b;\n"
                    << "\tif(a > UINT32_MAX/b)  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_i32(int32_t a, int32_t b, int32_t* out) {\n"
                    << "\t*out = a*b;\n"    // Wait, this isn't valid?
                    << "\tif(a > INT32_MAX/b)  return true;\n"
                    << "\tif(a < INT32_MIN/b)  return true;\n"
                    << "\tif( (a == -1) && (b == INT32_MIN))  return true;\n"
                    << "\tif( (b == -1) && (a == INT32_MIN))  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_u64(uint64_t a, uint64_t b, uint64_t* out) {\n"
                    << "\t*out = a*b;\n"
                    << "\tif(a > UINT64_MAX/b)  return true;\n"
                    << "\treturn false;\n"
                    << "}\n"
                    << "static inline bool __builtin_mul_overflow_i64(int64_t a, int64_t b, int64_t* out) {\n"
                    << "\t*out = a*b;\n"    // Wait, this isn't valid?
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
                    << "static inline _subcarry_u64(uint64_t a, uint64_t b, uint64_t* o) {\n"
                    << "\t""*o = a - b;\n"
                    << "\t""return (a > b ? *o >= b : *o > a);\n"
                    << "}\n"
                    << "static inline _subcarry_u32(uint32_t a, uint32_t b, uint32_t* o) {\n"
                    << "\t""*o = a - b;\n"
                    << "\t""return (a > b ? *o >= b : *o > a);\n"
                    << "}\n"
                    << "static inline _subcarry_u16(uint16_t a, uint16_t b, uint16_t* o) {\n"
                    << "\t""*o = a - b;\n"
                    << "\t""return (a > b ? *o >= b : *o > a);\n"
                    << "}\n"
                    << "static inline _subcarry_u8(uint8_t a, uint8_t b, uint8_t* o) {\n"
                    << "\t""*o = a - b;\n"
                    << "\t""return (a > b ? *o >= b : *o > a);\n"
                    << "}\n"
                    << "static inline uint64_t __builtin_bswap64(uint64_t v) { return _byteswap_uint64(v); }\n"
                    << "static inline uint8_t InterlockedCompareExchange8(volatile uint8_t* v, uint8_t n, uint8_t e){ return _InterlockedCompareExchange8(v, n, e); }\n"
                    << "static inline uint8_t InterlockedCompareExchangeNoFence8(volatile uint8_t* v, uint8_t n, uint8_t e){ return InterlockedCompareExchange8(v, n, e); }\n"
                    << "static inline uint8_t InterlockedCompareExchangeAcquire8(volatile uint8_t* v, uint8_t n, uint8_t e){ return InterlockedCompareExchange8(v, n, e); }\n"
                    << "static inline uint8_t InterlockedCompareExchangeRelease8(volatile uint8_t* v, uint8_t n, uint8_t e){ return InterlockedCompareExchange8(v, n, e); }\n"
                    //<< "static inline uint8_t InterlockedExchange8(volatile uint8_t* v, uint8_t n){ return _InterlockedExchange8((volatile char*)v, (char)n); }\n"
                    << "static inline uint8_t InterlockedExchangeNoFence8(volatile uint8_t* v, uint8_t n){ return InterlockedExchange8(v, n); }\n"
                    << "static inline uint8_t InterlockedExchangeAcquire8(volatile uint8_t* v, uint8_t n){ return InterlockedExchange8(v, n); }\n"
                    << "static inline uint8_t InterlockedExchangeRelease8(volatile uint8_t* v, uint8_t n){ return InterlockedExchange8(v, n); }\n"
                    ;
                break;
            }

            if( m_options.emulated_i128 )
            {
                m_of
                    << "typedef struct { uint64_t lo, hi; } uint128_t;\n"
                    << "typedef struct { uint64_t lo, hi; } int128_t;\n"
                    << "static inline float make_float(int is_neg, int exp, uint32_t mantissa_bits) { float rv; uint32_t vi=(mantissa_bits&((1<<23)-1))|((exp+127)<<23);if(is_neg)vi|=1<<31; memcpy(&rv, &vi, 4); return rv; }\n"
                    << "static inline double make_double(int is_neg, int exp, uint32_t mantissa_bits) { double rv; uint64_t vi=(mantissa_bits&((1ull<<52)-1))|((uint64_t)(exp+1023)<<52);if(is_neg)vi|=1ull<<63; memcpy(&rv, &vi, 4); return rv; }\n"
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
                    << "static inline int128_t shr128s(int128_t a, uint32_t b) { int128_t v; if(b == 0) { return a; } else if(b < 64) { v.lo = (a.lo >> b)|(a.hi << (64 - b)); v.hi = a.hi >> b; } else { v.lo = a.hi >> (b - 64); v.hi = 0; } return v; }\n"
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

            // TODO: Define this function in MIR.
            if( is_executable )
            {
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

                if( m_compiler == Compiler::Gcc )
                {
                    m_of
                        << "__thread jmp_buf* mrustc_panic_target;\n"
                        << "__thread void* mrustc_panic_value;\n"
                        ;
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
#ifdef _WIN32
            bool is_windows = true;
#else
            bool is_windows = false;
#endif
            switch( m_compiler )
            {
            case Compiler::Gcc:
                // Pick the compiler
                // - from `CC-${TRIPLE}` environment variable
                // - from the $CC environment variable
                // - `gcc-${TRIPLE}` (if available)
                // - `gcc` as fallback
                {
                    ::std::string varname = "CC-" +  Target_GetCurSpec().m_backend_c.m_c_compiler;
                    if( getenv(varname.c_str()) ) {
                        args.push_back( getenv(varname.c_str()) );
                    }
                    else if (system(("which " + Target_GetCurSpec().m_backend_c.m_c_compiler + "-gcc" + " >/dev/null 2>&1").c_str()) == 0) {
                        args.push_back( Target_GetCurSpec().m_backend_c.m_c_compiler + "-gcc" );
                    }
                    else if( getenv("CC") ) {
                        args.push_back( getenv("CC") );
                    }
                    else {
                        args.push_back("gcc");
                    }
                }
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
                    for( const auto& a : Target_GetCurSpec().m_backend_c.m_linker_opts )
                    {
                        args.push_back( a.c_str() );
                    }
                }
                else
                {
                    args.push_back("-c");
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
                    args.push_back("/Zi");
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

                    args.push_back("/link");

                    // Command-line specified linker search directories
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

            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), struct_ty_ptr, args, empty_fcn };
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
            if( m_emitted_fn_types.count(ty) ) {
                return ;
            }
            m_emitted_fn_types.insert(ty.clone());

            const auto& te = ty.m_data.as_Function();
            m_of << "typedef ";
            // TODO: ABI marker, need an ABI enum?
            if( *te.m_rettype == ::HIR::TypeRef::new_unit() )
                m_of << "void";
            else
                // TODO: Better emit_ctype call for return type?
                emit_ctype(*te.m_rettype);
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
                    emit_ctype(te.m_arg_types[i]);
                }
                m_of << " )";
            }
            m_of << ";";
        }
        void emit_type(const ::HIR::TypeRef& ty) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "type " << ty;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;

            TRACE_FUNCTION_F(ty);
            TU_IFLET( ::HIR::TypeRef::Data, ty.m_data, Tuple, te,
                if( te.size() > 0 )
                {
                    m_of << "typedef struct "; emit_ctype(ty); m_of << " {\n";
                    unsigned n_fields = 0;
                    for(unsigned int i = 0; i < te.size(); i++)
                    {
                        m_of << "\t";
                        size_t s, a;
                        Target_GetSizeAndAlignOf(sp, m_resolve, te[i], s, a);
                        if( s == 0 && m_options.disallow_empty_structs ) {
                            m_of << "// ZST: " << te[i] << "\n";
                            continue ;
                        }
                        else {
                            emit_ctype(te[i], FMT_CB(ss, ss << "_" << i;));
                            m_of << ";\n";
                            n_fields += 1;
                        }
                    }
                    if( n_fields == 0 && m_options.disallow_empty_structs )
                    {
                        m_of << "\tchar _d;\n";
                    }
                    m_of << "} "; emit_ctype(ty); m_of << ";\n";
                }

                auto drop_glue_path = ::HIR::Path(ty.clone(), "#drop_glue");
                auto args = ::std::vector< ::std::pair<::HIR::Pattern,::HIR::TypeRef> >();
                auto ty_ptr = ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Owned, ty.clone());
                ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), ty_ptr, args, empty_fcn };
                m_mir_res = &mir_res;
                m_of << "static void " << Trans_Mangle(drop_glue_path) << "("; emit_ctype(ty); m_of << "* rv) {\n";
                if( m_resolve.type_needs_drop_glue(sp, ty) )
                {
                    auto self = ::MIR::LValue::make_Deref({ box$(::MIR::LValue::make_Return({})) });
                    auto fld_lv = ::MIR::LValue::make_Field({ box$(self), 0 });
                    for(const auto& ity : te)
                    {
                        // TODO: What if it's a ZST?
                        emit_destructor_call(fld_lv, ity, /*unsized_valid=*/false, 1);
                        fld_lv.as_Field().field_index ++;
                    }
                }
                m_of << "}\n";
            )
            else TU_IFLET( ::HIR::TypeRef::Data, ty.m_data, Function, te,
                emit_type_fn(ty);
                m_of << " // " << ty << "\n";
            )
            else if( const auto* te = ty.m_data.opt_Array() )
            {
                m_of << "typedef struct "; emit_ctype(ty); m_of << " { ";
                if( te->size_val == 0 && m_options.disallow_empty_structs )
                {
                    m_of << "char _d;";
                }
                else
                {
                    emit_ctype(*te->inner); m_of << " DATA[" << te->size_val << "];";
                }
                m_of << " } "; emit_ctype(ty); m_of << ";";
                m_of << " // " << ty << "\n";
            }
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
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "struct " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;
            bool is_packed = item.m_repr == ::HIR::Struct::Repr::Packed;

            TRACE_FUNCTION_F(p);
            auto item_ty = ::HIR::TypeRef::new_path(p.clone(), &item);
            const auto* repr = Target_GetTypeRepr(sp, m_resolve, item_ty);

            ::std::vector<unsigned> fields;
            for(const auto& ent : repr->fields)
            {
                (void)ent;
                fields.push_back(fields.size());
            }
            ::std::sort(fields.begin(), fields.end(), [&](auto a, auto b){ return repr->fields[a].offset < repr->fields[b].offset; });

            m_of << "// struct " << p << "\n";

            // Determine if the type has an alignment hack
            bool has_manual_align = false;
            for(unsigned fld : fields )
            {
                const auto& ty = repr->fields[fld].ty;
                if( ty.m_data.is_Array() && ty.m_data.as_Array().size_val == 0 ) {
                    has_manual_align = true;
                }
            }

            // For repr(packed), mark as packed
            if(is_packed)
            {
                switch(m_compiler)
                {
                case Compiler::Msvc:
                    m_of << "#pragma pack(push, 1)\n";
                    break;
                case Compiler::Gcc:
                    break;
                }
            }
            if(has_manual_align)
            {
                switch(m_compiler)
                {
                case Compiler::Msvc:
                    m_of << "#pragma align(push, " << repr->align << ")\n";
                    break;
                case Compiler::Gcc:
                    break;
                }
            }
            m_of << "struct s_" << Trans_Mangle(p) << " {\n";

            bool has_unsized = false;
            size_t sized_fields = 0;
            for(unsigned fld : fields)
            {
                m_of << "\t";
                const auto& ty = repr->fields[fld].ty;

                if( const auto* te = ty.m_data.opt_Slice() ) {
                    emit_ctype( *te->inner, FMT_CB(ss, ss << "_" << fld << "[0]";) );
                    has_unsized = true;
                }
                else TU_IFLET(::HIR::TypeRef::Data, ty.m_data, TraitObject, te,
                    m_of << "unsigned char _" << fld << "[0]";
                    has_unsized = true;
                )
                else if( ty == ::HIR::CoreType::Str ) {
                    m_of << "uint8_t _" << fld << "[0]";
                    has_unsized = true;
                }
                else {
                    size_t s, a;
                    Target_GetSizeAndAlignOf(sp, m_resolve, ty, s, a);
                    if( s == 0 && m_options.disallow_empty_structs ) {
                        m_of << "// ZST: " << ty << "\n";
                        continue ;
                    }
                    else {

                        // TODO: Nested unsized?
                        emit_ctype( ty, FMT_CB(ss, ss << "_" << fld) );
                        sized_fields ++;

                        has_unsized |= (s == SIZE_MAX);
                    }
                }
                m_of << ";\n";
            }
            if( sized_fields == 0 && !has_unsized && m_options.disallow_empty_structs )
            {
                m_of << "\tchar _d;\n";
            }
            m_of << "}";
            if(is_packed || has_manual_align)
            {
                switch(m_compiler)
                {
                case Compiler::Msvc:
                    if( is_packed )
                        m_of << ";\n#pragma pack(pop)\n";
                    if( has_manual_align )
                        m_of << ";\n#pragma align(pop)\n";
                    break;
                case Compiler::Gcc:
                    m_of << " __attribute__((";
                    if( is_packed )
                        m_of << "packed,";
                    if( has_manual_align )
                        m_of << "__aligned__(" << repr->align << "),";
                    m_of << "));\n";
                    break;
                }
            }
            else
            {
                m_of << ";\n";
            }
            (void)has_unsized;
            if( true && repr->size > 0 && !has_unsized )
            {
                // TODO: Handle unsized (should check the size of the fixed-size region)
                m_of << "typedef char sizeof_assert_" << Trans_Mangle(p) << "[ (sizeof(struct s_" << Trans_Mangle(p) << ") == " << repr->size << ") ? 1 : -1 ];\n";
                //m_of << "typedef char alignof_assert_" << Trans_Mangle(p) << "[ (ALIGNOF(struct s_" << Trans_Mangle(p) << ") == " << repr->align << ") ? 1 : -1 ];\n";
            }

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
                m_of << "void " << Trans_Mangle( ::HIR::Path(struct_ty.clone(), m_resolve.m_lang_Drop, "drop") ) << "("; emit_ctype(struct_ty_ptr, FMT_CB(ss, ss << "rv";)); m_of << ");\n";
            }
            else if( m_resolve.is_type_owned_box(struct_ty) )
            {
                m_box_glue_todo.push_back( ::std::make_pair( mv$(struct_ty.m_data.as_Path().path.m_data.as_Generic()), &item ) );
                m_of << "static void " << Trans_Mangle(drop_glue_path) << "("; emit_ctype(struct_ty_ptr, FMT_CB(ss, ss << "rv";)); m_of << ");\n";
                return ;
            }

            ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), struct_ty_ptr, args, empty_fcn };
            m_mir_res = &mir_res;
            m_of << "static void " << Trans_Mangle(drop_glue_path) << "("; emit_ctype(struct_ty_ptr, FMT_CB(ss, ss << "rv";)); m_of << ") {\n";
            if( m_resolve.type_needs_drop_glue(sp, item_ty) )
            {
                // If this type has an impl of Drop, call that impl
                if( item.m_markings.has_drop_impl ) {
                    m_of << "\t" << Trans_Mangle( ::HIR::Path(struct_ty.clone(), m_resolve.m_lang_Drop, "drop") ) << "(rv);\n";
                }

                auto self = ::MIR::LValue::make_Deref({ box$(::MIR::LValue::make_Return({})) });
                auto fld_lv = ::MIR::LValue::make_Field({ box$(self), 0 });
                for(size_t i = 0; i < repr->fields.size(); i++)
                {
                    fld_lv.as_Field().field_index = i;

                    emit_destructor_call(fld_lv, repr->fields[i].ty, /*unsized_valid=*/true, /*indent=*/1);
                }
            }
            m_of << "}\n";
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

            // Drop glue (calls destructor if there is one)
            auto drop_glue_path = ::HIR::Path(item_ty.clone(), "#drop_glue");
            auto item_ptr_ty = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Owned, item_ty.clone());
            auto drop_impl_path = (item.m_markings.has_drop_impl ? ::HIR::Path(item_ty.clone(), m_resolve.m_lang_Drop, "drop") : ::HIR::Path(::HIR::SimplePath()));
            ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), item_ptr_ty, {}, empty_fcn };
            m_mir_res = &mir_res;

            if( item.m_markings.has_drop_impl )
            {
                m_of << "void " << Trans_Mangle(drop_impl_path) << "(union u_" << Trans_Mangle(p) << "*rv);\n";
            }

            m_of << "static void " << Trans_Mangle(drop_glue_path) << "(union u_" << Trans_Mangle(p) << "* rv) {\n";
            if( item.m_markings.has_drop_impl )
            {
                m_of << "\t" << Trans_Mangle(drop_impl_path) << "(rv);\n";
            }
            m_of << "}\n";
        }

        void emit_enum_path(const TypeRepr* repr, const TypeRepr::FieldPath& path)
        {
            if( TU_TEST1(repr->variants, Values, .field.index == path.index) )
            {
                m_of << ".TAG";
            }
            else
            {
                m_of << ".DATA.var_" << path.index;
            }
            const auto* ty = &repr->fields[path.index].ty;
            for(auto fld : path.sub_fields)
            {
                repr = Target_GetTypeRepr(sp, m_resolve, *ty);
                ty = &repr->fields[fld].ty;
                m_of << "._" << fld;
            }
            if( const auto* te = ty->m_data.opt_Borrow() )
            {
                if( metadata_type(*te->inner) != MetadataType::None ) {
                    m_of << ".PTR";
                }
            }
            else if( const auto* te = ty->m_data.opt_Pointer() )
            {
                if( metadata_type(*te->inner) != MetadataType::None ) {
                    m_of << ".PTR";
                }
            }
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
                // Avoid placing the tag in the union
                if( repr->variants.is_Values() && i == repr->variants.as_Values().field.index )
                    continue ;
                if( repr->fields[i].offset == repr->fields[0].offset )
                {
                    union_fields.push_back(i);
                }
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
            // If there multiple fields with the same offset, they're the data variants
            else if( union_fields.size() > 0 )
            {
                assert(1 + union_fields.size() + 1 >= repr->fields.size());
                // Make the union!
                // NOTE: The way the structure generation works is that enum variants are always first, so the field index = the variant index
                // TODO:
                if( !this->type_is_bad_zst(repr->fields[0].ty) || ::std::any_of(union_fields.begin(), union_fields.end(), [this,repr](auto x){ return !this->type_is_bad_zst(repr->fields[x].ty); }) )
                {
                    m_of << "\tunion {\n";
                    // > First field
                    {
                        m_of << "\t\t";
                        const auto& ty = repr->fields[0].ty;
                        if( this->type_is_bad_zst(ty) ) {
                            m_of << "// ZST: " << ty << "\n";
                        }
                        else {
                            emit_ctype( ty, FMT_CB(ss, ss << "var_0") );
                            m_of << ";\n";
                            //sized_fields ++;
                        }
                    }
                    // > All others
                    for(auto idx : union_fields)
                    {
                        m_of << "\t\t";

                        const auto& ty = repr->fields[idx].ty;
                        if( this->type_is_bad_zst(ty) ) {
                            m_of << "// ZST: " << ty << "\n";
                        }
                        else {
                            emit_ctype( ty, FMT_CB(ss, ss << "var_" << idx) );
                            m_of << ";\n";
                            //sized_fields ++;
                        }
                    }
                    m_of << "\t} DATA;\n";
                }

                if( repr->fields.size() == 1 + union_fields.size() )
                {
                    // No tag, the tag is in one of the fields.
                    DEBUG("Untagged, nonzero or other");
                }
                else
                {
                    //assert(repr->fields.back().offset != repr->fields.front().offset);
                    DEBUG("Tag present at offset " << repr->fields.back().offset << " - " << repr->fields.back().ty);

                    m_of << "\t";
                    emit_ctype(repr->fields.back().ty, FMT_CB(os, os << "TAG"));
                    m_of << ";\n";
                }
            }
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
            if( true && repr->size > 0 )
            {
                m_of << "typedef char sizeof_assert_" << Trans_Mangle(p) << "[ (sizeof(struct e_" << Trans_Mangle(p) << ") == " << repr->size << ") ? 1 : -1 ];\n";
            }

            // ---
            // - Drop Glue
            // ---
            auto struct_ty = ::HIR::TypeRef(p.clone(), &item);
            auto drop_glue_path = ::HIR::Path(struct_ty.clone(), "#drop_glue");
            auto struct_ty_ptr = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Owned, struct_ty.clone());
            auto drop_impl_path = (item.m_markings.has_drop_impl ? ::HIR::Path(struct_ty.clone(), m_resolve.m_lang_Drop, "drop") : ::HIR::Path(::HIR::SimplePath()));
            ::MIR::TypeResolve  mir_res { sp, m_resolve, FMT_CB(ss, ss << drop_glue_path;), struct_ty_ptr, {}, empty_fcn };
            m_mir_res = &mir_res;

            if( item.m_markings.has_drop_impl )
            {
                m_of << "void " << Trans_Mangle(drop_impl_path) << "(struct e_" << Trans_Mangle(p) << "*rv);\n";
            }

            m_of << "static void " << Trans_Mangle(drop_glue_path) << "(struct e_" << Trans_Mangle(p) << "* rv) {\n";
            if( m_resolve.type_needs_drop_glue(sp, item_ty) )
            {
                // If this type has an impl of Drop, call that impl
                if( item.m_markings.has_drop_impl )
                {
                    m_of << "\t" << Trans_Mangle(drop_impl_path) << "(rv);\n";
                }
                auto self = ::MIR::LValue::make_Deref({ box$(::MIR::LValue::make_Return({})) });

                if( const auto* e = repr->variants.opt_NonZero() )
                {
                    unsigned idx = 1 - e->zero_variant;
                    // TODO: Fat pointers?
                    m_of << "\tif( (*rv)"; emit_enum_path(repr, e->field); m_of << " != 0 ) {\n";
                    emit_destructor_call( ::MIR::LValue::make_Downcast({ box$(self), idx }), repr->fields[idx].ty, false, 2 );
                    m_of << "\t}\n";
                }
                else if( repr->fields.size() <= 1 )
                {
                    // Value enum
                    // Glue does nothing (except call the destructor, if there is one)
                }
                else if( const auto* e = repr->variants.opt_Values() )
                {
                    auto var_lv =::MIR::LValue::make_Downcast({ box$(self), 0 });

                    m_of << "\tswitch(rv->TAG) {\n";
                    for(unsigned int var_idx = 0; var_idx < e->values.size(); var_idx ++)
                    {
                        var_lv.as_Downcast().variant_index = var_idx;
                        m_of << "\tcase " << e->values[var_idx] << ":\n";
                        emit_destructor_call(var_lv, repr->fields[var_idx].ty, /*unsized_valid=*/false, /*indent=*/2);
                        m_of << "\t\tbreak;\n";
                    }
                    m_of << "\t}\n";
                }
            }
            m_of << "}\n";
            m_mir_res = nullptr;
        }

        void emit_constructor_enum(const Span& sp, const ::HIR::GenericPath& path, const ::HIR::Enum& item, size_t var_idx) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "enum cons " << path;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;
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
            const auto* repr = Target_GetTypeRepr(sp, m_resolve, ::HIR::TypeRef::new_path(p.clone(), &item));

            ASSERT_BUG(sp, item.m_data.is_Data(), "");
            const auto& var = item.m_data.as_Data().at(var_idx);
            ASSERT_BUG(sp, var.type.m_data.is_Path(), "");
            const auto& str = *var.type.m_data.as_Path().binding.as_Struct();
            ASSERT_BUG(sp, str.m_data.is_Tuple(), "");
            const auto& e = str.m_data.as_Tuple();


            m_of << "static struct e_" << Trans_Mangle(p) << " " << Trans_Mangle(path) << "(";
            for(unsigned int i = 0; i < e.size(); i ++)
            {
                if(i != 0)
                    m_of << ", ";
                emit_ctype( monomorph(e[i].ent), FMT_CB(ss, ss << "_" << i;) );
            }
            m_of << ") {\n";

            //if( repr->variants.
            m_of << "\tstruct e_" << Trans_Mangle(p) << " rv = {";
            switch(repr->variants.tag())
            {
            case TypeRepr::VariantMode::TAGDEAD:    throw "";
            TU_ARM(repr->variants, Values, ve) {
                m_of << " .TAG = "; emit_enum_variant_val(repr, var_idx); m_of << ",";
                } break;
            TU_ARM(repr->variants, NonZero, ve) {
                } break;
            TU_ARM(repr->variants, None, ve) {
                } break;
            }

            if( e.empty() )
            {
                if( m_options.disallow_empty_structs )
                {
                    m_of << " .DATA = { .var_" << var_idx << " = {0} }";
                }
                else
                {
                    // No fields, don't initialise
                }
            }
            else
            {
                if( this->type_is_bad_zst(repr->fields[var_idx].ty) )
                {
                    //m_of << " .DATA = { /* ZST Variant */ }";
                }
                else
                {
                    m_of << " .DATA = { .var_" << var_idx << " = {";
                    for(unsigned int i = 0; i < e.size(); i ++)
                    {
                        if(i != 0)
                        m_of << ",";
                        m_of << "\n\t\t_" << i;
                    }
                    m_of << "\n\t\t} }";
                }
            }
            m_of << " };\n";
            m_of << "\treturn rv;\n";
            m_of << "}\n";
            m_mir_res = nullptr;
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

        void emit_static_ext(const ::HIR::Path& p, const ::HIR::Static& item, const Trans_Params& params) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "extern static " << p;), ::HIR::TypeRef(), {}, empty_fcn };
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
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "static " << p;), ::HIR::TypeRef(), {}, empty_fcn };
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
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "static " << p;), ::HIR::TypeRef(), {}, empty_fcn };
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
                        MIR_ASSERT(*m_mir_res, pbe->m_data.is_Data(), "Getting inner type of a non-Data enum");
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
                m_of << "{";
                if( ty.m_data.is_Array() )
                {
                    if( ty.m_data.as_Array().size_val == 0 && m_options.disallow_empty_structs)
                    {
                        m_of << "0";
                    }
                    else
                    {
                        m_of << "{";
                    }
                }
                bool emitted_field = false;
                for(unsigned int i = 0; i < e.size(); i ++) {
                    const auto& ity = get_inner_type(0, i);
                    // Don't emit ZSTs if they're being omitted
                    if( this->type_is_bad_zst(ity) )
                        continue ;
                    if(emitted_field)   m_of << ",";
                    emitted_field = true;
                    m_of << " ";
                    emit_literal(ity, e[i], params);
                }
                if( (ty.m_data.is_Path() || ty.m_data.is_Tuple()) && !emitted_field && m_options.disallow_empty_structs )
                    m_of << "0";
                if( ty.m_data.is_Array() && !(ty.m_data.as_Array().size_val == 0 && m_options.disallow_empty_structs) )
                    m_of << "}";
                m_of << " }";
                ),
            (Variant,
                MIR_ASSERT(*m_mir_res, ty.m_data.is_Path(), "");
                MIR_ASSERT(*m_mir_res, ty.m_data.as_Path().binding.is_Enum(), "");
                const auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);
                const auto& enm = *ty.m_data.as_Path().binding.as_Enum();
                if( repr->variants.is_None() )
                {
                    m_of << "{}";
                }
                else if( const auto* ve = repr->variants.opt_NonZero() )
                {
                    if( e.idx == ve->zero_variant )
                    {
                        m_of << "{0}";
                    }
                    else
                    {
                        m_of << "{ { .var_" << e.idx << " = ";
                        emit_literal(get_inner_type(e.idx, 0), *e.val, params);
                        m_of << " } }";
                    }
                }
                else if( enm.is_value() )
                {
                    MIR_ASSERT(*m_mir_res, TU_TEST1((*e.val), List, .empty()), "Value-only enum with fields");
                    m_of << "{" << enm.get_value(e.idx) << "}";
                }
                else
                {
                    m_of << "{";
                    const auto& ity = get_inner_type(e.idx, 0);
                    if( this->type_is_bad_zst(ity) ) {
                        //m_of << " {}";
                    }
                    else {
                        m_of << " { .var_" << e.idx << " = ";
                        emit_literal(ity, *e.val, params);
                        m_of << " }, ";
                    }
                    m_of << ".TAG = "; emit_enum_variant_val(repr, e.idx);
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
                            auto vtable_path = ::HIR::Path(stat.m_type.clone(), trait_path.clone(), "vtable#");
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
                // TODO: Better type checking?
                if( !ty.m_data.is_Array() ) {
                    m_of << ", " << e.size();
                }
                m_of << "}";
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
                        // If the next character is a hex digit, close/reopen the string.
                        if( &v < &s.back() && isxdigit(*(&v+1)) )
                            m_of << "\"\"";
                    }
                }
            }
            m_of << "\"" << ::std::dec;
        }

        void emit_vtable(const ::HIR::Path& p, const ::HIR::Trait& trait) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "vtable " << p;), ::HIR::TypeRef(), {}, empty_fcn };
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

                    auto  arg_ty = ::HIR::TypeRef::new_unit();
                    for(const auto& ty : te->m_arg_types)
                        arg_ty.m_data.as_Tuple().push_back( ty.clone() );

                    m_of << "static ";
                    if( *te->m_rettype == ::HIR::TypeRef::new_unit() )
                        m_of << "void ";
                    else
                        emit_ctype(*te->m_rettype);
                    m_of << " " << Trans_Mangle(fcn_p) << "("; emit_ctype(type, FMT_CB(ss, ss << "*ptr";)); m_of << ", "; emit_ctype(arg_ty, FMT_CB(ss, ss << "args";)); m_of << ") {\n";
                    m_of << "\t";
                    if( *te->m_rettype == ::HIR::TypeRef::new_unit() )
                        ;
                    else
                        m_of << "return ";
                    m_of << "(*ptr)(";
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

                // Weak link for vtables
                switch(m_compiler)
                {
                case Compiler::Gcc:
                    m_of << "__attribute__((weak)) ";
                    break;
                case Compiler::Msvc:
                    m_of << "__declspec(selectany) ";
                    break;
                }

                emit_ctype(vtable_ty);
                m_of << " " << Trans_Mangle(p) << " = {\n";
            }

            auto monomorph_cb_trait = monomorphise_type_get_cb(sp, &type, &trait_path.m_params, nullptr);

            // Size, Alignment, and destructor
            if( type.m_data.is_Borrow() || m_resolve.type_is_copy(sp, type) )
            {
                m_of << "\t""noop_drop,\n";
            }
            else
            {
                m_of << "\t""(void*)" << Trans_Mangle(::HIR::Path(type.clone(), "#drop_glue")) << ",\n";
            }
            m_of << "\t""sizeof("; emit_ctype(type); m_of << "),\n";
            m_of << "\t""ALIGNOF("; emit_ctype(type); m_of << "),\n";

            for(unsigned int i = 0; i < trait.m_value_indexes.size(); i ++ )
            {
                // Find the corresponding vtable entry
                for(const auto& m : trait.m_value_indexes)
                {
                    // NOTE: The "3" is the number of non-method vtable entries
                    if( m.second.first != 3+i )
                        continue ;

                    //MIR_ASSERT(*m_mir_res, tr.m_values.at(m.first).is_Function(), "TODO: Handle generating vtables with non-function items");
                    DEBUG("- " << m.second.first << " = " << m.second.second << " :: " << m.first);

                    auto gpath = monomorphise_genericpath_with(sp, m.second.second, monomorph_cb_trait, false);
                    // NOTE: `void*` cast avoids mismatched pointer type errors due to the receiver being &mut()/&() in the vtable
                    m_of << "\t(void*)" << Trans_Mangle( ::HIR::Path(type.clone(), mv$(gpath), m.first) ) << ",\n";
                }
            }
            m_of << "\t};\n";

            m_mir_res = nullptr;
        }

        void emit_function_ext(const ::HIR::Path& p, const ::HIR::Function& item, const Trans_Params& params) override
        {
            ::MIR::Function empty_fcn;
            ::MIR::TypeResolve  top_mir_res { sp, m_resolve, FMT_CB(ss, ss << "extern fn " << p;), ::HIR::TypeRef(), {}, empty_fcn };
            m_mir_res = &top_mir_res;
            TRACE_FUNCTION_F(p);

            m_of << "// EXTERN extern \"" << item.m_abi << "\" " << p << "\n";
            // For MSVC, make a static wrapper that goes and calls the actual function
            if( item.m_linkage.name != "" && m_compiler == Compiler::Msvc )
            {
                m_of << "static ";
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
                    m_of << " asm(\"" << item.m_linkage.name << "\")";
                    break;
                case Compiler::Msvc:
                    m_of << " {\n";
                    // A few hacky hard-coded signatures
                    if( item.m_linkage.name == "SetFilePointerEx" )
                    {
                        // LARGE_INTEGER
                        m_of << "\tLARGE_INTEGER    arg1_v;\n";
                        m_of << "\targ1_v.QuadPart = arg1;\n";
                        m_of << "\treturn SetFilePointerEx(arg0, arg1_v, arg2, arg3);\n";
                    }
                    else if( item.m_linkage.name == "CopyFileExW" )
                    {
                        // Not field access to undo an Option<fn()>
                        m_of << "\treturn CopyFileExW(arg0, arg1, arg2.DATA.var_1._0, arg3, arg4, arg5);\n";
                    }
                    // BUG: libtest defines this as returning an i32, but it's void
                    else if( item.m_linkage.name == "GetSystemInfo" )
                    {
                        m_of << "\tGetSystemInfo(arg0);\n";
                        m_of << "\treturn 0;\n";
                    }
                    else
                    {
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
                    }
                    m_of << "}";
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
            const bool USE_STRUCTURED = true;  // Still not correct.
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
                TU_MATCHA( (code->blocks[i].terminator), (e),
                (Incomplete,
                    m_of << "\tfor(;;);\n";
                    ),
                (Return,
                    // If the return type is (), don't return a value.
                    if( ret_type == ::HIR::TypeRef::new_unit() )
                        m_of << "\treturn ;\n";
                    else
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

                        TU_MATCHA( (bb.terminator), (te),
                        (Incomplete, ),
                        (Return,
                            // TODO: If the return type is (), just emit "return"
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
                            // - This is valid, the next node should be a If (but could be a loop)
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

                ::HIR::TypeRef  tmp;
                const auto& ty = mir_res.get_lvalue_type(tmp, e.dst);
                if( e.dst.is_Deref() && this->type_is_bad_zst(ty) )
                {
                    m_of << "/* ZST deref */";
                    break;
                }

                TU_MATCHA( (e.src), (ve),
                (Use,
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
                    // If the inner value was a deref, just copy the pointer verbatim
                    TU_IFLET(::MIR::LValue, ve.val, Deref, le,
                        emit_lvalue(e.dst);
                        m_of << " = ";
                        emit_lvalue(*le.val);
                        special = true;
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
                    else {
                    }

                    // NOTE: If disallow_empty_structs is set, structs don't include ZST fields
                    // In this case, we need to avoid mentioning the removed fields
                    if( !special && m_options.disallow_empty_structs && ve.val.is_Field() && this->type_is_bad_zst(ty) )
                    {
                        // Work backwards to the first non-ZST field
                        const auto* val_fp = &ve.val.as_Field();
                        while( val_fp->val->is_Field() )
                        {
                            ::HIR::TypeRef  tmp;
                            const auto& ty = mir_res.get_lvalue_type(tmp, *val_fp->val);
                            if( !this->type_is_bad_zst(ty) )
                                break;
                        }
                        // Here, we have `val_fp` be a LValue::Field that refers to a ZST, but the inner of the field points to a non-ZST or a local

                        emit_lvalue(e.dst);
                        m_of << " = ";

                        // If the index is zero, then the best option is to borrow the source
                        if( val_fp->val->is_Downcast() )
                        {
                            m_of << "(void*)& "; emit_lvalue(*val_fp->val->as_Downcast().val);
                        }
                        else if( val_fp->field_index == 0 )
                        {
                            m_of << "(void*)& "; emit_lvalue(*val_fp->val);
                        }
                        else
                        {
                            ::HIR::TypeRef  tmp;
                            auto tmp_lv = ::MIR::LValue::make_Field({ box$(val_fp->val->clone()), val_fp->field_index - 1 });
                            bool use_parent = false;
                            for(;;)
                            {
                                const auto& ty = mir_res.get_lvalue_type(tmp, tmp_lv);
                                if( !this->type_is_bad_zst(ty) )
                                    break;
                                if( tmp_lv.as_Field().field_index == 0 )
                                {
                                    use_parent = true;
                                    break;
                                }
                                tmp_lv.as_Field().field_index -= 1;
                            }

                            // Reached index zero, with still ZST
                            if( use_parent )
                            {
                                m_of << "(void*)& "; emit_lvalue(*val_fp->val);
                            }
                            // Use the address after the previous item
                            else
                            {
                                m_of << "(void*)( & "; emit_lvalue(tmp_lv); m_of << " + 1 )";
                            }
                        }
                        special = true;
                    }

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
                    bool has_emitted = false;
                    for(unsigned int j = 0; j < ve.vals.size(); j ++)
                    {
                        if( m_options.disallow_empty_structs )
                        {
                            ::HIR::TypeRef  tmp;
                            const auto& ty = mir_res.get_param_type(tmp, ve.vals[j]);

                            if( this->type_is_bad_zst(ty) )
                            {
                                continue ;
                            }
                        }

                        if(has_emitted) {
                            m_of << ";\n" << indent;
                        }
                        has_emitted = true;

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
                        auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);

                        if( repr->variants.is_None() )
                        {
                            emit_lvalue(e.dst); m_of << ".DATA.var_0 = "; emit_param(ve.val);
                        }
                        else if( const auto* re = repr->variants.opt_NonZero() )
                        {
                            MIR_ASSERT(*m_mir_res, ve.index < 2, "");
                            if( ve.index == re->zero_variant ) {
                                // TODO: Use nonzero_path
                                m_of << "memset(&"; emit_lvalue(e.dst); m_of << ", 0, sizeof("; emit_ctype(ty); m_of << "))";
                            }
                            else {
                                emit_lvalue(e.dst);
                                m_of << ".DATA.var_" << ve.index << " = ";
                                emit_param(ve.val);
                            }
                            break;
                        }
                        else if( enm_p->is_value() )
                        {
                            emit_lvalue(e.dst); m_of << ".TAG = "; emit_enum_variant_val(repr, ve.index);
                        }
                        else
                        {
                            emit_lvalue(e.dst); m_of << ".TAG = "; emit_enum_variant_val(repr, ve.index);

                            ::HIR::TypeRef  tmp;
                            const auto& vty = mir_res.get_param_type(tmp, ve.val);
                            if( this->type_is_bad_zst(vty) )
                            {
                                m_of << "/* ZST field */";
                            }
                            else
                            {
                                m_of << ";\n" << indent;
                                emit_lvalue(e.dst); m_of << ".DATA";
                                m_of << ".var_" << ve.index << " = "; emit_param(ve.val);
                            }
                        }
                    }
                    else
                    {
                        BUG(mir_res.sp, "Unexpected type in Variant");
                    }
                    ),
                (Struct,
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
                        bool has_emitted = false;
                        for(unsigned int j = 0; j < ve.vals.size(); j ++)
                        {
                            // HACK: Don't emit assignment of PhantomData
                            ::HIR::TypeRef  tmp;
                            if( ve.vals[j].is_LValue() )
                            {
                                const auto& ty = mir_res.get_param_type(tmp, ve.vals[j]);
                                if( ve.vals[j].is_LValue() && m_resolve.is_type_phantom_data(ty) )
                                    continue ;

                                if( this->type_is_bad_zst(ty) )
                                {
                                    continue ;
                                }
                            }

                            if(has_emitted) {
                                m_of << ";\n" << indent;
                            }
                            has_emitted = true;

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
                case ::HIR::CoreType::F32:
                    emit_lvalue(dst);
                    m_of << " = ";
                    switch (ty.m_data.as_Primitive())
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
                    switch (ty.m_data.as_Primitive())
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
            const auto* repr = Target_GetTypeRepr(mir_res.sp, m_resolve, ty);
            MIR_ASSERT(mir_res, repr, "No repr for " << ty);

            if( const auto* e = repr->variants.opt_NonZero() )
            {
                MIR_ASSERT(mir_res, n_arms == 2, "NonZero optimised switch without two arms");
                m_of << indent << "if( "; emit_lvalue(val); emit_enum_path(repr, e->field); m_of << " != 0 )\n";
                m_of << indent << "\t";
                cb(1 - e->zero_variant);
                m_of << "\n";
                m_of << indent << "else\n";
                m_of << indent << "\t";
                cb(e->zero_variant);
                m_of << "\n";
            }
            else if( const auto* e = repr->variants.opt_Values() )
            {
                const auto& tag_ty = Target_GetInnerType(sp, m_resolve, *repr, e->field.index, e->field.sub_fields);
                bool is_signed = false;
                switch(tag_ty.m_data.as_Primitive())
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
                m_of << indent << "switch("; emit_lvalue(val); m_of << ".TAG) {\n";
                for(size_t j = 0; j < n_arms; j ++)
                {
                    // TODO: Get type of this field and check if it's signed.
                    if( is_signed ) {
                        m_of << indent << "case " << static_cast<int64_t>(e->values[j]) << ": ";
                    }
                    else {
                        m_of << indent << "case " << e->values[j] << ": ";
                    }
                    cb(j);
                    m_of << "break;\n";
                }
                m_of << indent << "default: abort();\n";
                m_of << indent << "}\n";
            }
            else if( repr->variants.is_None() )
            {
                m_of << indent; cb(0); m_of << "\n";
            }
            else
            {
                BUG(sp, "Unexpected variant type - " << repr->variants.tag_str());
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
                if( m_options.disallow_empty_structs && TU_TEST1(e.args[j], LValue, .is_Field()) )
                {
                    ::HIR::TypeRef tmp;
                    const auto& ty = m_mir_res->get_param_type(tmp, e.args[j]);
                    if( this->type_is_bad_zst(ty) )
                    {
                        if(!has_zst) {
                            m_of << "{\n";
                            indent.n ++ ;
                            m_of << indent;
                        }
                        has_zst = true;
                        emit_ctype(ty, FMT_CB(ss, ss << "zarg" << j;));
                        m_of << " = {0};\n";
                        m_of << indent;
                    }
                }
            }

            bool omit_assign = false;

            // If the return type is `()`, omit the assignment (all () returning functions are marked as returning
            // void)
            {
                ::HIR::TypeRef  tmp;
                if( m_mir_res->get_lvalue_type(tmp, e.ret_val) == ::HIR::TypeRef::new_unit() )
                {
                    omit_assign = true;
                }
            }

            TU_MATCHA( (e.fcn), (e2),
            (Value,
                {
                    ::HIR::TypeRef  tmp;
                    const auto& ty = mir_res.get_lvalue_type(tmp, e2);
                    MIR_ASSERT(mir_res, ty.m_data.is_Function(), "Call::Value on non-function - " << ty);

                    const auto& ret_ty = *ty.m_data.as_Function().m_rettype;
                    omit_assign |= ret_ty.m_data.is_Diverge();
                    if( !omit_assign )
                    {
                        emit_lvalue(e.ret_val); m_of << " = ";
                    }
                }
                m_of << "("; emit_lvalue(e2); m_of << ")";
                ),
            (Path,
                {
                    TU_MATCHA( (e2.m_data), (pe),
                    (Generic,
                        const auto& fcn = m_crate.get_function_by_path(sp, pe.m_path);
                        omit_assign |= fcn.m_return.m_data.is_Diverge();
                        // TODO: Monomorph.
                        ),
                    (UfcsUnknown,
                        ),
                    (UfcsInherent,
                        // Check if the return type is !
                        omit_assign |= m_resolve.m_crate.find_type_impls(*pe.type, [&](const auto& ty)->const auto& { return ty; },
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
                        // Check if the return type is !
                        const auto& tr = m_resolve.m_crate.get_trait_by_path(sp, pe.trait.m_path);
                        const auto& fcn = tr.m_values.find(pe.item)->second.as_Function();
                        const auto& rv_tpl = fcn.m_return;
                        if( rv_tpl.m_data.is_Diverge() || rv_tpl == ::HIR::TypeRef::new_unit() )
                        {
                            omit_assign |= true;
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
                    if(!omit_assign)
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
                if( has_zst )
                {
                    indent.n --;
                    m_of << indent << "}\n";
                }
                return ;
                )
            )
            m_of << "(";
            for(unsigned int j = 0; j < e.args.size(); j ++) {
                if(j != 0)  m_of << ",";
                m_of << " ";
                if( m_options.disallow_empty_structs && TU_TEST1(e.args[j], LValue, .is_Field()) )
                {
                    ::HIR::TypeRef tmp;
                    const auto& ty = m_mir_res->get_param_type(tmp, e.args[j]);
                    if( this->type_is_bad_zst(ty) )
                    {
                        m_of << "zarg" << j;
                        continue;
                    }
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
                    else if (::std::strcmp(r, "{ecx}") == 0 || ::std::strcmp(r, "{rcx}") == 0) {
                        return "c";
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
                else if (*it == '$' && isdigit(*(it + 1)) && *(it + 2) != 'x')
                    m_of << "%";
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
                m_of << "\" ("; emit_lvalue(v.second); m_of << ")";
            }
            m_of << ": ";
            for (unsigned int i = 0; i < e.inputs.size(); i++)
            {
                const auto& v = e.inputs[i];
                if (i != 0)    m_of << ", ";
                m_of << "\"" << H::convert_reg(v.first.c_str()) << "\" ("; emit_lvalue(v.second); m_of << ")";
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
            else if( e.tpl == "pause" )
            {
                if( !(e.inputs.size() == 0 && e.outputs.size() == 0) )
                    MIR_BUG(mir_res, "Hard-coded asm translation doesn't apply - `" << e.tpl << "` inputs=" << e.inputs << " outputs=" << e.outputs);
                m_of << indent << "_mm_pause();\n";
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

            if( Target_GetCurSpec().m_backend_c.m_c_compiler == "amd64" ) {
                MIR_TODO(mir_res, "MSVC amd64 doesn't support inline assembly, need to have a transform for '" << e.tpl << "'");
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
                        this->emit_ctype( params.monomorph(m_resolve, item.m_args[i].second), FMT_CB(os, os << "arg" << i;) );
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

        void emit_intrinsic_call(const ::std::string& name, const ::HIR::PathParams& params, const ::MIR::Terminator::Data_Call& e)
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
            auto get_atomic_ordering = [&](const ::std::string& name, size_t prefix_len)->Ordering {
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
                    else {
                        MIR_BUG(mir_res, "Unknown atomic ordering suffix - '" << suffix << "'");
                    }
                    throw "";
                };
            auto get_prim_size = [&mir_res](const ::HIR::TypeRef& ty)->unsigned {
                    if( !ty.m_data.is_Primitive() )
                        MIR_BUG(mir_res, "Unknown type for getting primitive size - " << ty);
                    switch( ty.m_data.as_Primitive() )
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
            auto emit_msvc_atomic_op = [&](const char* name, Ordering ordering, bool is_before_size=false) {
                const char* o_before = is_before_size ? get_atomic_suffix_msvc(ordering) : "";
                const char* o_after  = is_before_size ? "" : get_atomic_suffix_msvc(ordering);
                switch (params.m_types.at(0).m_data.as_Primitive())
                {
                case ::HIR::CoreType::U8:
                case ::HIR::CoreType::I8:
                    m_of << name << o_before << "8" << o_after << "(";
                    break;
                case ::HIR::CoreType::U16:
                case ::HIR::CoreType::I16:
                    m_of << name << o_before << "16" << o_after << "(";
                    break;
                case ::HIR::CoreType::U32:
                case ::HIR::CoreType::I32:
                    m_of << name << o_before << o_after << "(";
                    break;
                case ::HIR::CoreType::U64:
                case ::HIR::CoreType::I64:
                    m_of << name << o_before << "64" << o_after << "(";
                    break;
                case ::HIR::CoreType::Usize:
                case ::HIR::CoreType::Isize:
                    m_of << name << o_before;
                    if( Target_GetCurSpec().m_arch.m_pointer_bits == 64 )
                        m_of << "64";
                    else if( Target_GetCurSpec().m_arch.m_pointer_bits == 32 )
                        m_of << "";
                    else
                        MIR_TODO(mir_res, "Handle non 32/64 bit pointer types");
                    m_of << o_after << "(";
                    break;
                default:
                    MIR_BUG(mir_res, "Unsupported atomic type - " << params.m_types.at(0));
                }
                };
            auto emit_atomic_cast = [&]() {
                m_of << "(_Atomic "; emit_ctype(params.m_types.at(0)); m_of << "*)";
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
                    emit_lvalue(e.ret_val); m_of << "._0 = ";
                    emit_msvc_atomic_op("InterlockedCompareExchange", Ordering::SeqCst, true);  // TODO: Use ordering, but which one?
                    // Slot, Exchange (new value), Comparand (expected value) - Note different order to the gcc/stdc version
                    emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(2)); m_of << ", "; emit_param(e.args.at(1)); m_of << ")";
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
                    if( params.m_types.at(0) == ::HIR::CoreType::U8 || params.m_types.at(0) == ::HIR::CoreType::I8 )
                    {
                        //_InterlockedCompareExchange8 ?
                        if( params.m_types.at(0) == ::HIR::CoreType::U8 )
                            m_of << "*(volatile uint8_t*)";
                        else
                            m_of << "*(volatile int8_t*)";
                        emit_param(e.args.at(0));
                        switch(op)
                        {
                        case AtomicOp::Add: m_of << " += "; break;
                        case AtomicOp::Sub: m_of << " -= "; break;
                        case AtomicOp::And: m_of << " &= "; break;
                        case AtomicOp::Or:  m_of << " |= "; break;
                        case AtomicOp::Xor: m_of << " ^= "; break;
                        }
                        emit_param(e.args.at(1));
                        return ;
                    }
                    switch(op)
                    {
                    case AtomicOp::Add: emit_msvc_atomic_op("InterlockedExchangeAdd", ordering, true);    break;
                    case AtomicOp::Sub:
                        emit_msvc_atomic_op("InterlockedExchangeAdd", ordering, true);
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
                    //auto vtable_path = inner_ty.m_data.as_TraitObject().m_trait.m_path.clone();
                    //vtable_path.m_path.m_components.back() += "#vtable";
                    //auto vtable_ty = ::HIR::TypeRef
                    m_of << "((VTABLE_HDR*)"; emit_param(e.args.at(0)); m_of << ".META)->size";
                }
                else {
                    MIR_BUG(mir_res, "Unknown inner unsized type " << inner_ty << " for " << ty);
                }
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
                if( this->type_is_bad_zst(ty_dst) )
                {
                    m_of << "/* zst */";
                }
                else if( e.args.at(0).is_Constant() )
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
                    m_of << " jmp_buf jmpbuf; mrustc_panic_target = &jmpbuf;";
                    m_of << " if(setjmp(jmpbuf)) {";
                    // NOTE: gcc unwind has a pointer as its `local_ptr` parameter
                    m_of << " *(void**)("; emit_param(e.args.at(2)); m_of << ") = mrustc_panic_value;";
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
                    m_of << " mrustc_panic_target = NULL;";
                    m_of << " }";
                    break;
                default:
                    break;
                }
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
                        switch( ty.m_data.as_Primitive() )
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
            }
            else if( name == "unlikely" ) {
            }
            // Overflowing Arithmatic
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
                        emit_lvalue(e.ret_val); m_of << "._1 = _addcarry_u" << get_prim_size(params.m_types.at(0));
                        m_of << "(0, "; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
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
                        emit_lvalue(e.ret_val); m_of << "._1 = _subcarry_u" << get_prim_size(params.m_types.at(0));
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
                        emit_lvalue(e.ret_val); m_of << "._1 = __builtin_mul_overflow_" << params.m_types.at(0);
                        m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << "._0)";
                        break;
                    }
                }
            }
            else if( name == "overflowing_add" ) {
                if(m_options.emulated_i128 && params.m_types.at(0) == ::HIR::CoreType::U128)
                {
                    m_of << "add128_o";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                }
                else if(m_options.emulated_i128 && params.m_types.at(0) == ::HIR::CoreType::I128)
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
                        m_of << "_addcarry_u" << get_prim_size(params.m_types.at(0));
                        m_of << "(0, "; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                        break;
                    }
                }
            }
            else if( name == "overflowing_sub" ) {
                if(m_options.emulated_i128 && params.m_types.at(0) == ::HIR::CoreType::U128)
                {
                    m_of << "sub128_o";
                    m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                }
                else if(m_options.emulated_i128 && params.m_types.at(0) == ::HIR::CoreType::I128)
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
                        m_of << "_subcarry_u" << get_prim_size(params.m_types.at(0));
                        m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                        break;
                    }
                }
            }
            else if( name == "overflowing_mul" ) {
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
                        m_of << "__builtin_mul_overflow_" << params.m_types.at(0);
                        m_of << "("; emit_param(e.args.at(0)); m_of << ", "; emit_param(e.args.at(1)); m_of << ", &"; emit_lvalue(e.ret_val); m_of << ")";
                        break;
                    }
                }
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
                    switch(m_compiler)
                    {
                    case Compiler::Gcc:
                        m_of << "__builtin_popcount";
                        break;
                    case Compiler::Msvc:
                        if( params.m_types.at(0) == ::HIR::CoreType::U64 || params.m_types.at(0) == ::HIR::CoreType::I64 )
                        {
                            m_of << "__popcnt64";
                        }
                        else
                        {
                            m_of << "__popcnt";
                        }
                        break;
                    }
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
                    m_of << "atomic_load_explicit("; emit_atomic_cast(); emit_param(e.args.at(0)); m_of << ", " << get_atomic_ty_gcc(ordering) << ")";
                    break;
                case Compiler::Msvc:
                    //emit_msvc_atomic_op("InterlockedRead", ordering); emit_param(e.args.at(0)); m_of << ")";
                    emit_msvc_atomic_op("InterlockedCompareExchange", ordering, true); emit_param(e.args.at(0)); m_of << ", 0, 0)";
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
                    emit_msvc_atomic_op("InterlockedCompareExchange", ordering, true); emit_param(e.args.at(0)); m_of << ", ";
                    emit_param(e.args.at(1));
                    m_of << ", ";
                    emit_param(e.args.at(1));
                    m_of << ")";
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
                    emit_msvc_atomic_op("InterlockedExchange", ordering, true);
                    emit_param(e.args.at(0)); m_of << ", ";
                    emit_param(e.args.at(1)); m_of << ")";
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
            else {
                MIR_BUG(mir_res, "Unknown intrinsic '" << name << "'");
            }
            m_of << ";\n";
        }

        void emit_destructor_call(const ::MIR::LValue& slot, const ::HIR::TypeRef& ty, bool unsized_valid, unsigned indent_level)
        {
            // If the type doesn't need dropping, don't try.
            if( !m_resolve.type_needs_drop_glue(sp, ty) )
            {
                return ;
            }
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

                    if( this->type_is_bad_zst(ty) && (slot.is_Field() || slot.is_Downcast()) )
                    {
                        m_of << indent << Trans_Mangle(p) << "((void*)&";
                        if( slot.is_Field() )
                            emit_lvalue(*slot.as_Field().val);
                        else
                            emit_lvalue(*slot.as_Downcast().val);
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

        void emit_enum_variant_val(const TypeRepr* repr, unsigned idx)
        {
            const auto& ve = repr->variants.as_Values();
            const auto& tag_ty = Target_GetInnerType(sp, m_resolve, *repr, ve.field.index, ve.field.sub_fields);
            switch(tag_ty.m_data.as_Primitive())
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
                    bool emitted_field = false;
                    for(unsigned int i = 0; i < e.size(); i ++) {
                        const auto& ity = get_inner_type(0, i);
                        // Don't emit ZSTs if they're being omitted
                        if( this->type_is_bad_zst(ity) )
                            continue ;
                        if(emitted_field)  m_of << ";\n\t";
                        emitted_field = true;
                        assign_from_literal([&](){ emit_dst(); m_of << "._" << i; }, get_inner_type(0, i), e[i]);
                    }
                    //if( !emitted_field )
                    //{
                    //}
                }
                ),
            (Variant,
                MIR_ASSERT(*m_mir_res, ty.m_data.is_Path(), "");
                MIR_ASSERT(*m_mir_res, ty.m_data.as_Path().binding.is_Enum(), "");
                const auto* repr = Target_GetTypeRepr(sp, m_resolve, ty);
                MIR_ASSERT(*m_mir_res, repr, "");
                switch(repr->variants.tag())
                {
                case TypeRepr::VariantMode::TAGDEAD:    throw "";
                TU_ARM(repr->variants, None, ve)
                    BUG(sp, "");
                TU_ARM(repr->variants, NonZero, ve) {
                    if( e.idx == ve.zero_variant ) {
                        emit_dst(); emit_enum_path(repr, ve.field); m_of << " = 0";
                    }
                    else {
                        assign_from_literal([&](){ emit_dst(); }, get_inner_type(e.idx, 0), *e.val);
                    }
                    } break;
                TU_ARM(repr->variants, Values, ve) {
                    emit_dst(); emit_enum_path(repr, ve.field); m_of << " = ";

                    emit_enum_variant_val(repr, e.idx);
                    if( TU_TEST1((*e.val), List, .empty() == false) )
                    {
                        m_of << ";\n\t";
                        assign_from_literal([&](){ emit_dst(); m_of << ".DATA.var_" << e.idx; }, get_inner_type(e.idx, 0), *e.val);
                    }
                    } break;
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
                    m_of << ".DATA";
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
                    {
                        m_of << "_" << Trans_Mangle(t);
                    }
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
