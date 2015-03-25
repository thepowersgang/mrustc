#ifndef PARSE_COMMON_HPP_INCLUDED
#define PARSE_COMMON_HPP_INCLUDED
#include <iostream>
#include "../ast/ast.hpp"

#define GET_TOK(tok, lex) ((tok = lex.getToken()).type())
#define LOOK_AHEAD(lex) (lex.lookahead(0))
#define GET_CHECK_TOK(tok, lex, exp) do {\
    if((tok = lex.getToken()).type() != exp) { \
        DEBUG("GET_CHECK_TOK " << __FILE__ << ":" << __LINE__); \
        throw ParseError::Unexpected(lex, tok, Token(exp));\
    }\
} while(0)
#define CHECK_TOK(tok, exp) do {\
    if(tok.type() != exp) { \
        DEBUG("CHECK_TOK " << __FILE__ << ":" << __LINE__); \
        throw ParseError::Unexpected(lex, tok, Token(exp));\
    } \
} while(0)

enum eParsePathGenericMode
{
    PATH_GENERIC_NONE,
    PATH_GENERIC_EXPR,
    PATH_GENERIC_TYPE
};

extern AST::MetaItem   Parse_MetaItem(TokenStream& lex);
extern AST::Path   Parse_Path(TokenStream& lex, eParsePathGenericMode generic_mode);    // Auto-determines
extern AST::Path   Parse_Path(TokenStream& lex, bool is_abs, eParsePathGenericMode generic_mode);
extern AST::Path   Parse_PathFrom(TokenStream& lex, AST::Path src, eParsePathGenericMode generic_mode);
extern ::std::vector<TypeRef>   Parse_Path_GenericList(TokenStream& lex);

extern TypeRef     Parse_Type(TokenStream& lex);


extern AST::Pattern Parse_Pattern(TokenStream& lex, bool is_refutable);

extern void Parse_Use(TokenStream& lex, ::std::function<void(AST::Path, ::std::string)> fcn);
extern void Parse_Struct(AST::Module& mod, TokenStream& lex, bool is_public, const AST::MetaItems meta_items);
extern AST::Impl Parse_Impl(TokenStream& lex, bool is_unsafe=false);
extern void Parse_MacroRules(TokenStream& lex, AST::Module& mod, AST::MetaItems meta_items);

extern AST::Function    Parse_FunctionDef(TokenStream& lex, ::std::string abi, AST::MetaItems attrs, bool allow_self, bool can_be_prototype);
extern AST::Function    Parse_FunctionDefWithCode(TokenStream& lex, ::std::string abi, AST::MetaItems attrs, bool allow_self);
extern AST::Expr   Parse_Expr(TokenStream& lex, bool const_only);
extern AST::Expr   Parse_ExprBlock(TokenStream& lex);

extern bool Parse_IsTokValue(eTokenType tok_type);

#endif // PARSE_COMMON_HPP_INCLUDED
