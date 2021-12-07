/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/paths.cpp
 * - Parsing for module paths
 */
#include "parseerror.hpp"
#include "common.hpp"
#include "../ast/ast.hpp"

AST::Path   Parse_Path(TokenStream& lex, eParsePathGenericMode generic_mode);
AST::Path   Parse_Path(TokenStream& lex, bool is_abs, eParsePathGenericMode generic_mode);
::std::vector<AST::PathNode> Parse_PathNodes(TokenStream& lex, eParsePathGenericMode generic_mode);
AST::PathParams Parse_Path_GenericList(TokenStream& lex);

AST::Path Parse_Path(TokenStream& lex, eParsePathGenericMode generic_mode)
{
    TRACE_FUNCTION_F("generic_mode="<<generic_mode);

    Token   tok;
    switch( GET_TOK(tok, lex) )
    {
    case TOK_INTERPOLATED_PATH:
        return mv$(tok.frag_path());

    case TOK_RWORD_SELF:
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        return AST::Path::new_self(Parse_PathNodes(lex, generic_mode));

    case TOK_RWORD_SUPER: {
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        unsigned int count = 1;
        while( LOOK_AHEAD(lex) == TOK_RWORD_SUPER ) {
            count += 1;
            GET_TOK(tok, lex);
            if( lex.lookahead(0) != TOK_DOUBLE_COLON )
                return AST::Path::new_super(count, {});
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        }
        return AST::Path::new_super(count, Parse_PathNodes(lex, generic_mode));
        }

    case TOK_RWORD_CRATE:
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        return Parse_Path(lex, true, generic_mode);
    case TOK_DOUBLE_COLON:
        if( lex.lookahead(0) == TOK_STRING )
        {
        }
        // QUIRK: `::crate::foo` is valid (semi-surprisingly)
        // TODO: Reference?
        else if( lex.lookahead(0) == TOK_RWORD_CRATE )
        {
        }
        else if( lex.edition_after(AST::Edition::Rust2018) )
        {
            // The first component is a crate name
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            // HACK: if the crate name starts with `=` it's a 2018 absolute path (references a crate loaded with `--extern`)
            auto crate_name = RcString(std::string("=") + tok.ident().name.c_str());
            std::vector<AST::PathNode>  nodes;
            if(lex.lookahead(0) == TOK_DOUBLE_COLON)
            {
                GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
                nodes = Parse_PathNodes(lex, generic_mode);
            }
            return AST::Path(crate_name, ::std::move(nodes));
        }
        return Parse_Path(lex, true, generic_mode);

    case TOK_DOUBLE_LT:
        lex.putback( Token(TOK_LT) );
    case TOK_LT: {
        TypeRef ty = Parse_Type(lex, true);  // Allow trait objects without parens
        if( GET_TOK(tok, lex) == TOK_RWORD_AS ) {
            ::AST::Path trait = Parse_Path(lex, PATH_GENERIC_TYPE);
            GET_CHECK_TOK(tok, lex, TOK_GT);
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
            return AST::Path::new_ufcs_trait(mv$(ty), mv$(trait), Parse_PathNodes(lex, generic_mode));
        }
        else {
            PUTBACK(tok, lex);
            GET_CHECK_TOK(tok, lex, TOK_GT);
            // TODO: Terminating the "path" here is sometimes valid?
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
            // NOTE: <Foo>::BAR is actually `<Foo as _>::BAR` (in mrustc parleance)
            //return AST::Path(AST::Path::TagUfcs(), mv$(ty), Parse_PathNodes(lex, generic_mode));
            return AST::Path::new_ufcs_ty(mv$(ty), Parse_PathNodes(lex, generic_mode));
        }
        throw ""; }

    default:
        PUTBACK(tok, lex);
        return Parse_Path(lex, false, generic_mode);
    }
}
AST::Path Parse_Path(TokenStream& lex, bool is_abs, eParsePathGenericMode generic_mode)
{
    Token   tok;
    if( is_abs )
    {
        // QUIRK: `::crate::foo` is valid (semi-surprisingly)
        if( LOOK_AHEAD(lex) == TOK_RWORD_CRATE ) {
            GET_CHECK_TOK(tok, lex, TOK_RWORD_CRATE);
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
            return AST::Path("", Parse_PathNodes(lex, generic_mode));
        }
        else if( GET_TOK(tok, lex) == TOK_STRING ) {
            auto cratename = RcString::new_interned(tok.str());
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
            return AST::Path(cratename, Parse_PathNodes(lex, generic_mode));
        }
        else {
            PUTBACK(tok, lex);
            return AST::Path("", Parse_PathNodes(lex, generic_mode));
        }
    }
    else {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        auto hygine = tok.ident().hygiene;
        DEBUG("hygine = " << hygine);
        PUTBACK(tok, lex);
        return AST::Path::new_relative(mv$(hygine), Parse_PathNodes(lex, generic_mode));
    }
}

::std::vector<AST::PathNode> Parse_PathNodes(TokenStream& lex, eParsePathGenericMode generic_mode)
{
    TRACE_FUNCTION_F("generic_mode="<<generic_mode);

    Token tok;
    ::std::vector<AST::PathNode>    ret;

    while(true)
    {
        ::AST::PathParams   params;

        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        auto component = mv$( tok.ident().name );

        if( generic_mode == PATH_GENERIC_TYPE )
        {
            // If `foo::<` is seen in type context, then consume the `::` and continue on.
            if( lex.lookahead(0) == TOK_DOUBLE_COLON && (lex.lookahead(1) == TOK_LT || lex.lookahead(1) == TOK_DOUBLE_LT) )
            {
                GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
            }
            if( lex.lookahead(0) == TOK_LT || lex.lookahead(0) == TOK_DOUBLE_LT )
            {
                GET_TOK(tok, lex);
                // HACK! Handle breaking << into < <
                if( tok.type() == TOK_DOUBLE_LT )
                    lex.putback( Token(TOK_LT) );

                // Type-mode generics "::path::to::Type<A,B>"
                params = Parse_Path_GenericList(lex);
            }
            // HACK - 'Fn*(...) -> ...' notation
            else if( lex.lookahead(0) == TOK_PAREN_OPEN )
            {
                auto ps = lex.start_span();
                DEBUG("Fn() hack");
                ::std::vector<TypeRef>  args;
                GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
                do {
                    // Trailing comma or empty list support
                    if( lex.lookahead(0) == TOK_PAREN_CLOSE ) {
                        GET_TOK(tok, lex);
                        break;
                    }
                    args.push_back( Parse_Type(lex) );
                } while( GET_TOK(tok, lex) == TOK_COMMA );
                CHECK_TOK(tok, TOK_PAREN_CLOSE);

                TypeRef ret_type = TypeRef( TypeRef::TagUnit(), lex.point_span() );
                if( lex.lookahead(0) == TOK_THINARROW ) {
                    GET_TOK(tok, lex);
                    ret_type = Parse_Type(lex, false);
                }
                DEBUG("- Fn("<<args<<")->"<<ret_type<<"");

                // Encode into path, by converting Fn(A,B)->C into Fn<(A,B),Ret=C>
                params = ::AST::PathParams();
                params.m_entries.push_back( TypeRef(TypeRef::TagTuple(), lex.end_span(ps), mv$(args)) );
                params.m_entries.push_back( ::std::make_pair( RcString::new_interned("Output"), mv$(ret_type) ) );
            }
            else
            {
            }
        }
        if( lex.lookahead(0) != TOK_DOUBLE_COLON ) {
            ret.push_back( AST::PathNode(component, mv$(params)) );
            break;
        }
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        if( generic_mode == PATH_GENERIC_EXPR && (lex.lookahead(0) == TOK_LT || lex.lookahead(0) == TOK_DOUBLE_LT) )
        {
            GET_TOK(tok, lex);
            // HACK! Handle breaking << into < <
            if( tok.type() == TOK_DOUBLE_LT )
                lex.putback( Token(TOK_LT) );

            // Expr-mode generics "::path::to::function::<Type1,Type2>(arg1, arg2)"
            params = Parse_Path_GenericList(lex);
            if( lex.lookahead(0) != TOK_DOUBLE_COLON ) {
                ret.push_back( AST::PathNode(component, mv$(params)) );
                // Break out of loop down to return
                break;
            }
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        }
        ret.push_back( AST::PathNode(component, mv$(params)) );
    }
    DEBUG("ret = " << ret);
    return ret;
}
/// Parse a list of parameters within a path
::AST::PathParams Parse_Path_GenericList(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token   tok;

    ::AST::PathParams   rv;

    do {
        if( LOOK_AHEAD(lex) == TOK_GT || LOOK_AHEAD(lex) == TOK_DOUBLE_GT || LOOK_AHEAD(lex) == TOK_GTE || LOOK_AHEAD(lex) == TOK_DOUBLE_GT_EQUAL ) {
            GET_TOK(tok, lex);
            break;
        }
        switch(GET_TOK(tok, lex))
        {
        case TOK_LIFETIME:
            rv.m_entries.push_back(AST::LifetimeRef(/*lex.point_span(),*/ tok.ident()));
            break;
        case TOK_RWORD_TRUE:
        case TOK_RWORD_FALSE:
        case TOK_INTEGER:
        case TOK_FLOAT:
        case TOK_INTERPOLATED_EXPR:
        case TOK_BRACE_OPEN:
            PUTBACK(tok, lex);
            rv.m_entries.push_back( Parse_ExprVal(lex) );
            break;
        case TOK_IDENT:
            if( LOOK_AHEAD(lex) == TOK_EQUAL )
            {
                auto name = tok.ident().name;
                GET_CHECK_TOK(tok, lex, TOK_EQUAL);
                rv.m_entries.push_back( ::std::make_pair( mv$(name), Parse_Type(lex,false) ) );
                break;
            }
            if( LOOK_AHEAD(lex) == TOK_COLON )
            {
                auto name = tok.ident().name;
                GET_CHECK_TOK(tok, lex, TOK_COLON);
                // TODO: Trait list instead of duplicating the name
                do {
                    rv.m_entries.push_back( ::std::make_pair( name, Parse_Path(lex, PATH_GENERIC_TYPE) ) );
                    if(lex.lookahead(0) != TOK_PLUS)
                        break;
                    GET_CHECK_TOK(tok, lex, TOK_PLUS);
                } while(true);
                break;
            }
        default:
            PUTBACK(tok, lex);
            rv.m_entries.push_back( Parse_Type(lex) );
            break;
        }
    } while( GET_TOK(tok, lex) == TOK_COMMA );

    // HACK: Split >> into >
    if(tok.type() == TOK_DOUBLE_GT_EQUAL) {
        lex.putback(Token(TOK_GTE));
    }
    else if(tok.type() == TOK_GTE) {
        lex.putback(Token(TOK_EQUAL));
    }
    else if(tok.type() == TOK_DOUBLE_GT) {
        lex.putback(Token(TOK_GT));
    }
    else {
        CHECK_TOK(tok, TOK_GT);
    }

    return rv;
}

