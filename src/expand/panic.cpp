/*
* MRustC - Rust Compiler
* - By John Hodge (Mutabah/thePowersGang)
*
* expand/panic.cpp
* - panic! built-in macro (1.54)
*/
#include <synext_macro.hpp>
#include <parse/interpolated_fragment.hpp>
#include <ast/crate.hpp>
#include <hir/hir.hpp>
#include "../parse/ttstream.hpp"
#include "../parse/common.hpp"
#include "../parse/parseerror.hpp"

class CExpander_panic:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;

        auto edition = crate.m_edition;
        if( tt.hygiene().has_mod_path() && tt.hygiene().mod_path().crate != "" ) {
            edition = crate.m_extern_crates.at(tt.hygiene().mod_path().crate).m_hir->m_edition;
        }
        ::std::vector<TokenTree> toks;
        toks.push_back( Token(TOK_DOUBLE_COLON) );
        if( crate.m_load_std == AST::Crate::LOAD_STD ) {
            toks.push_back( Token(TOK_STRING, std::string(crate.m_ext_cratename_std.c_str())) );
        }
        else {
            toks.push_back( Token(TOK_STRING, std::string(crate.m_ext_cratename_core.c_str())) );
        }
        toks.push_back( Token(TOK_DOUBLE_COLON) );
        toks.push_back( Token(TOK_IDENT, RcString::new_interned("panic")) );
        toks.push_back( Token(TOK_DOUBLE_COLON) );
        switch(crate.m_edition)
        {
        case AST::Edition::Rust2015:
        case AST::Edition::Rust2018:
            toks.push_back( Token(TOK_IDENT, RcString::new_interned("panic_2015")) );
            break;
        case AST::Edition::Rust2021:
            toks.push_back( Token(TOK_IDENT, RcString::new_interned("panic_2021")) );
            break;
        }
        toks.push_back( Token(TOK_EXCLAM) );
        toks.push_back( Token(TOK_PAREN_OPEN) );
        if(tt.size() > 0) {
            toks.push_back( tt.clone() );
        }
        toks.push_back( Token(TOK_PAREN_CLOSE) );

        return box$( TTStreamO(sp, ParseState(), TokenTree(AST::Edition::Rust2015, Ident::Hygiene::new_scope(), mv$(toks))) );
    }
};

void Expand_init_panic()
{
    if( TARGETVER_LEAST_1_54 )
    {
        Register_Synext_Macro("panic", ::std::unique_ptr<ExpandProcMacro>(new CExpander_panic));
    }
}

