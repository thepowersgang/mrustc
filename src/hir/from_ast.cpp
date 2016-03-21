
#include "hir.hpp"
#include <ast/ast.hpp>
#include <ast/crate.hpp>

::HIR::Module LowerHIR_Module(::AST::Module module, ::HIR::SimplePath path);

/// \brief Converts the AST into HIR format
///
/// - Removes all possibility for unexpanded macros
/// - Performs name resolution and partial UFCS conversion? (TODO: This should be done on the AST, as it requires two passes with state)
/// - Performs desugaring of for/if-let/while-let/...
::HIR::Crate LowerHIR_FromAST(::AST::Crate crate)
{
    ::std::unordered_map< ::std::string, MacroRules >   macros;
    auto rootmod = LowerHIR_Module( mv$(crate.m_root_module), ::HIR::SimplePath() );
    return { mv$(rootmod), mv$(macros) };
}

::HIR::Module LowerHIR_Module(::AST::Module module, ::HIR::SimplePath path)
{
    throw ::std::runtime_error("");
}

