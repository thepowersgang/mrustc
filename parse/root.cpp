/*
 */
#include "preproc.hpp"
#include "../ast/ast.hpp"
#include "parseerror.hpp"
#include "common.hpp"
#include <cassert>

AST::Path Parse_Path(TokenStream& lex, bool is_abs, bool generic_ok)
{
    AST::Path   path;
    Token tok;
    do
    {
        tok = lex.getToken();
        if(tok.type() == TOK_LT)
        {
            throw ParseError::Todo("Parse_Path - Generics");
        }

        if(tok.type() != TOK_IDENT)
            throw ParseError::Unexpected(tok);
        path.append( tok.str() );
        tok = lex.getToken();
    } while( tok.type() == TOK_DOUBLE_COLON );
    lex.putback(tok);
    return path;
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
        return TypeRef(TypeRef::TagPath(), Parse_Path(lex, false, true)); // relative path
    case TOK_DOUBLE_COLON:
        // Path with generics
        //return TypeRef(TypeRef::TagPath(), Parse_Path(lex, true, true));
        throw ParseError::Todo("type ::");
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
    default:
        throw ParseError::Unexpected(tok);
    }
}

AST::TypeParams Parse_TypeParams(TokenStream& lex)
{
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
    throw ParseError::Todo("type param conditions (where)");
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
            mod.add_alias( is_public, Parse_Path(lex, true, false) );
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
            if(tok.type() != TOK_IDENT)
                throw ParseError::Unexpected(tok, Token(TOK_IDENT));
            ::std::string name = tok.str();

            GET_CHECK_TOK(tok, lex, TOK_COLON);
            TypeRef type = Parse_Type(lex);

            GET_CHECK_TOK(tok, lex, TOK_EQUAL);

            AST::Expr val = Parse_Expr(lex, true);

            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            mod.add_global(is_public, is_mut, name, type, val);
            break; }

        case TOK_RWORD_FN:
            throw ParseError::Todo("modroot fn");
        case TOK_RWORD_STRUCT: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            tok = lex.getToken();
            if( tok.type() == TOK_LT )
            {
                AST::TypeParams params = Parse_TypeParams(lex);
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
                throw ParseError::Todo("tuple struct");
            }
            else if(tok.type() == TOK_SEMICOLON)
            {
                throw ParseError::Todo("unit-like struct");
            }
            else if(tok.type() == TOK_BRACE_OPEN)
            {
                throw ParseError::Todo("full struct");
            }
            else
            {
                throw ParseError::Unexpected(tok);
            }

            throw ParseError::Todo("modroot struct"); }
        case TOK_RWORD_ENUM:
            throw ParseError::Todo("modroot enum");
        case TOK_RWORD_IMPL:
            throw ParseError::Todo("modroot impl");
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
