/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/pattern.cpp
 * - Parsing for patterns
 */
#include "common.hpp"
#include "parseerror.hpp"

// NEWNODE is needed for the Value pattern type
typedef ::std::unique_ptr<AST::ExprNode>    ExprNodeP;
#define NEWNODE(type, ...)  ExprNodeP(new type(__VA_ARGS__))
using AST::ExprNode;



::std::vector<AST::Pattern> Parse_PatternList(TokenStream& lex, bool is_refutable);
AST::Pattern Parse_PatternReal_Slice(TokenStream& lex, bool is_refutable);
AST::Pattern Parse_PatternReal_Path(TokenStream& lex, AST::Path path, bool is_refutable);
AST::Pattern Parse_PatternReal(TokenStream& lex, bool is_refutable);
AST::Pattern Parse_PatternStruct(TokenStream& lex, AST::Path path, bool is_refutable);


/// Parse a pattern
///
/// Examples:
/// - `Enum::Variant(a)`
/// - `(1, a)`
/// - `1 ... 2`
/// - `"string"`
/// - `mut x`
/// - `mut x @ 1 ... 2`
AST::Pattern Parse_Pattern(TokenStream& lex, bool is_refutable)
{
    TRACE_FUNCTION;

    Token   tok;
    tok = lex.getToken();
    
    if( tok.type() == TOK_MACRO )
    {
        return AST::Pattern( AST::Pattern::TagMacro(), box$(Parse_MacroInvocation(AST::MetaItems(), tok.str(), lex)));
    }
   
    bool expect_bind = false;
    bool is_mut = false;
    bool is_ref = false;
    // 1. Mutablity + Reference
    if( tok.type() == TOK_RWORD_REF )
    {
        is_ref = true;
        expect_bind = true;
        tok = lex.getToken();
    }
    if( tok.type() == TOK_RWORD_MUT )
    {
        is_mut = true;
        expect_bind = true;
        tok = lex.getToken();
    }
    
    ::std::string   bind_name;
    // If a 'ref' or 'mut' annotation was seen, the next name must be a binding name
    if( expect_bind )
    {
        CHECK_TOK(tok, TOK_IDENT);
        bind_name = tok.str();
        // If there's no '@' after it, it's a name binding only (_ pattern)
        if( GET_TOK(tok, lex) != TOK_AT )
        {
            lex.putback(tok);
            return AST::Pattern(AST::Pattern::TagBind(), bind_name);
        }
        
        tok = lex.getToken();
    }
    // Otherwise, handle MaybeBind
    else if( tok.type() == TOK_IDENT )
    {
        lex.putback(tok);
        AST::Path path = Parse_Path(lex, false, PATH_GENERIC_EXPR);
        // - If the path is trivial
        //if( path.is_relative() && path.size() == 1 && path[0].args().size() == 0 )
        if( path.is_trivial() )
        {
            switch( GET_TOK(tok, lex) )
            {
            //  - If the next token after that is '@', use as bind name and expect an actual pattern
            case TOK_AT:
                bind_name = path[0].name();
                GET_TOK(tok, lex);
                // - Fall though
                break;
            //  - Else, if the next token is  a '(' or '{', treat as a struct/enum
            case TOK_BRACE_OPEN:
            case TOK_PAREN_OPEN:
                lex.putback(tok);
                return Parse_PatternReal_Path(lex, path, is_refutable);
            //  - Else, treat as a MaybeBind
            default:
                lex.putback(tok);
                // if the pattern can be refuted (i.e this could be an enum variant), return MaybeBind
                if( is_refutable )
                    return AST::Pattern(AST::Pattern::TagMaybeBind(), path[0].name());
                // Otherwise, it IS a binding
                else
                    return AST::Pattern(AST::Pattern::TagBind(), path[0].name());
            }
        }
        else
        {
            // non-trivial path, has to be a pattern (not a bind)
            return Parse_PatternReal_Path(lex, path, is_refutable);
        }
    }
    else
    {
        // Otherwise, fall through
    }
    
    lex.putback(tok);
    AST::Pattern pat = Parse_PatternReal(lex, is_refutable);
    pat.set_bind(bind_name, is_ref, is_mut);
    return ::std::move(pat);
}

AST::Pattern Parse_PatternReal(TokenStream& lex, bool is_refutable);
AST::Pattern Parse_PatternReal1(TokenStream& lex, bool is_refutable);

AST::Pattern Parse_PatternReal(TokenStream& lex, bool is_refutable)
{
    Token   tok;
    AST::Pattern    ret = Parse_PatternReal1(lex, is_refutable);
    if( GET_TOK(tok, lex) == TOK_TRIPLE_DOT )
    {
        if( !ret.data().is_Value() )
            throw ParseError::Generic(lex, "Using '...' with a non-value on left");
        auto    leftval = ret.take_node();
        auto    right_pat = Parse_PatternReal1(lex, is_refutable);
        if( !right_pat.data().is_Value() )
            throw ParseError::Generic(lex, "Using '...' with a non-value on right");
        auto    rightval = right_pat.take_node();
        
        return AST::Pattern(AST::Pattern::TagValue(), ::std::move(leftval), ::std::move(rightval));
    }
    else
    {
        lex.putback(tok);
        return ret;
    }
}
AST::Pattern Parse_PatternReal1(TokenStream& lex, bool is_refutable)
{
    TRACE_FUNCTION;
    
    Token   tok;
    AST::Path   path;
    
    switch( GET_TOK(tok, lex) )
    {
    case TOK_UNDERSCORE:
        return AST::Pattern( );
    case TOK_DOUBLE_DOT:
        return AST::Pattern( AST::Pattern::TagWildcard() );
    case TOK_RWORD_BOX:
        return AST::Pattern( AST::Pattern::TagBox(), Parse_Pattern(lex, is_refutable) );
    case TOK_DOUBLE_AMP:
        lex.putback(TOK_AMP);
    case TOK_AMP:
        DEBUG("Ref");
        // NOTE: Falls back into "Pattern" not "PatternReal" to handle MaybeBind again
        if( GET_TOK(tok, lex) == TOK_RWORD_MUT )
            // TODO: Actually use mutability
            return AST::Pattern( AST::Pattern::TagReference(), Parse_Pattern(lex, is_refutable) );
        lex.putback(tok);
        return AST::Pattern( AST::Pattern::TagReference(), Parse_Pattern(lex, is_refutable) );
    case TOK_IDENT:
        lex.putback(tok);
        return Parse_PatternReal_Path( lex, Parse_Path(lex, false, PATH_GENERIC_EXPR), is_refutable );
    case TOK_DOUBLE_COLON:
        // 2. Paths are enum/struct names
        return Parse_PatternReal_Path( lex, Parse_Path(lex, true, PATH_GENERIC_EXPR), is_refutable );
    case TOK_DASH: {
        GET_CHECK_TOK(tok, lex, TOK_INTEGER);
        auto dt = tok.datatype();
        if(dt == CORETYPE_ANY)
            dt = CORETYPE_I32;
        return AST::Pattern( AST::Pattern::TagValue(), NEWNODE(AST::ExprNode_Integer, -tok.intval(), dt) );
        }
    case TOK_INTEGER:
        return AST::Pattern( AST::Pattern::TagValue(), NEWNODE(AST::ExprNode_Integer, tok.intval(), tok.datatype()) );
    case TOK_RWORD_TRUE:
        return AST::Pattern( AST::Pattern::TagValue(), NEWNODE( AST::ExprNode_Bool, true ) );
    case TOK_RWORD_FALSE:
        return AST::Pattern( AST::Pattern::TagValue(), NEWNODE( AST::ExprNode_Bool, false ) );
    case TOK_STRING:
        return AST::Pattern( AST::Pattern::TagValue(), NEWNODE(AST::ExprNode_String, tok.str()) );
    case TOK_BYTESTRING:
        return AST::Pattern( AST::Pattern::TagValue(), NEWNODE(AST::ExprNode_String, tok.str()) );
    case TOK_PAREN_OPEN:
        return AST::Pattern( AST::Pattern::TagTuple(), Parse_PatternList(lex, is_refutable) );
    case TOK_SQUARE_OPEN:
        return Parse_PatternReal_Slice(lex, is_refutable);
    default:
        throw ParseError::Unexpected(lex, tok);
    }
}
AST::Pattern Parse_PatternReal_Path(TokenStream& lex, AST::Path path, bool is_refutable)
{
    Token   tok;
    
    switch( GET_TOK(tok, lex) )
    {
    case TOK_PAREN_OPEN:
        return AST::Pattern(AST::Pattern::TagEnumVariant(), ::std::move(path), Parse_PatternList(lex, is_refutable));
    case TOK_BRACE_OPEN:
        return Parse_PatternStruct(lex, ::std::move(path), is_refutable);
    default:
        lex.putback(tok);
        return AST::Pattern(AST::Pattern::TagValue(), NEWNODE(AST::ExprNode_NamedValue, ::std::move(path)));
    }
}

AST::Pattern Parse_PatternReal_Slice(TokenStream& lex, bool is_refutable)
{
    auto sp = lex.start_span();
    Token   tok;
    
    auto rv = ::AST::Pattern( AST::Pattern::TagSlice() );
    auto& rv_array = rv.data().as_Slice();
    
    bool is_trailing = false;
    while(GET_TOK(tok, lex) != TOK_SQUARE_CLOSE)
    {
        if( tok.type() == TOK_IDENT && lex.lookahead(0) == TOK_DOUBLE_DOT) {
            if(is_trailing)
                ERROR(lex.end_span(sp), E0000, "Multiple instances of .. in a slice pattern");
            rv_array.extra_bind = mv$(tok.str());
            is_trailing = true;
            
            GET_TOK(tok, lex);  // TOK_DOUBLE_DOT
        }
        else if( tok.type() == TOK_DOUBLE_DOT ) {
            if(is_trailing)
                ERROR(lex.end_span(sp), E0000, "Multiple instances of .. in a slice pattern");
            rv_array.extra_bind = "_";
            is_trailing = true;
        }
        else {
            lex.putback(tok);
            if(is_trailing) {
                rv_array.trailing.push_back( Parse_Pattern(lex, is_refutable) );
            }
            else {
                rv_array.leading.push_back( Parse_Pattern(lex, is_refutable) );
            }
        }
        
        if( GET_TOK(tok, lex) != TOK_COMMA )
            break;
    }
    CHECK_TOK(tok, TOK_SQUARE_CLOSE);
    
    return rv;
}

::std::vector<AST::Pattern> Parse_PatternList(TokenStream& lex, bool is_refutable)
{
    TRACE_FUNCTION;
    Token tok;
    ::std::vector<AST::Pattern> child_pats;
    
    auto end = TOK_PAREN_CLOSE;
    do {
        if( GET_TOK(tok, lex) == end )
            break;
        else
            lex.putback(tok);
        
        AST::Pattern pat = Parse_Pattern(lex, is_refutable);
        DEBUG("pat = " << pat);
        child_pats.push_back( ::std::move(pat) );
    } while( GET_TOK(tok, lex) == TOK_COMMA );
    CHECK_TOK(tok, end);
    return child_pats;
}

AST::Pattern Parse_PatternStruct(TokenStream& lex, AST::Path path, bool is_refutable)
{
    TRACE_FUNCTION;
    Token tok;
    
    ::std::vector< ::std::pair< ::std::string, AST::Pattern> >  subpats;
    do {
        GET_TOK(tok, lex);
        DEBUG("tok = " << tok);
        if( tok.type() == TOK_BRACE_CLOSE )
            break;
        if( tok.type() == TOK_DOUBLE_DOT ) {
            GET_TOK(tok, lex);
            break;
        }
        
        bool is_short_bind = false;
        if( tok.type() == TOK_RWORD_REF ) {
            is_short_bind = true;
            GET_TOK(tok, lex);
        }
        if( tok.type() == TOK_RWORD_MUT ) {
            is_short_bind = true;
            GET_TOK(tok, lex);
        }
        
        CHECK_TOK(tok, TOK_IDENT);
        ::std::string   field = tok.str();
        GET_TOK(tok, lex);
        
        AST::Pattern    pat;
        if( is_short_bind || tok.type() != TOK_COLON ) {
            lex.putback(tok);
            pat = AST::Pattern(AST::Pattern::TagBind(), field);
        }
        else {
            CHECK_TOK(tok, TOK_COLON);
            pat = Parse_Pattern(lex, is_refutable);
        }
        
        subpats.push_back( ::std::make_pair(::std::move(field), ::std::move(pat)) );
    } while( GET_TOK(tok, lex) == TOK_COMMA );
    CHECK_TOK(tok, TOK_BRACE_CLOSE);
    
    return AST::Pattern(AST::Pattern::TagStruct(), ::std::move(path), ::std::move(subpats));
}

