/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/env.cpp
 * - env! and option_env! macros
 */
#include <synext_macro.hpp>
#include <parse/common.hpp>
#include <parse/ttstream.hpp>
#include <ast/expr.hpp> // ExprNode_*
#include <synext.hpp>   // for Expand_BareExpr

namespace {
    // Read a string out of the input stream
    ::std::string get_string(const Span& sp, const AST::Crate& crate, AST::Module& mod, const TokenTree& tt) {
        auto lex = TTStream(sp, tt);

        auto n = Parse_ExprVal(lex);
        ASSERT_BUG(sp, n, "No expression returned");
        if( lex.lookahead(0) != TOK_EOF ) {
            ERROR(sp, E0000, "Unexpected token after string literal - " << lex.getToken());
        }
        Expand_BareExpr(crate, mod, n);

        auto* string_np = dynamic_cast<AST::ExprNode_String*>(&*n);
        if( !string_np ) {
            ERROR(sp, E0000, "Expected a string literal - got " << *n);
        }
        return mv$( string_np->m_value );
    }
}

class CExpanderEnv:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        if( ident != "" )
            ERROR(sp, E0000, "env! doesn't take an ident");
        ::std::string   varname = get_string(sp, crate, mod,  tt);

        const char* var_val_cstr = getenv(varname.c_str());
        if( !var_val_cstr ) {
            ERROR(sp, E0000, "Environment variable '" << varname << "' not defined");
        }
        return box$( TTStreamO(sp, TokenTree(Token(TOK_STRING, var_val_cstr))) );
    }
};

class CExpanderOptionEnv:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        if( ident != "" )
            ERROR(sp, E0000, "option_env! doesn't take an ident");
        ::std::string   varname = get_string(sp, crate, mod,  tt);

        const char* var_val_cstr = getenv(varname.c_str());
        if( !var_val_cstr ) {
            ::std::vector< TokenTree>   rv;
            rv.reserve(7);
            rv.push_back( Token(TOK_IDENT, "None") );
            rv.push_back( Token(TOK_DOUBLE_COLON) );
            rv.push_back( Token(TOK_LT) );
            rv.push_back( Token(TOK_AMP) );
            rv.push_back( Token(TOK_LIFETIME, "static") );
            rv.push_back( Token(TOK_IDENT, "str") );
            rv.push_back( Token(TOK_GT) );
            return box$( TTStreamO(sp, TokenTree( {}, mv$(rv) )) );
        }
        else {
            ::std::vector< TokenTree>   rv;
            rv.reserve(4);
            rv.push_back( Token(TOK_IDENT, "Some") );
            rv.push_back( Token(TOK_PAREN_OPEN) );
            rv.push_back( Token(TOK_STRING, var_val_cstr) );
            rv.push_back( Token(TOK_PAREN_CLOSE) );
            return box$( TTStreamO(sp, TokenTree( {}, mv$(rv) )) );
        }
    }
};

STATIC_MACRO("env", CExpanderEnv);
STATIC_MACRO("option_env", CExpanderOptionEnv);
