/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/asm.cpp
 * - asm! macro
 */
#include <common.hpp>
#include <synext_macro.hpp>
#include <synext.hpp>   // for Expand_BareExpr
#include <parse/tokentree.hpp>
#include <parse/ttstream.hpp>
#include <parse/common.hpp>
#include <parse/parseerror.hpp>
#include <ast/expr.hpp>    // for ExprNode_*
#include <parse/interpolated_fragment.hpp>

namespace
{
    ::std::string get_string(const Span& sp, TokenStream& lex, const ::AST::Crate& crate, AST::Module& mod)
    {
        auto n = Parse_ExprVal(lex);
        ASSERT_BUG(sp, n, "No expression returned");
        Expand_BareExpr(crate, mod, n);

        auto* format_string_np = dynamic_cast<AST::ExprNode_String*>(&*n);
        if( !format_string_np ) {
            ERROR(sp, E0000, "asm! requires a string literal - got " << *n);
        }
        //const auto& format_string_sp = format_string_np->span();
        return mv$( format_string_np->m_value );
    }
}

class CAsmExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;
        auto lex = TTStream(tt);
        if( ident != "" )
            ERROR(sp, E0000, "format_args! doesn't take an ident");

        auto template_text = get_string(sp, lex,  crate, mod);
        ::std::vector<::AST::ExprNode_Asm::ValRef>  outputs;
        ::std::vector<::AST::ExprNode_Asm::ValRef>  inputs;
        ::std::vector<::std::string>    clobbers;
        ::std::vector<::std::string>    flags;

        // Outputs
        if( lex.lookahead(0) == TOK_COLON || lex.lookahead(0) == TOK_DOUBLE_COLON )
        {
            GET_TOK(tok, lex);
            if( tok.type() == TOK_DOUBLE_COLON ) {
                lex.putback(Token(TOK_COLON));
            }

            while( lex.lookahead(0) == TOK_STRING )
            {
                //auto name = get_string(sp, lex);
                GET_CHECK_TOK(tok, lex, TOK_STRING);
                auto name = mv$(tok.str());

                GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
                auto val = Parse_Expr0(lex);
                GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);

                outputs.push_back( ::AST::ExprNode_Asm::ValRef { mv$(name), mv$(val) } );

                if( lex.lookahead(0) != TOK_COMMA )
                    break;

                GET_TOK(tok, lex);
            }
        }

        // Inputs
        if( lex.lookahead(0) == TOK_COLON || lex.lookahead(0) == TOK_DOUBLE_COLON )
        {
            GET_TOK(tok, lex);
            if( tok.type() == TOK_DOUBLE_COLON ) {
                lex.putback(Token(TOK_COLON));
            }

            while( lex.lookahead(0) == TOK_STRING )
            {
                GET_CHECK_TOK(tok, lex, TOK_STRING);
                auto name = mv$(tok.str());

                GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
                auto val = Parse_Expr0(lex);
                GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);

                inputs.push_back( ::AST::ExprNode_Asm::ValRef { mv$(name), mv$(val) } );

                if( lex.lookahead(0) != TOK_COMMA )
                    break;
                GET_TOK(tok, lex);
            }
        }

        // Clobbers
        if( lex.lookahead(0) == TOK_COLON || lex.lookahead(0) == TOK_DOUBLE_COLON )
        {
            GET_TOK(tok, lex);
            if( tok.type() == TOK_DOUBLE_COLON ) {
                lex.putback(Token(TOK_COLON));
            }

            while( lex.lookahead(0) == TOK_STRING )
            {
                GET_CHECK_TOK(tok, lex, TOK_STRING);
                clobbers.push_back( mv$(tok.str()) );

                if( lex.lookahead(0) != TOK_COMMA )
                    break;
                GET_TOK(tok, lex);
            }
        }

        // Flags
        if( lex.lookahead(0) == TOK_COLON || lex.lookahead(0) == TOK_DOUBLE_COLON )
        {
            GET_TOK(tok, lex);
            if( tok.type() == TOK_DOUBLE_COLON ) {
                lex.putback(Token(TOK_COLON));
            }

            while( lex.lookahead(0) == TOK_STRING )
            {
                GET_CHECK_TOK(tok, lex, TOK_STRING);
                flags.push_back( mv$(tok.str()) );

                if( lex.lookahead(0) != TOK_COMMA )
                    break;
                GET_TOK(tok, lex);
            }
        }

        if( lex.lookahead(0) == TOK_COLON || lex.lookahead(0) == TOK_DOUBLE_COLON )
        {
            GET_TOK(tok, lex);
            if( tok.type() == TOK_DOUBLE_COLON ) {
                lex.putback(Token(TOK_COLON));
            }

            if( GET_TOK(tok, lex) == TOK_IDENT && tok.str() == "volatile" )
            {
                flags.push_back( "volatile" );
            }
            else
            {
                PUTBACK(tok, lex);
            }
        }

        if( lex.lookahead(0) != TOK_EOF )
        {
            ERROR(sp, E0000, "Unexpected token in asm! - " << lex.getToken());
        }

        ::AST::ExprNodeP rv = ::AST::ExprNodeP( new ::AST::ExprNode_Asm { mv$(template_text), mv$(outputs), mv$(inputs), mv$(clobbers), mv$(flags) } );
        // TODO: Convert this into an AST node
        return box$( TTStreamO(TokenTree(Token( InterpolatedFragment(InterpolatedFragment::EXPR, rv.release()) ))));
    }
};

STATIC_MACRO("asm", CAsmExpander);
