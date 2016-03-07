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

extern AST::Expr Macro_Invoke(const char* name, const MacroRules& rules, const TokenTree& tt, AST::Module& mod, MacroPosition position);
