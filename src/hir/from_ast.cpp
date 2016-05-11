
#include "hir.hpp"
#include <main_bindings.hpp>
#include <ast/ast.hpp>
#include <ast/crate.hpp>

::HIR::Module LowerHIR_Module(::AST::Module module, ::HIR::SimplePath path);

/// \brief Converts the AST into HIR format
///
/// - Removes all possibility for unexpanded macros
/// - Performs desugaring of for/if-let/while-let/...
::HIR::CratePtr LowerHIR_FromAST(::AST::Crate crate)
{
    ::std::unordered_map< ::std::string, MacroRules >   macros;
    auto rootmod = LowerHIR_Module( mv$(crate.m_root_module), ::HIR::SimplePath("") );
    return ::HIR::CratePtr( ::HIR::Crate { mv$(rootmod), mv$(macros) } );
}

::HIR::Module LowerHIR_Module(::AST::Module module, ::HIR::SimplePath path)
{
    ::HIR::Module   mod { };
    throw ::std::runtime_error("TODO: LowerHIR_Module");
}

