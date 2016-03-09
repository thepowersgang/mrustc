/**
 * Binding header for the macro_rules syntax extension
 */
#pragma once

#include <synext.hpp>
#include "../macros.hpp"

namespace AST {
    class Expr;
    class Module;
}
class TokenTree;
class TokenStream;

extern ::std::unique_ptr<TokenStream>   Macro_Invoke(const char* name, const MacroRules& rules, const TokenTree& tt, AST::Module& mod);
