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
    auto codegen = Trans_Codegen_GetGeneratorC(outfile);
    
    // 1. Emit structure/type definitions.
    // - Emit in the order they're needed.
    
    // 2. Emit function prototypes
    for(const auto& fcn : list.m_functions)
    {
        DEBUG("FUNCTION " << fcn.first);
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
            if( ent.second->pp.has_types() ) {
                auto mir = Trans_Monomorphise(crate, ent.second->pp, ent.second->ptr->m_code.m_mir);
                codegen->emit_function_code(ent.first, *ent.second->ptr, ent.second->pp,  mir);
            }
            else {
                codegen->emit_function_code(ent.first, *ent.second->ptr, ent.second->pp,  ent.second->ptr->m_code.m_mir);
            }
        }
    }
    
    codegen->finalise();
}

