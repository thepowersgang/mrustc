/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/rustc_diagnostics.cpp
 * - Stubbed handling for __register_diagnostic and __diagnostic_used
 */
#include <synext.hpp>
#include <parse/common.hpp>  // TokenTree etc
#include <parse/ttstream.hpp>

class CExpanderRegisterDiagnostic:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(TokenTree()) );
    }
};
class CExpanderDiagnosticUsed:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(TokenTree()) );
    }
};

STATIC_MACRO("__register_diagnostic", CExpanderRegisterDiagnostic)
STATIC_MACRO("__diagnostic_used", CExpanderDiagnosticUsed)

