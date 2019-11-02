/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * macro_rules/pattern_checks.hpp
 * - Checking helpers for the fragement patterns
 */
#pragma once

extern bool is_token_path(eTokenType tt);
extern bool is_token_pat(eTokenType tt);
extern bool is_token_type(eTokenType tt);
extern bool is_token_expr(eTokenType tt);
extern bool is_token_stmt(eTokenType tt);
extern bool is_token_item(eTokenType tt);
extern bool is_token_vis(eTokenType tt);
