/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/crate_post_load.cpp
 * - Updates the crate after deserialising
 */
#include <hir/hir.hpp>
#include <macro_rules/macro_rules.hpp>  // Used to update the crate name


void HIR::Crate::post_load_update(const RcString& name)
{
    // TODO: Do a pass across m_hir that
    // 1. Updates all absolute paths with the crate name
    // 2. Sets binding pointers where required
    // 3. Updates macros with the crate name
}

