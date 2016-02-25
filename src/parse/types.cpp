/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/types.cpp
 * - Parsing for type usages
 */
#include "common.hpp"
#include "parseerror.hpp"
#include "../types.hpp"
#include "../ast/ast.hpp"

// === PROTOTYPES ===
TypeRef Parse_Type(TokenStream& lex);
TypeRef Parse_Type_Int(TokenStream& lex);
TypeRef Parse_Type_Fn(TokenStream& lex);
TypeRef Parse_Type_Path(TokenStream& lex, ::std::vector<::std::string> hrls);

// === CODE ===
TypeRef Parse_Type(TokenStream& lex)
{
    ProtoSpan ps = lex.start_span();
    TypeRef rv = Parse_Type_Int(lex);
    //rv.set_span(lex.end_span(ps));
    return rv;
}

TypeRef Parse_Type_Int(TokenStream& lex)
{
    //TRACE_FUNCTION;

    Token tok;
    
    switch( GET_TOK(tok, lex) )
    {
    // '!' - Only ever used as part of function prototypes, but is kinda a type... not allowed here though
    case TOK_EXCLAM:
        throw ParseError::Generic(lex, "! is not a real type");
    // '_' = Wildcard (type inferrence variable)
    case TOK_UNDERSCORE:
        return TypeRef();
    // 'unsafe' - An unsafe function type
    case TOK_RWORD_UNSAFE:
        lex.putback(tok);
        return Parse_Type_Fn(lex);
    // 'extern' - A function type with an ABI
    case TOK_RWORD_EXTERN:
        lex.putback(tok);
        return Parse_Type_Fn(lex);
    // 'fn' - Rust function
    case TOK_RWORD_FN:
        lex.putback(tok);
        return Parse_Type_Fn(lex);
    // '<' - An associated type cast
    case TOK_LT:
    case TOK_DOUBLE_LT:
        lex.putback(tok);
        return TypeRef(TypeRef::TagPath(), Parse_Path(lex, PATH_GENERIC_TYPE));
    // 
    case TOK_RWORD_FOR: {
        GET_CHECK_TOK(tok, lex, TOK_LT);
        ::std::vector<::std::string>    hrls;
        do {
            GET_CHECK_TOK(tok, lex, TOK_LIFETIME);
            hrls.push_back( mv$(tok.str()) );
        } while( GET_TOK(tok, lex) == TOK_COMMA );
        CHECK_TOK(tok, TOK_GT);
        if( LOOK_AHEAD(lex) == TOK_RWORD_FN || LOOK_AHEAD(lex) == TOK_RWORD_EXTERN ) {
            // TODO: Handle HRLS in fn types
            return Parse_Type_Fn(lex);
        }
        else {
            return Parse_Type_Path(lex, hrls);
        }
        }
    // <ident> - Either a primitive, or a path
    case TOK_IDENT:
        // or a primitive
        if( auto ct = coretype_fromstring(tok.str()) )
        {
            return TypeRef(TypeRef::TagPrimitive(), ct);
        }
        if( tok.str() == "str" )
        {
            return TypeRef(TypeRef::TagPath(), AST::Path("", { AST::PathNode("#",{}), AST::PathNode("str",{}) }));
        }
        lex.putback(tok);
        return Parse_Type_Path(lex, {});
        // - Fall through to path handling
    // '::' - Absolute path
    case TOK_DOUBLE_COLON:
        lex.putback(tok);
        return Parse_Type_Path(lex, {});
    // 'super' - Parent relative path
    case TOK_RWORD_SUPER:
        lex.putback(tok);
        return Parse_Type_Path(lex, {});

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
            return TypeRef(TypeRef::TagReference(), true, Parse_Type(lex));
        }
        else {
            lex.putback(tok);
            // Immutable reference
            return TypeRef(TypeRef::TagReference(), false, Parse_Type(lex));
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
            return TypeRef(TypeRef::TagPointer(), true, Parse_Type(lex));
        case TOK_RWORD_CONST:
            // Immutable pointer
            return TypeRef(TypeRef::TagPointer(), false, Parse_Type(lex));
        default:
            throw ParseError::Unexpected(lex, tok, Token(TOK_RWORD_CONST));
        }
        throw ParseError::BugCheck("Reached end of Parse_Type:STAR");
    // '[' - Array type
    case TOK_SQUARE_OPEN: {
        // Array
        TypeRef inner = Parse_Type(lex);
        if( GET_TOK(tok, lex)  == TOK_SEMICOLON ) {
            // Sized array
            AST::Expr array_size = Parse_Expr(lex, true);
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
            return TypeRef(TypeRef::TagSizedArray(), inner, array_size.take_node());
        }
        else if( tok.type() == TOK_SQUARE_CLOSE )
        {
            return TypeRef(TypeRef::TagUnsizedArray(), inner);
        }
        else {
            throw ParseError::Unexpected(lex, tok/*, "; or ]"*/);
        }
        }
    
    // '(' - Tuple (or lifetime bounded trait)
    case TOK_PAREN_OPEN: {
        DEBUG("Tuple");
        if( GET_TOK(tok, lex) == TOK_PAREN_CLOSE )
            return TypeRef(TypeRef::TagTuple(), {});
        lex.putback(tok);
        
        TypeRef inner = Parse_Type(lex);
        if( GET_TOK(tok, lex) == TOK_PLUS )
        {
            // Lifetime bounded type, NOT a tuple
            GET_CHECK_TOK(tok, lex, TOK_LIFETIME);
            ::std::string   lifetime = tok.str();   
            GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
            // TODO: Actually use lifetime bound
            DEBUG("TODO: Use lifetime bound '" << lifetime << " on type " << inner);
            return ::std::move(inner);
        }
        else
        {
            ::std::vector<TypeRef>  types;
            types.push_back( ::std::move(inner) );
            lex.putback(tok);
            while( GET_TOK(tok, lex) == TOK_COMMA )
            {
                if( GET_TOK(tok, lex) == TOK_PAREN_CLOSE )
                    break;
                else
                    lex.putback(tok);
                types.push_back(Parse_Type(lex));
            }
            CHECK_TOK(tok, TOK_PAREN_CLOSE);
            return TypeRef(TypeRef::TagTuple(), types); }
        }
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    throw ParseError::BugCheck("Reached end of Parse_Type");
}

TypeRef Parse_Type_Fn(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token   tok;
    
    ::std::string   abi = "";
    
    GET_TOK(tok, lex);
    
    if( tok.type() == TOK_RWORD_UNSAFE )
    {
        // TODO: Unsafe functions in types
        GET_TOK(tok, lex);
    }
    if( tok.type() == TOK_RWORD_EXTERN )
    {
        if( GET_TOK(tok, lex) == TOK_STRING ) {
            abi = tok.str();
            GET_TOK(tok, lex);
        }
        else {
            abi = "C";
        }
    }
    CHECK_TOK(tok, TOK_RWORD_FN);

    ::std::vector<TypeRef>  args;
    GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
    while( LOOK_AHEAD(lex) != TOK_PAREN_CLOSE )
    {
        // Handle `ident: `
        if( lex.lookahead(0) == TOK_IDENT && lex.lookahead(1) == TOK_COLON ) {
            GET_TOK(tok, lex);
            GET_TOK(tok, lex);
        }
        args.push_back( Parse_Type(lex) );
        if( GET_TOK(tok, lex) != TOK_COMMA ) {
            lex.putback(tok);
            break;
        }
    }
    GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
    
    TypeRef ret_type = TypeRef(TypeRef::TagUnit());
    if( GET_TOK(tok, lex) == TOK_THINARROW )
    {
        ret_type = Parse_Type(lex);
    }
    else {
        lex.putback(tok);
    }
    
    return TypeRef(TypeRef::TagFunction(), ::std::move(abi), ::std::move(args), ::std::move(ret_type));
}

TypeRef Parse_Type_Path(TokenStream& lex, ::std::vector<::std::string> hrls)
{
    Token   tok;

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
    lex.putback(tok);
    if( hrls.size() > 0 || traits.size() > 1 || lifetimes.size() > 0 ) {
        if( lifetimes.size() )
            DEBUG("TODO: Lifetime bounds on trait objects");
        return TypeRef(mv$(hrls), ::std::move(traits));
    }
    else {
        return TypeRef(TypeRef::TagPath(), traits.at(0));
    }
}

