/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/target.hpp
 * - Target-specific information
 */
#pragma once

#include <cstddef>
#include <hir/type.hpp>
#include <hir_typeck/static.hpp>

enum class CodegenMode
{
    Gnu11,
    Msvc,
};

// NOTE: The default architecture is an unnamed 32-bit little-endian arch with all types natively aligned
struct TargetArch
{
    ::std::string   m_name;
    unsigned    m_pointer_bits;
    bool    m_big_endian;

    struct Atomics {
        bool u8 = true;
        bool u16 = true;
        bool u32 = true;
        bool u64 = false;
        bool ptr = true;
        Atomics(bool u8 = true, bool u16 = true, bool u32 = true, bool u64 = false, bool ptr = true)
            :u8(u8)
            ,u16(u16)
            ,u32(u32)
            ,u64(u64)
            ,ptr(ptr)
        {
        }
    } m_atomics;

    struct Alignments {
        uint8_t u16;
        uint8_t u32;
        uint8_t u64;
        uint8_t u128;
        uint8_t f32;
        uint8_t f64;
        uint8_t ptr;
        Alignments(uint8_t u16 = 2, uint8_t u32 = 4, uint8_t u64 = 8, uint8_t u128 = 16, uint8_t f32 = 4, uint8_t f64 = 8, uint8_t ptr = 4)
            :u16 (u16)
            ,u32 (u32 )
            ,u64 (u64 )
            ,u128(u128)
            ,f32 (f32 )
            ,f64 (f64 )
            ,ptr (ptr )
        {
        }
    } m_alignments;
};
struct BackendOptsC
{
    CodegenMode m_codegen_mode;
    bool    m_emulated_i128;    // Influences the chosen alignment for i128/u128
    ::std::string   m_c_compiler;   // MSVC arch / GNU triplet
    ::std::vector< ::std::string>   m_compiler_opts;
    ::std::vector< ::std::string>   m_linker_opts;
};
struct TargetSpec
{
    ::std::string   m_family;
    ::std::string   m_os_name;
    ::std::string   m_env_name;

    BackendOptsC    m_backend_c;
    TargetArch  m_arch;
};

struct StructRepr
{
    struct Ent {
        unsigned int    field_idx;
        size_t  size;
        size_t  align;
        ::HIR::TypeRef  ty;
    };
    // List of types, including padding (indicated by a UINT_MAX field idx)
    // Ordered as they would be emitted
    ::std::vector<Ent>  ents;
};
struct TypeRepr
{
    size_t  align = 0;
    size_t  size = 0;

    struct FieldPath {
        size_t  index;
        size_t  size;
        ::std::vector<size_t>   sub_fields;
    };
    TAGGED_UNION(VariantMode, None,
    (None, struct {
        }),
    // Tag is a fixed set of values in an offset.
    (Values, struct {
        FieldPath   field;
        ::std::vector<uint64_t> values;
        }),
    // Tag is based on a range of values
    //(Ranges, struct {
    //    size_t  offset;
    //    size_t  size;
    //    ::std::vector<::std::pair<uint64_t,uint64_t>> values;
    //    }),
    // Tag is a boolean based on if a region is zero/non-zero
    // Only valid for two-element enums
    (NonZero, struct {
        FieldPath   field;
        uint8_t zero_variant;
        })
    );
    VariantMode variants;

    struct Field {
        size_t  offset;
        ::HIR::TypeRef  ty;
    };
    ::std::vector<Field>    fields;
};

extern const TargetSpec& Target_GetCurSpec();
extern void Target_SetCfg(const ::std::string& target_name);
extern void Target_ExportCurSpec(const ::std::string& filename);

extern bool Target_GetSizeOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_size);
extern bool Target_GetAlignOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_align);
extern bool Target_GetSizeAndAlignOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_size, size_t& out_align);
extern const StructRepr* Target_GetStructRepr(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& struct_ty);

extern const TypeRepr* Target_GetTypeRepr(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty);

extern const ::HIR::TypeRef& Target_GetInnerType(const Span& sp, const StaticTraitResolve& resolve, const TypeRepr& repr, size_t idx, const ::std::vector<size_t>& sub_fields={}, size_t ofs=0);

