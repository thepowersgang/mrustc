/*
 */
#include <synext.hpp>
#include "../parse/common.hpp"
#include "../parse/parseerror.hpp"
#include "../parse/tokentree.hpp"
#include "../parse/lex.hpp"
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
                rv += FMT(vp->m_value);
            }
            else
            {
                ERROR(sp, E0000, "Unexpected expression type");
            }
        } while( GET_TOK(tok, lex) == TOK_COMMA );
        if( tok.type() != TOK_EOF )
            throw ParseError::Unexpected(lex, tok, {TOK_COMMA, TOK_EOF});
        
        return box$( TTStreamO(TokenTree(Token(TOK_STRING, mv$(rv)))) );
    }
};

STATIC_MACRO("concat", CConcatExpander);

