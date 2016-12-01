/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/vtable.cpp
 * - VTable Generation
 */
#include "main_bindings.hpp"
#include <hir/hir.hpp>
#include <hir/visitor.hpp>
#include <hir_typeck/common.hpp>    // visit_ty_with
#include <algorithm>    // ::std::any_of

namespace {
    class OuterVisitor:
        public ::HIR::Visitor
    {
        //StaticTraitResolve  m_resolve;
        ::std::function<::HIR::SimplePath(bool, ::std::string, ::HIR::Struct)>  m_new_type;
        ::HIR::SimplePath   m_lang_Sized;
    public:
        OuterVisitor(const ::HIR::Crate& crate)//:
            //m_resolve(crate)
        {
            m_lang_Sized = crate.get_lang_item_path_opt("sized");
        }
        
        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            auto saved_nt = mv$(m_new_type);
            
            ::std::vector< decltype(mod.m_mod_items)::value_type> new_types;
            m_new_type = [&](bool pub, auto name, auto s)->auto {
                auto boxed = box$( (::HIR::VisEnt< ::HIR::TypeItem> { pub, ::HIR::TypeItem( mv$(s) ) }) );
                auto ret = (p + name).get_simple_path();
                new_types.push_back( ::std::make_pair( mv$(name), mv$(boxed)) );
                return ret;
                };
            
            ::HIR::Visitor::visit_module(p, mod);
            for(auto& i : new_types )
                mod.m_mod_items.insert( mv$(i) );
            
            m_new_type = mv$(saved_nt);
        }
        
        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& tr) override
        {
            TRACE_FUNCTION_F(p);
            
            ::HIR::t_struct_fields fields;
            for(const auto& vi : tr.m_values)
            {
                TU_MATCHA( (vi.second), (ve),
                (Function,
                    if( ve.m_receiver == ::HIR::Function::Receiver::Free ) {
                        DEBUG("- '" << vi.first << "' Skip free function");  // ?
                        continue ;
                    }
                    if( ::std::any_of(ve.m_params.m_bounds.begin(), ve.m_params.m_bounds.end(), [&](const auto& b){
                        return b.is_TraitBound() && b.as_TraitBound().type == ::HIR::TypeRef("Self", 0xFFFF) && b.as_TraitBound().trait.m_path.m_path == m_lang_Sized;
                        }) )
                    {
                        DEBUG("- '" << vi.first << "' Skip where `Self: Sized`");
                        continue ;
                    }
                    if( ve.m_params.m_types.size() > 0 ) {
                        DEBUG("- '" << vi.first << "' NOT object safe (generic), not creating vtable");
                        return ;
                    }
                    
                    ::HIR::FunctionType ft;
                    ft.is_unsafe = ve.m_unsafe;
                    ft.m_abi = ve.m_abi;
                    ft.m_rettype = box$( ve.m_return.clone() );
                    for(const auto& t : ve.m_args)
                        ft.m_arg_types.push_back( t.second.clone() );
                    ft.m_arg_types[0] = ::HIR::TypeRef();   // Clear the first argument (the receiver)
                    ::HIR::TypeRef  fcn_type( mv$(ft) );
                    
                    // Detect use of `Self`
                    if( visit_ty_with(fcn_type, [&](const auto& t){
                        if( t == ::HIR::TypeRef("Self", 0xFFFF) )
                            return true;
                        return false;
                        })
                        )
                    {
                        DEBUG("- '" << vi.first << "' NOT object safe (Self), not creating vtable");
                        return ;
                    }
                    
                    fields.push_back( ::std::make_pair(
                        vi.first,
                        ::HIR::VisEnt< ::HIR::TypeRef> { true, mv$(fcn_type) }
                        ) );
                    ),
                (Static,
                    TODO(Span(), "Associated static in vtable");
                    ),
                (Constant,
                    //TODO(Span(), "Associated const in vtable");
                    )
                )
            }
            
            ::HIR::PathParams   params;
            ::HIR::GenericParams    args;
            // TODO: Would like to have access to the publicity marker
            auto item_path = m_new_type(true, FMT(p.get_name() << "#vtable"), ::HIR::Struct {
                mv$(args),
                ::HIR::Struct::Repr::Rust,
                ::HIR::Struct::Data(mv$(fields)),
                {}
                });
            DEBUG("Vtable structure created - " << item_path);
            ::HIR::GenericPath  path( mv$(item_path), mv$(params) );
            
            tr.m_values.insert( ::std::make_pair(
                "#vtable",
                ::HIR::TraitValueItem(::HIR::Static { false, ::HIR::TypeRef( mv$(path) ), {},{} })
                ) );
        }
        
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            //auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        }
    };
}

void HIR_Expand_VTables(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

