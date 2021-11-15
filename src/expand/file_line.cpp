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
#include <ast/crate.hpp>

namespace {
    const Span& get_top_span(const Span& sp) {
        auto* top_span = &sp;
        while((*top_span)->parent_span != Span())
        {
            top_span = &(*top_span)->parent_span;
        }
        return *top_span;
    }
}

class CExpanderFile:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(sp, ParseState(), TokenTree(Token(TOK_STRING, ::std::string(get_top_span(sp)->filename.c_str())))) );
    }
};

class CExpanderLine:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(sp, ParseState(), TokenTree(Token((uint64_t)get_top_span(sp)->start_line, CORETYPE_U32))) );
    }
};

class CExpanderColumn:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(sp, ParseState(), TokenTree(Token((uint64_t)get_top_span(sp)->start_ofs, CORETYPE_U32))) );
    }
};
class CExpanderUnstableColumn:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        return box$( TTStreamO(sp, ParseState(), TokenTree(Token((uint64_t)get_top_span(sp)->start_ofs, CORETYPE_U32))) );
    }
};

class CExpanderModulePath:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        ::std::string   path_str;
        path_str += crate.m_crate_name_set;
        for(const auto& comp : mod.path().nodes) {
            path_str += "::";
            path_str += comp.c_str();
        }
        return box$( TTStreamO(sp, ParseState(), TokenTree( Token(TOK_STRING, mv$(path_str)) )) );
    }
};

STATIC_MACRO("file", CExpanderFile);
STATIC_MACRO("line", CExpanderLine);
STATIC_MACRO("column", CExpanderColumn);
STATIC_MACRO("__rust_unstable_column", CExpanderUnstableColumn);
STATIC_MACRO("module_path", CExpanderModulePath);

