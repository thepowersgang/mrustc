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
    ::std::vector< ::std::string>   m_linker_opts_pre;
    ::std::vector< ::std::string>   m_linker_opts_post;
};
struct TargetSpec
{
    ::std::string   m_family;
    ::std::string   m_os_name;
    ::std::string   m_env_name;

    BackendOptsC    m_backend_c;
    TargetArch  m_arch;
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
    // Variants numbered 0 to N (potentially offset)
    (Linear, struct {
        // Note: If `field.sub_fields` has entries, then this is a niche optimisation.
        // Path of the variant
        FieldPath   field;
        // Offset for variants (when in a niche)
        size_t  offset;
        size_t  num_variants;

        bool uses_niche() const {
            return !field.sub_fields.empty();
        }
        bool is_niche(unsigned var_idx) const { 
            return uses_niche() && var_idx == field.index;
        }
        bool is_tag(unsigned var_idx) const {
            return !uses_niche() && var_idx == field.index;
        }
        }),
    // Tag is a fixed set of values in a field.
    // TODO: Encode niche in here too?
    (Values, struct {
        // NOTE: `field.sub_path` should always be empty?
        FieldPath   field;
        ::std::vector<uint64_t> values;
        bool is_tag(unsigned var_idx) const {
            return var_idx == field.index;
        }
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
        unsigned    zero_variant;
        })
    );
    VariantMode variants;

    struct Field {
        size_t  offset;
        ::HIR::TypeRef  ty;
    };
    ::std::vector<Field>    fields;

    /// <summary>
    /// Get the byte offset of a field
    /// </summary>
    /// <param name="sp">Invocation span (for error messages)</param>
    /// <param name="resolve">Resolve structure (shouldn't be needed)</param>
    /// <param name="path">Path to the field</param>
    /// <returns>Byte offset</returns>
    size_t get_offset(const Span& sp, const StaticTraitResolve& resolve, const FieldPath& path) const;

    /// <summary>
    /// Determines which enum variant is stored in an encoded literal
    /// </summary>
    /// <param name="sp">Invocation span (for error messages)</param>
    /// <param name="resolve">Resolve structure (shouldn't be needed)</param>
    /// <param name="lit">Literal covering the entire enum</param>
    /// <returns>Variant index and if the variant's data includes a tag field</returns>
    std::pair<unsigned, bool> get_enum_variant(const Span& sp, const StaticTraitResolve& resolve, const EncodedLiteralSlice& lit) const;
};
static inline std::ostream& operator<<(std::ostream& os, const TypeRepr::FieldPath& x) {
    os << x.size << "@" << x.index;
    for(auto idx : x.sub_fields)
        os << "." << idx;
    return os;
}

extern const TargetSpec& Target_GetCurSpec();
extern void Target_SetCfg(const ::std::string& target_name);
extern void Target_ExportCurSpec(const ::std::string& filename);
static inline unsigned Target_GetPointerBits() { return Target_GetCurSpec().m_arch.m_pointer_bits; }

extern bool Target_GetSizeOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_size);
extern bool Target_GetAlignOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_align);
extern bool Target_GetSizeAndAlignOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_size, size_t& out_align);

/// This function is for the MIR Optimisation tool, which has to be able to read and use existing layouts
extern void Target_ForceTypeRepr(const Span& sp, const ::HIR::TypeRef& ty, TypeRepr repr);
extern const TypeRepr* Target_GetTypeRepr(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty);

extern const ::HIR::TypeRef& Target_GetInnerType(const Span& sp, const StaticTraitResolve& resolve, const TypeRepr& repr, size_t idx, const ::std::vector<size_t>& sub_fields={}, size_t ofs=0);

