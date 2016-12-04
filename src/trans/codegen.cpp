/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/codegen.cpp
 * - Wrapper for translation
 */
#include "main_bindings.hpp"
#include "trans_list.hpp"
#include <hir/hir.hpp>

#include "codegen.hpp"
#include "monomorphise.hpp"

void Trans_Codegen(const ::std::string& outfile, const ::HIR::Crate& crate, const TransList& list)
{
    auto codegen = Trans_Codegen_GetGeneratorC(crate, outfile);
    
    // 1. Emit structure/type definitions.
    // - Emit in the order they're needed.
    
    // 2. Emit function prototypes
    for(const auto& ent : list.m_functions)
    {
        DEBUG("FUNCTION " << ent.first);
        assert( ent.second->ptr );
        if( ent.second->ptr->m_code.m_mir ) {
            codegen->emit_function_proto(ent.first, *ent.second->ptr, ent.second->pp);
        }
        else {
            codegen->emit_function_ext(ent.first, *ent.second->ptr, ent.second->pp);
        }
    }
    // 3. Emit statics
    for(const auto& ent : list.m_statics)
    {
        DEBUG("STATIC " << ent.first);
        
        if( ent.second->ptr )
        {
            codegen->emit_static_ext(ent.first);
        }
        else
        {
            codegen->emit_static_local(ent.first, *ent.second->ptr, ent.second->pp);
        }
    }
    
    
    // 4. Emit function code
    for(const auto& ent : list.m_functions)
    {
        if( ent.second->ptr && ent.second->ptr->m_code.m_mir ) {
            DEBUG("FUNCTION CODE " << ent.first);
            const auto& fcn = *ent.second->ptr;
            // TODO: If this is a provided trait method, it needs to be monomorphised too.
            bool is_method = ( fcn.m_args.size() > 0 && visit_ty_with(fcn.m_args[0].second, [&](const auto& x){return x == ::HIR::TypeRef("Self",0xFFFF);}) );
            if( ent.second->pp.has_types() || is_method ) {
                auto mir = Trans_Monomorphise(crate, ent.second->pp, fcn.m_code.m_mir);
                // TODO: MIR Optimisation
                codegen->emit_function_code(ent.first, fcn, ent.second->pp,  mir);
            }
            else {
                codegen->emit_function_code(ent.first, fcn, ent.second->pp,  fcn.m_code.m_mir);
            }
        }
    }
    
    codegen->finalise();
}

