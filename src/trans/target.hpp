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

extern bool Target_GetSizeOf(const Span& sp, const ::HIR::TypeRef& ty, size_t& out_size);
extern bool Target_GetAlignOf(const Span& sp, const ::HIR::TypeRef& ty, size_t& out_align);

