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
    // HACK: Split >> into >
    if(tok.type() == TOK_DOUBLE_GT) {
        lex.putback(Token(TOK_GT));
    }
    else {
        CHECK_TOK(tok, TOK_GT);
    }
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
        if( generic_mode == PATH_GENERIC_TYPE && (tok.type() == TOK_LT || tok.type() == TOK_DOUBLE_LT) )
        {
            // HACK! Handle breaking << into < <
            if( tok.type() == TOK_DOUBLE_LT )
                lex.putback( Token(TOK_LT) );
            
            // Type-mode generics "::path::to::Type<A,B>"
            params = Parse_Path_GenericList(lex);
            tok = lex.getToken();
        }
        if( tok.type() != TOK_DOUBLE_COLON ) {
            path.append( AST::PathNode(component, params) );
            break;
        }
        tok = lex.getToken();
        if( generic_mode == PATH_GENERIC_EXPR && (tok.type() == TOK_LT || tok.type() == TOK_DOUBLE_LT) )
        {
            // HACK! Handle breaking << into < <
            if( tok.type() == TOK_DOUBLE_LT )
                lex.putback( Token(TOK_LT) );
            
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

TypeRef Parse_Type(TokenStream& lex)
{
    TRACE_FUNCTION;

    Token tok = lex.getToken();
    switch(tok.type())
    {
    case TOK_LT: {
        DEBUG("Associated type");
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
        throw ParseError::BugCheck("Reached end of Parse_Type:AMP");
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
            throw ParseError::Unexpected(tok, Token(TOK_RWORD_CONST));
        }
        throw ParseError::BugCheck("Reached end of Parse_Type:STAR");
    case TOK_SQUARE_OPEN: {
        // Array
        TypeRef inner = Parse_Type(lex);
        tok = lex.getToken();
        if( tok.type() == TOK_COMMA ) {
            // Sized array
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_DOT);
            AST::Expr array_size = Parse_Expr(lex, true);
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
            return TypeRef(TypeRef::TagSizedArray(), inner, array_size.take_node());
        }
        else {
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
            return TypeRef(TypeRef::TagUnsizedArray(), inner);
        }
        throw ParseError::BugCheck("Reached end of Parse_Type:SQUARE");
        }
    case TOK_PAREN_OPEN: {
        DEBUG("Tuple");
        ::std::vector<TypeRef>  types;
        if( GET_TOK(tok, lex) == TOK_PAREN_CLOSE )
            return TypeRef(TypeRef::TagTuple(), types);
        lex.putback(tok);
        do
        {
            TypeRef type = Parse_Type(lex);
            types.push_back(type);
        } while( GET_TOK(tok, lex) == TOK_COMMA );
        CHECK_TOK(tok, TOK_PAREN_CLOSE);
        return TypeRef(TypeRef::TagTuple(), types); }
    case TOK_EXCLAM:
        throw ParseError::Todo("noreturn type");
    default:
        throw ParseError::Unexpected(tok);
    }
    throw ParseError::BugCheck("Reached end of Parse_Type");
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
AST::Function Parse_FunctionDef(TokenStream& lex, bool allow_no_code=false)
{
    TRACE_FUNCTION;

    Token   tok;

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

    AST::Function::Class    fcn_class = AST::Function::CLASS_UNBOUND;
    GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
    GET_TOK(tok, lex);
    if( tok.type() == TOK_AMP )
    {
        // By-reference method
        if( GET_TOK(tok, lex) == TOK_LIFETIME )
        {
            throw ParseError::Todo("Lifetimes on self in methods");
        }
        if( tok.type() == TOK_RWORD_MUT )
        {
            GET_CHECK_TOK(tok, lex, TOK_RWORD_SELF);
            fcn_class = AST::Function::CLASS_MUTMETHOD;
        }
        else
        {
            CHECK_TOK(tok, TOK_RWORD_SELF);
            fcn_class = AST::Function::CLASS_REFMETHOD;
        }
        GET_TOK(tok, lex);
    }
    else if( tok.type() == TOK_RWORD_SELF )
    {
        // By-value method
        fcn_class = AST::Function::CLASS_VALMETHOD;
        GET_TOK(tok, lex);
        throw ParseError::Todo("By-value methods");
    }
    else
    {
        // Unbound method
    }
    ::std::vector<AST::StructItem>  args;
    if( tok.type() != TOK_PAREN_CLOSE )
    {
        lex.putback(tok);
        // Argument list
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

    AST::Expr   code;
    if( GET_TOK(tok, lex) == TOK_BRACE_OPEN )
    {
        lex.putback(tok);
        code = Parse_ExprBlock(lex);
    }
    else
    {
        if( !allow_no_code )
        {
            throw ParseError::Generic("Expected code for function");
        }
    }

    return AST::Function(params, fcn_class, ret_type, args, code);
}

AST::TypeAlias Parse_TypeAlias(TokenStream& lex, const ::std::vector<AST::MetaItem> meta_items)
{
    TRACE_FUNCTION;

    Token   tok;

    // Params
    tok = lex.getToken();
    AST::TypeParams params;
    if( tok.type() == TOK_LT )
    {
        params = Parse_TypeParams(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);
        tok = lex.getToken();
    }
    
    CHECK_TOK(tok, TOK_EQUAL);
    
    // Type
    TypeRef type = Parse_Type(lex);
    GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
    
    return AST::TypeAlias( ::std::move(params), ::std::move(type) );
}

void Parse_Struct(AST::Module& mod, TokenStream& lex, const bool is_public, const ::std::vector<AST::MetaItem> meta_items)
{
    TRACE_FUNCTION;

    Token   tok;

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
}

AST::Trait Parse_TraitDef(TokenStream& lex, const ::std::vector<AST::MetaItem> meta_items)
{
    TRACE_FUNCTION;

    Token   tok;
    
    AST::TypeParams params;
    if( GET_TOK(tok, lex) == TOK_LT )
    {
        params = Parse_TypeParams(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);
        tok = lex.getToken();
    }
    // TODO: Support "for Sized?"
    if(tok.type() == TOK_RWORD_WHERE)
    {
        if( params.size() == 0 )
            throw ParseError::Generic("Where clause with no generic params");
        Parse_TypeConds(lex, params);
        tok = lex.getToken();
    }

    
    AST::Trait trait(params);
        
    CHECK_TOK(tok, TOK_BRACE_OPEN);
    while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
    {
        switch(tok.type())
        {
        case TOK_RWORD_STATIC: {
            throw ParseError::Todo("Associated static");
            break; }
        case TOK_RWORD_TYPE: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            if( GET_TOK(tok, lex) == TOK_COLON ) {
                throw ParseError::Todo("Type bounds on associated type");
            }
            if( tok.type() == TOK_RWORD_WHERE ) {
                throw ParseError::Todo("Where clause on associated type");
            }
            TypeRef default_type;
            if( tok.type() == TOK_EQUAL ) {
                default_type = Parse_Type(lex);
            }
            trait.add_type( ::std::move(name), ::std::move(default_type) );
            break; }
        case TOK_RWORD_FN: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            trait.add_function( ::std::move(name), Parse_FunctionDef(lex, true) );
            break; }
        default:
            throw ParseError::Generic("Unexpected token, expected 'type' or 'fn'");
        }
    }
    
    return trait;
}

AST::Enum Parse_EnumDef(TokenStream& lex, const ::std::vector<AST::MetaItem> meta_items)
{
    TRACE_FUNCTION;

    Token   tok;
    
    tok = lex.getToken();
    // Type params supporting "where"
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
    
    // Body
    CHECK_TOK(tok, TOK_BRACE_OPEN);
    ::std::vector<AST::StructItem>   variants;
    while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
    {
        CHECK_TOK(tok, TOK_IDENT);
        ::std::string   name = tok.str();
        ::std::vector<TypeRef>  types;
        if( GET_TOK(tok, lex) == TOK_PAREN_OPEN )
        {
            // Get type list
            // TODO: Handle 'Variant()'?
            do
            {
                types.push_back( Parse_Type(lex) );
            } while( GET_TOK(tok, lex) == TOK_COMMA );
            CHECK_TOK(tok, TOK_PAREN_CLOSE);
            GET_TOK(tok, lex);
        }
        
        variants.push_back( AST::StructItem(::std::move(name), TypeRef(TypeRef::TagTuple(), ::std::move(types))) );
        if( tok.type() != TOK_COMMA )
            break;
    }
    CHECK_TOK(tok, TOK_BRACE_CLOSE);

    
    return AST::Enum( ::std::move(params), ::std::move(variants) );
}

/// Parse a meta-item declaration (either #![ or #[)
AST::MetaItem Parse_MetaItem(TokenStream& lex)
{
    Token tok;
    GET_CHECK_TOK(tok, lex, TOK_IDENT);
    ::std::string   name = tok.str();
    switch(GET_TOK(tok, lex))
    {
    case TOK_EQUAL:
        throw ParseError::Todo("Meta item key-value");
    case TOK_PAREN_OPEN: {
        ::std::vector<AST::MetaItem>    items;
        do {
            items.push_back(Parse_MetaItem(lex));
        } while(GET_TOK(tok, lex) == TOK_COMMA);
        CHECK_TOK(tok, TOK_PAREN_CLOSE);
        return AST::MetaItem(name, items); }
    default:
        lex.putback(tok);
        return AST::MetaItem(name);
    }
}

AST::Impl Parse_Impl(TokenStream& lex)
{
    Token   tok;

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

    AST::Impl   impl( ::std::move(params), ::std::move(impl_type), ::std::move(trait_type) );

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
        case TOK_RWORD_FN: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            impl.add_function(is_public, name, Parse_FunctionDef(lex));
            break; }

        default:
            throw ParseError::Unexpected(tok);
        }
    }

    return impl;
}

void Parse_Use_Wildcard(const AST::Path& base_path, ::std::function<void(AST::Path, ::std::string)> fcn)
{
    fcn(base_path, ""); // HACK! Empty path indicates wilcard import
}

void Parse_Use(Preproc& lex, ::std::function<void(AST::Path, ::std::string)> fcn)
{
    TRACE_FUNCTION;

    Token   tok;
    AST::Path   path = AST::Path( AST::Path::TagAbsolute() );
    
    switch( GET_TOK(tok, lex) )
    {
    case TOK_RWORD_SELF:
        throw ParseError::Todo("Parse_Use - self");
        break;
    case TOK_RWORD_SUPER:
        throw ParseError::Todo("Parse_Use - super");
        break;
    case TOK_IDENT:
        path.append( AST::PathNode(tok.str(), {}) );
        break;
    default:
        throw ParseError::Unexpected(tok);
    }
    // TODO: Use from crate root
    while( GET_TOK(tok, lex) == TOK_DOUBLE_COLON )
    {
        if( GET_TOK(tok, lex) == TOK_IDENT )
        {
            path.append( AST::PathNode(tok.str(), {}) );
        }
        else
        {
            switch( tok.type() )
            {
            case TOK_BRACE_OPEN:
                do {
                    if( GET_TOK(tok, lex) == TOK_RWORD_SELF ) {
                        fcn(path, path[path.size()-1].name());
                    }
                    else {
                        CHECK_TOK(tok, TOK_IDENT);
                        fcn(path + AST::PathNode(tok.str(), {}), tok.str());
                    }
                } while( GET_TOK(tok, lex) == TOK_COMMA );
                CHECK_TOK(tok, TOK_BRACE_CLOSE);
                return;
            case TOK_STAR:
                Parse_Use_Wildcard(path, fcn);
                // early return - can't have anything else after
                return;
            default:
                throw ParseError::Unexpected(tok);
            }
            GET_TOK(tok, lex);
            break;
        }
    }
    
    ::std::string name;
    // TODO: This should only be allowed if the last token was an ident
    if( tok.type() == TOK_RWORD_AS )
    {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        name = tok.str();
    }
    else
    {
        lex.putback(tok);
        name = path[path.size()-1].name();
    }
    
    fcn(path, name);
}

void Parse_ModRoot(Preproc& lex, AST::Crate& crate, AST::Module& mod, const ::std::string& path)
{
    TRACE_FUNCTION;

    const bool nested_module = (path.size() == 0);  // 'mod name { code }', as opposed to 'mod name;'
    Token   tok;

    if( crate.m_load_std )
    {
        // Import the prelude
        AST::Path   prelude_path = AST::Path(AST::Path::TagAbsolute());
        prelude_path.append( AST::PathNode("std", {}) );
        prelude_path.append( AST::PathNode("prelude", {}) );
        Parse_Use_Wildcard(prelude_path,
            [&mod](AST::Path p, std::string s) {
                mod.add_alias(false, p, s);
                }
            );
    }

    // Attributes on module/crate (will continue loop)
    while( GET_TOK(tok, lex) == TOK_CATTR_OPEN )
    {
        AST::MetaItem item = Parse_MetaItem(lex);
        GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);

        mod.add_attr( item );
    }
    lex.putback(tok);

    // TODO: Iterate attributes, and check for handlers on each
    
    for(;;)
    {
        // Check 1 - End of module (either via a closing brace, or EOF)
        switch(GET_TOK(tok, lex))
        {
        case TOK_BRACE_CLOSE:
            if( !nested_module )
                throw ParseError::Unexpected(tok);
            return ;
        case TOK_EOF:
            if( nested_module )
                throw ParseError::Unexpected(tok);
            return ;
        default:
            lex.putback(tok);
            break;
        }

        // Attributes on the following item
        ::std::vector<AST::MetaItem>    meta_items;
        while( GET_TOK(tok, lex) == TOK_ATTR_OPEN )
        {
            meta_items.push_back( Parse_MetaItem(lex) );
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
        }
        lex.putback(tok);

        // Module visibility
        bool    is_public = false;
        if( GET_TOK(tok, lex) == TOK_RWORD_PUB )
        {
            is_public = true;
        }
        else
        {
            lex.putback(tok);
        }

        // The actual item!
        switch( GET_TOK(tok, lex) )
        {
        case TOK_RWORD_USE:
            Parse_Use(lex, [&mod,is_public](AST::Path p, std::string s) { mod.add_alias(is_public, p, s); });
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

        case TOK_RWORD_FN: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            mod.add_function(is_public, name, Parse_FunctionDef(lex));
            break; }
        case TOK_RWORD_TYPE: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            mod.add_typealias(is_public, name, Parse_TypeAlias(lex, meta_items));
            break; }
        case TOK_RWORD_STRUCT:
            Parse_Struct(mod, lex, is_public, meta_items);
            break;
        case TOK_RWORD_ENUM: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            mod.add_enum(is_public, name, Parse_EnumDef(lex, meta_items));
            break; }
        case TOK_RWORD_IMPL:
            mod.add_impl(Parse_Impl(lex));
            break;
        case TOK_RWORD_TRAIT: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            mod.add_trait(is_public, name, Parse_TraitDef(lex, meta_items));
            break; }

        case TOK_RWORD_MOD: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            AST::Module submod(name);
            DEBUG("Sub module '"<<name<<"'");
            switch( GET_TOK(tok, lex) )
            {
            case TOK_BRACE_OPEN:
                Parse_ModRoot(lex, crate, submod, "");
                break;
            case TOK_SEMICOLON:
                throw ParseError::Todo("sub-modules from other files");
            default:
                throw ParseError::Generic("Expected { or ; after module name");
            }
            mod.add_submod(is_public, ::std::move(submod));
            break; }

        default:
            throw ParseError::Unexpected(tok);
        }
    }
}

AST::Crate Parse_Crate(::std::string mainfile)
{
    Token   tok;
    
    Preproc lex(mainfile); 
    AST::Crate  crate;
    AST::Module& rootmod = crate.root_module();
    
    // Attributes on module/crate
    while( GET_TOK(tok, lex) == TOK_CATTR_OPEN )
    {
        AST::MetaItem item = Parse_MetaItem(lex);
        GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);

        rootmod.add_attr( item );
    }
    lex.putback(tok);
    
    // Check for crate attributes
    for( const auto& attr : rootmod.attrs() )
    {
        if( attr.name() == "no_std" ) {
            crate.m_load_std = false;
        }
        else {
            // TODO:
        }
    }
        
    if( crate.m_load_std )
    {
        // Load the standard library (add 'extern crate std;')
        crate.load_extern_crate("std");
        rootmod.add_ext_crate("std", "std");
        // Prelude imports are handled in Parse_ModRoot
    }
    
    // Include the std if the 'no_std' attribute was absent
    // - First need to load the std macros, then can import the prelude
    
    Parse_ModRoot(lex, crate, rootmod, mainfile);
    
    return crate;
}
