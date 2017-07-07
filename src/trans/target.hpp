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


extern const TargetSpec& Target_GetCurSpec();
extern void Target_SetCfg(const ::std::string& target_name);
extern bool Target_GetSizeOf(const Span& sp, const ::HIR::TypeRef& ty, size_t& out_size);
extern bool Target_GetAlignOf(const Span& sp, const ::HIR::TypeRef& ty, size_t& out_align);

