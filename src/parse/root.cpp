/*
 */
#include "preproc.hpp"
#include "../ast/ast.hpp"
#include "parseerror.hpp"
#include "common.hpp"
#include "../macros.hpp"
#include <cassert>

extern AST::Pattern Parse_Pattern(TokenStream& lex);
AST::MetaItem   Parse_MetaItem(TokenStream& lex);
void Parse_ModRoot(TokenStream& lex, AST::Crate& crate, AST::Module& mod, LList<AST::Module*> *prev_modstack, const ::std::string& path);

AST::Path   Parse_Path(TokenStream& lex, eParsePathGenericMode generic_mode);
AST::Path   Parse_Path(TokenStream& lex, bool is_abs, eParsePathGenericMode generic_mode);
::std::vector<TypeRef>  Parse_Path_GenericList(TokenStream& lex);
AST::Path   Parse_PathFrom(TokenStream& lex, AST::Path path, eParsePathGenericMode generic_mode);

AST::Path Parse_Path(TokenStream& lex, eParsePathGenericMode generic_mode)
{
    Token   tok;
    if( GET_TOK(tok, lex) == TOK_DOUBLE_COLON )
        return Parse_Path(lex, true, generic_mode);
    else
    {
        lex.putback(tok);
        return Parse_Path(lex, false, generic_mode);
    }
}
AST::Path Parse_Path(TokenStream& lex, bool is_abs, eParsePathGenericMode generic_mode)
{
    if( is_abs )
    {
        Token   tok;
        if( GET_TOK(tok, lex) == TOK_STRING ) {
            ::std::string   cratename = tok.str();
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
            return Parse_PathFrom(lex, AST::Path(cratename, {}), generic_mode);
        }
        else {
            lex.putback(tok);
            return Parse_PathFrom(lex, AST::Path(AST::Path::TagAbsolute()), generic_mode);
        }
    }
    else
        return Parse_PathFrom(lex, AST::Path(), generic_mode);
}

/// Parse a list of parameters within a path
::std::vector<TypeRef> Parse_Path_GenericList(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token   tok;

    ::std::vector<TypeRef>  types;
    ::std::vector< ::std::string>   lifetimes;
    ::std::map< ::std::string, TypeRef> assoc_bounds;
    ::std::vector<unsigned int> int_args;
    do {
        switch(GET_TOK(tok, lex))
        {
        case TOK_LIFETIME:
            lifetimes.push_back( tok.str() );
            break;
        case TOK_IDENT:
            if( LOOK_AHEAD(lex) == TOK_EQUAL )
            {
                ::std::string name = tok.str();
                GET_CHECK_TOK(tok, lex, TOK_EQUAL);
                assoc_bounds.insert( ::std::make_pair( ::std::move(name), Parse_Type(lex) ) );
                break;
            }
        default:
            lex.putback(tok);
            types.push_back( Parse_Type(lex) );
            break;
        }
    } while( GET_TOK(tok, lex) == TOK_COMMA );

    // HACK: Split >> into >
    if(tok.type() == TOK_DOUBLE_GT) {
        lex.putback(Token(TOK_GT));
    }
    else {
        CHECK_TOK(tok, TOK_GT);
    }
    
    // TODO: Actually use the lifetimes/assoc_bounds
    
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
            GET_TOK(tok, lex);
        }
        path.append( AST::PathNode(component, params) );
    }
    lex.putback(tok);
    return path;
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

TypeRef Parse_Type(TokenStream& lex);
TypeRef Parse_Type_Fn(TokenStream& lex, ::std::string abi);

TypeRef Parse_Type(TokenStream& lex)
{
    //TRACE_FUNCTION;

    Token tok;
    
    switch( GET_TOK(tok, lex) )
    {
    case TOK_RWORD_EXTERN: {
        GET_CHECK_TOK(tok, lex, TOK_STRING);
        ::std::string abi = tok.str();
        GET_CHECK_TOK(tok, lex, TOK_RWORD_FN);
        return Parse_Type_Fn(lex, abi);
        }
    case TOK_RWORD_FN:
        return Parse_Type_Fn(lex, "");
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
        throw ParseError::Generic(lex, "! is not a real type");
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    throw ParseError::BugCheck("Reached end of Parse_Type");
}

TypeRef Parse_Type_Fn(TokenStream& lex, ::std::string abi)
{
    TRACE_FUNCTION;
    Token   tok;
    
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

/// Parse type parameters in a definition
void Parse_TypeBound(TokenStream& lex, AST::TypeParams& ret, TypeRef checked_type)
{
    TRACE_FUNCTION;
    Token tok;
    
    do
    {
        if(GET_TOK(tok, lex) == TOK_LIFETIME) {
            ret.add_bound( AST::GenericBound(checked_type, tok.str()) );
        }
        else if( tok.type() == TOK_QMARK ) {
            ret.add_bound( AST::GenericBound(checked_type, Parse_Path(lex, PATH_GENERIC_TYPE), true) );
        }
        else {
            lex.putback(tok);
            ret.add_bound( AST::GenericBound(checked_type, Parse_Path(lex, PATH_GENERIC_TYPE)) );
        }
    } while( GET_TOK(tok, lex) == TOK_PLUS );
    lex.putback(tok);
}

AST::TypeParams Parse_TypeParams(TokenStream& lex)
{
    TRACE_FUNCTION;

    AST::TypeParams ret;
    Token tok;
    do {
        bool is_lifetime = false;
        switch( GET_TOK(tok, lex) )
        {
        case TOK_IDENT:
            break;
        case TOK_LIFETIME:
            is_lifetime = true;
            break;
        default:
            // Oopsie!
            throw ParseError::Unexpected(lex, tok);
        }
        ::std::string param_name = tok.str();
        ret.add_param( AST::TypeParam( is_lifetime, param_name ) );
        if( GET_TOK(tok, lex) == TOK_COLON )
        {
            if( is_lifetime )
            {
                throw ParseError::Todo(lex, "lifetime param conditions");
            }
            else
            {
                Parse_TypeBound(lex, ret, TypeRef(TypeRef::TagArg(), param_name));
            }
            GET_TOK(tok, lex);
        }
        
        if( tok.type() == TOK_EQUAL )
        {
            ret.params().back().setDefault( Parse_Type(lex) );
            GET_TOK(tok, lex);
        }
    } while( tok.type() == TOK_COMMA );
    lex.putback(tok);
    return ret;
}

void Parse_WhereClause(TokenStream& lex, AST::TypeParams& params)
{
    TRACE_FUNCTION;
    Token   tok;
    
    do {
        if( GET_TOK(tok, lex) == TOK_LIFETIME )
        {
        }
        else
        {
            lex.putback(tok);
            TypeRef type = Parse_Type(lex);
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            Parse_TypeBound(lex, params, type);
        }
    } while( GET_TOK(tok, lex) == TOK_COMMA );
    lex.putback(tok);
}

// Parse a single function argument
::std::pair< AST::Pattern, TypeRef> Parse_Function_Arg(TokenStream& lex, bool expect_named)
{
    TRACE_FUNCTION;
    Token   tok;
    
    AST::Pattern pat;
    
    if( expect_named || (LOOK_AHEAD(lex) == TOK_IDENT && lex.lookahead(1) == TOK_COLON) )
    {
        pat = Parse_Pattern(lex);
        GET_CHECK_TOK(tok, lex, TOK_COLON);
    }
    
    TypeRef type = Parse_Type(lex);
    
    
    return ::std::make_pair( ::std::move(pat), ::std::move(type) );
}

/// Parse a function definition (after the 'fn <name>')
AST::Function Parse_FunctionDef(TokenStream& lex, ::std::string abi, AST::MetaItems attrs, bool allow_self, bool can_be_prototype)
{
    TRACE_FUNCTION;

    Token   tok;

    // Parameters
    AST::TypeParams params;
    if( GET_TOK(tok, lex) == TOK_LT )
    {
        params = Parse_TypeParams(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);
    }
    else {
        lex.putback(tok);
    }

    AST::Function::Class    fcn_class = AST::Function::CLASS_UNBOUND;
    AST::Function::Arglist  args;

    GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
    GET_TOK(tok, lex);
    
    // Handle self
    if( tok.type() == TOK_AMP )
    {
        // By-reference method?
        
        unsigned int ofs = 0;
        if( lex.lookahead(0) == TOK_LIFETIME )
            ofs ++;
        
        if( lex.lookahead(ofs) == TOK_RWORD_SELF || (lex.lookahead(ofs) == TOK_RWORD_MUT && lex.lookahead(ofs+1) == TOK_RWORD_SELF) )
        {
            ::std::string   lifetime;
            if( GET_TOK(tok, lex) == TOK_LIFETIME ) {
                lifetime = tok.str();
                GET_TOK(tok, lex);
            }
            if( tok.type() == TOK_RWORD_MUT )
            {
                GET_CHECK_TOK(tok, lex, TOK_RWORD_SELF);
                fcn_class = AST::Function::CLASS_MUTMETHOD;
            }
            else
            {
                fcn_class = AST::Function::CLASS_REFMETHOD;
            }
            DEBUG("TODO: UFCS / self lifetimes");
            if( allow_self == false )
                throw ParseError::Generic(lex, "Self binding not expected");
            //args.push_back( ::std::make_pair(
            //    AST::Pattern(),
            //    TypeRef(TypeRef::TagReference(), lifetime, (fcn_class == AST::Function::CLASS_MUTMETHOD), )
            //) );
            
            // Prime tok for next step
            GET_TOK(tok, lex);
        }
        else
        {
            // Unbound method
            lex.putback(tok);   // un-eat the '&'
        }
    }
    else if( tok.type() == TOK_RWORD_MUT )
    {
        if( LOOK_AHEAD(lex) == TOK_RWORD_SELF )
        {
            GET_TOK(tok, lex);
            if( allow_self == false )
                throw ParseError::Generic(lex, "Self binding not expected");
            fcn_class = AST::Function::CLASS_MUTVALMETHOD;
            GET_TOK(tok, lex);
        }
    }
    else if( tok.type() == TOK_RWORD_SELF )
    {
        // By-value method
        if( allow_self == false )
            throw ParseError::Generic(lex, "Self binding not expected");
        fcn_class = AST::Function::CLASS_VALMETHOD;
        GET_TOK(tok, lex);
    }
    else
    {
        // Unbound method
    }
    
    if( tok.type() != TOK_PAREN_CLOSE )
    {
        // Comma after self
        if( fcn_class != AST::Function::CLASS_UNBOUND )
        {
            CHECK_TOK(tok, TOK_COMMA);
        }
        else {
            lex.putback(tok);
        }
        
        // Argument list
        do {
            args.push_back( Parse_Function_Arg(lex, true) );
        } while( GET_TOK(tok, lex) == TOK_COMMA );
        CHECK_TOK(tok, TOK_PAREN_CLOSE);
    }
    else {
        // Eat 'tok', negative comparison
    }

    TypeRef ret_type = TypeRef(TypeRef::TagUnit());;
    if( GET_TOK(tok, lex) == TOK_THINARROW )
    {
        // Return type
        if( GET_TOK(tok, lex) == TOK_EXCLAM ) {
            ret_type = TypeRef(TypeRef::TagInvalid());
        }
        else {
            lex.putback(tok);
            ret_type = Parse_Type(lex);
        }
    }
    else
    {
        lex.putback(tok);
    }

    if( GET_TOK(tok, lex) == TOK_RWORD_WHERE )
    {
        Parse_WhereClause(lex, params);
    }
    else {
        lex.putback(tok);
    }

    return AST::Function(params, fcn_class, ret_type, args);
}

AST::Function Parse_FunctionDefWithCode(TokenStream& lex, ::std::string abi, AST::MetaItems attrs, bool allow_self)
{
    TRACE_FUNCTION;
    Token   tok;
    auto ret = Parse_FunctionDef(lex, abi, ::std::move(attrs), allow_self, false);
    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
    lex.putback(tok);
    ret.set_code( Parse_ExprBlock(lex) );
    return ret;
}

AST::TypeAlias Parse_TypeAlias(TokenStream& lex, const AST::MetaItems meta_items)
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

void Parse_Struct(AST::Module& mod, TokenStream& lex, const bool is_public, const AST::MetaItems meta_items)
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
            Parse_WhereClause(lex, params);
            tok = lex.getToken();
        }
    }
    if(tok.type() == TOK_PAREN_OPEN)
    {
        // Tuple structs
        ::std::vector<AST::StructItem>  refs;
        while(GET_TOK(tok, lex) != TOK_PAREN_CLOSE)
        {
            bool    is_pub = false;
            if(tok.type() == TOK_RWORD_PUB)
                is_pub = true;
            else
                lex.putback(tok);
            
            refs.push_back( AST::StructItem( "", Parse_Type(lex), is_pub ) );
            if( GET_TOK(tok, lex) != TOK_COMMA )
                break;
        }
        CHECK_TOK(tok, TOK_PAREN_CLOSE);
        GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
        if( refs.size() == 0 )
            throw ParseError::Generic(lex, "Use 'struct Name;' instead of 'struct Name();' ... ning-nong");
        mod.add_struct(is_public, ::std::move(name), ::std::move(params), ::std::move(refs));
    }
    else if(tok.type() == TOK_SEMICOLON)
    {
        // Unit-like struct
        mod.add_struct(is_public, name, params, ::std::vector<AST::StructItem>());
    }
    else if(tok.type() == TOK_BRACE_OPEN)
    {
        ::std::vector<AST::StructItem>  items;
        while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
        {
            bool    is_pub = false;
            if(tok.type() == TOK_RWORD_PUB) {
                is_pub = true;
                GET_TOK(tok, lex);
            }
            
            CHECK_TOK(tok, TOK_IDENT);
            ::std::string   name = tok.str();
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            TypeRef type = Parse_Type(lex);
            items.push_back( AST::StructItem( ::std::move(name), ::std::move(type), is_pub ) );
            tok = lex.getToken();
            if(tok.type() == TOK_BRACE_CLOSE)
                break;
            CHECK_TOK(tok, TOK_COMMA);
        }
        if( items.size() == 0 )
            throw ParseError::Generic(lex, "Use 'struct Name;' instead of 'struct Name { }' ... ning-nong");
        mod.add_struct(is_public, name, params, items);
    }
    else
    {
        throw ParseError::Unexpected(lex, tok);
    }
}

AST::Trait Parse_TraitDef(TokenStream& lex, const AST::MetaItems& meta_items)
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
    
    // Trait bounds "trait Trait : 'lifetime + OtherTrait + OtherTrait2"
    if(tok.type() == TOK_COLON)
    {
        // 'Self' is a special generic type only valid within traits
        Parse_TypeBound(lex, params, TypeRef(TypeRef::TagArg(), "Self"));
        GET_TOK(tok, lex);
    }
    
    // TODO: Support "for Sized?"
    if(tok.type() == TOK_RWORD_WHERE)
    {
        if( params.n_params() == 0 )
            throw ParseError::Generic("Where clause with no generic params");
        Parse_WhereClause(lex, params);
        tok = lex.getToken();
    }

    
    AST::Trait trait(params);
        
    CHECK_TOK(tok, TOK_BRACE_OPEN);
    while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
    {
        AST::MetaItems  item_attrs;
        while( tok.type() == TOK_ATTR_OPEN )
        {
            item_attrs.push_back( Parse_MetaItem(lex) );
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
            GET_TOK(tok, lex);
        }
        
        ::std::string   abi = "rust";
        switch(tok.type())
        {
        case TOK_RWORD_STATIC: {
            throw ParseError::Todo("Associated static");
            break; }
        // Associated type
        case TOK_RWORD_TYPE: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            if( GET_TOK(tok, lex) == TOK_COLON )
            {
                // Bounded associated type
                TypeRef a_type = TypeRef(TypeRef::TagAssoc(), TypeRef(TypeRef::TagArg(), "Self"), TypeRef(), name);
                Parse_TypeBound(lex, params, a_type);
                GET_TOK(tok, lex);
            }
            if( tok.type() == TOK_RWORD_WHERE ) {
                throw ParseError::Todo(lex, "Where clause on associated type");
            }
            
            TypeRef default_type;
            if( tok.type() == TOK_EQUAL ) {
                default_type = Parse_Type(lex);
                GET_TOK(tok, lex);
            }
            
            CHECK_TOK(tok, TOK_SEMICOLON);
            trait.add_type( ::std::move(name), ::std::move(default_type) );
            break; }

        // Functions (possibly unsafe)
        case TOK_RWORD_UNSAFE:
            item_attrs.push_back( AST::MetaItem("#UNSAFE") );
            if( GET_TOK(tok, lex) == TOK_RWORD_EXTERN )
        case TOK_RWORD_EXTERN:
            {
                abi = "C";
                if( GET_TOK(tok, lex) == TOK_STRING )
                    abi = tok.str();
                else
                    lex.putback(tok);
                
                GET_TOK(tok, lex);
            }
            CHECK_TOK(tok, TOK_RWORD_FN);
        case TOK_RWORD_FN: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            // Self allowed, prototype-form allowed (optional names and no code)
            auto fcn = Parse_FunctionDef(lex, abi, item_attrs, true, true);
            if( GET_TOK(tok, lex) == TOK_BRACE_OPEN )
            {
                lex.putback(tok);
                fcn.set_code( Parse_ExprBlock(lex) );
            }
            else if( tok.type() == TOK_SEMICOLON )
            {
                // Accept it
            }
            else
            {
                throw ParseError::Unexpected(lex, tok);
            }
            trait.add_function( ::std::move(name), ::std::move(fcn) );
            break; }
        default:
            throw ParseError::Unexpected(lex, tok);
        }
    }
    
    return trait;
}

AST::Enum Parse_EnumDef(TokenStream& lex, const AST::MetaItems meta_items)
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
            Parse_WhereClause(lex, params);
            tok = lex.getToken();
        }
    }
    
    // Body
    CHECK_TOK(tok, TOK_BRACE_OPEN);
    ::std::vector<AST::EnumVariant>   variants;
    while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
    {
        AST::MetaItems  item_attrs;
        while( tok.type() == TOK_ATTR_OPEN )
        {
            item_attrs.push_back( Parse_MetaItem(lex) );
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
            GET_TOK(tok, lex);
        }
        
        CHECK_TOK(tok, TOK_IDENT);
        ::std::string   name = tok.str();
        if( GET_TOK(tok, lex) == TOK_PAREN_OPEN )
        {
            ::std::vector<TypeRef>  types;
            // Get type list
            do
            {
                types.push_back( Parse_Type(lex) );
            } while( GET_TOK(tok, lex) == TOK_COMMA );
            CHECK_TOK(tok, TOK_PAREN_CLOSE);
            GET_TOK(tok, lex);
            variants.push_back( AST::EnumVariant(::std::move(name), ::std::move(types)) );
        }
        else if( tok.type() == TOK_EQUAL )
        {
            bool is_neg = false;
            if( GET_TOK(tok, lex) == TOK_DASH )
                is_neg = true;
            else
                lex.putback(tok);
            GET_CHECK_TOK(tok, lex, TOK_INTEGER);
            int64_t val = (is_neg ? -tok.intval() : tok.intval());
            variants.push_back( AST::EnumVariant(::std::move(name), val) );
            GET_TOK(tok, lex);
        }
        else
        {
            variants.push_back( AST::EnumVariant(::std::move(name), ::std::vector<TypeRef>()) );
        }
        
        if( tok.type() != TOK_COMMA )
            break;
    }
    CHECK_TOK(tok, TOK_BRACE_CLOSE);

    
    return AST::Enum( ::std::move(params), ::std::move(variants) );
}

/// Parse a meta-item declaration (either #![ or #[)
AST::MetaItem Parse_MetaItem(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token tok;
    GET_CHECK_TOK(tok, lex, TOK_IDENT);
    ::std::string   name = tok.str();
    switch(GET_TOK(tok, lex))
    {
    case TOK_EQUAL:
        GET_CHECK_TOK(tok, lex, TOK_STRING);
        return AST::MetaItem(name, tok.str());
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

AST::Impl Parse_Impl(TokenStream& lex, bool is_unsafe=false)
{
    TRACE_FUNCTION;
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
    
    TypeRef trait_type;
    TypeRef impl_type;
    // - Handle negative impls, which must be a trait
    // "impl !Trait for Type {}"
    if( GET_TOK(tok, lex) == TOK_EXCLAM )
    {
        trait_type = Parse_Type(lex);
        GET_CHECK_TOK(tok, lex, TOK_RWORD_FOR);
        impl_type = Parse_Type(lex);
        
        if( GET_TOK(tok, lex) == TOK_RWORD_WHERE )
        {
            Parse_WhereClause(lex, params);
            GET_TOK(tok, lex);
        }
        CHECK_TOK(tok, TOK_BRACE_OPEN);
        // negative impls can't have any content
        GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);
        
        return AST::Impl(AST::Impl::TagNegative(), ::std::move(params),
                ::std::move(impl_type), ::std::move(trait_type)
                );
    }
    else
    {
        // - Don't care which at this stage
        lex.putback(tok);
        
        impl_type = Parse_Type(lex);
        if( GET_TOK(tok, lex) == TOK_RWORD_FOR )
        {
            // Implementing a trait for another type, get the target type
            trait_type = impl_type;
            impl_type = Parse_Type(lex);
        }
        else {
            lex.putback(tok);
        }
    }
    
    // Where clause
    if( GET_TOK(tok, lex) == TOK_RWORD_WHERE )
    {
        Parse_WhereClause(lex, params);
    }
    else {
        lex.putback(tok);
    }
    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);

    AST::Impl   impl( ::std::move(params), ::std::move(impl_type), ::std::move(trait_type) );

    // A sequence of method implementations
    while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
    {
        AST::MetaItems  item_attrs;
        while( tok.type() == TOK_ATTR_OPEN )
        {
            item_attrs.push_back( Parse_MetaItem(lex) );
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
            GET_TOK(tok, lex);
        }
        
        bool is_public = false;
        if(tok.type() == TOK_RWORD_PUB) {
            is_public = true;
            GET_TOK(tok, lex);
        }
        
        if(tok.type() == TOK_RWORD_UNSAFE) {
            item_attrs.push_back( AST::MetaItem("#UNSAFE") );
            GET_TOK(tok, lex);
        }
        
        ::std::string   abi = "rust";
        switch(tok.type())
        {
        case TOK_RWORD_TYPE: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            GET_CHECK_TOK(tok, lex, TOK_EQUAL);
            impl.add_type(is_public, name, Parse_Type(lex));
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            break; }
        case TOK_RWORD_EXTERN:
            {
                abi = "C";
                if( GET_TOK(tok, lex) == TOK_STRING )
                    abi = tok.str();
                else
                    lex.putback(tok);
                
                GET_TOK(tok, lex);
            }
            CHECK_TOK(tok, TOK_RWORD_FN);
        case TOK_RWORD_FN: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            // - Self allowed, can't be prototype-form
            impl.add_function(is_public, ::std::move(name), Parse_FunctionDefWithCode(lex, abi, ::std::move(item_attrs), true));
            break; }

        default:
            throw ParseError::Unexpected(lex, tok);
        }
    }

    return impl;
}

void Parse_ExternBlock(TokenStream& lex, AST::Module& mod, ::std::string abi)
{
    TRACE_FUNCTION;
    Token   tok;
    
    while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
    {
        AST::MetaItems  meta_items;
        while( tok.type() == TOK_ATTR_OPEN )
        {
            meta_items.push_back( Parse_MetaItem(lex) );
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
            GET_TOK(tok, lex);
        }
        
        bool is_public = false;
        if( tok.type() == TOK_RWORD_PUB ) {
            is_public = true;
            GET_TOK(tok, lex);
        }
        switch(tok.type())
        {
        case TOK_RWORD_FN:
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            // parse function as prototype
            // - no self
            mod.add_function(is_public, tok.str(), Parse_FunctionDef(lex, abi, ::std::move(meta_items), false, true));
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            break;
        default:
            throw ParseError::Unexpected(lex, tok);
        }
    }
}

void Parse_Use_Wildcard(const AST::Path& base_path, ::std::function<void(AST::Path, ::std::string)> fcn)
{
    fcn(base_path, ""); // HACK! Empty path indicates wilcard import
}

void Parse_Use(TokenStream& lex, ::std::function<void(AST::Path, ::std::string)> fcn)
{
    TRACE_FUNCTION;

    Token   tok;
    AST::Path   path = AST::Path( AST::Path::TagAbsolute() );
    
    switch( GET_TOK(tok, lex) )
    {
    case TOK_RWORD_SELF:
        path = AST::Path( );    // relative path
        break;
    case TOK_RWORD_SUPER:
        path = AST::Path( AST::Path::TagSuper() );
        break;
    case TOK_IDENT:
        path.append( AST::PathNode(tok.str(), {}) );
        break;
    default:
        throw ParseError::Unexpected(lex, tok);
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
                throw ParseError::Unexpected(lex, tok);
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

::std::vector<MacroPatEnt> Parse_MacroRules_Pat(TokenStream& lex, bool allow_sub, enum eTokenType open, enum eTokenType close)
{
    TRACE_FUNCTION;
    Token tok;
    
    ::std::vector<MacroPatEnt>  ret;
    
     int    depth = 0;
    while( GET_TOK(tok, lex) != close || depth > 0 )
    {
        if( tok.type() == open )
        {
            depth ++;
        }
        else if( tok.type() == close )
        {
            if(depth == 0)
                throw ParseError::Generic(FMT("Unmatched " << Token(close) << " in macro pattern"));
            depth --;
        }
        
        switch(tok.type())
        {
        case TOK_DOLLAR:
            switch( GET_TOK(tok, lex) )
            {
            case TOK_IDENT: {
                ::std::string   name = tok.str();
                GET_CHECK_TOK(tok, lex, TOK_COLON);
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                ::std::string   type = tok.str();
                if(0)
                    ;
                else if( type == "tt" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_TT) );
                else if( type == "ident" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_IDENT) );
                else if( type == "path" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_PATH) );
                else if( type == "expr" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_EXPR) );
                else if( type == "ty" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_TYPE) );
                else
                    throw ParseError::Generic(FMT("Unknown fragment type " << type));
                break; }
            case TOK_PAREN_OPEN:
                if( allow_sub )
                {
                    auto subpat = Parse_MacroRules_Pat(lex, false, TOK_PAREN_OPEN, TOK_PAREN_CLOSE);
                    enum eTokenType joiner = TOK_NULL;
                    GET_TOK(tok, lex);
                    if( tok.type() != TOK_PLUS && tok.type() != TOK_STAR )
                    {
                        DEBUG("Joiner = " << tok);
                        joiner = tok.type();
                        GET_TOK(tok, lex);
                    }
                    DEBUG("tok = " << tok);
                    switch(tok.type())
                    {
                    case TOK_PLUS:
                        DEBUG("$()+ " << subpat);
                        ret.push_back( MacroPatEnt(Token(joiner), true, ::std::move(subpat)) );
                        break;
                    case TOK_STAR:
                        DEBUG("$()* " << subpat);
                        ret.push_back( MacroPatEnt(Token(joiner), false, ::std::move(subpat)) );
                        break;
                    default:
                        throw ParseError::Unexpected(lex, tok);
                    }
                }
                else
                {
                    throw ParseError::Generic(FMT("Nested repetitions in macro"));
                }
                break;
            default:
                throw ParseError::Unexpected(lex, tok);
            }
            break;
        case TOK_EOF:
            throw ParseError::Unexpected(lex, tok);
        default:
            ret.push_back( MacroPatEnt(tok) );
            break;
        }
    }
    
    return ret;
}

::std::vector<MacroRuleEnt> Parse_MacroRules_Cont(TokenStream& lex, bool allow_sub, enum eTokenType open, enum eTokenType close)
{
    TRACE_FUNCTION;
    
    Token tok;
    ::std::vector<MacroRuleEnt> ret;
    
     int    depth = 0;
    while( GET_TOK(tok, lex) != close || depth > 0 )
    {
        if( tok.type() == TOK_EOF ) {
            throw ParseError::Unexpected(lex, tok);
        }
        if( tok.type() == TOK_NULL )    continue ;
        
        if( tok.type() == open )
        {
            DEBUG("depth++");
            depth ++;
        }
        else if( tok.type() == close )
        {
            DEBUG("depth--");
            if(depth == 0)
                throw ParseError::Generic(FMT("Unmatched " << Token(close) << " in macro content"));
            depth --;
        }
        
        if( tok.type() == TOK_DOLLAR )
        {
            GET_TOK(tok, lex);
            
            if( allow_sub && tok.type() == TOK_PAREN_OPEN )
            {
                auto content = Parse_MacroRules_Cont(lex, false, TOK_PAREN_OPEN, TOK_PAREN_CLOSE);
                
                GET_TOK(tok, lex);
                enum eTokenType joiner = TOK_NULL;
                if( tok.type() != TOK_PLUS && tok.type() != TOK_STAR )
                {
                    joiner = tok.type();
                    GET_TOK(tok, lex);
                }
                DEBUG("joiner = " << Token(joiner) << ", content = " << content);
                switch(tok.type())
                {
                case TOK_STAR:
                    ret.push_back( MacroRuleEnt(joiner, ::std::move(content)) );
                    break;
                case TOK_PLUS:
                    // TODO: Ensure that the plusses match
                    ret.push_back( MacroRuleEnt(joiner, ::std::move(content)) );
                    break;
                default:
                    throw ParseError::Unexpected(lex, tok);
                }
                
            }
            else if( tok.type() == TOK_IDENT )
            {
                ret.push_back( MacroRuleEnt(tok.str()) );
            }
            else if( tok.type() == TOK_RWORD_CRATE )
            {
                ret.push_back( MacroRuleEnt("*crate") );
            }
            else
            {
                throw ParseError::Unexpected(lex, tok);
            }
        }
        else
        {
            ret.push_back( MacroRuleEnt(tok) );
        }
    }
    
    return ret;
}

MacroRule Parse_MacroRules_Var(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token tok;
    
    MacroRule   rule;
    
    // Pattern
    enum eTokenType close;
    switch(GET_TOK(tok, lex))
    {
    case TOK_BRACE_OPEN:    close = TOK_BRACE_CLOSE;    break;
    case TOK_PAREN_OPEN:    close = TOK_PAREN_CLOSE;    break;
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    // - Pattern entries
    rule.m_pattern = Parse_MacroRules_Pat(lex, true, tok.type(), close);
    
    GET_CHECK_TOK(tok, lex, TOK_FATARROW);

    // Replacement
    switch(GET_TOK(tok, lex))
    {
    case TOK_BRACE_OPEN:    close = TOK_BRACE_CLOSE;    break;
    case TOK_PAREN_OPEN:    close = TOK_PAREN_CLOSE;    break;
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    rule.m_contents = Parse_MacroRules_Cont(lex, true, tok.type(), close);

    DEBUG("Rule - ["<<rule.m_pattern<<"] => "<<rule.m_contents<<"");
    
    return rule;
}

void Parse_MacroRules(TokenStream& lex, AST::Module& mod, AST::MetaItems& meta_items)
{
    TRACE_FUNCTION;
    
    Token tok;
    
    GET_CHECK_TOK(tok, lex, TOK_IDENT);
    ::std::string name = tok.str();
    
    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
    
    ::std::vector<MacroRule>    rules;
    while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
    {
        lex.putback(tok);
        
        rules.push_back( Parse_MacroRules_Var(lex) );
        if(GET_TOK(tok, lex) != TOK_SEMICOLON) {
            CHECK_TOK(tok, TOK_BRACE_CLOSE);
            break;
        }
    }
    
    bool is_pub = meta_items.has("macro_export");
    
    mod.add_macro( is_pub, name, MacroRules(move(rules)) );
}

void Parse_ModRoot_Items(TokenStream& lex, AST::Crate& crate, AST::Module& mod, LList<AST::Module*>& modstack, const ::std::string& path)
{
    //TRACE_FUNCTION;
    const bool nested_module = (path == "-");  // 'mod name { code }', as opposed to 'mod name;'
    Token   tok;

    for(;;)
    {
        // Check 1 - End of module (either via a closing brace, or EOF)
        switch(GET_TOK(tok, lex))
        {
        case TOK_BRACE_CLOSE:
            if( !nested_module ) {
                DEBUG("Brace close in file root");
                throw ParseError::Unexpected(lex, tok);
            }
            return ;
        case TOK_EOF:
            if( nested_module ) {
                DEBUG("EOF in nested module");
                throw ParseError::Unexpected(lex, tok);
            }
            return ;
        default:
            lex.putback(tok);
            break;
        }

        // Attributes on the following item
        AST::MetaItems  meta_items;
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
        case TOK_MACRO:
            if( tok.str() == "macro_rules" )
            {
                // TODO: Handle #[macro_export]
                Parse_MacroRules(lex, mod, meta_items);
            }
            else
            {
                TokenTree tt = Parse_TT(lex, true);
                if( tt.size() == 0 ) {
                    throw ParseError::Unexpected(lex, tt.tok());
                }
                ::std::string name = tok.str();
                
                auto expanded_macro = Macro_Invoke(lex, name.c_str(), tt);
                // Pass "!" as 'path' to allow termination on EOF
                Parse_ModRoot_Items(*expanded_macro, crate, mod, modstack, "!");
            }
            break;
        
        case TOK_RWORD_USE:
            Parse_Use(lex, [&mod,is_public](AST::Path p, std::string s) { mod.add_alias(is_public, p, s); });
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            break;
        
        case TOK_RWORD_EXTERN:
            switch( GET_TOK(tok, lex) )
            {
            case TOK_STRING: {
                ::std::string abi = tok.str();
                switch(GET_TOK(tok, lex))
                {
                case TOK_RWORD_FN:
                    throw ParseError::Todo(lex, "'extern \"<ABI>\" fn'");
                case TOK_BRACE_OPEN:
                    Parse_ExternBlock(lex, mod, ::std::move(abi));
                    break;
                default:
                    throw ParseError::Unexpected(lex, tok);
                }
                }
                break;
            case TOK_RWORD_CRATE: {
                ::std::string   path, name;
                // TODO: Handle #[macro_use]/#[macro_use(...)]
                if( GET_TOK(tok, lex) == TOK_STRING )
                {
                    path = tok.str();
                    GET_CHECK_TOK(tok, lex, TOK_RWORD_AS);
                    GET_CHECK_TOK(tok, lex, TOK_IDENT);
                    name = tok.str();
                }
                else if( tok.type() == TOK_IDENT )
                {
                    path = name = tok.str();
                }
                else
                {
                    throw ParseError::Unexpected(lex, tok);
                }
                crate.load_extern_crate(path);
                mod.add_ext_crate(path, name);
            
                auto at = meta_items.get("macro_use");
                if( at )
                {
                    if( at->has_sub_items() )
                    {
                        throw ParseError::Todo("selective macro_use");
                    }
                    else
                    {
                        mod.add_macro_import(crate, name, "");
                    }
                }    
            
                GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
                break; }
            default:
                throw ParseError::Unexpected(lex, tok);
            }
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

        case TOK_RWORD_UNSAFE:
            meta_items.push_back( AST::MetaItem("#UNSAFE") );
            switch(GET_TOK(tok, lex))
            {
            case TOK_RWORD_FN:
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                // - self not allowed, not prototype
                mod.add_function(is_public, tok.str(), Parse_FunctionDefWithCode(lex, "rust", ::std::move(meta_items), false));
                break;
            case TOK_RWORD_TRAIT: {
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                ::std::string name = tok.str();
                mod.add_trait(is_public, name, Parse_TraitDef(lex, meta_items));
                break;
                }
            case TOK_RWORD_IMPL:
                mod.add_impl(Parse_Impl(lex, true));
                break;
            default:
                throw ParseError::Unexpected(lex, tok);
            }
            break;
        case TOK_RWORD_FN: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            // - self not allowed, not prototype
            mod.add_function(is_public, name, Parse_FunctionDefWithCode(lex, "rust", ::std::move(meta_items), false));
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
                Parse_ModRoot(lex, crate, submod, &modstack, "-");
                break;
            case TOK_SEMICOLON:
                DEBUG("Mod = " << name << ", curpath = " << path);
                if( path == "-" || path == "!" ) {
                    throw ParseError::Generic(lex, "Cannot load module from file within nested context");
                }
                else if( path.back() != '/' )
                {
                    throw ParseError::Generic( FMT("Can't load from files outside of mod.rs or crate root") );
                }
                else if( meta_items.has("path") )
                {
                    ::std::string newpath_dir  = path + meta_items.get("path")->string();
                    ::std::ifstream ifs_dir (newpath_dir);
                    if( !ifs_dir.is_open() )
                    {
                    }
                    else
                    {
                        ::std::string   newdir( newpath_dir.begin(), newpath_dir.begin() + newpath_dir.find_last_of('/') );
                        Preproc sub_lex(newpath_dir);
                        Parse_ModRoot(sub_lex, crate, submod, &modstack, newdir);
                    }
                }
                else
                {
                    ::std::string newpath_dir  = path + name + "/";
                    ::std::string newpath_file = path + name + ".rs";
                    ::std::ifstream ifs_dir (newpath_dir + "mod.rs");
                    ::std::ifstream ifs_file(newpath_file);
                    if( ifs_dir.is_open() && ifs_file.is_open() )
                    {
                        // Collision
                    }
                    else if( ifs_dir.is_open() )
                    {
                        // Load from dir
                        Preproc sub_lex(newpath_dir + "mod.rs");
                        Parse_ModRoot(sub_lex, crate, submod, &modstack, newpath_dir);
                    }
                    else if( ifs_file.is_open() )
                    {
                        // Load from file
                        Preproc sub_lex(newpath_file);
                        Parse_ModRoot(sub_lex, crate, submod, &modstack, newpath_file);
                    }
                    else
                    {
                        // Can't find file
                        throw ParseError::Generic( FMT("Can't find file for " << name << " in '" << path << "'") );
                    }
                }
                break;
            default:
                throw ParseError::Generic("Expected { or ; after module name");
            }
            submod.prescan();
            mod.add_submod(is_public, ::std::move(submod));
            Macro_SetModule(modstack);
        
            // import macros
            {
                auto at = meta_items.get("macro_use");
                if( at )
                {
                    if( at->has_sub_items() )
                    {
                        throw ParseError::Todo("selective macro_use");
                    }
                    else
                    {
                        mod.add_macro_import(crate, name, "");
                    }
                }
            }
            break; }

        default:
            throw ParseError::Unexpected(lex, tok);
        }
    }
}

void Parse_ModRoot(TokenStream& lex, AST::Crate& crate, AST::Module& mod, LList<AST::Module*> *prev_modstack, const ::std::string& path)
{
    TRACE_FUNCTION;
    LList<AST::Module*>  modstack(prev_modstack, &mod);
    Macro_SetModule(modstack);

    Token   tok;

    if( crate.m_load_std )
    {
        // Import the prelude
        AST::Path   prelude_path = AST::Path( "std", { AST::PathNode("prelude", {}), AST::PathNode("v1", {}) } );
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
    
    Parse_ModRoot_Items(lex, crate, mod, modstack, path);
}

AST::Crate Parse_Crate(::std::string mainfile)
{
    Token   tok;
    
    Preproc lex(mainfile);
    
    size_t p = mainfile.find_last_of('/');
    ::std::string mainpath = (p != ::std::string::npos ? ::std::string(mainfile.begin(), mainfile.begin()+p+1) : "./");
     
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
        rootmod.add_macro_import(crate, "std", "");
        // Prelude imports are handled in Parse_ModRoot
    }
    
    // Include the std if the 'no_std' attribute was absent
    // - First need to load the std macros, then can import the prelude
    
    Parse_ModRoot(lex, crate, rootmod, NULL, mainpath);
    
    return crate;
}
