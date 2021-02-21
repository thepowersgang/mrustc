/*
* MRustC - Rust Compiler
* - By John Hodge (Mutabah/thePowersGang)
*
* expand/compile_error.cpp
* - compile_error! handler
*/
#include <synext.hpp>
#include "../parse/common.hpp"
#include "../parse/parseerror.hpp"
#include "../parse/tokentree.hpp"
#include "../parse/ttstream.hpp"
#include "../parse/lex.hpp" // For Codepoint
#include <ast/expr.hpp>
#include <ast/crate.hpp>

class CExpander_CompileError:
    public ExpandProcMacro
{
    ::std::unique_ptr<TokenStream> expand(const Span& sp, const AST::Crate& crate, const TokenTree& tt, AST::Module& mod) override
    {
        ERROR(sp, E0000, "compile_error! " << tt);
    }
};

STATIC_MACRO("compile_error", CExpander_CompileError);

