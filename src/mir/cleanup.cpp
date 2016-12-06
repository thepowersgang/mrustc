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

const ::HIR::Literal* MIR_Cleanup_GetConstant(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::Path& path,  ::HIR::TypeRef& out_ty)
{
    TU_MATCHA( (path.m_data), (pe),
    (Generic,
        const auto& constant = resolve.m_crate.get_constant_by_path(sp, pe.m_path);
        if( pe.m_params.m_types.size() != 0 )
            TODO(sp, "Generic constants - " << path);
        out_ty = constant.m_type.clone();
        return &constant.m_value_res;
        ),
    (UfcsUnknown,
        ),
    (UfcsKnown,
        ),
    (UfcsInherent,
        )
    )
    return nullptr;
}

::MIR::Terminator MIR_Cleanup_Virtualize(
    const Span& sp, const ::MIR::TypeResolve& state, ::MIR::Function& fcn,
    ::MIR::BasicBlock& block, ::MIR::Terminator::Data_CallPath& e,
    const ::HIR::TypeRef::Data::Data_TraitObject& te, const ::HIR::Path::Data::Data_UfcsKnown& pe
    )
{
    assert( te.m_trait.m_trait_ptr );
    const auto& trait = *te.m_trait.m_trait_ptr;
    
    // 1. Get the vtable index for this function
    auto it = trait.m_value_indexes.find( pe.item );
    while( it != trait.m_value_indexes.end() )
    {
        DEBUG("- " << it->second.second);
        if( it->second.second.m_path == pe.trait.m_path )
        {
            // TODO: Match generics using match_test_generics comparing to the trait args
            break ;
        }
        ++ it;
    }
    if( it == trait.m_value_indexes.end() || it->first != pe.item )
        BUG(sp, "Calling method '" << pe.item << "' from " << pe.trait << " through " << te.m_trait.m_path << " which isn't in the vtable");
    unsigned int vtable_idx = it->second.first;
    
    // 2. Load from the vtable
    auto vtable_ty_spath = pe.trait.m_path;
    vtable_ty_spath.m_components.back() += "#vtable";
    const auto& vtable_ref = state.m_resolve.m_crate.get_struct_by_path(sp, vtable_ty_spath);
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
    auto vtable_rval = ::MIR::RValue::make_DstMeta({ ::MIR::LValue::make_Deref({ box$(e.args.front().clone()) }) });
    block.statements.push_back( ::MIR::Statement::make_Assign({ vtable_lv.clone(), mv$(vtable_rval) }) );
    
    auto ptr_rval = ::MIR::RValue::make_DstPtr({ ::MIR::LValue::make_Deref({ box$(e.args.front().clone()) }) });
    auto ptr_lv = ::MIR::LValue::make_Temporary({ static_cast<unsigned int>(fcn.temporaries.size()) });
    fcn.temporaries.push_back( ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Shared, ::HIR::TypeRef::new_unit()) );
    block.statements.push_back( ::MIR::Statement::make_Assign({ ptr_lv.clone(), mv$(ptr_rval) }) );
    e.args.front() = mv$(ptr_lv);
    
    // Update the terminator with the new information.
    auto vtable_fcn = ::MIR::LValue::make_Field({ box$(::MIR::LValue::make_Deref({ box$(vtable_lv) })), vtable_idx });
    return ::MIR::Terminator::make_CallValue({
        e.ret_block, e.panic_block,
        mv$(e.ret_val), mv$(vtable_fcn),
        mv$(e.args)
        });
}

void MIR_Cleanup(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    Span    sp;
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };
    
    for(auto& block : fcn.blocks)
    {
        for(auto it = block.statements.begin(); it != block.statements.end(); ++ it)
        {
            auto& stmt = *it;
            ::std::vector< ::MIR::Statement>    new_stmts;
            auto new_temporary = [&](::HIR::TypeRef ty, ::MIR::RValue val)->::MIR::LValue {
                auto rv = ::MIR::LValue::make_Temporary({ static_cast<unsigned int>(fcn.temporaries.size()) });
                fcn.temporaries.push_back( mv$(ty) );
                new_stmts.push_back( ::MIR::Statement::make_Assign({ rv.clone(), mv$(val) }) );
                return rv;
                };
            if( stmt.is_Assign() )
            {
                auto& se = stmt.as_Assign();
                
                TU_IFLET( ::MIR::RValue, se.src, Constant, e,
                    // TODO: Replace `Const` with actual values
                    TU_IFLET( ::MIR::Constant, e, Const, ce,
                        // 1. Find the constant
                        ::HIR::TypeRef  ty;
                        const auto* lit_ptr = MIR_Cleanup_GetConstant(sp, resolve, ce.p, ty);
                        if( lit_ptr )
                        {
                            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
                            (
                                //TODO(sp, "Literal of type " << ty << " - " << *lit_ptr);
                                ),
                            (Primitive,
                                switch(te)
                                {
                                case ::HIR::CoreType::Char:
                                case ::HIR::CoreType::Usize:
                                case ::HIR::CoreType::U64:
                                case ::HIR::CoreType::U32:
                                case ::HIR::CoreType::U16:
                                case ::HIR::CoreType::U8:
                                    e = ::MIR::Constant::make_Uint( lit_ptr->as_Integer() );
                                    break;
                                case ::HIR::CoreType::Isize:
                                case ::HIR::CoreType::I64:
                                case ::HIR::CoreType::I32:
                                case ::HIR::CoreType::I16:
                                case ::HIR::CoreType::I8:
                                    e = ::MIR::Constant::make_Int( lit_ptr->as_Integer() );
                                    break;
                                case ::HIR::CoreType::F64:
                                case ::HIR::CoreType::F32:
                                    e = ::MIR::Constant::make_Float( lit_ptr->as_Float() );
                                    break;
                                case ::HIR::CoreType::Bool:
                                    e = ::MIR::Constant::make_Bool( !!lit_ptr->as_Integer() );
                                    break;
                                case ::HIR::CoreType::Str:
                                    BUG(sp, "Const of type `str` - " << ce.p);
                                }
                                ),
                            (Pointer,
                                if( lit_ptr->is_BorrowOf() ) {
                                    // TODO: 
                                }
                                else {
                                    auto lval = new_temporary( ::HIR::CoreType::Usize, ::MIR::RValue( ::MIR::Constant::make_Uint( lit_ptr->as_Integer() ) ) );
                                    se.src = ::MIR::RValue::make_Cast({ mv$(lval), mv$(ty) });
                                }
                                ),
                            (Borrow,
                                if( lit_ptr->is_BorrowOf() ) {
                                    // TODO: 
                                }
                                else if( te.inner->m_data.is_Slice() && *te.inner->m_data.as_Slice().inner == ::HIR::CoreType::U8 ) {
                                    ::std::vector<uint8_t>  bytestr;
                                    for(auto v : lit_ptr->as_String())
                                        bytestr.push_back( static_cast<uint8_t>(v) );
                                    e = ::MIR::Constant::make_Bytes( mv$(bytestr) );
                                }
                                else if( *te.inner == ::HIR::CoreType::Str ) {
                                    e = ::MIR::Constant::make_StaticString( lit_ptr->as_String() );
                                }
                                else {
                                    TODO(sp, "Const with type " << ty);
                                }
                                )
                            )
                        }
                    )
                )
            }
            
            for(auto& v : new_stmts) {
                it = block.statements.insert(it, mv$(v));
            }
        }
        
        TU_IFLET( ::MIR::Terminator, block.terminator, CallPath, e,
            
            // Detect calling `<Trait as Trait>::method()` and replace with vtable call
            if( e.fcn_path.m_data.is_UfcsKnown() && e.fcn_path.m_data.as_UfcsKnown().type->m_data.is_TraitObject() )
            {
                const auto& pe = e.fcn_path.m_data.as_UfcsKnown();
                const auto& te = pe.type->m_data.as_TraitObject();
                // TODO: What if the method is from a supertrait?

                if( te.m_trait.m_path == pe.trait || resolve.find_named_trait_in_trait(
                        sp, pe.trait.m_path, pe.trait.m_params,
                        *te.m_trait.m_trait_ptr, te.m_trait.m_path.m_path, te.m_trait.m_path.m_params,
                        *pe.type,
                        [](const auto&, auto){}
                        )
                    )
                {
                    auto new_term = MIR_Cleanup_Virtualize(sp, state, fcn, block, e, te, pe);
                    block.terminator = mv$(new_term);
                }
            }
        )
    }
}

