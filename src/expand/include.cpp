/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/include.cpp
 * - include!/include_str!/include_bytes! support
 */
#include <synext_macro.hpp>
#include <synext.hpp>   // for Expand_BareExpr
#include <parse/common.hpp>
#include <parse/parseerror.hpp> // for GET_CHECK_TOK
#include <parse/ttstream.hpp>
#include <parse/lex.hpp>    // Lexer (new files)
#include <ast/expr.hpp>
#include <ast/crate.hpp>

namespace {

    ::std::string get_string(const Span& sp, TokenStream& lex, const ::AST::Crate& crate, AST::Module& mod)
    {
        auto n = Parse_ExprVal(lex);
        ASSERT_BUG(sp, n, "No expression returned");
        Expand_BareExpr(crate, mod, n);

        auto* string_np = dynamic_cast<AST::ExprNode_String*>(&*n);
        if( !string_np ) {
            ERROR(sp, E0000, "include! requires a string literal - got " << *n);
        }
        return mv$( string_np->m_value );
    }

    ::std::string get_path_relative_to(const ::std::string& base_path, ::std::string path)
    {
        DEBUG(base_path << ", " << path);
        // Absolute
        if( path[0] == '/' || path[0] == '\\' ) {
            return path;
        }
        // Windows absolute
        else if( isalnum(path[0]) && path[1] == ':' && (path[2] == '/' || path[2] == '\\') ) {
            return path;
        }
        else if( base_path.size() == 0 ) {
            return path;
        }
        else if( base_path.back() == '/' || base_path.back() == '\\' ) {
            return base_path + path;
        }
        else {

            auto slash_fwd = base_path.find_last_of('/');
            auto slash_back = base_path.find_last_of('\\');
            auto slash =
                slash_fwd == std::string::npos ? slash_back
                : slash_back == std::string::npos ? slash_fwd
                : std::max(slash_fwd, slash_back)
                ;
            if( slash == ::std::string::npos )
            {
                return path;
            }
            else
            {
                DEBUG("> slash = " << slash);
                slash += 1;
                ::std::string   rv;
                rv.reserve( slash + path.size() );
                rv.append( base_path.begin(), base_path.begin() + slash );
                rv.append( path.begin(), path.end() );
                return rv;
            }
        }
    }
};

class CIncludeExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;
        auto lex = TTStream(sp, ParseState(), tt);

        auto path = get_string(sp, lex, crate, mod);
        GET_CHECK_TOK(tok, lex, TOK_EOF);

        ::std::string file_path = get_path_relative_to(mod.m_file_info.path, mv$(path));
        crate.m_extra_files.push_back(file_path);

        try {
            ParseState  ps;
            ps.module = &mod;
            DEBUG("Edition = " << crate.m_edition);
            return box$( Lexer(file_path, crate.m_edition, ps) );
        }
        catch(::std::runtime_error& e)
        {
            ERROR(sp, E0000, e.what());
        }
    }
};

class CIncludeBytesExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;
        auto lex = TTStream(sp, ParseState(), tt);

        auto path = get_string(sp, lex, crate, mod);
        GET_CHECK_TOK(tok, lex, TOK_EOF);

        ::std::string file_path = get_path_relative_to(mod.m_file_info.path, mv$(path));
        crate.m_extra_files.push_back(file_path);

        ::std::ifstream is(file_path);
        if( !is.good() ) {
            ERROR(sp, E0000, "Cannot open file " << file_path << " for include_bytes!");
        }
        ::std::stringstream   ss;
        ss << is.rdbuf();

        ::std::vector<TokenTree>    toks;
        toks.push_back(Token(TOK_BYTESTRING, mv$(ss.str())));
        return box$( TTStreamO(sp, ParseState(), TokenTree(AST::Edition::Rust2015, Ident::Hygiene::new_scope(), mv$(toks))) );
    }
};

class CIncludeStrExpander:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        Token   tok;
        auto lex = TTStream(sp, ParseState(), tt);

        auto path = get_string(sp, lex, crate, mod);
        GET_CHECK_TOK(tok, lex, TOK_EOF);

        ::std::string file_path = get_path_relative_to(mod.m_file_info.path, mv$(path));
        crate.m_extra_files.push_back(file_path);

        ::std::ifstream is(file_path);
        if( !is.good() ) {
            ERROR(sp, E0000, "Cannot open file " << file_path << " for include_str!");
        }
        ::std::stringstream   ss;
        ss << is.rdbuf();

        ::std::vector<TokenTree>    toks;
        toks.push_back(Token(TOK_STRING, mv$(ss.str())));
        return box$( TTStreamO(sp, ParseState(), TokenTree(AST::Edition::Rust2015, Ident::Hygiene::new_scope(), mv$(toks))) );
    }
};

// TODO: include_str! and include_bytes!

STATIC_MACRO("include", CIncludeExpander);
STATIC_MACRO("include_bytes", CIncludeBytesExpander);
STATIC_MACRO("include_str", CIncludeStrExpander);


