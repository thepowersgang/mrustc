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

/// Mappings from internal type names to the core type enum
static const struct {
    const char* name;
    enum eCoreType  type;
} CORETYPES[] = {
    {"bool", CORETYPE_BOOL},
    {"char", CORETYPE_CHAR},
    {"f32", CORETYPE_F32},
    {"f64", CORETYPE_F64},
    {"i16", CORETYPE_I16},
    {"i32", CORETYPE_I32},
    {"i64", CORETYPE_I64},
    {"i8", CORETYPE_I8},
    {"int", CORETYPE_INT},
    {"isize", CORETYPE_INT},
    {"u16", CORETYPE_U16},
    {"u32", CORETYPE_U32},
    {"u64", CORETYPE_U64},
    {"u8",  CORETYPE_U8},
    {"uint", CORETYPE_UINT},
    {"usize", CORETYPE_UINT},
};

// === PROTOTYPES ===
TypeRef Parse_Type(TokenStream& lex);
TypeRef Parse_Type_Int(TokenStream& lex);
TypeRef Parse_Type_Fn(TokenStream& lex);

// === CODE ===
TypeRef Parse_Type(TokenStream& lex)
{
    ProtoSpan ps = lex.start_span();
    TypeRef rv = Parse_Type_Int(lex);
    rv.set_span(lex.end_span(ps));
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
        lex.putback(tok);
        return TypeRef(TypeRef::TagPath(), Parse_Path(lex, PATH_GENERIC_TYPE));
        #if 0
        {
        DEBUG("Associated type");
        // TODO: This should instead use the path code, not a special case in typing
        // <Type as Trait>::Inner
        TypeRef base = Parse_Type(lex);
        GET_CHECK_TOK(tok, lex, TOK_RWORD_AS);
        TypeRef trait = Parse_Type(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);
        // TODO: Is just '<Type as Trait>' valid?
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        ::std::string   inner_name = tok.str();
        return TypeRef(TypeRef::TagAssoc(), ::std::move(base), ::std::move(trait), ::std::move(inner_name));
        }
        #endif
    // <ident> - Either a primitive, or a path
    case TOK_IDENT:
        // or a primitive
        for(unsigned int i = 0; i < sizeof(CORETYPES)/sizeof(CORETYPES[0]); i ++)
        {
            if( tok.str() < CORETYPES[i].name )
                break;
            if( tok.str() == CORETYPES[i].name )
                return TypeRef(TypeRef::TagPrimitive(), CORETYPES[i].type);
        }
        if( tok.str() == "str" )
        {
            // TODO: Create an internal newtype for 'str'
            return TypeRef(TypeRef::TagPath(), AST::Path("", { AST::PathNode("#",{}), AST::PathNode("str",{}) }));
        }
        // - Fall through to path handling
    // '::' - Absolute path
    case TOK_DOUBLE_COLON: {
        lex.putback(tok);
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
        if( traits.size() > 1 || lifetimes.size() > 0 ) {
            if( lifetimes.size() )
                DEBUG("TODO: Lifetime bounds on trait objects");
            return TypeRef(::std::move(traits));
        }
        else 
            return TypeRef(TypeRef::TagPath(), traits.at(0));
        }
    // 'super' - Parent relative path
    case TOK_RWORD_SUPER:
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        return TypeRef(TypeRef::TagPath(), AST::Path(AST::Path::TagSuper(), Parse_PathNodes(lex, PATH_GENERIC_TYPE)));

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
        GET_CHECK_TOK(tok, lex, TOK_STRING);
        abi = tok.str();
        GET_TOK(tok, lex);
    }
    CHECK_TOK(tok, TOK_RWORD_FN);

    ::std::vector<TypeRef>  args;
    GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
    while( LOOK_AHEAD(lex) != TOK_PAREN_CLOSE )
    {
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

