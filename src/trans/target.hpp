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

extern const TargetSpec& Target_GetCurSpec();
extern void Target_SetCfg(const ::std::string& target_name);
extern bool Target_GetSizeOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_size);
extern bool Target_GetAlignOf(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& ty, size_t& out_align);
extern const StructRepr* Target_GetStructRepr(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::TypeRef& struct_ty);

