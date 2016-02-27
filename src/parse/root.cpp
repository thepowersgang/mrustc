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

::std::string dirname(::std::string input) {
    while( input.size() > 0 && input.back() != '/' ) {
        input.pop_back();
    }
    return input;
}

AST::MetaItem   Parse_MetaItem(TokenStream& lex);
void Parse_ModRoot(TokenStream& lex, AST::Module& mod, LList<AST::Module*> *prev_modstack, bool file_controls_dir, const ::std::string& path);

::std::vector< ::std::string> Parse_HRB(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token   tok;
    
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
    return lifetimes;
}
/// Parse type parameters in a definition
void Parse_TypeBound(TokenStream& lex, AST::GenericParams& ret, TypeRef checked_type, ::std::vector< ::std::string> lifetimes = {})
{
    TRACE_FUNCTION;
    Token tok;
    
    do
    {
        if(GET_TOK(tok, lex) == TOK_LIFETIME) {
            ret.add_bound(AST::GenericBound::make_TypeLifetime( {type: checked_type, bound: tok.str()} ));
        }
        else if( tok.type() == TOK_QMARK ) {
            ret.add_bound(AST::GenericBound::make_MaybeTrait( {type: checked_type, trait: Parse_Path(lex, PATH_GENERIC_TYPE)} ));
        }
        else {
            if( tok.type() == TOK_RWORD_FOR )
            {
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
            }
            else {
                lex.putback(tok);
            }
            
            ret.add_bound( AST::GenericBound::make_IsTrait( {type: checked_type, hrls: lifetimes, trait: Parse_Path(lex, PATH_GENERIC_TYPE) }) );
        }
    } while( GET_TOK(tok, lex) == TOK_PLUS );
    lex.putback(tok);
}

/// Parse type parameters within '<' and '>' (definition)
AST::GenericParams Parse_GenericParams(TokenStream& lex)
{
    TRACE_FUNCTION;

    AST::GenericParams ret;
    Token tok;
    do {
        bool is_lifetime = false;
        if( GET_TOK(tok, lex) == TOK_GT ) {
            break ;
        }
        switch( tok.type() )
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
                    ret.add_bound(AST::GenericBound::make_Lifetime( {test: param_name, bound: tok.str()} ));
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
void Parse_WhereClause(TokenStream& lex, AST::GenericParams& params)
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
            auto lhs = mv$(tok.str());
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            GET_CHECK_TOK(tok, lex, TOK_LIFETIME);
            auto rhs = mv$(tok.str());
            params.add_bound( AST::GenericBound::make_Lifetime({lhs, rhs}) );
        }
        // Higher-ranked types/lifetimes
        else if( tok.type() == TOK_RWORD_FOR )
        {
            ::std::vector< ::std::string>   lifetimes = Parse_HRB(lex);
            
            TypeRef type = Parse_Type(lex);
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            Parse_TypeBound(lex, params, type, lifetimes);
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
    AST::GenericParams params;
    if( GET_TOK(tok, lex) == TOK_LT )
    {
        params = Parse_GenericParams(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);
    }
    else {
        lex.putback(tok);
    }

    AST::Function::Arglist  args;

    GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
    GET_TOK(tok, lex);
    
    // Handle self
    if( tok.type() == TOK_AMP )
    {
        // By-reference method?
        // TODO: If a lifetime is seen (and not a prototype), it is definitely a self binding
        
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
                args.push_back( ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "self"), TypeRef(TypeRef::TagReference(), true, TypeRef("Self"))) );
            }
            else
            {
                args.push_back( ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "self"), TypeRef(TypeRef::TagReference(), false, TypeRef("Self"))) );
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
        }
    }
    else if( tok.type() == TOK_RWORD_MUT )
    {
        if( LOOK_AHEAD(lex) == TOK_RWORD_SELF )
        {
            GET_TOK(tok, lex);
            if( allow_self == false )
                throw ParseError::Generic(lex, "Self binding not expected");
            TypeRef ty;
            if( GET_TOK(tok, lex) == TOK_COLON ) {
                // Typed mut self
                ty = Parse_Type(lex);
            }
            else {
                lex.putback(tok);
                ty = TypeRef("Self");
            }
            args.push_back( ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "self"), ty) );
            GET_TOK(tok, lex);
        }
    }
    else if( tok.type() == TOK_RWORD_SELF )
    {
        // By-value method
        if( allow_self == false )
            throw ParseError::Generic(lex, "Self binding not expected");
        TypeRef ty;
        if( GET_TOK(tok, lex) == TOK_COLON ) {
            // Typed mut self
            ty = Parse_Type(lex);
        }
        else {
            lex.putback(tok);
            ty = TypeRef("Self");
        }
        args.push_back( ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), "self"), ty) );
        GET_TOK(tok, lex);
    }
    else
    {
        // Unbound method
    }
    
    if( tok.type() != TOK_PAREN_CLOSE )
    {
        // Comma after self
        if( args.size() )
        {
            CHECK_TOK(tok, TOK_COMMA);
        }
        else {
            lex.putback(tok);
        }
        
        // Argument list
        do {
            if( LOOK_AHEAD(lex) == TOK_PAREN_CLOSE ) {
                GET_TOK(tok, lex);
                break;
            }
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

    return AST::Function(::std::move(attrs), ::std::move(params), ::std::move(ret_type), ::std::move(args));
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
    AST::GenericParams params;
    if( tok.type() == TOK_LT )
    {
        params = Parse_GenericParams(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);
        tok = lex.getToken();
    }
    
    if( tok.type() == TOK_RWORD_WHERE )
    {
        Parse_WhereClause(lex, params);
        GET_TOK(tok, lex);
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
    AST::GenericParams params;
    if( tok.type() == TOK_LT )
    {
        params = Parse_GenericParams(lex);
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

        if(LOOK_AHEAD(lex) == TOK_RWORD_WHERE)
        {
            GET_TOK(tok, lex);
            Parse_WhereClause(lex, params);
        }
        // TODO: Where block
        GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
        //if( refs.size() == 0 )
        //    WARNING( , W000, "Use 'struct Name;' instead of 'struct Name();' ... ning-nong");
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
        //if( items.size() == 0 )
        //    WARNING( , W000, "Use 'struct Name;' instead of 'struct Nam { };' ... ning-nong");
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
    
    AST::GenericParams params;
    if( GET_TOK(tok, lex) == TOK_LT )
    {
        params = Parse_GenericParams(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);
        tok = lex.getToken();
    }
    
    // Trait bounds "trait Trait : 'lifetime + OtherTrait + OtherTrait2"
    ::std::vector<AST::Path>    supertraits;
    if(tok.type() == TOK_COLON)
    {
        do {
            if( GET_TOK(tok, lex) == TOK_LIFETIME ) {
                // TODO: Need a better way of indiciating 'static than just an invalid path
                supertraits.push_back( AST::Path() );
            }
            else {
                lex.putback(tok);
                supertraits.push_back( Parse_Path(lex, PATH_GENERIC_TYPE) );
            }
        } while( GET_TOK(tok, lex) == TOK_PLUS );
    }
    
    // TODO: Support "for Sized?"
    if(tok.type() == TOK_RWORD_WHERE)
    {
        //if( params.ty_params().size() == 0 )
        //    throw ParseError::Generic("Where clause with no generic params");
        Parse_WhereClause(lex, params);
        tok = lex.getToken();
    }

    
    AST::Trait trait( mv$(meta_items), mv$(params), mv$(supertraits) );
        
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
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = mv$(tok.str());
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            auto ty = Parse_Type(lex);
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            
            ::AST::Expr val;
            if(GET_TOK(tok, lex) == TOK_EQUAL) {
                val = Parse_Expr(lex, true);
                GET_TOK(tok, lex);
            }
            CHECK_TOK(tok, TOK_SEMICOLON);
            
            trait.add_static( mv$(name), ::AST::Static(mv$(item_attrs), AST::Static::STATIC, mv$(ty), val) );
            break; }
        case TOK_RWORD_CONST: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = mv$(tok.str());
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            auto ty = Parse_Type(lex);
            
            ::AST::Expr val;
            if(GET_TOK(tok, lex) == TOK_EQUAL) {
                val = Parse_Expr(lex, true);
                GET_TOK(tok, lex);
            }
            CHECK_TOK(tok, TOK_SEMICOLON);
            
            trait.add_static( mv$(name), ::AST::Static(mv$(item_attrs), AST::Static::CONST, mv$(ty), val) );
            break; }
        // Associated type
        case TOK_RWORD_TYPE: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            if( GET_TOK(tok, lex) == TOK_COLON )
            {
                // Bounded associated type
                TypeRef a_type = TypeRef( AST::Path(AST::Path::TagUfcs(), TypeRef(TypeRef::TagArg(), "Self"), TypeRef(), {AST::PathNode(name)}) );
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
    AST::GenericParams params;
    if( tok.type() == TOK_LT )
    {
        params = Parse_GenericParams(lex);
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
        auto sp = lex.start_span();
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
                if(LOOK_AHEAD(lex) == TOK_PAREN_CLOSE)
                {
                    GET_TOK(tok, lex);
                    break;
                }
                
                AST::MetaItems  field_attrs;
                while( LOOK_AHEAD(lex) == TOK_ATTR_OPEN )
                {
                    GET_TOK(tok, lex);
                    field_attrs.push_back( Parse_MetaItem(lex) );
                    GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
                }
                
                types.push_back( Parse_Type(lex) );
            } while( GET_TOK(tok, lex) == TOK_COMMA );
            CHECK_TOK(tok, TOK_PAREN_CLOSE);
            GET_TOK(tok, lex);
            variants.push_back( AST::EnumVariant(mv$(item_attrs), mv$(name), mv$(types)) );
        }
        else if( tok.type() == TOK_BRACE_OPEN )
        {
            ::std::vector<::AST::StructItem>   fields;
            do
            {
                if(LOOK_AHEAD(lex) == TOK_BRACE_CLOSE)
                {
                    GET_TOK(tok, lex);
                    break;
                }
                
                AST::MetaItems  field_attrs;
                while( LOOK_AHEAD(lex) == TOK_ATTR_OPEN )
                {
                    GET_TOK(tok, lex);
                    field_attrs.push_back( Parse_MetaItem(lex) );
                    GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
                }
                
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                auto name = mv$(tok.str());
                GET_CHECK_TOK(tok, lex, TOK_COLON);
                auto ty = Parse_Type(lex);
                // TODO: Field attributes
                fields.push_back( ::AST::Item<TypeRef>(mv$(name), mv$(ty), true) );
            } while( GET_TOK(tok, lex) == TOK_COMMA );
            CHECK_TOK(tok, TOK_BRACE_CLOSE);
            GET_TOK(tok, lex);
            
            variants.push_back( AST::EnumVariant(mv$(item_attrs), mv$(name), mv$(fields)) );
        }
        else if( tok.type() == TOK_EQUAL )
        {
            auto node = Parse_Expr(lex, true);
            variants.push_back( AST::EnumVariant(mv$(item_attrs), mv$(name), mv$(node)) );
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
        if( LOOK_AHEAD(lex) != TOK_PAREN_CLOSE )
        {
            do {
                items.push_back(Parse_MetaItem(lex));
            } while(GET_TOK(tok, lex) == TOK_COMMA);
            CHECK_TOK(tok, TOK_PAREN_CLOSE);
        }
        else {
            GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
        }
        return AST::MetaItem(name, items); }
    default:
        lex.putback(tok);
        return AST::MetaItem(name);
    }
}

void Parse_Impl(TokenStream& lex, AST::Module& mod, AST::MetaItems attrs, bool is_unsafe/*=false*/);
void Parse_Impl_Item(TokenStream& lex, AST::Impl& impl);

void Parse_Impl(TokenStream& lex, AST::Module& mod, AST::MetaItems attrs, bool is_unsafe/*=false*/)
{
    TRACE_FUNCTION;
    Token   tok;

    AST::GenericParams params;
    // 1. (optional) type parameters
    if( GET_TOK(tok, lex) == TOK_LT )
    {
        params = Parse_GenericParams(lex);
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
            trait_path = mv$(impl_type.path());
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

    while( LOOK_AHEAD(lex) == TOK_CATTR_OPEN )
    {
        GET_TOK(tok, lex);
        attrs.push_back( Parse_MetaItem(lex) );
        GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
    }
    
    // TODO: Pass #[] attrs to impl blocks
    AST::Impl   impl( mv$(attrs), ::std::move(params), ::std::move(impl_type), ::std::move(trait_path) );

    // A sequence of method implementations
    while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
    {
        if( tok.type() == TOK_MACRO )
        {
            impl.add_macro_invocation( Parse_MacroInvocation( AST::MetaItems(), mv$(tok.str()), lex ) );
            // - Silently consume ';' after the macro
            if( GET_TOK(tok, lex) != TOK_SEMICOLON )
                lex.putback(tok);
        }
        else
        {
            lex.putback(tok);
            Parse_Impl_Item(lex, impl);
        }
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
    case TOK_RWORD_TYPE: {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        ::std::string name = tok.str();
        GET_CHECK_TOK(tok, lex, TOK_EQUAL);
        impl.add_type(is_public, name, Parse_Type(lex));
        GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
        break; }
    case TOK_RWORD_CONST:
        if( GET_TOK(tok, lex) != TOK_RWORD_FN && tok.type() != TOK_RWORD_UNSAFE )
        {
            CHECK_TOK(tok, TOK_IDENT);
            auto name = mv$(tok.str());
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            auto ty = Parse_Type(lex);
            GET_CHECK_TOK(tok, lex, TOK_EQUAL);
            auto val = Parse_Expr(lex, true);
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            
            impl.add_static( is_public, mv$(name), ::AST::Static(mv$(item_attrs), AST::Static::CONST, mv$(ty), mv$(val)) );
            break ;
        }
        else if( tok.type() == TOK_RWORD_UNSAFE )
        {
            if( GET_TOK(tok, lex) != TOK_RWORD_FN )
                ERROR(lex.end_span(lex.start_span()), E0000, "");
            item_attrs.push_back( AST::MetaItem("#UNSAFE") );
        }
        if( 0 )
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

    AST::MetaItems  block_attrs;
    while( GET_TOK(tok, lex) == TOK_CATTR_OPEN )
    {
        block_attrs.push_back( Parse_MetaItem(lex) );
        GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
    }
    lex.putback(tok);
    // TODO: Use `block_attrs`
    
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
        case TOK_RWORD_STATIC: {
            bool is_mut = false;
            if( GET_TOK(tok, lex) == TOK_RWORD_MUT )
                is_mut = true;
            else
                lex.putback(tok);
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = mv$(tok.str());
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            auto type = Parse_Type(lex);
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            
            auto static_class = is_mut ? ::AST::Static::MUT : ::AST::Static::STATIC;
            mod.add_static(is_public, mv$(name), ::AST::Static(mv$(meta_items), static_class,  type, ::AST::Expr()));
            break; }
        default:
            throw ParseError::Unexpected(lex, tok, {TOK_RWORD_FN, TOK_RWORD_STATIC});
        }
    }
}

void Parse_Use_Wildcard(AST::Path base_path, ::std::function<void(AST::Path, ::std::string)> fcn)
{
    fcn( mv$(base_path), ""); // HACK! Empty path indicates wilcard import
}
void Parse_Use_Set(TokenStream& lex, const AST::Path& base_path, ::std::function<void(AST::Path, ::std::string)> fcn)
{
    TRACE_FUNCTION;

    Token   tok;
    do {
        AST::Path   path;
        ::std::string   name;
        if( GET_TOK(tok, lex) == TOK_RWORD_SELF ) {
            path = ::AST::Path(base_path);
            name = base_path[base_path.size()-1].name();
        }
        else if( tok.type() == TOK_BRACE_CLOSE ) {
            break ;
        }
        else {
            CHECK_TOK(tok, TOK_IDENT);
            path = base_path + AST::PathNode(tok.str(), {});
            name = mv$(tok.str());
        }
        if( GET_TOK(tok, lex) == TOK_RWORD_AS ) {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            name = mv$(tok.str());
        }
        else {
            lex.putback(tok);
        }
        fcn(mv$(path), mv$(name));
    } while( GET_TOK(tok, lex) == TOK_COMMA );
    lex.putback(tok);
}

void Parse_Use(TokenStream& lex, ::std::function<void(AST::Path, ::std::string)> fcn)
{
    TRACE_FUNCTION;

    Token   tok;
    AST::Path   path = AST::Path("", {});
    ::std::vector<AST::PathNode>    nodes;
    ProtoSpan   span_start = lex.start_span();
    
    switch( GET_TOK(tok, lex) )
    {
    case TOK_RWORD_SELF:
        path = AST::Path( AST::Path::TagSelf(), {} );    // relative path
        break;
    case TOK_RWORD_SUPER: {
        unsigned int count = 1;
        while( LOOK_AHEAD(lex) == TOK_DOUBLE_COLON && lex.lookahead(1) == TOK_RWORD_SUPER ) {
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
            GET_CHECK_TOK(tok, lex, TOK_RWORD_SUPER);
            count += 1;
        }
        path = AST::Path( AST::Path::TagSuper(), count, {} );
        break; }
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
    case TOK_BRACE_OPEN:
        Parse_Use_Set(lex, path, fcn);
        GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);
        return;
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    while( GET_TOK(tok, lex) == TOK_DOUBLE_COLON )
    {
        if( GET_TOK(tok, lex) == TOK_IDENT )
        {
            path.append( AST::PathNode(tok.str(), {}) );
        }
        else
        {
            //path.set_span( lex.end_span(span_start) );
            switch( tok.type() )
            {
            case TOK_BRACE_OPEN:
                Parse_Use_Set(lex, mv$(path), fcn);
                GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);
                break ;
            case TOK_STAR:
                Parse_Use_Wildcard( mv$(path), fcn );
                break ;
            default:
                throw ParseError::Unexpected(lex, tok);
            }
            // early return - This branch is either the end of the use statement, or a syntax error
            return ;
        }
    }
    //path.set_span( lex.end_span(span_start) );
    
    ::std::string name;
    // This should only be allowed if the last token was an ident
    // - Above checks ensure this
    if( tok.type() == TOK_RWORD_AS )
    {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        name = tok.str();
    }
    else
    {
        lex.putback(tok);
        assert(path.nodes().size() > 0);
        name = path.nodes().back().name();
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
                else if( type == "meta" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_META) );
                else if( type == "block" )
                    ret.push_back( MacroPatEnt(name, MacroPatEnt::PAT_BLOCK) );
                else
                    throw ParseError::Generic(lex, FMT("Unknown fragment type '" << type << "'"));
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
    
    eTokenType  close;
    switch(GET_TOK(tok,lex))
    {
    case TOK_BRACE_OPEN:    close = TOK_BRACE_CLOSE;    break;
    case TOK_PAREN_OPEN:    close = TOK_PAREN_CLOSE;    break;
    case TOK_SQUARE_OPEN:   close = TOK_SQUARE_CLOSE;   break;
    default:
        // TODO: Synerror
        throw ParseError::Unexpected(lex, tok, {TOK_BRACE_OPEN, TOK_PAREN_OPEN, TOK_SQUARE_OPEN});
        break;
    }
    
    ::std::vector<MacroRule>    rules;
    while( GET_TOK(tok, lex) != close )
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

::AST::MacroInvocation Parse_MacroInvocation(::AST::MetaItems meta_items, ::std::string name, TokenStream& lex)
{
    Token   tok;
    ::std::string   ident;
    if( GET_TOK(tok, lex) == TOK_IDENT ) {
        ident = mv$(tok.str());
    }
    else {
        lex.putback(tok);
    }
    TokenTree tt = Parse_TT(lex, true);
    return ::AST::MacroInvocation( mv$(meta_items), mv$(name), mv$(ident), mv$(tt));
}

void Parse_ExternCrate(TokenStream& lex, AST::Module& mod, AST::MetaItems meta_items)
{
    Token   tok;
    ::std::string   path, name;
    switch( GET_TOK(tok, lex) )
    {
    // `extern crate "crate-name" as crate_name;`
    // TODO: rustc no longer supports this feature
    case TOK_STRING:
        path = tok.str();
        GET_CHECK_TOK(tok, lex, TOK_RWORD_AS);
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        name = tok.str();
        break;
    // `extern crate crate_name;`
    case TOK_IDENT:
        name = tok.str();
        if(GET_TOK(tok, lex) == TOK_RWORD_AS) {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            name = mv$(tok.str());
        }
        else {
            lex.putback(tok);
            name = path;
        }
        break;
    default:
        throw ParseError::Unexpected(lex, tok, {TOK_STRING, TOK_IDENT});
    }
    GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
    
    mod.add_ext_crate(path, name);

    // Handle #[macro_use]/#[macro_use(...)]
    //auto at = meta_items.get("macro_use");
    //if( at )
    //{
    //    if( at->has_sub_items() )
    //    {
    //        throw ParseError::Todo("selective macro_use");
    //    }
    //    else
    //    {
    //        mod.add_macro_import(crate, name, "");
    //    }
    //}
}

void Parse_Mod_Item(TokenStream& lex, LList<AST::Module*>& modstack, bool file_controls_dir, const ::std::string& file_path, AST::Module& mod, bool is_public, AST::MetaItems meta_items)
{
    //TRACE_FUNCTION;
    Token   tok;

    // The actual item!
    switch( GET_TOK(tok, lex) )
    {
    // `use ...`
    case TOK_RWORD_USE:
        Parse_Use(lex, [&mod,is_public,&file_path](AST::Path p, std::string s) {
                DEBUG(file_path << " - use " << p << " as '" << s << "'");
                mod.add_alias(is_public, mv$(p), s);
            });
        GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
        break;
    
    case TOK_RWORD_EXTERN:
        switch( GET_TOK(tok, lex) )
        {
        // `extern "<ABI>" fn ...`
        // `extern "<ABI>" { ...`
        case TOK_STRING: {
            ::std::string abi = tok.str();
            switch(GET_TOK(tok, lex))
            {
            // `extern "<ABI>" fn ...`
            case TOK_RWORD_FN:
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                mod.add_function(is_public, tok.str(), Parse_FunctionDefWithCode(lex, abi, ::std::move(meta_items), false));
                break;
            // `extern "<ABI>" { ...`
            case TOK_BRACE_OPEN:
                Parse_ExternBlock(lex, mod, ::std::move(abi));
                break;
            default:
                throw ParseError::Unexpected(lex, tok, {TOK_RWORD_FN, TOK_BRACE_OPEN});
            }
            break; }
        // `extern fn ...`
        case TOK_RWORD_FN:
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            mod.add_function(is_public, tok.str(), Parse_FunctionDefWithCode(lex, "C", ::std::move(meta_items), false));
            break;
        // `extern { ...`
        case TOK_BRACE_OPEN:
            Parse_ExternBlock(lex, mod, "C");
            break;
        // `extern crate "crate-name" as crate_name;`
        // `extern crate crate_name;`
        case TOK_RWORD_CRATE:
            Parse_ExternCrate(lex, mod, mv$(meta_items));
            break;
        default:
            throw ParseError::Unexpected(lex, tok, {TOK_STRING, TOK_RWORD_FN, TOK_BRACE_OPEN, TOK_RWORD_CRATE});
        }
        break;
    
    // `const NAME`
    // `const fn`
    case TOK_RWORD_CONST: {
        switch( GET_TOK(tok, lex) )
        {
        case TOK_IDENT: {
            ::std::string name = tok.str();

            GET_CHECK_TOK(tok, lex, TOK_COLON);
            TypeRef type = Parse_Type(lex);
            GET_CHECK_TOK(tok, lex, TOK_EQUAL);
            AST::Expr val = Parse_Expr(lex, true);
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            mod.add_static(is_public, name, AST::Static(::std::move(meta_items), AST::Static::CONST, type, val));
            break; }
        case TOK_RWORD_UNSAFE:
            GET_CHECK_TOK(tok, lex, TOK_RWORD_FN);
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            mod.add_function(is_public, tok.str(), Parse_FunctionDefWithCode(lex, "rust", ::std::move(meta_items), false));
            break;
        case TOK_RWORD_FN: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            // - self not allowed, not prototype
            // TODO: Mark as const
            mod.add_function(is_public, tok.str(), Parse_FunctionDefWithCode(lex, "rust", ::std::move(meta_items), false));
            break; }
        default:
            throw ParseError::Unexpected(lex, tok, {TOK_IDENT, TOK_RWORD_FN});
        }
        break; }
    // `static NAME`
    // `static mut NAME`
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

    // `unsafe fn`
    // `unsafe trait`
    // `unsafe impl`
    case TOK_RWORD_UNSAFE:
        meta_items.push_back( AST::MetaItem("#UNSAFE") );
        switch(GET_TOK(tok, lex))
        {
        // `unsafe extern fn`
        case TOK_RWORD_EXTERN: {
            ::std::string   abi = "C";
            if(GET_TOK(tok, lex) == TOK_STRING) {
                abi = mv$(tok.str());
            }
            else {
                lex.putback(tok);
            }
            GET_CHECK_TOK(tok, lex, TOK_RWORD_FN);
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            mod.add_function(is_public, tok.str(), Parse_FunctionDefWithCode(lex, abi, ::std::move(meta_items), false));
            break; }
        // `unsafe fn`
        case TOK_RWORD_FN:
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            // - self not allowed, not prototype
            // TODO: Mark as unsafe
            mod.add_function(is_public, tok.str(), Parse_FunctionDefWithCode(lex, "rust", ::std::move(meta_items), false));
            break;
        // `unsafe trait`
        case TOK_RWORD_TRAIT: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            ::std::string name = tok.str();
            // TODO: Mark as unsafe
            mod.add_trait(is_public, name, Parse_TraitDef(lex, meta_items));
            break; }
        // `unsafe impl`
        case TOK_RWORD_IMPL:
            Parse_Impl(lex, mod, mv$(meta_items), true);
            break;
        default:
            throw ParseError::Unexpected(lex, tok, {TOK_RWORD_FN, TOK_RWORD_TRAIT, TOK_RWORD_IMPL});
        }
        break;
    // `fn`
    case TOK_RWORD_FN: {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        ::std::string name = tok.str();
        // - self not allowed, not prototype
        mod.add_function(is_public, name, Parse_FunctionDefWithCode(lex, "rust", ::std::move(meta_items), false));
        break; }
    // `type`
    case TOK_RWORD_TYPE: {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        ::std::string name = tok.str();
        mod.add_typealias(is_public, name, Parse_TypeAlias(lex, meta_items));
        break; }
    // `struct`
    case TOK_RWORD_STRUCT: {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        ::std::string name = tok.str();
        mod.add_struct( is_public, name, Parse_Struct(lex, meta_items) );
        break; }
    // `enum`
    case TOK_RWORD_ENUM: {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        ::std::string name = tok.str();
        mod.add_enum(is_public, name, Parse_EnumDef(lex, meta_items));
        break; }
    // `impl`
    case TOK_RWORD_IMPL:
        Parse_Impl(lex, mod, mv$(meta_items));
        break;
    // `trait`
    case TOK_RWORD_TRAIT: {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        ::std::string name = tok.str();
        mod.add_trait(is_public, name, Parse_TraitDef(lex, meta_items));
        break; }

    case TOK_RWORD_MOD: {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        auto name = tok.str();
        DEBUG("Sub module '" << name << "'");
        AST::Module submod( mv$(meta_items), mv$(tok.str()));
        
        // Rules for external files (/ path handling):
        // - IF using stdin (path='-') - Disallow and propagate '-' as path
        // - IF a #[path] attribute was passed, allow
        // - IF in crate root or mod.rs, allow (input flag)
        // - else, disallow and set flag
        ::std::string path_attr = (submod.attrs().has("path") ? submod.attrs().get("path")->string() : "");
        
        ::std::string   sub_path;
        bool    sub_file_controls_dir = true;
        if( file_path == "-" ) {
            if( path_attr.size() ) {
                throw ParseError::Generic(lex, "Attempt to set path when reading stdin");
            }
            sub_path = "-";
        }
        else if( path_attr.size() > 0 )
        {
            sub_path = dirname(file_path) + path_attr;
        }
        else if( file_controls_dir )
        {
            sub_path = dirname(file_path) + name;
        }
        else
        {
            sub_path = file_path;
            sub_file_controls_dir = false;
        }
        DEBUG("Mod '" << name << "', sub_path = " << sub_path);
        
        switch( GET_TOK(tok, lex) )
        {
        case TOK_BRACE_OPEN: {
            Parse_ModRoot(lex, submod, &modstack, sub_file_controls_dir, sub_path+"/");
            GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);
            break; }
        case TOK_SEMICOLON:
            if( sub_path == "-" ) {
                throw ParseError::Generic(lex, "Cannot load module from file when reading stdin");
            }
            else if( path_attr.size() == 0 && ! file_controls_dir )
            {
                throw ParseError::Generic(lex, "Can't load from files outside of mod.rs or crate root");
            }
            else
            {
                ::std::string newpath_dir  = sub_path + "/";
                ::std::string newpath_file = path_attr.size() > 0 ? sub_path : sub_path + ".rs";
                ::std::ifstream ifs_dir (newpath_dir + "mod.rs");
                ::std::ifstream ifs_file(newpath_file);
                if( ifs_dir.is_open() && ifs_file.is_open() )
                {
                    // Collision
                    throw ParseError::Generic(lex, "Both modname.rs and modname/mod.rs exist");
                }
                else if( ifs_dir.is_open() )
                {
                    // Load from dir
                    Lexer sub_lex(newpath_dir + "mod.rs");
                    Parse_ModRoot(sub_lex, submod, &modstack, sub_file_controls_dir, newpath_dir);
                    GET_CHECK_TOK(tok, sub_lex, TOK_EOF);
                }
                else if( ifs_file.is_open() )
                {
                    // Load from file
                    Lexer sub_lex(newpath_file);
                    Parse_ModRoot(sub_lex, submod, &modstack, sub_file_controls_dir, newpath_file);
                    GET_CHECK_TOK(tok, sub_lex, TOK_EOF);
                }
                else
                {
                    // Can't find file
                    throw ParseError::Generic(lex, FMT("Can't find file for '" << name << "' in '" << file_path << "'") );
                }
            }
            break;
        default:
            throw ParseError::Generic("Expected { or ; after module name");
        }
        submod.prescan();
        mod.add_submod(is_public, ::std::move(submod));
        Macro_SetModule(modstack);
        break; }

    default:
        throw ParseError::Unexpected(lex, tok);
    }
}

void Parse_ModRoot_Items(TokenStream& lex, AST::Module& mod, LList<AST::Module*>& modstack, bool file_controls_dir, const ::std::string& path)
{
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

        // root-level macros
        if( GET_TOK(tok, lex) == TOK_MACRO )
        {
            ::std::string   name = mv$(tok.str());
            // `macro_rules! ...`
            //if( name == "macro_rules" )
            //{
            //    Parse_MacroRules(lex, mod, mv$(meta_items));
            //}
            //else
            //{
                mod.add_macro_invocation( Parse_MacroInvocation( mv$(meta_items), mv$(name), lex ) );
            //}
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
        if( GET_TOK(tok, lex) == TOK_RWORD_PUB ) {
            is_public = true;
        }
        else {
            lex.putback(tok);
        }

        Parse_Mod_Item(lex, modstack, file_controls_dir,path,  mod, is_public, mv$(meta_items));
    }
}

void Parse_ModRoot(TokenStream& lex, AST::Module& mod, LList<AST::Module*> *prev_modstack, bool file_controls_dir, const ::std::string& path)
{
    TRACE_FUNCTION;
    LList<AST::Module*>  modstack(prev_modstack, &mod);
    Macro_SetModule(modstack);

    Token   tok;

    // Attributes on module/crate (will continue loop)
    while( GET_TOK(tok, lex) == TOK_CATTR_OPEN )
    {
        AST::MetaItem item = Parse_MetaItem(lex);
        GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);

        mod.add_attr( item );
    }
    lex.putback(tok);

    // TODO: Iterate attributes, and check for handlers on each
    
    Parse_ModRoot_Items(lex, mod, modstack, file_controls_dir, path);
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
            // TODO: Load core instead
        }
        else if( attr.name() == "no_core" ) {
            crate.m_load_std = false;
        }
        else {
            // TODO:
        }
    }
    
    Parse_ModRoot(lex, rootmod, NULL, true, mainpath);
    
    return crate;
}
