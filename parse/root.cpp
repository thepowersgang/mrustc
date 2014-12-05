/*
 */
#include "preproc.hpp"
#include "../ast/ast.hpp"
#include "parseerror.hpp"
#include "common.hpp"
#include <cassert>

unsigned int TraceLog::depth = 0;

::std::vector<TypeRef> Parse_Path_GenericList(TokenStream& lex)
{
    TRACE_FUNCTION;

    ::std::vector<TypeRef>  types;
    Token   tok;
    do {
        types.push_back( Parse_Type(lex) );
    } while( GET_TOK(tok, lex) == TOK_COMMA );
    CHECK_TOK(tok, TOK_GT);
    return types;
}

AST::Path Parse_PathFrom(TokenStream& lex, AST::Path path, eParsePathGenericMode generic_mode)
{
    TRACE_FUNCTION;

    Token tok;

    tok = lex.getToken();
    while(true)
    {
        ::std::vector<TypeRef>  params;

        CHECK_TOK(tok, TOK_IDENT);
        ::std::string component = tok.str();

        tok = lex.getToken();
        if(generic_mode == PATH_GENERIC_TYPE && tok.type() == TOK_LT)
        {
            // Type-mode generics "::path::to::Type<A,B>"
            params = Parse_Path_GenericList(lex);
            tok = lex.getToken();
        }
        if( tok.type() != TOK_DOUBLE_COLON ) {
            path.append( AST::PathNode(component, params) );
            break;
        }
        tok = lex.getToken();
        if( generic_mode == PATH_GENERIC_EXPR && tok.type() == TOK_LT )
        {
            // Expr-mode generics "::path::to::function::<Type1,Type2>(arg1, arg2)"
            params = Parse_Path_GenericList(lex);
            tok = lex.getToken();
            if( tok.type() != TOK_DOUBLE_COLON ) {
                path.append( AST::PathNode(component, params) );
                break;
            }
        }
        path.append( AST::PathNode(component, params) );
    }
    lex.putback(tok);
    return path;
}

AST::Path Parse_Path(TokenStream& lex, bool is_abs, eParsePathGenericMode generic_mode)
{
    if( is_abs )
        return Parse_PathFrom(lex, AST::Path(AST::Path::TagAbsolute()), generic_mode);
    else
        return Parse_PathFrom(lex, AST::Path(), generic_mode);
}

static const struct {
    const char* name;
    enum eCoreType  type;
} CORETYPES[] = {
    {"char", CORETYPE_CHAR},
    {"uint", CORETYPE_UINT},
    {"int", CORETYPE_INT},
    {"u8", CORETYPE_U8},
    {"i8", CORETYPE_I8},
    {"u16", CORETYPE_U16},
    {"i16", CORETYPE_I16},
    {"u32", CORETYPE_U32},
    {"i32", CORETYPE_I32},
    {"u64", CORETYPE_U64},
    {"i64", CORETYPE_I64},
    {"f32", CORETYPE_F32},
    {"f64", CORETYPE_F64}
};

TypeRef Parse_Type(TokenStream& lex)
{
    TRACE_FUNCTION;

    Token tok = lex.getToken();
    switch(tok.type())
    {
    case TOK_IDENT:
        // Either a path (with generics)
        if( tok.str() == "_" )
            return TypeRef();
        for(unsigned int i = 0; i < sizeof(CORETYPES)/sizeof(CORETYPES[0]); i ++)
        {
            if( tok.str() < CORETYPES[i].name )
                break;
            if( tok.str() == CORETYPES[i].name )
                return TypeRef(TypeRef::TagPrimitive(), CORETYPES[i].type);
        }
        // or a primitive
        lex.putback(tok);
        return TypeRef(TypeRef::TagPath(), Parse_Path(lex, false, PATH_GENERIC_TYPE)); // relative path
    case TOK_DOUBLE_COLON:
        // Path with generics
        return TypeRef(TypeRef::TagPath(), Parse_Path(lex, true, PATH_GENERIC_TYPE));
    case TOK_AMP:
        // Reference
        tok = lex.getToken();
        if( tok.type() == TOK_RWORD_MUT ) {
            // Mutable reference
            return TypeRef(TypeRef::TagReference(), true, Parse_Type(lex));
        }
        else {
            lex.putback(tok);
            // Immutable reference
            return TypeRef(TypeRef::TagReference(), false, Parse_Type(lex));
        }
        break;
    case TOK_STAR:
        // Pointer
        tok = lex.getToken();
        switch( tok.type() )
        {
        case TOK_RWORD_MUT:
            // Mutable pointer
            return TypeRef(TypeRef::TagPointer(), true, Parse_Type(lex));
        case TOK_RWORD_CONST:
            // Immutable pointer
            return TypeRef(TypeRef::TagPointer(), false, Parse_Type(lex));
        default:
            throw ParseError::Unexpected(tok, Token(TOK_RWORD_CONST));
        }
        break;
    case TOK_SQUARE_OPEN: {
        // Array
        TypeRef inner = Parse_Type(lex);
        tok = lex.getToken();
        if( tok.type() == TOK_COMMA ) {
            // Sized array
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_DOT);
            AST::Expr array_size = Parse_Expr(lex, true);
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
            return TypeRef(TypeRef::TagSizedArray(), inner, array_size);
        }
        else {
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
            return TypeRef(TypeRef::TagUnsizedArray(), inner);
        }
        break; }
    case TOK_PAREN_OPEN: {
        ::std::vector<TypeRef>  types;
        if( (tok = lex.getToken()).type() == TOK_PAREN_CLOSE)
            return TypeRef(TypeRef::TagTuple(), types);
        do
        {
            TypeRef type = Parse_Type(lex);
            types.push_back(type);
        } while( (tok = lex.getToken()).type() == TOK_COMMA );
        GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
        return TypeRef(TypeRef::TagTuple(), types); }
    case TOK_EXCLAM:
        throw ParseError::Todo("noreturn type");
    default:
        throw ParseError::Unexpected(tok);
    }
}

AST::TypeParams Parse_TypeParams(TokenStream& lex)
{
    TRACE_FUNCTION;

    AST::TypeParams ret;
    Token tok;
    do {
        bool is_lifetime = false;
        tok = lex.getToken();
        switch(tok.type())
        {
        case TOK_IDENT:
            break;
        case TOK_LIFETIME:
            is_lifetime = true;
            break;
        default:
            // Oopsie!
            throw ParseError::Unexpected(tok);
        }
        AST::TypeParam  param( is_lifetime, tok.str() );
        tok = lex.getToken();
        if( tok.type() == TOK_COLON )
        {
            // TODO: Conditions
            if( is_lifetime )
            {
                throw ParseError::Todo("lifetime param conditions");
            }

            do {
                tok = lex.getToken();
                if(tok.type() == TOK_LIFETIME)
                    param.addLifetimeBound(tok.str());
                else {
                    lex.putback(tok);
                    param.addTypeBound(Parse_Type(lex));
                }
                tok = lex.getToken();
            } while(tok.type() == TOK_PLUS);
        }
        ret.push_back(param);
    } while( tok.type() == TOK_COMMA );
    lex.putback(tok);
    return ret;
}

void Parse_TypeConds(TokenStream& lex, AST::TypeParams& params)
{
    TRACE_FUNCTION;
    throw ParseError::Todo("type param conditions (where)");
}

/// Parse a function definition (after the 'fn')
AST::Function Parse_FunctionDef(TokenStream& lex)
{
    Token   tok;

    // Name
    GET_CHECK_TOK(tok, lex, TOK_IDENT);
    ::std::string name = tok.str();

    // Parameters
    AST::TypeParams params;
    if( GET_TOK(tok, lex) == TOK_LT )
    {
        params = Parse_TypeParams(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);

        //if(GET_TOK(tok, lex) == TOK_RWORD_WHERE)
        //{
        //    Parse_TypeConds(lex, params);
        //    tok = lex.getToken();
        //}
    }
    else {
        lex.putback(tok);
    }
    GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
    ::std::vector<AST::StructItem>  args;
    if( GET_TOK(tok, lex) != TOK_PAREN_CLOSE )
    {
        // Argument list
        lex.putback(tok);
        do {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string   name = tok.str();
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            TypeRef type = Parse_Type(lex);
            args.push_back( ::std::make_pair(name, type) );
            tok = lex.getToken();
        } while( tok.type() == TOK_COMMA );
        CHECK_TOK(tok, TOK_PAREN_CLOSE);
    }
    else {
        // Eat 'tok', negative comparison
    }

    TypeRef ret_type;
    if( GET_TOK(tok, lex) == TOK_THINARROW )
    {
        // Return type
        ret_type = Parse_Type(lex);
    }
    else
    {
        lex.putback(tok);
    }

    AST::Expr   code = Parse_ExprBlock(lex);

    return AST::Function(name, params, ret_type, args, code);
}

AST::Module Parse_ModRoot(bool is_own_file, Preproc& lex)
{
    AST::Module mod;
    for(;;)
    {
        bool    is_public = false;
        Token tok = lex.getToken();
        switch(tok.type())
        {
        case TOK_BRACE_CLOSE:
            if( is_own_file )
                throw ParseError::Unexpected(tok);
            return mod;
        case TOK_EOF:
            if( !is_own_file )
                throw ParseError::Unexpected(tok);
            return mod;

        case TOK_RWORD_PUB:
            assert(!is_public);
            is_public = false;
            break;

        case TOK_RWORD_USE:
            // TODO: Do manual path parsing here, as use has its own special set of quirks
            mod.add_alias( is_public, Parse_Path(lex, true, PATH_GENERIC_NONE) );
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            break;

        case TOK_RWORD_CONST: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();

            GET_CHECK_TOK(tok, lex, TOK_COLON);
            TypeRef type = Parse_Type(lex);
            GET_CHECK_TOK(tok, lex, TOK_EQUAL);
            AST::Expr val = Parse_Expr(lex, true);
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            mod.add_constant(is_public, name, type, val);
            break; }
        case TOK_RWORD_STATIC: {
            tok = lex.getToken();
            bool is_mut = false;
            if(tok.type() == TOK_RWORD_MUT) {
                is_mut = true;
                tok = lex.getToken();
            }
            CHECK_TOK(tok, TOK_IDENT);
            ::std::string name = tok.str();

            GET_CHECK_TOK(tok, lex, TOK_COLON);
            TypeRef type = Parse_Type(lex);

            GET_CHECK_TOK(tok, lex, TOK_EQUAL);

            AST::Expr val = Parse_Expr(lex, true);

            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            mod.add_global(is_public, is_mut, name, type, val);
            break; }

        case TOK_RWORD_FN:
            mod.add_function(is_public, Parse_FunctionDef(lex));
            break;
        case TOK_RWORD_STRUCT: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            tok = lex.getToken();
            AST::TypeParams params;
            if( tok.type() == TOK_LT )
            {
                params = Parse_TypeParams(lex);
                GET_CHECK_TOK(tok, lex, TOK_GT);
                tok = lex.getToken();
                if(tok.type() == TOK_RWORD_WHERE)
                {
                    Parse_TypeConds(lex, params);
                    tok = lex.getToken();
                }
            }
            if(tok.type() == TOK_PAREN_OPEN)
            {
                TypeRef inner = Parse_Type(lex);
                tok = lex.getToken();
                if(tok.type() != TOK_PAREN_CLOSE)
                {
                    ::std::vector<TypeRef>  refs;
                    refs.push_back(inner);
                    while( (tok = lex.getToken()).type() == TOK_COMMA )
                    {
                        refs.push_back( Parse_Type(lex) );
                    }
                    if( tok.type() != TOK_PAREN_CLOSE )
                        throw ParseError::Unexpected(tok, Token(TOK_PAREN_CLOSE));
                    inner = TypeRef(TypeRef::TagTuple(), refs);
                }
                throw ParseError::Todo("tuple struct");
            }
            else if(tok.type() == TOK_SEMICOLON)
            {
                throw ParseError::Todo("unit-like struct");
            }
            else if(tok.type() == TOK_BRACE_OPEN)
            {
                ::std::vector<AST::StructItem>  items;
                while( (tok = lex.getToken()).type() != TOK_BRACE_CLOSE )
                {
                    CHECK_TOK(tok, TOK_IDENT);
                    ::std::string   name = tok.str();
                    GET_CHECK_TOK(tok, lex, TOK_COLON);
                    TypeRef type = Parse_Type(lex);
                    items.push_back( ::std::make_pair(name, type) );
                    tok = lex.getToken();
                    if(tok.type() == TOK_BRACE_CLOSE)
                        break;
                    if(tok.type() != TOK_COMMA)
                        throw ParseError::Unexpected(tok, Token(TOK_COMMA));
                }
                mod.add_struct(is_public, name, params, items);
            }
            else
            {
                throw ParseError::Unexpected(tok);
            }
            break; }
        case TOK_RWORD_ENUM:
            throw ParseError::Todo("modroot enum");
        case TOK_RWORD_IMPL: {
            AST::TypeParams params;
            // 1. (optional) type parameters
            if( GET_TOK(tok, lex) == TOK_LT )
            {
                params = Parse_TypeParams(lex);
                GET_CHECK_TOK(tok, lex, TOK_GT);
            }
            else {
                lex.putback(tok);
            }
            // 2. Either a trait name (with type params), or the type to impl
            // - Don't care which at this stage
            TypeRef trait_type;
            TypeRef impl_type = Parse_Type(lex);
            if( GET_TOK(tok, lex) == TOK_RWORD_FOR )
            {
                // Implementing a trait for another type, get the target type
                trait_type = impl_type;
                impl_type = Parse_Type(lex);
            }
            else {
                lex.putback(tok);
            }
            // Where clause
            if( GET_TOK(tok, lex) == TOK_RWORD_WHERE )
            {
                Parse_TypeConds(lex, params);
            }
            else {
                lex.putback(tok);
            }
            GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);

            AST::Impl   impl(impl_type, trait_type);

            // A sequence of method implementations
            while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
            {
                bool is_public = false;
                if(tok.type() == TOK_RWORD_PUB) {
                    is_public = true;
                    GET_TOK(tok, lex);
                }
                switch(tok.type())
                {
                case TOK_RWORD_FN:
                    impl.add_function(is_public, Parse_FunctionDef(lex));
                    break;

                default:
                    throw ParseError::Unexpected(tok);
                }
            }

            mod.add_impl(impl);
            break; }
        case TOK_RWORD_TRAIT:
            throw ParseError::Todo("modroot trait");

        default:
            throw ParseError::Unexpected(tok);
        }
    }
}

void Parse_Crate(::std::string mainfile)
{
    Preproc lex(mainfile);
    AST::Module rootmodule = Parse_ModRoot(true, lex);
}
