/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/types.cpp
 * - Parsing for type usages
 */
#include "common.hpp"
#include "parseerror.hpp"
#include <ast/types.hpp>
#include <ast/ast.hpp>

// === PROTOTYPES ===
//TypeRef Parse_Type(TokenStream& lex, bool allow_trait_list);
TypeRef Parse_Type_Int(TokenStream& lex, bool allow_trait_list);
TypeRef Parse_Type_Fn(TokenStream& lex, ::std::vector<::std::string> hrls = {});
TypeRef Parse_Type_Path(TokenStream& lex, ::std::vector<::std::string> hrls, bool allow_trait_list);

// === CODE ===
TypeRef Parse_Type(TokenStream& lex, bool allow_trait_list)
{
    ProtoSpan ps = lex.start_span();
    TypeRef rv = Parse_Type_Int(lex, allow_trait_list);
    //rv.set_span(lex.end_span(ps));
    return rv;
}

TypeRef Parse_Type_Int(TokenStream& lex, bool allow_trait_list)
{
    //TRACE_FUNCTION;
    auto ps = lex.start_span();

    Token tok;
    
    switch( GET_TOK(tok, lex) )
    {
    case TOK_INTERPOLATED_TYPE:
        return mv$(tok.frag_type());
    case TOK_MACRO:
        return TypeRef(TypeRef::TagMacro(), Parse_MacroInvocation(ps, mv$(tok.str()), lex));
    // '!' - Only ever used as part of function prototypes, but is kinda a type... not allowed here though
    case TOK_EXCLAM:
        return TypeRef( Span(tok.get_pos()), TypeData::make_Bang({}) );
    // '_' = Wildcard (type inferrence variable)
    case TOK_UNDERSCORE:
        return TypeRef(Span(tok.get_pos()));
    
    // 'unsafe' - An unsafe function type
    case TOK_RWORD_UNSAFE:
    // 'extern' - A function type with an ABI
    case TOK_RWORD_EXTERN:
    // 'fn' - Rust function
    case TOK_RWORD_FN:
        PUTBACK(tok, lex);
        return Parse_Type_Fn(lex);
    
    // '<' - An associated type cast
    case TOK_LT:
    case TOK_DOUBLE_LT: {
        PUTBACK(tok, lex);
        auto path = Parse_Path(lex, PATH_GENERIC_TYPE);
        return TypeRef(TypeRef::TagPath(), lex.end_span(ps), mv$(path));
        }
    // 
    case TOK_RWORD_FOR: {
        GET_CHECK_TOK(tok, lex, TOK_LT);
        ::std::vector<::std::string>    hrls;
        do {
            GET_CHECK_TOK(tok, lex, TOK_LIFETIME);
            hrls.push_back( mv$(tok.str()) );
        } while( GET_TOK(tok, lex) == TOK_COMMA );
        CHECK_TOK(tok, TOK_GT);
        switch(LOOK_AHEAD(lex))
        {
        case TOK_RWORD_UNSAFE:
        case TOK_RWORD_EXTERN:
        case TOK_RWORD_FN:
            // TODO: Handle HRLS in fn types
            return Parse_Type_Fn(lex, hrls);
        default:
            return Parse_Type_Path(lex, hrls, true);
        }
        }
    // <ident> - Either a primitive, or a path
    case TOK_IDENT:
        // or a primitive
        //if( auto ct = coretype_fromstring(tok.str()) )
        //{
        //    return TypeRef(TypeRef::TagPrimitive(), Span(tok.get_pos()), ct);
        //}
        PUTBACK(tok, lex);
        return Parse_Type_Path(lex, {}, allow_trait_list);
        // - Fall through to path handling
    // '::' - Absolute path
    case TOK_DOUBLE_COLON:
        PUTBACK(tok, lex);
        return Parse_Type_Path(lex, {}, allow_trait_list);
    // 'self' - This relative path
    // 'super' - Parent relative path
    case TOK_RWORD_SELF:
    case TOK_RWORD_SUPER:
        PUTBACK(tok, lex);
        return Parse_Type_Path(lex, {}, allow_trait_list);

    // HACK! Convert && into & &
    case TOK_DOUBLE_AMP:
        lex.putback(Token(TOK_AMP));
    // '&' - Reference type
    case TOK_AMP: {
        ::std::string   lifetime;
        // Reference
        tok = lex.getToken();
        if( tok.type() == TOK_LIFETIME ) {
            lifetime = tok.str();
            tok = lex.getToken();
        }
        if( tok.type() == TOK_RWORD_MUT ) {
            // Mutable reference
            return TypeRef(TypeRef::TagReference(), lex.end_span(ps), true, Parse_Type(lex, false));
        }
        else {
            PUTBACK(tok, lex);
            // Immutable reference
            return TypeRef(TypeRef::TagReference(), lex.end_span(ps), false, Parse_Type(lex, false));
        }
        throw ParseError::BugCheck("Reached end of Parse_Type:AMP");
        }
    // '*' - Raw pointer
    case TOK_STAR:
        // Pointer
        switch( GET_TOK(tok, lex) )
        {
        case TOK_RWORD_MUT:
            // Mutable pointer
            return TypeRef(TypeRef::TagPointer(), lex.end_span(ps), true, Parse_Type(lex, false));
        case TOK_RWORD_CONST:
            // Immutable pointer
            return TypeRef(TypeRef::TagPointer(), lex.end_span(ps), false, Parse_Type(lex, false));
        default:
            throw ParseError::Unexpected(lex, tok, {TOK_RWORD_CONST, TOK_RWORD_MUT});
        }
        throw ParseError::BugCheck("Reached end of Parse_Type:STAR");
    // '[' - Array type
    case TOK_SQUARE_OPEN: {
        // Array
        TypeRef inner = Parse_Type(lex);
        if( GET_TOK(tok, lex)  == TOK_SEMICOLON ) {
            // Sized array
            AST::Expr array_size = Parse_Expr(lex);
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
            return TypeRef(TypeRef::TagSizedArray(), lex.end_span(ps), inner, array_size.take_node());
        }
        else if( tok.type() == TOK_SQUARE_CLOSE )
        {
            return TypeRef(TypeRef::TagUnsizedArray(), lex.end_span(ps), inner);
        }
        else {
            throw ParseError::Unexpected(lex, tok/*, "; or ]"*/);
        }
        }
    
    // '(' - Tuple (or lifetime bounded trait)
    case TOK_PAREN_OPEN: {
        DEBUG("Tuple");
        if( GET_TOK(tok, lex) == TOK_PAREN_CLOSE )
            return TypeRef(TypeRef::TagTuple(), lex.end_span(ps), {});
        PUTBACK(tok, lex);
        
        TypeRef inner = Parse_Type(lex, true);
        if( LOOK_AHEAD(lex) == TOK_PAREN_CLOSE )
        {
            // Type in parens, NOT a tuple
            GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
            return inner;
        }
        else
        {
            ::std::vector<TypeRef>  types;
            types.push_back( mv$(inner) );
            while( GET_TOK(tok, lex) == TOK_COMMA )
            {
                if( GET_TOK(tok, lex) == TOK_PAREN_CLOSE )
                    break;
                else
                    PUTBACK(tok, lex);
                types.push_back(Parse_Type(lex));
            }
            CHECK_TOK(tok, TOK_PAREN_CLOSE);
            return TypeRef(TypeRef::TagTuple(), lex.end_span(ps), mv$(types)); }
        }
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    throw ParseError::BugCheck("Reached end of Parse_Type");
}

TypeRef Parse_Type_Fn(TokenStream& lex, ::std::vector<::std::string> hrls)
{
    auto ps = lex.start_span();
    // TODO: HRLs
    TRACE_FUNCTION;
    Token   tok;
    
    ::std::string   abi = "";
    bool    is_unsafe = false;
    
    GET_TOK(tok, lex);
    
    if( tok.type() == TOK_RWORD_UNSAFE )
    {
        is_unsafe = true;
        GET_TOK(tok, lex);
    }
    if( tok.type() == TOK_RWORD_EXTERN )
    {
        if( GET_TOK(tok, lex) == TOK_STRING ) {
            abi = tok.str();
            if( abi == "" )
                ERROR(lex.getPosition(), E0000, "Empty ABI");
            GET_TOK(tok, lex);
        }
        else {
            abi = "C";
        }
    }
    CHECK_TOK(tok, TOK_RWORD_FN);

    ::std::vector<TypeRef>  args;
    bool is_variadic = false;
    GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
    while( LOOK_AHEAD(lex) != TOK_PAREN_CLOSE )
    {
        if( LOOK_AHEAD(lex) == TOK_TRIPLE_DOT ) {
            GET_TOK(tok, lex);
            is_variadic = true;
            break; 
        }
        // Handle `ident: `
        if( lex.lookahead(0) == TOK_IDENT && lex.lookahead(1) == TOK_COLON ) {
            GET_TOK(tok, lex);
            GET_TOK(tok, lex);
        }
        args.push_back( Parse_Type(lex) );
        if( GET_TOK(tok, lex) != TOK_COMMA ) {
            PUTBACK(tok, lex);
            break;
        }
    }
    GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
    
    TypeRef ret_type = TypeRef(TypeRef::TagUnit(), Span(tok.get_pos()));
    if( GET_TOK(tok, lex) == TOK_THINARROW )
    {
        ret_type = Parse_Type(lex, false);
    }
    else {
        PUTBACK(tok, lex);
    }
    
    return TypeRef(TypeRef::TagFunction(), lex.end_span(ps), is_unsafe, mv$(abi), mv$(args), is_variadic, mv$(ret_type));
}

TypeRef Parse_Type_Path(TokenStream& lex, ::std::vector<::std::string> hrls, bool allow_trait_list)
{
    Token   tok;

    auto ps = lex.start_span();
    
    if( ! allow_trait_list )
    {
        return TypeRef(TypeRef::TagPath(), lex.end_span(ps), Parse_Path(lex, PATH_GENERIC_TYPE));
    }
    else
    {
        ::std::vector<AST::Path>    traits;
        ::std::vector< ::std::string>   lifetimes;
        do {
            if( LOOK_AHEAD(lex) == TOK_LIFETIME ) {
                GET_TOK(tok, lex);
                lifetimes.push_back( tok.str() );
            }
            else
                traits.push_back( Parse_Path(lex, PATH_GENERIC_TYPE) );
        } while( GET_TOK(tok, lex) == TOK_PLUS );
        PUTBACK(tok, lex);
        if( hrls.size() > 0 || traits.size() > 1 || lifetimes.size() > 0 ) {
            if( lifetimes.size() )
                DEBUG("TODO: Lifetime bounds on trait objects");
            return TypeRef(lex.end_span(ps), mv$(hrls), ::std::move(traits));
        }
        else {
            return TypeRef(TypeRef::TagPath(), lex.end_span(ps), traits.at(0));
        }
    }
}

