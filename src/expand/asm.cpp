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
    bool    expand_early() const override { return true; }
    
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        // TODO: Convert this into an AST node
        return box$( TTStreamO(TokenTree()) );
        
        #if 0
        Token   tok;
        auto lex = TTStream(tt);
        if( ident != "" )
            ERROR(sp, E0000, "format_args! doesn't take an ident");
        
        auto n = Parse_ExprVal(lex);
        auto format_string = dynamic_cast<AST::ExprNode_String&>(*n).m_value;
        
        ::std::map< ::std::string, unsigned int>   named_args_index;
        ::std::vector<TokenTree>    named_args;
        ::std::vector<TokenTree>    free_args;
        
        // - Parse the arguments
        while( GET_TOK(tok, lex) == TOK_COMMA )
        {
            // - Named parameters
            if( lex.lookahead(0) == TOK_IDENT && lex.lookahead(1) == TOK_EQUAL )
            {
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                auto name = mv$(tok.str());
                
                GET_CHECK_TOK(tok, lex, TOK_EQUAL);
                
                auto expr_tt = Parse_TT_Expr(lex);
                
                auto ins_rv = named_args_index.insert( ::std::make_pair(mv$(name), named_args.size()) );
                if( ins_rv.second == false ) {
                    ERROR(sp, E0000, "Duplicate definition of named argument `" << ins_rv.first->first << "`");
                }
                named_args.push_back( mv$(expr_tt) );
            }
            // - Free parameters
            else
            {
                auto expr_tt = Parse_TT_Expr(lex);
                free_args.push_back( mv$(expr_tt) );
            }
        }
        
        // - Parse the format string
        ::std::vector< FmtFrag> fragments;
        ::std::string   tail;
        ::std::tie( fragments, tail ) = parse_format_string(sp, format_string,  named_args_index, free_args.size());
        
        // TODO: Properly expand format_args! (requires mangling to fit ::core::fmt::rt::v1)
        // - For now, just emits the text with no corresponding format fragments
        
        ::std::vector<TokenTree> toks;
        {
            switch(crate.m_load_std)
            {
            case ::AST::Crate::LOAD_NONE:
                break;
            case ::AST::Crate::LOAD_CORE:
                toks.push_back( TokenTree(TOK_DOUBLE_COLON) );
                toks.push_back( Token(TOK_STRING, "core") );
                break;
            case ::AST::Crate::LOAD_STD:
                toks.push_back( TokenTree(TOK_DOUBLE_COLON) );
                toks.push_back( Token(TOK_IDENT, "std") );
                break;
            }
            
            // ::fmt::Arguments::new_v1
            toks.push_back( TokenTree(TOK_DOUBLE_COLON) );
            toks.push_back( Token(TOK_IDENT, "fmt") );
            toks.push_back( TokenTree(TOK_DOUBLE_COLON) );
            toks.push_back( Token(TOK_IDENT, "Arguments") );
            toks.push_back( TokenTree(TOK_DOUBLE_COLON) );
            toks.push_back( Token(TOK_IDENT, "new_v1") );
            // (
            toks.push_back( TokenTree(TOK_PAREN_OPEN) );
            {
                toks.push_back( TokenTree(TOK_AMP) );
                // Raw string fragments
                toks.push_back( TokenTree(TOK_SQUARE_OPEN) );
                for(const auto& frag : fragments ) {
                    toks.push_back( Token(TOK_STRING, frag.leading_text) );
                    toks.push_back( TokenTree(TOK_COMMA) );
                }
                toks.push_back( TokenTree(TOK_SQUARE_CLOSE) );
                toks.push_back( TokenTree(TOK_COMMA) );
                
                // TODO: Fragments to format
                // - The format stored by mrustc doesn't quite work with how rustc (and fmt::rt::v1) works
                toks.push_back( TokenTree(TOK_AMP) );
                toks.push_back( TokenTree(TOK_SQUARE_OPEN) );
                toks.push_back( TokenTree(TOK_SQUARE_CLOSE) );
            }
            // )
            toks.push_back( TokenTree(TOK_PAREN_CLOSE) );
        }
        
        return box$( TTStreamO(TokenTree(mv$(toks))) );
        #endif
    }
};

STATIC_MACRO("asm", CAsmExpander);
