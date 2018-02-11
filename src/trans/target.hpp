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

struct TargetArch
{
    ::std::string   m_name;
    unsigned    m_pointer_bits;
    bool    m_big_endian;

    struct {
        bool u8;
        bool u16;
        bool u32;
        bool u64;
        bool ptr;
    } m_atomics;
};
struct TargetSpec
{
    ::std::string   m_family;
    ::std::string   m_os_name;
    ::std::string   m_env_name;

    CodegenMode m_codegen_mode;
    ::std::string   m_c_compiler;   // MSVC arch / GNU triplet
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
extern bool Target_GetSizeOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_size);
extern bool Target_GetAlignOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_align);
extern bool Target_GetSizeAndAlignOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_size, size_t& out_align);
extern const StructRepr* Target_GetStructRepr(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& struct_ty);

extern const TypeRepr* Target_GetTypeRepr(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty);

extern const ::HIR::TypeRef& Target_GetInnerType(const Span& sp, const StaticTraitResolve& resolve, const TypeRepr& repr, size_t idx, const ::std::vector<size_t>& sub_fields={}, size_t ofs=0);

