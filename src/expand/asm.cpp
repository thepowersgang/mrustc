/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/asm.cpp
 * - asm! macro
 */
#include <common.hpp>
#include <synext_macro.hpp>
#include <parse/tokentree.hpp>

class CAsmExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        // TODO: Convert this into an AST node
        return box$( TTStreamO(TokenTree()) );
    }
};

STATIC_MACRO("asm", CAsmExpander);
