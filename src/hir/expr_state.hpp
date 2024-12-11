/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/expr_state.hpp
 * - Extra state for expression pointers
 */
#pragma once
#include <hir/hir.hpp>

namespace HIR {

class ExprState
{
public:
    ::HIR::SimplePath   m_mod_path;
    const ::HIR::Module&    m_module;

    const ::HIR::GenericParams*   m_impl_generics;
    const ::HIR::GenericParams*   m_item_generics;

    ::std::vector< ::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >   m_traits;

    enum class Stage {
        Created,
        ConstEvalRequest,
        ConstEval,
        TypecheckRequest,
        Typecheck,
        PostTypecheck,
        SbcRequest,
        Sbc,
        ExpandRequest,
        Expand,
        MirRequest,
        Mir,
    };
    mutable Stage   stage;

    ExprState(const ::HIR::Module& mod_ptr, ::HIR::SimplePath mod_path):
        m_mod_path(::std::move(mod_path)),
        m_module(mod_ptr),
        m_impl_generics(nullptr),
        m_item_generics(nullptr),
        stage(Stage::Created)
    {
    }
};

}
