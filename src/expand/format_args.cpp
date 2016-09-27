/*
 */
#include <synext_macro.hpp>
#include <synext.hpp>   // for Expand_BareExpr
#include "../parse/common.hpp"
#include "../parse/parseerror.hpp"
#include "../parse/tokentree.hpp"
#include "../parse/lex.hpp"
#include <ast/crate.hpp>    // for m_load_std
#include <ast/expr.hpp>    // for ExprNode_*

namespace {
    
    struct FmtArgs
    {
        enum class Align {
            Unspec,
            Left,
            Center,
            Right,
        };
        enum class Sign {
            Unspec,
            Plus,
            Minus,
        };
        
        Align   align = Align::Unspec;
        char    align_char = ' ';
        
        Sign    sign = Sign::Unspec;
        bool    alternate = false;
        bool    zero_pad = false;
        
        bool width_is_arg = false;
        unsigned int width = 0;
        
        bool prec_is_arg = false;
        unsigned int prec = 0;
    };
    
    struct FmtFrag
    {
        ::std::string   leading_text;
        
        unsigned int    arg_index;
        
        const char* trait_name;
        // TODO: Support case where this hasn't been edited (telling the formatter that it has nothing to apply)
        FmtArgs     args;
    };

    ::std::tuple< ::std::vector<FmtFrag>, ::std::string> parse_format_string(const Span& sp, const ::std::string& format_string, const ::std::map< ::std::string,unsigned int>& named, unsigned int n_free)
    {
        unsigned int n_named = named.size();
        unsigned int next_free = 0;
        
        ::std::vector<FmtFrag>  frags;
        ::std::string   cur_literal;
        
        const char* s = format_string.c_str();
        for( ; *s; s ++)
        {
            if( *s != '{' )
            {
                if( *s == '}' ) {
                    s ++;
                    if( *s != '}' )
                        ERROR(sp, E0000, "'}' must be escaped as '}}' in format strings");
                    // - fall with *s == '}'
                }
                cur_literal += *s;
            }
            else
            {
                s ++;
                if( *s == '{' ) {
                    cur_literal += '{';
                    continue ;
                }
                
                unsigned int index = ~0u;
                const char* trait_name;
                FmtArgs args;
                
                // Formatting parameter
                if( *s != ':' && *s != '}' ) {
                    // Parse either an integer or an identifer
                    TODO(sp, "Parse named/positional formatting fragment at \"" << s);
                }
                else {
                    // Leave (for now)
                    // - If index is ~0u at the end of this block, it's set to the next arg
                    // - This allows {:.*} to format correctly (taking <prec> then <arg>)
                }
                
                // If next character is ':', parse extra information
                if( *s == ':' ) {
                    s ++;   // eat ':'
                    
                    // Alignment
                    if( s[0] != '\0' && (s[1] == '<' || s[1] == '^' || s[1] == '>') ) {
                        args.align_char = s[0];
                        s ++;
                    }
                    if( *s == '<' ) {
                        args.align = FmtArgs::Align::Left;
                        s ++;
                    }
                    else if( *s == '^' ) {
                        args.align = FmtArgs::Align::Center;
                        s ++;
                    }
                    else if( *s == '>' ) {
                        args.align = FmtArgs::Align::Right;
                        s ++;
                    }
                    else {
                        //args.align = FmtArgs::Align::Unspec;
                    }
                    
                    // Sign
                    if( *s == '+' ) {
                        args.sign = FmtArgs::Sign::Plus;
                        s ++;
                    }
                    else if( *s == '-' ) {
                        args.sign = FmtArgs::Sign::Minus;
                        s ++;
                    }
                    else {
                        args.sign = FmtArgs::Sign::Unspec;
                    }
                    
                    if( *s == '#' ) {
                        args.alternate = true;
                        s ++;
                    }
                    else {
                        //args.alternate = false;
                    }
                    
                    if( *s == '0' ) {
                        args.zero_pad = true;
                        s ++;
                    }
                    else {
                        //args.zero_pad = false;
                    }
                    
                    // Padded width
                    if( ::std::isdigit(*s) /*|| *s == '*'*/ ) {
                        unsigned int val = 0;
                        while( ::std::isdigit(*s) )
                        {
                            val *= 10;
                            val += *s - '0';
                            s ++;
                        }
                        args.width = val;
                        
                        if( *s == '$' ) {
                            args.width_is_arg = true;
                            s ++;
                        }
                        else {
                            //args.width_is_arg = false;
                        }
                    }
                    // Precision
                    if( *s == '.' ) {
                        s ++;
                        // '*' - Use next argument
                        if( *s == '*' ) {
                            args.prec_is_arg = true;
                            if( next_free == n_free ) {
                                ERROR(sp, E0000, "Not enough arguments passed, expected at least " << n_free+1);
                            }
                            args.prec = next_free + n_named;
                            next_free ++;
                        }
                        else {
                            unsigned int val = 0;
                            while( ::std::isdigit(*s) )
                            {
                                val *= 10;
                                val += *s - '0';
                                s ++;
                            }
                            args.prec = val;
                            
                            if( *s == '$' ) {
                                args.prec_is_arg = true;
                                s ++;
                            }
                            else {
                                //args.prec_is_arg = false;
                            }
                        }
                    }
                    
                    // Parse ident?
                    // - Lazy way is to just handle a single char and ensure that it is just a single char
                    if( s[0] != '}' && s[0] != '\0' && s[1] != '}' ) {
                        TODO(sp, "Parse formatting fragment at \"" << s << "\" (long type)");
                    }
                    
                    switch(s[0])
                    {
                    case '\0':
                        ERROR(sp, E0000, "Unexpected end of formatting string");
                    default:
                        ERROR(sp, E0000, "Unknown formatting type specifier '" << *s << "'");
                    case '}':      trait_name = "Display"; break;
                    case '?': s++; trait_name = "Debug"  ; break;
                    case 'b': s++; trait_name = "Binary"; break;
                    case 'o': s++; trait_name = "Octal" ; break;
                    case 'x': s++; trait_name = "LowerHex"; break;
                    case 'X': s++; trait_name = "UpperHex"; break;
                    case 'p': s++; trait_name = "Pointer" ; break;
                    case 'e': s++; trait_name = "LowerExp"; break;
                    case 'E': s++; trait_name = "UpperExp"; break;
                    }
                    assert(*s == '}');
                }
                else {
                    if( *s != '}' )
                        ERROR(sp, E0000, "Malformed formatting fragment, unexpected " << *s);
                    // Otherwise, it's just a trivial Display call
                    trait_name = "Display";
                }
                
                // Set index if unspecified
                if( index == ~0u )
                {
                    if( next_free == n_free ) {
                        ERROR(sp, E0000, "Not enough arguments passed, expected at least " << n_free+1);
                    }
                    index = next_free + n_named;
                    next_free ++;
                }
                
                frags.push_back( FmtFrag {
                    mv$(cur_literal),
                    index, trait_name,
                    mv$(args)
                    });
            }
        }
        
        return ::std::make_tuple( mv$(frags), mv$(cur_literal) );
    }
}

class CFormatArgsExpander:
    public ExpandProcMacro
{
    bool    expand_early() const override { return true; }
    
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const ::AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;
        
        auto lex = TTStream(tt);
        if( ident != "" )
            ERROR(sp, E0000, "format_args! doesn't take an ident");
        
        auto n = Parse_ExprVal(lex);
        ASSERT_BUG(sp, n, "No expression returned");
        Expand_BareExpr(crate, mod, n);

        auto* format_string_np = dynamic_cast<AST::ExprNode_String*>(&*n);
        if( !format_string_np ) {
            ERROR(sp, E0000, "format_args! requires a string literal - got " << *n);
        }
        const auto& format_string = format_string_np->m_value;
        
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
    }
};

STATIC_MACRO("format_args", CFormatArgsExpander);

