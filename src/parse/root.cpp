/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/root.cpp
 * - Parsing at the module level (highest-level parsing)
 *
 * Entrypoint:
 * - Parse_Crate : Handles crate attrbutes, and passes on to Parse_ModRoot
 * - Parse_ModRoot
 */
#include "../ast/ast.hpp"
#include "parseerror.hpp"
#include "common.hpp"
#include "../macros.hpp"
#include <cassert>

AST::MetaItem   Parse_MetaItem(TokenStream& lex);
void Parse_ModRoot(TokenStream& lex, AST::Crate& crate, AST::Module& mod, LList<AST::Module*> *prev_modstack, const ::std::string& path);

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

/// Parse type parameters within '<' and '>' (definition)
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
        if( is_lifetime )
            ret.add_lft_param( param_name );
        else
            ret.add_ty_param( AST::TypeParam( param_name ) );
            
        if( GET_TOK(tok, lex) == TOK_COLON )
        {
            if( is_lifetime )
            {
                do {
                    GET_CHECK_TOK(tok, lex, TOK_LIFETIME);
                    ret.add_bound( AST::GenericBound( param_name, tok.str() ) );
                } while( GET_TOK(tok, lex) == TOK_PLUS );
            }
            else
            {
                Parse_TypeBound(lex, ret, TypeRef(TypeRef::TagArg(), param_name));
                GET_TOK(tok, lex);
            }
        }
        
        if( !is_lifetime && tok.type() == TOK_EQUAL )
        {
            ret.ty_params().back().setDefault( Parse_Type(lex) );
            GET_TOK(tok, lex);
        }
    } while( tok.type() == TOK_COMMA );
    lex.putback(tok);
    return ret;
}


/// Parse the contents of a 'where' clause
void Parse_WhereClause(TokenStream& lex, AST::TypeParams& params)
{
    TRACE_FUNCTION;
    Token   tok;
    
    do {
        GET_TOK(tok, lex);
        if( tok.type() == TOK_BRACE_OPEN ) {
            break;
        }
        
        if( tok.type() == TOK_LIFETIME )
        {
            throw ParseError::Todo(lex, "Lifetime bounds in 'where' clauses");
        }
        // Higher-ranked types/lifetimes
        else if( tok.type() == TOK_RWORD_FOR )
        {
            ::std::vector< ::std::string>   lifetimes;
            GET_CHECK_TOK(tok, lex, TOK_LT);
            do {
                switch(GET_TOK(tok, lex))
                {
                case TOK_LIFETIME:
                    lifetimes.push_back(tok.str());
                    break;    
                default:
                    throw ParseError::Unexpected(lex, tok, Token(TOK_LIFETIME));
                }
            } while( GET_TOK(tok, lex) == TOK_COMMA );
            CHECK_TOK(tok, TOK_GT);
            
            // Parse a bound as normal
            TypeRef type = Parse_Type(lex);
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            Parse_TypeBound(lex, params, type);
            
            // And store the higher-ranked lifetime list
            params.bounds().back().set_higherrank( mv$(lifetimes) );
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
    TRACE_FUNCTION_F("expect_named = " << expect_named);
    Token   tok;
    
    AST::Pattern pat;
    
    // If any of the following
    // - Expecting a named parameter (i.e. defining a function in root or impl)
    // - Next token is an underscore (only valid as a pattern here)
    // - Next token is 'mut' (a mutable parameter slot)
    // - Next two are <ident> ':' (a trivial named parameter)
    // NOTE: When not expecting a named param, destructuring patterns are not allowed
    if( expect_named
      || LOOK_AHEAD(lex) == TOK_UNDERSCORE
      || LOOK_AHEAD(lex) == TOK_RWORD_MUT
      || (LOOK_AHEAD(lex) == TOK_IDENT && lex.lookahead(1) == TOK_COLON)
      )
    {
        // Function args can't be refuted
        pat = Parse_Pattern(lex, false);
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
            args.push_back( Parse_Function_Arg(lex, !can_be_prototype) );
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

    return AST::Function(::std::move(attrs), ::std::move(params), fcn_class, ::std::move(ret_type), ::std::move(args));
}

AST::Function Parse_FunctionDefWithCode(TokenStream& lex, ::std::string abi, AST::MetaItems attrs, bool allow_self)
{
    Token   tok;
    auto ret = Parse_FunctionDef(lex, abi, ::std::move(attrs), allow_self, false);
    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
    lex.putback(tok);
    ret.set_code( Parse_ExprBlock(lex) );
    return ret;
}

AST::TypeAlias Parse_TypeAlias(TokenStream& lex, AST::MetaItems meta_items)
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
    
    return AST::TypeAlias( ::std::move(meta_items), ::std::move(params), ::std::move(type) );
}

AST::Struct Parse_Struct(TokenStream& lex, const AST::MetaItems meta_items)
{
    TRACE_FUNCTION;

    Token   tok;

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
            AST::MetaItems  item_attrs;
            while( tok.type() == TOK_ATTR_OPEN )
            {
                item_attrs.push_back( Parse_MetaItem(lex) );
                GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
                GET_TOK(tok, lex);
            }
            
            bool    is_pub = false;
            if(tok.type() == TOK_RWORD_PUB)
                is_pub = true;
            else
                lex.putback(tok);
            
            // TODO: Save item_attrs
            refs.push_back( AST::StructItem( "", Parse_Type(lex), is_pub ) );
            if( GET_TOK(tok, lex) != TOK_COMMA )
                break;
        }
        CHECK_TOK(tok, TOK_PAREN_CLOSE);
        GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
        if( refs.size() == 0 )
            throw ParseError::Generic(lex, "Use 'struct Name;' instead of 'struct Name();' ... ning-nong");
        return AST::Struct(::std::move(meta_items), ::std::move(params), ::std::move(refs));
    }
    else if(tok.type() == TOK_SEMICOLON)
    {
        // Unit-like struct
        return AST::Struct(::std::move(meta_items), ::std::move(params), ::std::vector<AST::StructItem>());
    }
    else if(tok.type() == TOK_BRACE_OPEN)
    {
        ::std::vector<AST::StructItem>  items;
        while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
        {
            AST::MetaItems  item_attrs;
            while( tok.type() == TOK_ATTR_OPEN )
            {
                item_attrs.push_back( Parse_MetaItem(lex) );
                GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
                GET_TOK(tok, lex);
            }
            
            bool    is_pub = false;
            if(tok.type() == TOK_RWORD_PUB) {
                is_pub = true;
                GET_TOK(tok, lex);
            }
            
            CHECK_TOK(tok, TOK_IDENT);
            ::std::string   name = tok.str();
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            TypeRef type = Parse_Type(lex);
            
            // TODO: Save item_attrs
            items.push_back( AST::StructItem( ::std::move(name), ::std::move(type), is_pub ) );
            if(GET_TOK(tok, lex) == TOK_BRACE_CLOSE)
                break;
            CHECK_TOK(tok, TOK_COMMA);
        }
        if( items.size() == 0 )
            throw ParseError::Generic(lex, "Use 'struct Name;' instead of 'struct Name { }' ... ning-nong");
        return AST::Struct(::std::move(meta_items), ::std::move(params), ::std::move(items));
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
        //if( params.ty_params().size() == 0 )
        //    throw ParseError::Generic("Where clause with no generic params");
        Parse_WhereClause(lex, params);
        tok = lex.getToken();
    }

    
    AST::Trait trait(mv$(meta_items), mv$(params));
        
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
                TypeRef a_type = TypeRef( AST::Path(AST::Path::TagUfcs(), TypeRef(TypeRef::TagArg(), "Self"), TypeRef())+ name);
                //TypeRef a_type = TypeRef(TypeRef::TagAssoc(), TypeRef(TypeRef::TagArg(), "Self"), TypeRef(), name);
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
            variants.push_back( AST::EnumVariant(mv$(item_attrs), mv$(name), mv$(types)) );
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
            variants.push_back( AST::EnumVariant(mv$(item_attrs), mv$(name), val) );
            GET_TOK(tok, lex);
        }
        else
        {
            variants.push_back( AST::EnumVariant(mv$(item_attrs), mv$(name), ::std::vector<TypeRef>()) );
        }
        
        if( tok.type() != TOK_COMMA )
            break;
    }
    CHECK_TOK(tok, TOK_BRACE_CLOSE);

    
    return AST::Enum( mv$(meta_items), mv$(params), mv$(variants) );
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

void Parse_Impl(TokenStream& lex, AST::Module& mod, bool is_unsafe/*=false*/);
void Parse_Impl_Item(TokenStream& lex, AST::Impl& impl);

void Parse_Impl(TokenStream& lex, AST::Module& mod, bool is_unsafe/*=false*/)
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
    
    AST::Path   trait_path;
    TypeRef impl_type;
    // - Handle negative impls, which must be a trait
    // "impl !Trait for Type {}"
    if( GET_TOK(tok, lex) == TOK_EXCLAM )
    {
        trait_path = Parse_Path(lex, PATH_GENERIC_TYPE);
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
        
        mod.add_neg_impl( AST::ImplDef( AST::MetaItems(), ::std::move(params), ::std::move(trait_path), ::std::move(impl_type) ) );
        return ;
    }
    else
    {
        // - Don't care which at this stage
        lex.putback(tok);
        
        impl_type = Parse_Type(lex);
        // TODO: Handle the "impl Any + Send" syntax here
        
        if( GET_TOK(tok, lex) == TOK_RWORD_FOR )
        {
            if( !impl_type.is_path() )
                throw ParseError::Generic(lex, "Trait was not a path");
            trait_path = impl_type.path();
            // Implementing a trait for another type, get the target type
            if( GET_TOK(tok, lex) == TOK_DOUBLE_DOT )
            {
                // Default impl
                impl_type = TypeRef();
            }
            else
            {
                lex.putback(tok);
                impl_type = Parse_Type(lex);
            }
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

    // TODO: Pass #[] attrs to impl blocks
    AST::Impl   impl( AST::MetaItems(), ::std::move(params), ::std::move(impl_type), ::std::move(trait_path) );

    // A sequence of method implementations
    while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
    {
        lex.putback(tok);
        Parse_Impl_Item(lex, impl);
    }

    mod.add_impl( ::std::move(impl) );
}

void Parse_Impl_Item(TokenStream& lex, AST::Impl& impl)
{
    TRACE_FUNCTION;
    Token   tok;
    
    GET_TOK(tok, lex);
    
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
    case TOK_MACRO:
        {
            TokenTree tt = Parse_TT(lex, true);
            if( tt.is_token() ) {
                DEBUG("TT was a single token (not a sub-tree)");
                throw ParseError::Unexpected(lex, tt.tok());
            }
            
            auto expanded_macro = Macro_Invoke(lex, tok.str().c_str(), tt);
            auto& lex = *expanded_macro;
            while( GET_TOK(tok, lex) != TOK_EOF )
            {
                lex.putback(tok);
                Parse_Impl_Item(lex, impl);
            }
        }
        if(GET_TOK(tok, lex) != TOK_SEMICOLON)
            lex.putback(tok);
        break;
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
    // Leading :: is allowed and ignored for the $crate feature
    case TOK_DOUBLE_COLON:
        // Absolute path
        // HACK! mrustc emits $crate as `::"crate-name"`
        if( LOOK_AHEAD(lex) == TOK_STRING )
        {
            GET_CHECK_TOK(tok, lex, TOK_STRING);
            path.set_crate(tok.str());
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        }
        else {
            lex.putback(tok);
        }
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
                else if( type == "pat" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_PAT) );
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
                    auto subpat = Parse_MacroRules_Pat(lex, true, TOK_PAREN_OPEN, TOK_PAREN_CLOSE);
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
                    throw ParseError::Generic(lex, FMT("Nested repetitions in macro"));
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
            
            if( tok.type() == TOK_PAREN_OPEN )
            {   
                if( !allow_sub )
                    throw ParseError::Unexpected(lex, tok);
                
                auto content = Parse_MacroRules_Cont(lex, true, TOK_PAREN_OPEN, TOK_PAREN_CLOSE);
                
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

void Parse_MacroRules(TokenStream& lex, AST::Module& mod, AST::MetaItems meta_items)
{
    TRACE_FUNCTION_F("meta_items="<<meta_items);
    
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
    const bool nested_module = (path == "-" || path == "!");  // 'mod name { code }', as opposed to 'mod name;'
    Token   tok;

    for(;;)
    {
        // Check 1 - End of module (either via a closing brace, or EOF)
        switch(GET_TOK(tok, lex))
        {
        case TOK_BRACE_CLOSE:
        case TOK_EOF:
            lex.putback(tok);
            return;
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
        DEBUG("meta_items = " << meta_items);

        if( GET_TOK(tok, lex) == TOK_MACRO )
        {
            if( tok.str() == "macro_rules" )
            {
                Parse_MacroRules(lex, mod, mv$(meta_items));
            }
            else
            {
                DEBUG("Invoke macro '"<<tok.str()<<"'");
                TokenTree tt = Parse_TT(lex, true);
                if( tt.is_token() ) {
                    DEBUG("TT was a single token (not a sub-tree)");
                    throw ParseError::Unexpected(lex, tt.tok());
                }
                ::std::string name = tok.str();
                
                auto expanded_macro = Macro_Invoke(lex, name.c_str(), tt);
                // Pass "!" as 'path' to allow termination on EOF
                Parse_ModRoot_Items(*expanded_macro, crate, mod, modstack, "!");
            }
            // - Silently consume ';' after the macro
            if( GET_TOK(tok, lex) != TOK_SEMICOLON )
                lex.putback(tok);
            continue ;
        }
        else {
            lex.putback(tok);
        }
    
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
            mod.add_static(is_public, name, AST::Static(::std::move(meta_items), AST::Static::CONST, type, val));
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
            mod.add_static(is_public, name,
                AST::Static(::std::move(meta_items), (is_mut ? AST::Static::MUT : AST::Static::STATIC), type, val)
                );
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
                Parse_Impl(lex, mod, true);
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
        case TOK_RWORD_STRUCT: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            mod.add_struct( is_public, name, Parse_Struct(lex, meta_items) );
            break; }
        case TOK_RWORD_ENUM: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            mod.add_enum(is_public, name, Parse_EnumDef(lex, meta_items));
            break; }
        case TOK_RWORD_IMPL:
            Parse_Impl(lex, mod);
            break;
        case TOK_RWORD_TRAIT: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            mod.add_trait(is_public, name, Parse_TraitDef(lex, meta_items));
            break; }

        case TOK_RWORD_MOD: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            // TODO: Remove this copy, by keeping record of macro_use()
            AST::Module submod(meta_items, name);
            DEBUG("Sub module '"<<name<<"'");
            switch( GET_TOK(tok, lex) )
            {
            case TOK_BRACE_OPEN: {
                ::std::string   subpath = ( path.back() != '/' ? "-" : path + name + "/" );
                Parse_ModRoot(lex, crate, submod, &modstack, subpath);
                GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);
                break; }
            case TOK_SEMICOLON:
                DEBUG("Mod = " << name << ", curpath = " << path);
                if( nested_module ) {
                    throw ParseError::Generic(lex, "Cannot load module from file within nested context");
                }
                else if( path.back() != '/' )
                {
                    throw ParseError::Generic( FMT("Can't load from files outside of mod.rs or crate root") );
                }
                else if( submod.attrs().has("path") )
                {
                    ::std::string newpath_dir  = path + submod.attrs().get("path")->string();
                    ::std::ifstream ifs_dir (newpath_dir);
                    if( !ifs_dir.is_open() )
                    {
                    }
                    else
                    {
                        ::std::string   newdir( newpath_dir.begin(), newpath_dir.begin() + newpath_dir.find_last_of('/') );
                        Lexer sub_lex(newpath_dir);
                        Parse_ModRoot(sub_lex, crate, submod, &modstack, newdir);
                        GET_CHECK_TOK(tok, sub_lex, TOK_EOF);
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
                        Lexer sub_lex(newpath_dir + "mod.rs");
                        Parse_ModRoot(sub_lex, crate, submod, &modstack, newpath_dir);
                        GET_CHECK_TOK(tok, sub_lex, TOK_EOF);
                    }
                    else if( ifs_file.is_open() )
                    {
                        // Load from file
                        Lexer sub_lex(newpath_file);
                        Parse_ModRoot(sub_lex, crate, submod, &modstack, newpath_file);
                        GET_CHECK_TOK(tok, sub_lex, TOK_EOF);
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
    
    Lexer lex(mainfile);
    
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
    for( const auto& attr : rootmod.attrs().m_items )
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
