/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/file_line.cpp
 * - file! line! and macro_path! macros
 */
#include <synext.hpp>
#include "../parse/common.hpp"
#include "../parse/ttstream.hpp"

class CExpanderFile:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(TokenTree(Token(TOK_STRING, sp.filename.c_str()))) );
    }
};

class CExpanderLine:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(TokenTree(Token((uint64_t)sp.start_line, CORETYPE_U32))) );
    }
};

class CExpanderModulePath:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        ::std::string   path_str;
        for(const auto& comp : mod.path().nodes()) {
            if( &comp != &mod.path().nodes().front() )
                path_str += "::";
            path_str += comp.name();
        }
        return box$( TTStreamO(TokenTree( Token(TOK_STRING, mv$(path_str)) )) );
    }
};

STATIC_MACRO("file", CExpanderFile);
STATIC_MACRO("line", CExpanderLine);
STATIC_MACRO("module_path", CExpanderModulePath);

