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

AST::Pattern Parse_PatternReal(TokenStream& lex, bool is_refutable);
AST::Pattern Parse_PatternReal1(TokenStream& lex, bool is_refutable);


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
    auto ps = lex.start_span();

    Token   tok;
    tok = lex.getToken();
    
    if( tok.type() == TOK_MACRO )
    {
        return AST::Pattern( AST::Pattern::TagMacro(), box$(Parse_MacroInvocation(ps, AST::MetaItems(), tok.str(), lex)));
    }
    if( tok.type() == TOK_INTERPOLATED_PATTERN )
    {
        return mv$(tok.frag_pattern());
    }
   
    bool expect_bind = false;
    ::AST::Pattern::BindType    bind_type = AST::Pattern::BIND_MOVE;
    bool is_mut = false;
    // 1. Mutablity + Reference
    if( tok.type() == TOK_RWORD_REF )
    {
        expect_bind = true;
        tok = lex.getToken();
        if( tok.type() == TOK_RWORD_MUT )
        {
            bind_type = AST::Pattern::BIND_MUTREF;
            GET_TOK(tok, lex);
        }
        else
        {
            bind_type = AST::Pattern::BIND_REF;
        }
    }
    else if( tok.type() == TOK_RWORD_MUT )
    {
        is_mut = true;
        expect_bind = true;
        tok = lex.getToken();
    }
    else
    {
        // Fall through
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
            PUTBACK(tok, lex);
            return AST::Pattern(AST::Pattern::TagBind(), bind_name);
        }
        
        tok = lex.getToken();
    }
    // Otherwise, handle MaybeBind
    else if( tok.type() == TOK_IDENT )
    {
        switch( LOOK_AHEAD(lex) )
        {
        // Known path `ident::`
        case TOK_DOUBLE_COLON:
            break;
        // Known struct `Ident {` or `Ident (`
        case TOK_BRACE_OPEN:
        case TOK_PAREN_OPEN:
            break;
        // Known value `IDENT ...`
        case TOK_TRIPLE_DOT:
            break;
        // Known binding `ident @`
        case TOK_AT:
            bind_name = mv$(tok.str());
            GET_TOK(tok, lex);
            GET_TOK(tok, lex);  // Match lex.putback() below
            break;
        default:  // Maybe bind
            // if the pattern can be refuted (i.e this could be an enum variant), return MaybeBind
            if( is_refutable )
                return AST::Pattern(AST::Pattern::TagMaybeBind(), mv$(tok.str()));
            // Otherwise, it IS a binding
            else
                return AST::Pattern(AST::Pattern::TagBind(), mv$(tok.str()));
        }
    }
    else
    {
        // Otherwise, fall through
    }
    
    PUTBACK(tok, lex);
    AST::Pattern pat = Parse_PatternReal(lex, is_refutable);
    pat.set_bind(bind_name, bind_type, is_mut);
    return ::std::move(pat);
}

AST::Pattern Parse_PatternReal(TokenStream& lex, bool is_refutable)
{
    Token   tok;
    AST::Pattern    ret = Parse_PatternReal1(lex, is_refutable);
    if( GET_TOK(tok, lex) == TOK_TRIPLE_DOT )
    {
        if( !ret.data().is_Value() )
            throw ParseError::Generic(lex, "Using '...' with a non-value on left");
        auto& ret_v = ret.data().as_Value();
        
        auto    right_pat = Parse_PatternReal1(lex, is_refutable);
        if( !right_pat.data().is_Value() )
            throw ParseError::Generic(lex, "Using '...' with a non-value on right");
        auto    rightval = mv$( right_pat.data().as_Value().start );
        ret_v.end = mv$(rightval);
        
        return ret;
    }
    else
    {
        PUTBACK(tok, lex);
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
    //case TOK_DOUBLE_DOT:
    //    return AST::Pattern( AST::Pattern::TagWildcard() );
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
        PUTBACK(tok, lex);
        return AST::Pattern( AST::Pattern::TagReference(), Parse_Pattern(lex, is_refutable) );
    case TOK_RWORD_SELF:
    case TOK_RWORD_SUPER:
    case TOK_IDENT:
        PUTBACK(tok, lex);
        return Parse_PatternReal_Path( lex, Parse_Path(lex, PATH_GENERIC_EXPR), is_refutable );
    case TOK_DOUBLE_COLON:
        // 2. Paths are enum/struct names
        return Parse_PatternReal_Path( lex, Parse_Path(lex, true, PATH_GENERIC_EXPR), is_refutable );
    case TOK_DASH:
        if(GET_TOK(tok, lex) == TOK_INTEGER)
        {
            auto dt = tok.datatype();
            if(dt == CORETYPE_ANY)
                dt = CORETYPE_I32;
            return AST::Pattern( AST::Pattern::TagValue(), AST::Pattern::Value::make_Integer({dt, -tok.intval()}) );
        }
        //else if( tok.type() == TOK_FLOAT )
        //{
        //    auto dt = tok.datatype();
        //    if(dt == CORETYPE_ANY)
        //        dt = CORETYPE_F32;
        //    return AST::Pattern( AST::Pattern::TagValue(), AST::Pattern::Value::make_Integer({dt, reinterpret_cast<uint64_t>(-tok.floatval()), dt}) );
        //}
        else
        {
            throw ParseError::Unexpected(lex, tok, {TOK_INTEGER, TOK_FLOAT});
        }
    //case TOK_FLOAT:
    //    return AST::Pattern( AST::Pattern::TagValue(), AST::Pattern::Value::make_Integer({tok.datatype(), reinterpret_cast<uint64_t>(tok.floatval())}) );
    case TOK_INTEGER:
        return AST::Pattern( AST::Pattern::TagValue(), AST::Pattern::Value::make_Integer({tok.datatype(), tok.intval()}) );
    case TOK_RWORD_TRUE:
        return AST::Pattern( AST::Pattern::TagValue(), AST::Pattern::Value::make_Integer({CORETYPE_BOOL, 1}) );
    case TOK_RWORD_FALSE:
        return AST::Pattern( AST::Pattern::TagValue(), AST::Pattern::Value::make_Integer({CORETYPE_BOOL, 0}) );
    case TOK_STRING:
        return AST::Pattern( AST::Pattern::TagValue(), AST::Pattern::Value::make_String( mv$(tok.str()) ) );
    case TOK_BYTESTRING:
        // TODO: Differentiate byte and UTF-8 strings
        return AST::Pattern( AST::Pattern::TagValue(), AST::Pattern::Value::make_String( mv$(tok.str()) ) );
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
        if( LOOK_AHEAD(lex) == TOK_DOUBLE_DOT ) {
            GET_TOK(tok, lex);
            GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
            return AST::Pattern( AST::Pattern::TagEnumVariant(), ::std::move(path) );
        }
        return AST::Pattern( AST::Pattern::TagEnumVariant(), ::std::move(path), Parse_PatternList(lex, is_refutable) );
    case TOK_BRACE_OPEN:
        return Parse_PatternStruct(lex, ::std::move(path), is_refutable);
    default:
        PUTBACK(tok, lex);
        return AST::Pattern( AST::Pattern::TagValue(), AST::Pattern::Value::make_Named(mv$(path)) );
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
        ::std::string   binding_name;
        if( tok.type() == TOK_RWORD_REF && lex.lookahead(0) == TOK_IDENT && lex.lookahead(1) == TOK_DOUBLE_DOT ) {
            GET_TOK(tok, lex);
            // TODO: Bind type
            binding_name = tok.str();
        }
        else if( tok.type() == TOK_IDENT && lex.lookahead(0) == TOK_DOUBLE_DOT) {
            // TODO: Bind type
            binding_name = tok.str();
        }
        else if( tok.type() == TOK_UNDERSCORE && lex.lookahead(0) == TOK_DOUBLE_DOT) {
            binding_name = "_";
        }
        else if( tok.type() == TOK_DOUBLE_DOT ) {
            binding_name = "_";
            PUTBACK(tok, lex);
        }
        else {
        }
        
        if( binding_name != "" ) {
            if(is_trailing)
                ERROR(lex.end_span(sp), E0000, "Multiple instances of .. in a slice pattern");
            rv_array.extra_bind = mv$(binding_name);
            is_trailing = true;
            GET_TOK(tok, lex);  // TOK_DOUBLE_DOT
        }
        else {
            PUTBACK(tok, lex);
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
            PUTBACK(tok, lex);
        
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
    
    bool is_exhaustive = true;
    ::std::vector< ::std::pair< ::std::string, AST::Pattern> >  subpats;
    do {
        GET_TOK(tok, lex);
        DEBUG("tok = " << tok);
        if( tok.type() == TOK_BRACE_CLOSE )
            break;
        if( tok.type() == TOK_DOUBLE_DOT ) {
            is_exhaustive = false;
            GET_TOK(tok, lex);
            break;
        }
        
        bool is_short_bind = false;
        bool is_box = false;
        AST::Pattern::BindType bind_type = AST::Pattern::BIND_MOVE;
        bool is_mut = false;
        if( tok.type() == TOK_RWORD_BOX ) {
            is_box = true;
            is_short_bind = true;
            GET_TOK(tok, lex);
        }
        if( tok.type() == TOK_RWORD_REF ) {
            is_short_bind = true;
            GET_TOK(tok, lex);
            if( tok.type() == TOK_RWORD_MUT ) {
                bind_type = AST::Pattern::BIND_MUTREF;
                GET_TOK(tok, lex);
            }
            else {
                bind_type = AST::Pattern::BIND_REF;
            }
        }
        else if( tok.type() == TOK_RWORD_MUT ) {
            is_mut = true;
            is_short_bind = true;
            GET_TOK(tok, lex);
        }
        
        CHECK_TOK(tok, TOK_IDENT);
        ::std::string   field = tok.str();
        GET_TOK(tok, lex);
        
        AST::Pattern    pat;
        if( is_short_bind || tok.type() != TOK_COLON ) {
            PUTBACK(tok, lex);
            pat = AST::Pattern(AST::Pattern::TagBind(), field);
            pat.set_bind(field, bind_type, is_mut);
            if( is_box )
            {
                pat = AST::Pattern(AST::Pattern::TagBox(), mv$(pat));
            }
        }
        else {
            CHECK_TOK(tok, TOK_COLON);
            pat = Parse_Pattern(lex, is_refutable);
        }
        
        subpats.push_back( ::std::make_pair(::std::move(field), ::std::move(pat)) );
    } while( GET_TOK(tok, lex) == TOK_COMMA );
    CHECK_TOK(tok, TOK_BRACE_CLOSE);
    
    return AST::Pattern(AST::Pattern::TagStruct(), ::std::move(path), ::std::move(subpats), is_exhaustive);
}

