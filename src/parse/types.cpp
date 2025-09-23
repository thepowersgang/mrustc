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
TypeRef Parse_Type_Fn(TokenStream& lex, AST::HigherRankedBounds hrbs = {});
TypeRef Parse_Type_Path(TokenStream& lex, AST::HigherRankedBounds hrbs, bool allow_trait_list);
TypeRef Parse_Type_TraitObject(TokenStream& lex, ::AST::HigherRankedBounds hrbs = {});
TypeRef Parse_Type_ErasedType(TokenStream& lex, bool allow_trait_list);

// === CODE ===
TypeRef Parse_Type(TokenStream& lex, bool allow_trait_list)
{
    //ProtoSpan ps = lex.start_span();
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
    // '!' - Only ever used as part of function prototypes, but is kinda a type... not allowed here though
    case TOK_EXCLAM:
        return TypeRef(lex.sub_span(tok.get_pos()), TypeData::make_Bang({}) );
    // '_' = Wildcard (type inferrence variable)
    case TOK_UNDERSCORE:
        return TypeRef(lex.sub_span(tok.get_pos()));

    // 'unsafe' - An unsafe function type
    case TOK_RWORD_UNSAFE:
    // 'extern' - A function type with an ABI
    case TOK_RWORD_EXTERN:
    // 'fn' - Rust function
    case TOK_RWORD_FN:
        PUTBACK(tok, lex);
        return Parse_Type_Fn(lex);

    case TOK_RWORD_IMPL:
        return Parse_Type_ErasedType(lex, allow_trait_list);

    // '<' - An associated type cast
    case TOK_LT:
    case TOK_THINARROW_LEFT:
    case TOK_DOUBLE_LT: {
        PUTBACK(tok, lex);
        auto path = Parse_Path(lex, PATH_GENERIC_TYPE);
        return TypeRef(TypeRef::TagPath(), lex.end_span(ps), mv$(path));
        }
    //
    case TOK_RWORD_FOR: {
        auto hrls = Parse_HRB(lex);
        switch(LOOK_AHEAD(lex))
        {
        case TOK_RWORD_UNSAFE:
        case TOK_RWORD_EXTERN:
        case TOK_RWORD_FN:
            return Parse_Type_Fn(lex, hrls);
        default:
            return Parse_Type_Path(lex, hrls, true);
        }
        }
    case TOK_RWORD_DYN: {
        ::AST::HigherRankedBounds hrbs = Parse_HRB_Opt(lex);
        return Parse_Type_TraitObject(lex, mv$(hrbs));
        }
    // <ident> - Either a primitive, or a path
    case TOK_IDENT:
        // TODO: Only allow if the next token isn't `::` or `!`
        if( TARGETVER_LEAST_1_29 && tok.ident().name == "dyn" )
        {
            ::AST::HigherRankedBounds hrbs = Parse_HRB_Opt(lex);
            return Parse_Type_TraitObject(lex, mv$(hrbs));
        }
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
    // 'self' - This relative path
    case TOK_RWORD_SELF:
    // 'super' - Parent relative path
    case TOK_RWORD_SUPER:
    // 'crate' - Crate-relative path
    case TOK_RWORD_CRATE:
    // ':path' fragment
    case TOK_INTERPOLATED_PATH:
        PUTBACK(tok, lex);
        return Parse_Type_Path(lex, {}, allow_trait_list);

    // HACK! Convert && into & &
    case TOK_DOUBLE_AMP:
        lex.putback(Token(TOK_AMP));
    // '&' - Reference type
    case TOK_AMP: {
        AST::LifetimeRef lifetime;
        // Reference
        tok = lex.getToken();
        if( tok.type() == TOK_LIFETIME ) {
            lifetime = AST::LifetimeRef(/*lex.point_span(), */tok.ident());
            tok = lex.getToken();
        }
        bool is_mut = false;
        if( tok.type() == TOK_RWORD_MUT ) {
            is_mut = true;
        }
        else {
            PUTBACK(tok, lex);
        }
        return TypeRef(TypeRef::TagReference(), lex.end_span(ps), ::std::move(lifetime), is_mut, Parse_Type(lex, false));
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
            // Inferred size - unspecified
            if( lex.getTokenIf(TOK_UNDERSCORE) ) {
                GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
                return TypeRef(TypeRef::TagSizedArray(), lex.end_span(ps), mv$(inner), nullptr);
            }
            else {
                // Sized array
                AST::Expr array_size = Parse_Expr(lex);
                GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
                return TypeRef(TypeRef::TagSizedArray(), lex.end_span(ps), mv$(inner), array_size.take_node());
            }
        }
        else if( tok.type() == TOK_SQUARE_CLOSE )
        {
            return TypeRef(TypeRef::TagUnsizedArray(), lex.end_span(ps), mv$(inner));
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
                types.push_back( Parse_Type(lex) );
            }
            CHECK_TOK(tok, TOK_PAREN_CLOSE);
            return TypeRef(TypeRef::TagTuple(), lex.end_span(ps), mv$(types));
        }
        }
    default:
        throw ParseError::Unexpected(lex, tok);
    }
    throw ParseError::BugCheck("Reached end of Parse_Type");
}

TypeRef Parse_Type_Fn(TokenStream& lex, ::AST::HigherRankedBounds hrbs)
{
    auto ps = lex.start_span();
    TRACE_FUNCTION;
    Token   tok;

    ::std::string   abi = "";
    bool    is_unsafe = false;

    GET_TOK(tok, lex);

    // `unsafe`
    if( tok.type() == TOK_RWORD_UNSAFE )
    {
        is_unsafe = true;
        GET_TOK(tok, lex);
    }
    // `exern`
    if( tok.type() == TOK_RWORD_EXTERN )
    {
        if( GET_TOK(tok, lex) == TOK_STRING ) {
            abi = tok.str();
            if( abi == "" )
                ERROR(lex.point_span(), E0000, "Empty ABI");
            GET_TOK(tok, lex);
        }
        else {
            abi = "C";
        }
    }
    // `fn`
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
        if( (lex.lookahead(0) == TOK_IDENT || lex.lookahead(0) == TOK_UNDERSCORE) && lex.lookahead(1) == TOK_COLON ) {
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

    // `-> RetType`
    TypeRef ret_type = TypeRef(TypeRef::TagUnit(), lex.point_span());
    if( GET_TOK(tok, lex) == TOK_THINARROW )
    {
        ret_type = Parse_Type(lex, false);
    }
    else {
        PUTBACK(tok, lex);
    }

    return TypeRef(TypeRef::TagFunction(), lex.end_span(ps), mv$(hrbs), is_unsafe, mv$(abi), mv$(args), is_variadic, mv$(ret_type));
}

TypeRef Parse_Type_Path(TokenStream& lex, ::AST::HigherRankedBounds hrbs, bool allow_trait_list)
{
    Token   tok;

    auto ps = lex.start_span();

    auto path = Parse_Path(lex, PATH_GENERIC_TYPE);
    if( lex.lookahead(0) == TOK_EXCLAM )
    {
        GET_CHECK_TOK(tok, lex, TOK_EXCLAM);
        return TypeRef(TypeRef::TagMacro(), Parse_MacroInvocation(ps, path, lex));
    }
    else if( hrbs.empty() && !allow_trait_list )
    {
        return TypeRef(TypeRef::TagPath(), lex.end_span(ps), mv$(path));
    }
    else
    {
        ::std::vector<Type_TraitPath>   traits;
        ::std::vector<AST::LifetimeRef> lifetimes;

        traits.push_back(Type_TraitPath { mv$(hrbs), mv$(path) });

        if( allow_trait_list )
        {
            while( GET_TOK(tok, lex) == TOK_PLUS )
            {
                if( LOOK_AHEAD(lex) == TOK_LIFETIME ) {
                    GET_TOK(tok, lex);
                    lifetimes.push_back(AST::LifetimeRef( /*lex.point_span(),*/ tok.ident() ));
                }
                else
                {
                    if( lex.lookahead(0) == TOK_RWORD_FOR )
                    {
                        hrbs = Parse_HRB(lex);
                    }
                    traits.push_back({ mv$(hrbs), Parse_Path(lex, PATH_GENERIC_TYPE) });
                }
            }
            PUTBACK(tok, lex);
        }

        if( !traits[0].hrbs.empty() || traits.size() > 1 || lifetimes.size() > 0 )
        {
            if( lifetimes.empty())
                lifetimes.push_back(AST::LifetimeRef());
            return TypeRef(lex.end_span(ps), mv$(traits), mv$(lifetimes));
        }
        else
        {
            return TypeRef(TypeRef::TagPath(), lex.end_span(ps), mv$(*traits.at(0).path));
        }
    }
}
TypeRef Parse_Type_TraitObject(TokenStream& lex, ::AST::HigherRankedBounds hrbs)
{
    Token   tok;
    auto ps = lex.start_span();

    ::std::vector<Type_TraitPath>   traits;
    ::std::vector<AST::LifetimeRef> lifetimes;

    for( ;; )
    {
        bool is_first = traits.empty() && lifetimes.empty();
        if( LOOK_AHEAD(lex) == TOK_LIFETIME ) {
            GET_TOK(tok, lex);

            if( is_first && !hrbs.empty() ) {
                // TODO: Error
            }

            lifetimes.push_back(AST::LifetimeRef( /*lex.point_span(),*/ tok.ident() ));
        }
        else
        {
            if( lex.getTokenIf(TOK_RWORD_FOR) ) {
                hrbs = Parse_HRB(lex);
            }
            else {
            }

            bool is_paren = lex.getTokenIf(TOK_PAREN_OPEN);

            traits.push_back({ mv$(hrbs), Parse_Path(lex, PATH_GENERIC_TYPE) });

            if( is_paren ) {
                GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
            }
        }

        if( !lex.getTokenIf(TOK_PLUS) )
            break;
    }

    if( lifetimes.empty() ) {
        lifetimes.push_back(AST::LifetimeRef());
    }
    return TypeRef(lex.end_span(ps), mv$(traits), mv$(lifetimes));
}
TypeRef Parse_Type_ErasedType(TokenStream& lex, bool allow_trait_list)
{
    Token   tok;

    auto ps = lex.start_span();
    TypeData::Data_ErasedType   rv_data;
    do {
        if( LOOK_AHEAD(lex) == TOK_LIFETIME ) {
            GET_TOK(tok, lex);
            rv_data.lifetimes.push_back(AST::LifetimeRef( /*lex.point_span(),*/ tok.ident() ));
        }
        else if( LOOK_AHEAD(lex) == TOK_QMARK ) {
            GET_TOK(tok, lex);
            AST::HigherRankedBounds hrbs = Parse_HRB_Opt(lex);
            rv_data.maybe_traits.push_back({ mv$(hrbs), Parse_Path(lex, PATH_GENERIC_TYPE) });
        }
        else if( lex.getTokenIf(TOK_PAREN_OPEN) )
        {
            AST::HigherRankedBounds hrbs = Parse_HRB_Opt(lex);
            rv_data.traits.push_back({ mv$(hrbs), Parse_Path(lex, PATH_GENERIC_TYPE) });
            lex.getTokenCheck(TOK_PAREN_CLOSE);
        }
        else
        {
            if( lex.getTokenIf(TOK_TILDE) ) {
                GET_CHECK_TOK(tok, lex, TOK_RWORD_CONST);
            }
            else if( lex.getTokenIf(TOK_RWORD_CONST) ) {
            }
            AST::HigherRankedBounds hrbs = Parse_HRB_Opt(lex);
            rv_data.traits.push_back({ mv$(hrbs), Parse_Path(lex, PATH_GENERIC_TYPE) });
        }
    } while( GET_TOK(tok, lex) == TOK_PLUS );
    PUTBACK(tok, lex);

    return TypeRef(lex.end_span(ps), mv$(rv_data));
}

