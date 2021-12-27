/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/assert.cpp
 * - assert! built-in macro (1.29)
 */
#include <synext_macro.hpp>
#include <synext.hpp>   // for Expand_BareExpr
#include <parse/interpolated_fragment.hpp>
#include <ast/crate.hpp>
#include "../parse/ttstream.hpp"
#include "../parse/common.hpp"
#include "../parse/parseerror.hpp"

class CExpander_assert:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;

        auto lex = TTStream(sp, ParseState(), tt);
        lex.parse_state().module = &mod;

        // assertion condition
        auto n = Parse_Expr0(lex);
        ASSERT_BUG(sp, n, "No expression returned");

        ::std::vector<TokenTree> toks;

        toks.push_back( Token(TOK_RWORD_IF) );
        toks.push_back( Token(TOK_EXCLAM) );

        GET_TOK(tok, lex);
        if( tok == TOK_COMMA && lex.lookahead(0) == TOK_EOF ) {
            GET_TOK(tok, lex);
        }
        if( tok == TOK_COMMA )
        {
            toks.push_back( Token(InterpolatedFragment(InterpolatedFragment::EXPR, n.release())) );
            toks.push_back( Token(TOK_BRACE_OPEN) );
            // User-provided message
            toks.push_back( Token(TOK_IDENT, RcString::new_interned("panic")) );
            toks.push_back( Token(TOK_EXCLAM) );
            toks.push_back( Token(TOK_PAREN_OPEN) );

            auto fmt = Parse_Expr0(lex);
            // If there's a comma, it's a formatting sequence
            if( lex.getTokenIf(TOK_COMMA) )
            {
                toks.push_back( Token(InterpolatedFragment(InterpolatedFragment::EXPR, fmt.release())) );

                while(lex.lookahead(0) != TOK_EOF )
                {
                    toks.push_back(TOK_COMMA);

                    if( (lex.lookahead(0) == TOK_IDENT || Token::type_is_rword(lex.lookahead(0))) && lex.lookahead(1) == TOK_EQUAL )
                    {
                        toks.push_back( lex.getToken() );
                        toks.push_back( lex.getToken() );
                        toks.push_back( Token(InterpolatedFragment(InterpolatedFragment::EXPR, Parse_Expr0(lex).release())) );
                    }
                    else
                    {
                        toks.push_back( Token(InterpolatedFragment(InterpolatedFragment::EXPR, Parse_Expr0(lex).release())) );
                    }
                    if( lex.lookahead(0) != TOK_COMMA )
                        break;
                    GET_CHECK_TOK(tok, lex, TOK_COMMA);
                }
            }
            else
            {   // Single-argument: Treat as a `Display`-able value
                toks.push_back( Token(TOK_STRING, std::string("{}")) );
                toks.push_back(TOK_COMMA);
                toks.push_back( Token(InterpolatedFragment(InterpolatedFragment::EXPR, fmt.release())) );
            }

            GET_CHECK_TOK(tok, lex, TOK_EOF);
            toks.push_back( Token(TOK_PAREN_CLOSE) );
        }
        else if( tok == TOK_EOF )
        {
            ::std::stringstream ss;
            ss << "assertion failed: ";
            n->print(ss);

            toks.push_back( Token(InterpolatedFragment(InterpolatedFragment::EXPR, n.release())) );

            toks.push_back( Token(TOK_BRACE_OPEN) );
            // Auto-generated message
            toks.push_back( Token(TOK_IDENT, RcString::new_interned("panic")) );
            toks.push_back( Token(TOK_EXCLAM) );
            toks.push_back( Token(TOK_PAREN_OPEN) );
            toks.push_back( Token(TOK_STRING, ss.str()) );
            toks.push_back( Token(TOK_PAREN_CLOSE) );
        }
        else
        {
            throw ParseError::Unexpected(lex, tok, {TOK_COMMA, TOK_EOF});
        }

        toks.push_back( Token(TOK_BRACE_CLOSE) );

        return box$( TTStreamO(sp, ParseState(), TokenTree(AST::Edition::Rust2015, Ident::Hygiene::new_scope(), mv$(toks))) );
    }
};

void Expand_init_assert()
{
    if( TARGETVER_LEAST_1_29 )
    {
        Register_Synext_Macro("assert", ::std::unique_ptr<ExpandProcMacro>(new CExpander_assert));
    }
}

