/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/cleanup.cpp
 * - MIR Cleanup
 *
 * Removes artefacts left after monomorphisation
 * - Converts <Trait as Trait>::method() into a vtable call
 * - Replaces constants by their value
 */
#include "main_bindings.hpp"
#include "mir.hpp"
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>
#include <mir/helpers.hpp>

void MIR_Cleanup(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    Span    sp;
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };
    
    for(auto& block : fcn.blocks)
    {
        for(auto& stmt : block.statements)
        {
            if( stmt.is_Assign() )
            {
                auto& se = stmt.as_Assign();
                
                TU_IFLET( ::MIR::RValue, se.src, Constant, e,
                    // TODO: Replace `Const` with actual values
                )
            }
        }
        
        TU_IFLET( ::MIR::Terminator, block.terminator, CallPath, e,
            
            // Detect calling `<Trait as Trait>::method()` and replace with vtable call
            if( e.fcn_path.m_data.is_UfcsKnown() && e.fcn_path.m_data.as_UfcsKnown().type->m_data.is_TraitObject() )
            {
                const auto& pe = e.fcn_path.m_data.as_UfcsKnown();
                const auto& te = pe.type->m_data.as_TraitObject();
                // TODO: What if the method is from a supertrait?
                if( pe.trait == te.m_trait.m_path )
                {
                    assert( te.m_trait.m_trait_ptr );
                    const auto& trait = *te.m_trait.m_trait_ptr;
                    
                    // 1. Get the vtable index for this function
                    if( trait.m_value_indexes.count(pe.item) == 0 )
                        BUG(sp, "Calling method '" << pe.item << "' of " << pe.trait << " which isn't in the vtable");
                    unsigned int vtable_idx = trait.m_value_indexes.at( pe.item );
                    
                    // 2. Load from the vtable
                    auto vtable_ty_spath = pe.trait.m_path;
                    vtable_ty_spath.m_components.back() += "#vtable";
                    const auto& vtable_ref = resolve.m_crate.get_struct_by_path(sp, vtable_ty_spath);
                    // Copy the param set from the trait in the trait object
                    ::HIR::PathParams   vtable_params = te.m_trait.m_path.m_params.clone();
                    // - Include associated types on bound
                    for(const auto& ty_b : te.m_trait.m_type_bounds) {
                        auto idx = trait.m_type_indexes.at(ty_b.first);
                        if(vtable_params.m_types.size() <= idx)
                            vtable_params.m_types.resize(idx+1);
                        vtable_params.m_types[idx] = ty_b.second.clone();
                    }
                    auto vtable_ty = ::HIR::TypeRef::new_pointer(
                        ::HIR::BorrowType::Shared,
                        ::HIR::TypeRef( ::HIR::GenericPath(vtable_ty_spath, mv$(vtable_params)), &vtable_ref )
                        );
                    
                    // Allocate a temporary for the vtable pointer itself
                    auto vtable_lv = ::MIR::LValue::make_Temporary({ static_cast<unsigned int>(fcn.temporaries.size()) });
                    fcn.temporaries.push_back( mv$(vtable_ty) );
                    // - Load the vtable and store it
                    auto vtable_rval = ::MIR::RValue::make_DstMeta({
                        ::MIR::LValue::make_Deref({ box$(e.args.front().clone()) })
                        });
                    block.statements.push_back( ::MIR::Statement::make_Assign({ vtable_lv.clone(), mv$(vtable_rval) }) );
                    
                    // Update the terminator with the new information.
                    auto vtable_fcn = ::MIR::LValue::make_Field({ box$(::MIR::LValue::make_Deref({ box$(vtable_lv) })), vtable_idx });
                    auto new_term = ::MIR::Terminator::make_CallValue({
                        e.ret_block, e.panic_block,
                        mv$(e.ret_val), mv$(vtable_fcn),
                        mv$(e.args)
                        });
                    
                    block.terminator = mv$(new_term);
                }
            }
        )
    }
}

