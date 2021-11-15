/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/rustc_diagnostics.cpp
 * - Stubbed handling for __register_diagnostic and __diagnostic_used
 */
#include <synext.hpp>
#include <parse/parseerror.hpp> // For GET_CHECK_TOK
#include <parse/common.hpp>  // TokenTree etc
#include <parse/ttstream.hpp>
#include <ast/crate.hpp>

class CExpanderRegisterDiagnostic:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(sp, ParseState(), TokenTree()) );
    }
};
class CExpanderDiagnosticUsed:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(sp, ParseState(), TokenTree()) );
    }
};
class CExpanderBuildDiagnosticArray:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        auto lex = TTStream(sp, ParseState(), tt);

        Token   tok;

        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        //auto crate_name = mv$(tok.str());
        GET_CHECK_TOK(tok, lex, TOK_COMMA);
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        auto item_name = tok.ident();
        GET_CHECK_TOK(tok, lex, TOK_EOF);

        ::std::vector<TokenTree>    toks;
        toks.push_back( TOK_RWORD_STATIC );
        toks.push_back( Token(TOK_IDENT, item_name) );
        // : [(&'static str, &'static str); 0]
        toks.push_back( TOK_COLON );
        toks.push_back( TOK_SQUARE_OPEN );
        toks.push_back( TOK_PAREN_OPEN );
        toks.push_back( TOK_AMP ); toks.push_back( Token(TOK_LIFETIME, RcString::new_interned("static")) ); toks.push_back( Token(TOK_IDENT, RcString::new_interned("str")) );
        toks.push_back( TOK_COMMA );
        toks.push_back( TOK_AMP ); toks.push_back( Token(TOK_LIFETIME, RcString::new_interned("static")) ); toks.push_back( Token(TOK_IDENT, RcString::new_interned("str")) );
        toks.push_back( TOK_PAREN_CLOSE );
        toks.push_back( TOK_SEMICOLON );
        toks.push_back( Token(static_cast<uint64_t>(0), CORETYPE_UINT) );
        toks.push_back( TOK_SQUARE_CLOSE );
        // = [];
        toks.push_back( TOK_EQUAL );
        toks.push_back( TOK_SQUARE_OPEN );
        toks.push_back( TOK_SQUARE_CLOSE );
        toks.push_back( TOK_SEMICOLON );

        return box$( TTStreamO(sp, ParseState(), TokenTree( AST::Edition::Rust2015, lex.get_hygiene(), mv$(toks) )) );
    }
};

STATIC_MACRO("__register_diagnostic", CExpanderRegisterDiagnostic)
STATIC_MACRO("__diagnostic_used", CExpanderDiagnosticUsed)
STATIC_MACRO("__build_diagnostic_array", CExpanderBuildDiagnosticArray)

