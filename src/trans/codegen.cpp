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
#include <mir/mir.hpp>
#include <mir/operations.hpp>

#include "codegen.hpp"
#include "monomorphise.hpp"

namespace {
    struct PtrComp
    {
        template<typename T>
        bool operator()(const T* lhs, const T* rhs) const { return *lhs < *rhs; }
    };
    
    struct TypeVisitor
    {
        const ::HIR::Crate& m_crate;
        CodeGenerator&  codegen;
        ::std::set< ::HIR::TypeRef> visited;
        ::std::set< const ::HIR::TypeRef*, PtrComp> active_set;
        
        TypeVisitor(const ::HIR::Crate& crate, CodeGenerator& codegen):
            m_crate(crate),
            codegen(codegen)
        {}
        
        void visit_struct(const ::HIR::GenericPath& path, const ::HIR::Struct& item) {
            static Span sp;
            ::HIR::TypeRef  tmp;
            auto monomorph = [&](const auto& x)->const auto& {
                if( monomorphise_type_needed(x) ) {
                    tmp = monomorphise_type(sp, item.m_params, path.m_params, x);
                    //m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return x;
                }
                };
            TU_MATCHA( (item.m_data), (e),
            (Unit,
                ),
            (Tuple,
                for(const auto& fld : e) {
                    visit_type( monomorph(fld.ent) );
                }
                ),
            (Named,
                for(const auto& fld : e)
                    visit_type( monomorph(fld.second.ent) );
                )
            )
            codegen.emit_struct(sp, path, item);
        }
        void visit_union(const ::HIR::GenericPath& path, const ::HIR::Union& item) {
        }
        void visit_enum(const ::HIR::GenericPath& path, const ::HIR::Enum& item) {
            static Span sp;
            ::HIR::TypeRef  tmp;
            auto monomorph = [&](const auto& x)->const auto& {
                if( monomorphise_type_needed(x) ) {
                    tmp = monomorphise_type(sp, item.m_params, path.m_params, x);
                    //m_resolve.expand_associated_types(sp, tmp);
                    return tmp;
                }
                else {
                    return x;
                }
                };
            for(const auto& variant : item.m_variants)
            {
                TU_MATCHA( (variant.second), (e),
                (Unit,
                    ),
                (Value,
                    ),
                (Tuple,
                    for(const auto& ty : e)
                        visit_type( monomorph(ty.ent) );
                    ),
                (Struct,
                    for(const auto& fld : e)
                        visit_type( monomorph(fld.second.ent) );
                    )
                )
            }
            
            codegen.emit_enum(sp, path, item);
        }
        
        void visit_type(const ::HIR::TypeRef& ty)
        {
            // Already done
            if( visited.find(ty) != visited.end() )
                return ;
            
            if( active_set.find(&ty) != active_set.end() ) {
                // TODO: Handle recursion
                return ;
            }
            active_set.insert( &ty );
            
            TU_MATCHA( (ty.m_data), (te),
            // Impossible
            (Infer,
                ),
            (Generic,
                ),
            (ErasedType,
                ),
            (Closure,
                ),
            // Nothing to do
            (Diverge,
                ),
            (Primitive,
                ),
            // Recursion!
            (Path,
                TU_MATCHA( (te.binding), (tpb),
                (Unbound,   ),
                (Opaque,   ),
                (Struct,
                    visit_struct(te.path.m_data.as_Generic(), *tpb);
                    ),
                (Union,
                    visit_union(te.path.m_data.as_Generic(), *tpb);
                    ),
                (Enum,
                    visit_enum(te.path.m_data.as_Generic(), *tpb);
                    )
                )
                ),
            (TraitObject,
                static Span sp;
                // Ensure that the data trait's vtable is present
                const auto& trait = *te.m_trait.m_trait_ptr;
                
                auto vtable_ty_spath = te.m_trait.m_path.m_path;
                vtable_ty_spath.m_components.back() += "#vtable";
                const auto& vtable_ref = m_crate.get_struct_by_path(sp, vtable_ty_spath);
                // Copy the param set from the trait in the trait object
                ::HIR::PathParams   vtable_params = te.m_trait.m_path.m_params.clone();
                // - Include associated types on bound
                for(const auto& ty_b : te.m_trait.m_type_bounds) {
                    auto idx = trait.m_type_indexes.at(ty_b.first);
                    if(vtable_params.m_types.size() <= idx)
                        vtable_params.m_types.resize(idx+1);
                    vtable_params.m_types[idx] = ty_b.second.clone();
                }
                
                
                visit_type( ::HIR::TypeRef( ::HIR::GenericPath(vtable_ty_spath, mv$(vtable_params)), &vtable_ref ) );
                ),
            (Array,
                visit_type(*te.inner);
                ),
            (Slice,
                visit_type(*te.inner);
                ),
            (Borrow,
                visit_type(*te.inner);
                ),
            (Pointer,
                visit_type(*te.inner);
                ),
            (Tuple,
                for(const auto& sty : te)
                    visit_type(sty);
                ),
            (Function,
                visit_type(*te.m_rettype);
                for(const auto& sty : te.m_arg_types)
                    visit_type(sty);
                )
            )
            active_set.erase( active_set.find(&ty) );
            
            codegen.emit_type(ty);
            visited.insert( ty.clone() );
        }
    };
}

void Trans_Codegen(const ::std::string& outfile, const ::HIR::Crate& crate, const TransList& list)
{
    auto codegen = Trans_Codegen_GetGeneratorC(crate, outfile);
    
    // 1. Emit structure/type definitions.
    // - Emit in the order they're needed.
    {
        TRACE_FUNCTION;
        
        TypeVisitor tv { crate, *codegen };
        for(const auto& ent : list.m_functions)
        {
            TRACE_FUNCTION_F("Enumerate fn " << ent.first);
            assert(ent.second->ptr);
            const auto& fcn = *ent.second->ptr;
            const auto& pp = ent.second->pp;
            
            tv.visit_type( pp.monomorph(crate, fcn.m_return) );
            for(const auto& arg : fcn.m_args)
                tv.visit_type( pp.monomorph(crate, arg.second) );
            
            if( fcn.m_code.m_mir )
            {
                const auto& mir = *fcn.m_code.m_mir;
                for(const auto& ty : mir.named_variables)
                    tv.visit_type(pp.monomorph(crate, ty));
                for(const auto& ty : mir.temporaries)
                    tv.visit_type(pp.monomorph(crate, ty));
            }
        }
        for(const auto& ent : list.m_statics)
        {
            TRACE_FUNCTION_F("Enumerate static " << ent.first);
            assert(ent.second->ptr);
            const auto& stat = *ent.second->ptr;
            const auto& pp = ent.second->pp;
            
            tv.visit_type( pp.monomorph(crate, stat.m_type) );
        }
    }
    
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
        assert(ent.second->ptr);
        const auto& stat = *ent.second->ptr;
        
        if( ! stat.m_value_res.is_Invalid() )
        {
            codegen->emit_static_local(ent.first, stat, ent.second->pp);
        }
        else
        {
            codegen->emit_static_ext(ent.first, stat, ent.second->pp);
        }
    }
    
    
    // 4. Emit function code
    for(const auto& ent : list.m_functions)
    {
        if( ent.second->ptr && ent.second->ptr->m_code.m_mir )
        {
            TRACE_FUNCTION_F(ent.first);
            DEBUG("FUNCTION CODE " << ent.first);
            const auto& fcn = *ent.second->ptr;
            // TODO: If this is a provided trait method, it needs to be monomorphised too.
            bool is_method = ( fcn.m_args.size() > 0 && visit_ty_with(fcn.m_args[0].second, [&](const auto& x){return x == ::HIR::TypeRef("Self",0xFFFF);}) );
            if( ent.second->pp.has_types() || is_method )
            {
                ::StaticTraitResolve    resolve { crate };
                auto ret_type = ent.second->pp.monomorph(crate, fcn.m_return);
                ::HIR::Function::args_t args;
                for(const auto& a : fcn.m_args)
                    args.push_back(::std::make_pair( ::HIR::Pattern{}, ent.second->pp.monomorph(crate, a.second) ));
                auto mir = Trans_Monomorphise(crate, ent.second->pp, fcn.m_code.m_mir);
                MIR_Validate(resolve, ::HIR::ItemPath(), *mir, args, ret_type);
                MIR_Cleanup(resolve, ::HIR::ItemPath(), *mir, args, ret_type);
                // TODO: MIR Optimisation
                MIR_Validate(resolve, ::HIR::ItemPath(), *mir, args, ret_type);
                codegen->emit_function_code(ent.first, fcn, ent.second->pp,  mir);
            }
            else {
                codegen->emit_function_code(ent.first, fcn, ent.second->pp,  fcn.m_code.m_mir);
            }
        }
    }
    
    codegen->finalise();
}

