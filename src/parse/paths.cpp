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
AST::Path   Parse_PathFrom(TokenStream& lex, AST::Path path, eParsePathGenericMode generic_mode);
::std::vector<TypeRef>  Parse_Path_GenericList(TokenStream& lex);

AST::Path Parse_Path(TokenStream& lex, eParsePathGenericMode generic_mode)
{
    Token   tok;
    switch( GET_TOK(tok, lex) )
    {
    case TOK_DOUBLE_COLON:
        return Parse_Path(lex, true, generic_mode);
    case TOK_LT: {
        TypeRef ty = Parse_Type(lex);
        TypeRef trait;
        if( GET_TOK(tok, lex) == TOK_RWORD_AS ) {
            trait = Parse_Type(lex);
        }
        else
            lex.putback(tok);
        GET_CHECK_TOK(tok, lex, TOK_GT);
        // TODO: Terminating the "path" here is sometimes valid
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        return Parse_PathFrom(lex, AST::Path(AST::Path::TagUfcs(), ty, trait), PATH_GENERIC_EXPR);
        }
    default:
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
        return Parse_PathFrom(lex, AST::Path(AST::Path::TagRelative()), generic_mode);
}

AST::Path Parse_PathFrom(TokenStream& lex, AST::Path path, eParsePathGenericMode generic_mode)
{
    TRACE_FUNCTION_F("path = " << path);
    
    Token tok;

    tok = lex.getToken();
    while(true)
    {
        ::std::vector<TypeRef>  params;

        CHECK_TOK(tok, TOK_IDENT);
        ::std::string component = tok.str();

        GET_TOK(tok, lex);
        if( generic_mode == PATH_GENERIC_TYPE )
        {
            if( tok.type() == TOK_LT || tok.type() == TOK_DOUBLE_LT )
            {
                // HACK! Handle breaking << into < <
                if( tok.type() == TOK_DOUBLE_LT )
                    lex.putback( Token(TOK_LT) );
                
                // Type-mode generics "::path::to::Type<A,B>"
                params = Parse_Path_GenericList(lex);
                tok = lex.getToken();
            }
            // HACK - 'Fn*(...) -> ...' notation
            else if( tok.type() == TOK_PAREN_OPEN )
            {
                DEBUG("Fn() hack");
                ::std::vector<TypeRef>  args;
                if( GET_TOK(tok, lex) == TOK_PAREN_CLOSE )
                {
                    // Empty list
                }
                else
                {
                    lex.putback(tok);
                    do {
                        args.push_back( Parse_Type(lex) );
                    } while( GET_TOK(tok, lex) == TOK_COMMA );
                }
                CHECK_TOK(tok, TOK_PAREN_CLOSE);
                
                TypeRef ret_type = TypeRef( TypeRef::TagUnit() );
                if( GET_TOK(tok, lex) == TOK_THINARROW ) {
                    ret_type = Parse_Type(lex);
                }
                else {
                    lex.putback(tok);
                }
                DEBUG("- Fn("<<args<<")->"<<ret_type<<"");
                
                // Encode into path, by converting Fn(A,B)->C into Fn<(A,B),Ret=C>
                params = ::std::vector<TypeRef> { TypeRef(TypeRef::TagTuple(), ::std::move(args)) };
                // TODO: Use 'ret_type' as an associated type bound
                
                GET_TOK(tok, lex);
            }
            else
            {
            }
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
    if( path.is_trivial() ) {
        path = AST::Path(path[0].name());
    }
    DEBUG("path = " << path);
    return path;
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

