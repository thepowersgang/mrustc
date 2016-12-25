/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/include.cpp
 * - include!/include_str!/include_bytes! support
 */
#include <synext_macro.hpp>
#include <parse/common.hpp>
#include <parse/parseerror.hpp> // for GET_CHECK_TOK
#include <parse/ttstream.hpp>
#include <parse/lex.hpp>    // Lexer (new files)
#include <ast/expr.hpp>

namespace {
    ::std::string get_path_relative_to(const ::std::string& base_path, ::std::string path)
    {
        if( base_path.size() == 0 ) {
            return path;
        }
        else if( base_path[base_path.size()-1] == '/' ) {
            return base_path + path;
        }
        else {

            auto slash = base_path.find_last_of('/');
            if( slash == ::std::string::npos )
            {
                return path;
            }
            else
            {
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
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const ::std::string& ident, const TokenTree& tt, AST::Module& mod) override
    {
        if( ident != "" )
            ERROR(sp, E0000, "include! doesn't take an ident");

        Token   tok;
        auto lex = TTStream(tt);

        // TODO: Parse+expand
        GET_CHECK_TOK(tok, lex, TOK_STRING);
        auto path = mv$(tok.str());
        GET_CHECK_TOK(tok, lex, TOK_EOF);

        ::std::string file_path = get_path_relative_to(mod.m_file_info.path, mv$(path));

        return box$( Lexer(file_path) );
    }
};

// TODO: include_str! and include_bytes!

STATIC_MACRO("include", CIncludeExpander);


