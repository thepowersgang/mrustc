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

namespace {
    const Span& get_top_span(const Span& sp) {
        if( sp.outer_span ) {
            return get_top_span(*sp.outer_span);
        }
        else {
            return sp;
        }
    }
}

class CExpanderFile:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(sp, TokenTree(Token(TOK_STRING, get_top_span(sp).filename.c_str()))) );
    }
};

class CExpanderLine:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(sp, TokenTree(Token((uint64_t)get_top_span(sp).start_line, CORETYPE_U32))) );
    }
};

class CExpanderColumn:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(sp, TokenTree(Token((uint64_t)get_top_span(sp).start_ofs, CORETYPE_U32))) );
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
        return box$( TTStreamO(sp, TokenTree( Token(TOK_STRING, mv$(path_str)) )) );
    }
};

STATIC_MACRO("file", CExpanderFile);
STATIC_MACRO("line", CExpanderLine);
STATIC_MACRO("column", CExpanderColumn);
STATIC_MACRO("module_path", CExpanderModulePath);

