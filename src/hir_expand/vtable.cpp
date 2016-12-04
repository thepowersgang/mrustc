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
        const ::HIR::Crate& m_crate;
        //StaticTraitResolve  m_resolve;
        ::std::function<::HIR::SimplePath(bool, ::std::string, ::HIR::Struct)>  m_new_type;
        ::HIR::SimplePath   m_lang_Sized;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_crate(crate)
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
            static Span sp;
            TRACE_FUNCTION_F(p);
            
            ::HIR::GenericPath  trait_path( p.get_simple_path() );
            {
                unsigned int i = 0;
                for(const auto& tp : tr.m_params.m_types) {
                    trait_path.m_params.m_types.push_back( ::HIR::TypeRef(tp.m_name, i) );
                    i ++;
                }
            }
            
            ::std::unordered_map< ::std::string,unsigned int>  assoc_type_indexes;
            struct Foo {
                ::HIR::Trait*   trait_ptr;
                ::HIR::GenericParams    params;
                unsigned int    i;
                void add_types_from_trait(const ::HIR::Trait& tr) {
                    for(const auto& ty : tr.m_types) {
                        auto rv = trait_ptr->m_type_indexes.insert( ::std::make_pair(ty.first, i) );
                        if(rv.second == false) {
                            //TODO(Span(), "Handle conflicting associated types - '" << ty.first << "'");
                        }
                        else {
                            params.m_types.push_back( ::HIR::TypeParamDef { "a#"+ty.first, {}, ty.second.is_sized } );
                        }
                        i ++;
                    }
                    // TODO: Iterate supertraits too.
                    for(const auto& st : tr.m_parent_traits) {
                        add_types_from_trait(*st.m_trait_ptr);
                    }
                }
            };
            Foo visitor { &tr, {}, static_cast<unsigned int>(tr.m_params.m_types.size()) };
            for(const auto& tp : tr.m_params.m_types) {
                visitor.params.m_types.push_back( ::HIR::TypeParamDef { tp.m_name, {}, tp.m_is_sized } );
            }
            visitor.add_types_from_trait(tr);
            auto args = mv$(visitor.params);
            
            auto clone_cb = [&](const auto& t, auto& o) {
                if(t.m_data.is_Path() && t.m_data.as_Path().path.m_data.is_UfcsKnown()) {
                    const auto& pe = t.m_data.as_Path().path.m_data.as_UfcsKnown();
                    DEBUG("t=" << t);
                    if( *pe.type == ::HIR::TypeRef("Self", 0xFFFF) /*&& pe.trait == trait_path*/ ) {
                        // Replace with a new type param, need to know the index of it
                        o = ::HIR::TypeRef("a#"+pe.item, tr.m_type_indexes.at(pe.item));
                        return true;
                    }
                }
                return false;
                };
            auto clone_self_cb = [](const auto& t, auto&o) {
                if( t == ::HIR::TypeRef("Self", 0xFFFF) ) {
                    o = ::HIR::TypeRef::new_unit();
                    return true;
                }
                return false;
                };
            
            ::HIR::t_struct_fields fields;
            for(auto& vi : tr.m_values)
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
                        tr.m_value_indexes.clear();
                        tr.m_type_indexes.clear();
                        return ;
                    }
                    
                    ::HIR::FunctionType ft;
                    ft.is_unsafe = ve.m_unsafe;
                    ft.m_abi = ve.m_abi;
                    ft.m_rettype = box$( clone_ty_with(sp, ve.m_return, clone_cb) );
                    ft.m_arg_types.reserve( ve.m_args.size() );
                    ft.m_arg_types.push_back( clone_ty_with(sp, ve.m_args[0].second, clone_self_cb) );
                    for(unsigned int i = 1; i < ve.m_args.size(); i ++)
                        ft.m_arg_types.push_back( clone_ty_with(sp, ve.m_args[i].second, clone_cb) );
                    // Clear the first argument (the receiver)
                    ::HIR::TypeRef  fcn_type( mv$(ft) );
                    
                    // Detect use of `Self` and don't create the vtable if there is.
                    if( visit_ty_with(fcn_type, [&](const auto& t){ return (t == ::HIR::TypeRef("Self", 0xFFFF)); }) )
                    {
                        DEBUG("- '" << vi.first << "' NOT object safe (Self), not creating vtable - " << fcn_type);
                        tr.m_value_indexes.clear();
                        tr.m_type_indexes.clear();
                        return ;
                    }
                    
                    tr.m_value_indexes[vi.first] = fields.size();
                    DEBUG("- '" << vi.first << "' is @" << fields.size());
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
            {
                unsigned int i = 0;
                for(const auto& tp : tr.m_params.m_types) {
                    params.m_types.push_back( ::HIR::TypeRef(tp.m_name, i) );
                    i ++;
                }
                for(const auto& ty : tr.m_type_indexes) {
                    ::HIR::Path path( ::HIR::TypeRef("Self",0xFFFF), trait_path.clone(), ty.first );
                    params.m_types.push_back( ::HIR::TypeRef( mv$(path) ) );
                }
            }
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
            static Span sp;
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            //auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
            
            // Check if the trait has a vtable, and if it does emit an associated static for it.
            const auto& tr = m_crate.get_trait_by_path(sp, trait_path);
            if(tr.m_value_indexes.size() > 0)
            {
                auto trait_gpath = ::HIR::GenericPath(trait_path, impl.m_trait_args.clone());
                
                ::std::vector< ::HIR::Literal>  vals;
                vals.resize( tr.m_value_indexes.size() );
                for(const auto& m : tr.m_value_indexes)
                {
                    //ASSERT_BUG(sp, tr.m_values.at(m.first).is_Function(), "TODO: Handle generating vtables with non-function items");
                    vals.at(m.second) = ::HIR::Literal::make_BorrowOf( ::HIR::Path(impl.m_type.clone(), trait_gpath.clone(), m.first) );
                }
                
                auto vtable_sp = trait_path;
                vtable_sp.m_components.back() += "#vtable";
                auto vtable_params = impl.m_trait_args.clone();
                for(const auto& ty : tr.m_type_indexes) {
                    ::HIR::Path path( impl.m_type.clone(), mv$(trait_gpath), ty.first );
                    vtable_params.m_types.push_back( ::HIR::TypeRef( mv$(path) ) );
                }
                
                const auto& vtable_ref = m_crate.get_struct_by_path(sp, vtable_sp);
                impl.m_statics.insert(::std::make_pair( "#vtable", ::HIR::TraitImpl::ImplEnt<::HIR::Static> { true, ::HIR::Static {
                    false,
                    ::HIR::TypeRef::new_path(::HIR::GenericPath(mv$(vtable_sp), mv$(vtable_params)), &vtable_ref),
                    {},
                    ::HIR::Literal::make_List( mv$(vals) )
                    } } ));
            }
        }
    };
}

void HIR_Expand_VTables(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

