/**
 * Binding header for the macro_rules syntax extension
 */
#pragma once

#include <synext.hpp>

namespace AST {
    class Expr;
    class Module;
}
class TokenTree;
class TokenStream;
class MacroRules;

extern ::std::unique_ptr<TokenStream>   Macro_Invoke(const char* name, const MacroRules& rules, TokenTree tt, AST::Module& mod);
