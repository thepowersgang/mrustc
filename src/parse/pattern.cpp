/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/pattern.cpp
 * - Parsing for patterns
 */
#include "common.hpp"
#include "parseerror.hpp"
#include <ast/expr.hpp> // To convert :expr

// NEWNODE is needed for the Value pattern type
typedef ::std::unique_ptr<AST::ExprNode>    ExprNodeP;
#define NEWNODE(type, ...)  ExprNodeP(new type(__VA_ARGS__))
using AST::ExprNode;



AST::Pattern Parse_Pattern1(TokenStream& lex, AllowOrPattern allow_or);
AST::Pattern::Value Parse_PatternValue(TokenStream& lex);
AST::Pattern::TuplePat Parse_PatternTuple(TokenStream& lex, bool* maybe_just_paren=nullptr);
AST::Pattern Parse_PatternReal_Slice(TokenStream& lex);
AST::Pattern Parse_PatternReal_Path(TokenStream& lex, ProtoSpan ps, AST::Path path);
AST::Pattern Parse_PatternStruct(TokenStream& lex, ProtoSpan ps, AST::Path path);

AST::Pattern Parse_PatternReal(TokenStream& lex, AllowOrPattern allow_or);
AST::Pattern Parse_PatternReal1(TokenStream& lex, AllowOrPattern allow_or);


/// Parse a pattern
///
/// Examples:
/// - `Enum::Variant(a)`
/// - `(1, a)`
/// - `1 ... 2`
/// - `"string"`
/// - `mut x`
/// - `mut x @ 1 ... 2`
AST::Pattern Parse_Pattern(TokenStream& lex, AllowOrPattern allow_or)
{
    auto ps = lex.start_span();
    auto rv = Parse_Pattern1(lex, allow_or);
    if( allow_or == AllowOrPattern::Yes && lex.lookahead(0) == TOK_PIPE )
    {
        // NOTE: Legal for refutable positions (as long as all possibilities are covered)
        std::vector<AST::Pattern>   pats;
        pats.push_back(std::move(rv));
        while( lex.lookahead(0) == TOK_PIPE )
        {
            lex.getToken();
            pats.push_back(Parse_Pattern1(lex, allow_or));
        }
        return AST::Pattern( lex.end_span(ps), AST::Pattern::Data::make_Or(mv$(pats)) ); 
    }
    else
    {
        return rv;
    }
}
AST::Pattern Parse_Pattern1(TokenStream& lex, AllowOrPattern allow_or)
{
    TRACE_FUNCTION;
    auto ps = lex.start_span();

    Token   tok;
    tok = lex.getToken();

    // TODO: Why is this here explicitly?
    if( tok.type() == TOK_IDENT && lex.lookahead(0) == TOK_EXCLAM )
    {
        lex.getToken();
        return AST::Pattern( AST::Pattern::TagMacro(), lex.end_span(ps), box$(Parse_MacroInvocation(ps, tok.ident().name, lex)));
    }
    if( tok.type() == TOK_INTERPOLATED_PATTERN )
    {
        return mv$(tok.frag_pattern());
    }

    bool expect_bind = false;
    auto bind_type = AST::PatternBinding::Type::MOVE;
    bool is_mut = false;
    // 1. Mutablity + Reference
    if( tok.type() == TOK_RWORD_REF )
    {
        expect_bind = true;
        tok = lex.getToken();
        if( tok.type() == TOK_RWORD_MUT )
        {
            bind_type = AST::PatternBinding::Type::MUTREF;
            GET_TOK(tok, lex);
        }
        else
        {
            bind_type = AST::PatternBinding::Type::REF;
        }
    }
    else if( tok.type() == TOK_RWORD_MUT )
    {
        is_mut = true;
        expect_bind = true;
        GET_TOK(tok, lex);
    }
    else
    {
        // Fall through
    }

    AST::PatternBinding binding;
    AST::Pattern    pat;
    // If a 'ref' or 'mut' annotation was seen, the next name must be a binding name
    if( expect_bind )
    {
        CHECK_TOK(tok, TOK_IDENT);
        auto bind_name = tok.ident();
        // If there's no '@' after it, it's a name binding only (_ pattern)
        if( GET_TOK(tok, lex) != TOK_AT )
        {
            PUTBACK(tok, lex);
            return AST::Pattern(AST::Pattern::TagBind(), lex.end_span(ps), mv$(bind_name), bind_type, is_mut);
        }
        binding = AST::PatternBinding( mv$(bind_name), bind_type, is_mut );

        // '@' consumed, move on to next token
        //GET_TOK(tok, lex);
        pat = Parse_Pattern1(lex, allow_or);
    }
    // Otherwise, handle MaybeBind
    else if( tok.type() == TOK_IDENT )
    {
        switch( LOOK_AHEAD(lex) )
        {
        // Known path `ident::`
        case TOK_DOUBLE_COLON:
        // Known struct `Ident {` or `Ident (`
        case TOK_BRACE_OPEN:
        case TOK_PAREN_OPEN:
        // Known value `IDENT ...`
        case TOK_DOUBLE_DOT:
        case TOK_TRIPLE_DOT:
        case TOK_DOUBLE_DOT_EQUAL:
            PUTBACK(tok, lex);
            pat = Parse_PatternReal(lex, allow_or);
            break;
        // Known binding `ident @`
        case TOK_AT:
            binding = AST::PatternBinding( tok.ident(), bind_type/*MOVE*/, is_mut/*false*/ );
            GET_TOK(tok, lex);  // '@'
            pat = Parse_Pattern1(lex, allow_or);
            break;
        default: {  // Maybe bind
            auto name = tok.ident();
            // if the pattern can be refuted (i.e this could be an enum variant), return MaybeBind
            if( true /*is_refutable*/ ) {
                assert(bind_type == ::AST::PatternBinding::Type::MOVE);
                assert(is_mut == false);
                return AST::Pattern(AST::Pattern::TagMaybeBind(), lex.end_span(ps), mv$(name));
            }
            // Otherwise, it IS a binding
            else {
                return AST::Pattern(AST::Pattern::TagBind(), lex.end_span(ps), mv$(name), bind_type, is_mut);
            }
            throw "";}
        }
    }
    else
    {
        // Otherwise, fall through
        PUTBACK(tok, lex);
        pat = Parse_PatternReal(lex, allow_or);
    }
    if(binding.is_valid()) {
        pat.bindings().insert( pat.bindings().begin(), mv$(binding) );
    }
    return pat;
}

AST::Pattern Parse_PatternReal(TokenStream& lex, AllowOrPattern allow_or)
{
    Token   tok;
    if( LOOK_AHEAD(lex) == TOK_INTERPOLATED_PATTERN )
    {
        GET_TOK(tok, lex);
        return mv$(tok.frag_pattern());
    }
    auto ps = lex.start_span();
    AST::Pattern    ret = Parse_PatternReal1(lex, allow_or);
    if( (GET_TOK(tok, lex) == TOK_TRIPLE_DOT)
     || (TARGETVER_LEAST_1_29 && tok.type() == TOK_DOUBLE_DOT_EQUAL)
      )
    {
        if( !ret.data().is_Value() )
            throw ParseError::Generic(lex, "Using '...' with a non-value on left");
        auto& ret_v = ret.data().as_Value();
        auto leftval = std::move(ret_v.start);

        auto rightval = Parse_PatternValue(lex);
        if( rightval.is_Invalid() )
            throw ParseError::Generic(lex, "Using '...' with a no RHS value");

        return AST::Pattern(lex.end_span(ps), AST::Pattern::Data::make_Value({ mv$(leftval), mv$(rightval) }));
    }
    else if( TARGETVER_LEAST_1_39 && tok.type() == TOK_DOUBLE_DOT )
    {
        if( !ret.data().is_Value() )
            throw ParseError::Generic(lex, "Using `..` with a non-value on left");
        auto& ret_v = ret.data().as_Value();
        auto leftval = std::move(ret_v.start);

        auto rightval = Parse_PatternValue(lex);
        if( rightval.is_Invalid() ) {
            // Right-open range!
            // - Perfectly valid
        }

        return AST::Pattern(lex.end_span(ps), AST::Pattern::Data::make_ValueLeftInc({ mv$(leftval), mv$(rightval) }));
    }
    else
    {
        PUTBACK(tok, lex);
        return ret;
    }
}
AST::Pattern::Value Parse_PatternValue(TokenStream& lex)
{
    TRACE_FUNCTION;

    Token   tok;
    switch( GET_TOK(tok, lex) )
    {
    case TOK_RWORD_CRATE:
    case TOK_RWORD_SELF:
    case TOK_RWORD_SUPER:
    case TOK_IDENT:
    case TOK_LT:
    case TOK_DOUBLE_LT:
    case TOK_INTERPOLATED_PATH:
    case TOK_DOUBLE_COLON:
        PUTBACK(tok, lex);
        return AST::Pattern::Value::make_Named( Parse_Path(lex, PATH_GENERIC_EXPR) );

    case TOK_DASH:
        if(GET_TOK(tok, lex) == TOK_INTEGER)
        {
            auto dt = tok.datatype();
            // TODO: Ensure that the type is ANY or a signed integer
            return AST::Pattern::Value::make_Integer({dt, ~tok.intval() + 1});
        }
        else if( tok.type() == TOK_FLOAT )
        {
            return AST::Pattern::Value::make_Float({tok.datatype(), -tok.floatval()});
        }
        else
        {
            throw ParseError::Unexpected(lex, tok, {TOK_INTEGER, TOK_FLOAT});
        }
    case TOK_FLOAT:
        return AST::Pattern::Value::make_Float({tok.datatype(), tok.floatval()});
    case TOK_INTEGER:
        return AST::Pattern::Value::make_Integer({tok.datatype(), tok.intval()});
    case TOK_RWORD_TRUE:
        return AST::Pattern::Value::make_Integer({CORETYPE_BOOL, 1});
    case TOK_RWORD_FALSE:
        return AST::Pattern::Value::make_Integer({CORETYPE_BOOL, 0});
    case TOK_STRING:
        return AST::Pattern::Value::make_String( mv$(tok.str()) );
    case TOK_BYTESTRING:
        return AST::Pattern::Value::make_ByteString({ mv$(tok.str()) });
    case TOK_INTERPOLATED_EXPR: {
        auto e = tok.take_frag_node();
        // TODO: Visitor?
        if( auto* n = dynamic_cast<AST::ExprNode_String*>(e.get()) ) {
            return AST::Pattern::Value::make_String( mv$(n->m_value) );
        }
        else if( auto* n = dynamic_cast<AST::ExprNode_ByteString*>(e.get()) ) {
            return AST::Pattern::Value::make_ByteString({ mv$(n->m_value) });
        }
        else if( auto* n = dynamic_cast<AST::ExprNode_Bool*>(e.get()) ) {
            return AST::Pattern::Value::make_Integer({CORETYPE_BOOL, n->m_value});
        }
        else if( auto* n = dynamic_cast<AST::ExprNode_Integer*>(e.get()) ) {
            return AST::Pattern::Value::make_Integer({n->m_datatype, n->m_value});
        }
        else if( auto* n = dynamic_cast<AST::ExprNode_Float*>(e.get()) ) {
            return AST::Pattern::Value::make_Float({n->m_datatype, n->m_value});
        }
        else {
            TODO(lex.point_span(), "Convert :expr into a pattern value - " << *e);
        }
        } break;
    default:
        PUTBACK(tok, lex);
        return AST::Pattern::Value::make_Invalid({});
    }
}
AST::Pattern Parse_PatternReal1(TokenStream& lex, AllowOrPattern allow_or)
{
    TRACE_FUNCTION;
    auto ps = lex.start_span();

    Token   tok;
    AST::Path   path;

    switch( GET_TOK(tok, lex) )
    {
    case TOK_UNDERSCORE:
        return AST::Pattern( lex.end_span(ps), AST::Pattern::Data() );
    //case TOK_DOUBLE_DOT:
    //    return AST::Pattern( AST::Pattern::TagWildcard() );
    case TOK_RWORD_BOX:
        return AST::Pattern( AST::Pattern::TagBox(), lex.end_span(ps), Parse_Pattern1(lex, allow_or) );
    case TOK_DOUBLE_AMP:
        lex.putback(TOK_AMP);
    case TOK_AMP: {
        DEBUG("Ref");
        // NOTE: Falls back into "Pattern" not "PatternReal" to handle MaybeBind again
        bool is_mut = false;
        if( GET_TOK(tok, lex) == TOK_RWORD_MUT )
            is_mut = true;
        else
            PUTBACK(tok, lex);
        return AST::Pattern( AST::Pattern::TagReference(), lex.end_span(ps), is_mut, Parse_Pattern1(lex, allow_or) );
        }
    case TOK_RWORD_CRATE:
    case TOK_RWORD_SELF:
    case TOK_RWORD_SUPER:
    case TOK_IDENT:
    case TOK_LT:
    case TOK_DOUBLE_LT:
    case TOK_INTERPOLATED_PATH:
    case TOK_DOUBLE_COLON:
        PUTBACK(tok, lex);
        return Parse_PatternReal_Path( lex, ps, Parse_Path(lex, PATH_GENERIC_EXPR) );
    case TOK_DASH:
    case TOK_FLOAT:
    case TOK_INTEGER:
    case TOK_RWORD_TRUE:
    case TOK_RWORD_FALSE:
    case TOK_STRING:
    case TOK_BYTESTRING:
    case TOK_INTERPOLATED_EXPR:
        PUTBACK(tok, lex);
        return AST::Pattern( AST::Pattern::TagValue(), lex.end_span(ps), Parse_PatternValue(lex) );

    case TOK_PAREN_OPEN: {
        bool just_paren = false;
        auto tpat = Parse_PatternTuple(lex, &just_paren);
        // If it was `(<pat>)` (and not `(<pat>,)`) then unwrap to the first element
        if(just_paren) {
            assert(tpat.start.size() == 1);
            assert(!tpat.has_wildcard);
            assert(tpat.end.size() == 0);
            return std::move(tpat.start.front());
        }
        return AST::Pattern( AST::Pattern::TagTuple(), lex.end_span(ps), std::move(tpat) );
        }
    case TOK_SQUARE_OPEN:
        return Parse_PatternReal_Slice(lex);
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    throw "unreachable";
}
AST::Pattern Parse_PatternReal_Path(TokenStream& lex, ProtoSpan ps, AST::Path path)
{
    Token   tok;

    switch( GET_TOK(tok, lex) )
    {
    case TOK_PAREN_OPEN:
        return AST::Pattern( AST::Pattern::TagNamedTuple(), lex.end_span(ps), mv$(path), Parse_PatternTuple(lex, nullptr) );
    case TOK_BRACE_OPEN:
        return Parse_PatternStruct(lex, ps, mv$(path));
    default:
        PUTBACK(tok, lex);
        return AST::Pattern( AST::Pattern::TagValue(), lex.end_span(ps), AST::Pattern::Value::make_Named(mv$(path)) );
    }
}

AST::Pattern Parse_PatternReal_Slice(TokenStream& lex)
{
    auto ps = lex.start_span();
    Token   tok;

    ::std::vector< ::AST::Pattern>  leading;
    ::std::vector< ::AST::Pattern>  trailing;
    ::AST::PatternBinding   inner_binding;
    bool is_split = false;

    while(GET_TOK(tok, lex) != TOK_SQUARE_CLOSE)
    {
        bool has_binding = true;
        ::AST::PatternBinding  binding;
        // `ref foo ..` or `ref foo @ ..`
        if( tok.type() == TOK_RWORD_REF && lex.lookahead(0) == TOK_IDENT
            && (
                lex.lookahead(1) == TOK_DOUBLE_DOT
                || (lex.lookahead(1) == TOK_AT && lex.lookahead(2) == TOK_DOUBLE_DOT)
                ) ) {
            GET_TOK(tok, lex);
            binding = ::AST::PatternBinding( tok.ident(), ::AST::PatternBinding::Type::REF, false );
        }
        // `foo ..` or `foo @ ..`
        else if( tok.type() == TOK_IDENT && (
            lex.lookahead(0) == TOK_DOUBLE_DOT
            || (lex.lookahead(0) == TOK_AT && lex.lookahead(1) == TOK_DOUBLE_DOT)
            ) ) {
            binding = ::AST::PatternBinding( tok.ident(), ::AST::PatternBinding::Type::MOVE, false );
        }
        // `_ ..` or `_ @ ..`
        else if( tok.type() == TOK_UNDERSCORE && (
            lex.lookahead(0) == TOK_DOUBLE_DOT
            || (lex.lookahead(0) == TOK_AT && lex.lookahead(1) == TOK_DOUBLE_DOT)
            ) ) {
            // No binding, but switching to trailing
        }
        else if( tok.type() == TOK_DOUBLE_DOT ) {
            // No binding, but switching to trailing
            PUTBACK(tok, lex);
        }
        else {
            has_binding = false;
        }

        if( has_binding ) {
            if(is_split)
                ERROR(lex.end_span(ps), E0000, "Multiple instances of .. in a slice pattern");

            inner_binding = mv$(binding);
            is_split = true;
            if(lex.lookahead(0) == TOK_AT)
                GET_CHECK_TOK(tok, lex, TOK_AT);
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_DOT);
        }
        else {
            PUTBACK(tok, lex);
            if(!is_split) {
                leading.push_back( Parse_Pattern(lex) );
            }
            else {
                trailing.push_back( Parse_Pattern(lex) );
            }
        }

        if( GET_TOK(tok, lex) != TOK_COMMA )
            break;
    }
    CHECK_TOK(tok, TOK_SQUARE_CLOSE);

    if( is_split )
    {
        return ::AST::Pattern( lex.end_span(ps), ::AST::Pattern::Data::make_SplitSlice({ mv$(leading), mv$(inner_binding), mv$(trailing) }) );
    }
    else
    {
        assert( !inner_binding.is_valid() );
        assert( trailing.empty() );
        return ::AST::Pattern( lex.end_span(ps), ::AST::Pattern::Data::make_Slice({ mv$(leading) }) );
    }
}

::AST::Pattern::TuplePat Parse_PatternTuple(TokenStream& lex, bool *just_paren)
{
    TRACE_FUNCTION;
    auto sp = lex.start_span();
    Token tok;
    if(just_paren) *just_paren = false;

    ::std::vector<AST::Pattern> leading;
    while( LOOK_AHEAD(lex) != TOK_PAREN_CLOSE && LOOK_AHEAD(lex) != TOK_DOUBLE_DOT )
    {
        leading.push_back( Parse_Pattern(lex) );

        if( GET_TOK(tok, lex) != TOK_COMMA ) {
            CHECK_TOK(tok, TOK_PAREN_CLOSE);
            // If this was just a parenthesised pattern, then indicate to the caller
            if(just_paren) *just_paren = (leading.size() == 1);
            return AST::Pattern::TuplePat { mv$(leading), false, {} };
        }
    }

    if( LOOK_AHEAD(lex) != TOK_DOUBLE_DOT )
    {
        GET_TOK(tok, lex);

        CHECK_TOK(tok, TOK_PAREN_CLOSE);
        return AST::Pattern::TuplePat { mv$(leading), false, {} };
    }
    GET_CHECK_TOK(tok, lex, TOK_DOUBLE_DOT);

    ::std::vector<AST::Pattern> trailing;
    if( GET_TOK(tok, lex) == TOK_COMMA )
    {
        while( LOOK_AHEAD(lex) != TOK_PAREN_CLOSE )
        {
            trailing.push_back( Parse_Pattern(lex) );

            if( GET_TOK(tok, lex) != TOK_COMMA ) {
                PUTBACK(tok, lex);
                break;
            }
        }
        GET_TOK(tok, lex);
    }

    CHECK_TOK(tok, TOK_PAREN_CLOSE);
    return ::AST::Pattern::TuplePat { mv$(leading), true, mv$(trailing) };
}

AST::Pattern Parse_PatternStruct(TokenStream& lex, ProtoSpan ps, AST::Path path)
{
    TRACE_FUNCTION;
    Token tok;

    // #![feature(relaxed_adts)]
    if( LOOK_AHEAD(lex) == TOK_INTEGER )
    {
        bool split_allowed = false;
        ::std::map<unsigned int, AST::Pattern> pats;
        while( GET_TOK(tok, lex) == TOK_INTEGER )
        {
            unsigned int ofs = static_cast<unsigned int>(tok.intval());
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            auto val = Parse_Pattern(lex);
            if( ! pats.insert( ::std::make_pair(ofs, mv$(val)) ).second ) {
                ERROR(lex.point_span(), E0000, "Duplicate index");
            }

            if( GET_TOK(tok,lex) == TOK_BRACE_CLOSE )
                break;
            CHECK_TOK(tok, TOK_COMMA);
        }
        if( tok.type() == TOK_DOUBLE_DOT ) {
            split_allowed = true;
            GET_TOK(tok, lex);
        }
        CHECK_TOK(tok, TOK_BRACE_CLOSE);

        bool has_split = false;
        ::std::vector<AST::Pattern> leading;
        ::std::vector<AST::Pattern> trailing;
        unsigned int i = 0;
        for(auto& p : pats)
        {
            if( p.first != i ) {
                if( has_split || !split_allowed ) {
                    ERROR(lex.point_span(), E0000, "Missing index " << i);
                }
                has_split = true;
                i = p.first;
            }
            if( ! has_split ) {
                leading.push_back( mv$(p.second) );
            }
            else {
                trailing.push_back( mv$(p.second) );
            }
            i ++;
        }

        return AST::Pattern(AST::Pattern::TagNamedTuple(), lex.end_span(ps), mv$(path),  AST::Pattern::TuplePat { mv$(leading), has_split, mv$(trailing) });
    }

    bool is_exhaustive = true;
    ::std::vector< AST::StructPatternEntry >  subpats;
    do {
        if( lex.lookahead(0) == TOK_BRACE_CLOSE ) {
            GET_TOK(tok, lex);
            break;
        }
        if( lex.lookahead(0) == TOK_DOUBLE_DOT ) {
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_DOT);
            is_exhaustive = false;
            GET_TOK(tok, lex);
            break;
        }

        auto attrs = Parse_ItemAttrs(lex);

        GET_TOK(tok, lex);
        DEBUG("tok = " << tok);


        auto inner_ps = lex.start_span();
        bool is_short_bind = false;
        bool is_box = false;
        auto bind_type = AST::PatternBinding::Type::MOVE;
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
                bind_type = AST::PatternBinding::Type::MUTREF;
                GET_TOK(tok, lex);
            }
            else {
                bind_type = AST::PatternBinding::Type::REF;
            }
        }
        else if( tok.type() == TOK_RWORD_MUT ) {
            is_mut = true;
            is_short_bind = true;
            GET_TOK(tok, lex);
        }

        CHECK_TOK(tok, TOK_IDENT);
        auto field_ident = tok.ident();
        RcString field_name;
        GET_TOK(tok, lex);

        AST::Pattern    pat;
        if( is_short_bind || tok.type() != TOK_COLON ) {
            PUTBACK(tok, lex);
            pat = AST::Pattern(lex.end_span(inner_ps), {});
            field_name = field_ident.name;
            pat.bindings().push_back( AST::PatternBinding(mv$(field_ident), bind_type, is_mut) );
            if( is_box )
            {
                pat = AST::Pattern(AST::Pattern::TagBox(), lex.end_span(inner_ps), mv$(pat));
            }
        }
        else {
            CHECK_TOK(tok, TOK_COLON);
            field_name = mv$(field_ident.name);
            pat = Parse_Pattern(lex);
        }

        subpats.push_back(AST::StructPatternEntry { mv$(attrs), mv$(field_name), mv$(pat) });
    } while( GET_TOK(tok, lex) == TOK_COMMA );
    CHECK_TOK(tok, TOK_BRACE_CLOSE);

    return AST::Pattern(AST::Pattern::TagStruct(), lex.end_span(ps), ::std::move(path), ::std::move(subpats), is_exhaustive);
}

