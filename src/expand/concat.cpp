/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/concat.cpp
 * - concat! handler
 */
#include <synext.hpp>
#include "../parse/common.hpp"
#include "../parse/parseerror.hpp"
#include "../parse/tokentree.hpp"
#include "../parse/ttstream.hpp"
#include "../parse/lex.hpp" // For Codepoint
#include <ast/expr.hpp>

class CConcatExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;

        auto lex = TTStream(tt);
        if( ident != "" )
            ERROR(sp, E0000, "format_args! doesn't take an ident");

        ::std::string   rv;
        do {
            if( LOOK_AHEAD(lex) == TOK_EOF ) {
                GET_TOK(tok, lex);
                break ;
            }

            auto v = Parse_Expr0(lex);
            DEBUG("concat - v=" << *v);
            Expand_BareExpr(crate, mod,  v);
            DEBUG("concat[pe] - v=" << *v);
            if( auto* vp = dynamic_cast<AST::ExprNode_String*>(v.get()) )
            {
                rv += vp->m_value;
            }
            else if( auto* vp = dynamic_cast<AST::ExprNode_Integer*>(v.get()) )
            {
                if( vp->m_datatype == CORETYPE_CHAR ) {
                    rv += Codepoint { static_cast<uint32_t>(vp->m_value) };
                }
                else {
                    rv += format(vp->m_value);
                }
            }
            else if( auto* vp = dynamic_cast<AST::ExprNode_Float*>(v.get()) )
            {
                rv += format(vp->m_value);
            }
            else if( auto* vp = dynamic_cast<AST::ExprNode_Bool*>(v.get()) )
            {
                rv += (vp->m_value ? "true" : "false");
            }
            else
            {
                ERROR(sp, E0000, "Unexpected expression type in concat! argument");
            }
        } while( GET_TOK(tok, lex) == TOK_COMMA );
        if( tok.type() != TOK_EOF )
            throw ParseError::Unexpected(lex, tok, {TOK_COMMA, TOK_EOF});

        return box$( TTStreamO(TokenTree(Token(TOK_STRING, mv$(rv)))) );
    }
};

STATIC_MACRO("concat", CConcatExpander);

