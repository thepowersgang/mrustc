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
        ::std::function<::HIR::SimplePath(bool, RcString, ::HIR::Struct)>  m_new_type;
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
                auto boxed = box$( (::HIR::VisEnt< ::HIR::TypeItem> { (pub ? ::HIR::Publicity::new_global() : ::HIR::Publicity::new_none()), ::HIR::TypeItem( mv$(s) ) }) );
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

            ::HIR::GenericPath  trait_path( p.get_simple_path(), tr.m_params.make_nop_params(0) );

            ::std::unordered_map< ::std::string,unsigned int>  assoc_type_indexes;
            struct Foo {
                ::HIR::Trait*   trait_ptr;
                ::HIR::GenericParams    params;
                unsigned int    i;
                void add_types_from_trait(const ::HIR::Trait& tr) {
                    for(const auto& ty : tr.m_types) {
                        DEBUG(ty.first << " #" << i);
                        auto rv = trait_ptr->m_type_indexes.insert( ::std::make_pair(ty.first, i) );
                        if(rv.second == false) {
                            //TODO(Span(), "Handle conflicting associated types - '" << ty.first << "'");
                        }
                        else {
                            params.m_types.push_back( ::HIR::TypeParamDef { RcString::new_interned(FMT("a#" << ty.first)), {}, ty.second.is_sized } );
                        }
                        i ++;
                    }
                }
            };
            Foo visitor { &tr, {}, static_cast<unsigned int>(tr.m_params.m_types.size()) };
            for(const auto& tp : tr.m_params.m_types) {
                visitor.params.m_types.push_back( ::HIR::TypeParamDef { tp.m_name, {}, tp.m_is_sized } );
            }
            visitor.add_types_from_trait(tr);
            for(const auto& st : tr.m_all_parent_traits)
            {
                assert(st.m_trait_ptr);
                visitor.add_types_from_trait(*st.m_trait_ptr);
            }
            auto args = mv$(visitor.params);

            struct VtableConstruct {
                const OuterVisitor* m_outer;
                ::HIR::Trait*   trait_ptr;
                ::HIR::t_struct_fields fields;

                bool add_ents_from_trait(const ::HIR::Trait& tr, const ::HIR::GenericPath& trait_path)
                {
                    TRACE_FUNCTION_F(trait_path);
                    struct M: public Monomorphiser {
                        ::HIR::Trait*   trait_ptr;

                        ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& g) const override {
                            return HIR::TypeRef(g.name, g.binding);
                        }
                        ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& g) const override {
                            return g;
                        }
                        ::HIR::LifetimeRef get_lifetime(const Span& sp, const ::HIR::GenericRef& g) const override {
                            // Shift lifetimes from 1 (value) to 3 (HRL)
                            if( (g.binding >> 8) == 1 ) {
                                return (g.binding & 0xFF) + 3*256;
                            }
                            return HIR::LifetimeRef(g.binding);
                        }

                        HIR::TypeRef monomorph_type(const Span& sp, const HIR::TypeRef& t, bool allow_infer=true) const override {
                            if(t.data().is_Path() && t.data().as_Path().path.m_data.is_UfcsKnown()) {
                                const auto& pe = t.data().as_Path().path.m_data.as_UfcsKnown();
                                bool is_self = (pe.type == ::HIR::TypeRef(RcString::new_interned("Self"), 0xFFFF));
                                auto it = trait_ptr->m_type_indexes.find(pe.item);
                                bool has_item = (it != trait_ptr->m_type_indexes.end());
                                // TODO: Check the trait against m_type_indexes
                                if( is_self /*&& pe.trait == trait_path*/ && has_item ) {
                                    DEBUG("[clone_cb] t=" << t << " -> " << it->second);
                                    // Replace with a new type param, need to know the index of it
                                    return ::HIR::TypeRef( RcString::new_interned(FMT("a#" << pe.item)), it->second);
                                }
                                else {
                                    DEBUG("[clone_cb] t=" << t << "(" << is_self << has_item << ")");
                                }
                            }
                            return Monomorphiser::monomorph_type(sp, t, allow_infer);
                        }
                    } m;
                    m.trait_ptr = trait_ptr;
                    auto clone_self_cb = [](const auto& t, auto&o) {
                        if( t == ::HIR::TypeRef("Self", 0xFFFF) ) {
                            o = ::HIR::TypeRef::new_unit();
                            return true;
                        }
                        return false;
                        };
                    for(auto& vi : tr.m_values)
                    {
                        TU_MATCH_HDRA( (vi.second), {)
                        TU_ARMA(Function, ve) {
                            if( ve.m_receiver == ::HIR::Function::Receiver::Free ) {
                                DEBUG("- '" << vi.first << "' Skip free function");  // ?
                                continue ;
                            }
                            if( ::std::any_of(ve.m_params.m_bounds.begin(), ve.m_params.m_bounds.end(), [&](const auto& b){
                                return b.is_TraitBound()
                                    && b.as_TraitBound().type == ::HIR::TypeRef("Self", 0xFFFF)
                                    && b.as_TraitBound().trait.m_path.m_path == m_outer->m_lang_Sized;
                                }) )
                            {
                                DEBUG("- '" << vi.first << "' Skip where `Self: Sized`");
                                continue ;
                            }
                            if( ve.m_params.is_generic() ) {
                                DEBUG("- '" << vi.first << "' NOT object safe (generic), not creating vtable");
                                return false;
                            }
                            // NOTE: by-value methods are valid in 1.39 (Added for FnOnce)
                            if( !TARGETVER_LEAST_1_39 )
                            {
                                if( ve.m_receiver == ::HIR::Function::Receiver::Value ) {
                                    DEBUG("- '" << vi.first << "' NOT object safe (by-value), not creating vtable");
                                    return false;
                                }
                            }

                            ::HIR::TypeData_FunctionPointer ft;
                            ft.hrls.m_lifetimes = ve.m_params.m_lifetimes;
                            ft.is_unsafe = ve.m_unsafe;
                            ft.m_abi = ve.m_abi;
                            ft.m_rettype = m.monomorph_type(sp, ve.m_return);
                            ft.m_arg_types.reserve( ve.m_args.size() );
                            ft.m_arg_types.push_back( clone_ty_with(sp, m.monomorph_type(sp, ve.m_args[0].second), clone_self_cb) );
                            if( ve.m_receiver == ::HIR::Function::Receiver::Value ) {
                                ft.m_arg_types[0] = HIR::TypeRef::new_borrow(HIR::BorrowType::Owned, mv$(ft.m_arg_types[0]));
                            }
                            for(unsigned int i = 1; i < ve.m_args.size(); i ++)
                                ft.m_arg_types.push_back( m.monomorph_type(sp, ve.m_args[i].second) );
                            // Clear the first argument (the receiver)
                            ::HIR::TypeRef  fcn_type( mv$(ft) );

                            // Detect use of `Self` and don't create the vtable if there is.
                            // NOTE: Associated types where replaced by clone_ty_with
                            if( visit_ty_with(fcn_type, [&](const auto& t){ return (t == ::HIR::TypeRef("Self", 0xFFFF)); }) )
                            {
                                DEBUG("- '" << vi.first << "' NOT object safe (uses Self), not creating vtable - " << fcn_type);
                                return false;
                            }

                            trait_ptr->m_value_indexes.insert( ::std::make_pair(
                                vi.first,
                                ::std::make_pair(static_cast<unsigned int>(fields.size()), trait_path.clone())
                                ) );
                            DEBUG("- '" << vi.first << "' is @" << fields.size());
                            fields.push_back( ::std::make_pair(
                                vi.first,
                                ::HIR::VisEnt< ::HIR::TypeRef> { ::HIR::Publicity::new_global(), mv$(fcn_type) }
                                ) );
                            }
                        TU_ARMA(Static, ve) {
                            if( vi.first != "vtable#" )
                            {
                                TODO(Span(), "Associated static in vtable");
                            }
                            }
                        TU_ARMA(Constant, ve) {
                            //TODO(Span(), "Associated const in vtable");
                            }
                        }
                    }
                    for(const auto& st : tr.m_all_parent_traits) {
                        ::HIR::TypeRef  self("Self", 0xFFFF);
                        auto st_gp = MonomorphStatePtr(&self, &trait_path.m_params, nullptr).monomorph_genericpath(sp, st.m_path, false);
                        // NOTE: Doesn't trigger non-object-safe
                        add_ents_from_trait(*st.m_trait_ptr, st_gp);
                    }
                    return true;
                }
            };

            VtableConstruct vtc { this, &tr, {} };
            // - Drop glue pointer
            ::HIR::TypeData_FunctionPointer ft;
            ft.is_unsafe = false;
            ft.m_abi = ABI_RUST;
            ft.m_rettype = ::HIR::TypeRef::new_unit();
            ft.m_arg_types.push_back( ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Owned, ::HIR::TypeRef::new_unit()) );
            vtc.fields.push_back(::std::make_pair( "#drop_glue", ::HIR::VisEnt<::HIR::TypeRef> { ::HIR::Publicity::new_none(), ::HIR::TypeRef(mv$(ft)) } ));
            // - Size of data
            vtc.fields.push_back(::std::make_pair( "#size", ::HIR::VisEnt<::HIR::TypeRef> { ::HIR::Publicity::new_none(), ::HIR::CoreType::Usize } ));
            // - Alignment of data
            vtc.fields.push_back(::std::make_pair( "#align", ::HIR::VisEnt<::HIR::TypeRef> { ::HIR::Publicity::new_none(), ::HIR::CoreType::Usize } ));
            // - Add methods
            if( ! vtc.add_ents_from_trait(tr, trait_path) )
            {
                tr.m_value_indexes.clear();
                tr.m_type_indexes.clear();
                return ;
            }
            auto fields = mv$(vtc.fields);

            ::HIR::PathParams   params;
            {
                unsigned int i = 0;
                for(const auto& tp : tr.m_params.m_types) {
                    params.m_types.push_back( ::HIR::TypeRef(tp.m_name, i) );
                    i ++;
                }
                for(const auto& ty : tr.m_type_indexes) {
                    ::HIR::Path path( ::HIR::TypeRef("Self",0xFFFF), trait_path.clone(), ty.first );
                    params.m_types.push_back( ::HIR::TypeRef::new_path( mv$(path), {} ) );
                }
            }
            // TODO: Would like to have access to the publicity marker
            auto item_path = m_new_type(
                true,
                RcString::new_interned(FMT(p.get_name() << "#vtable")),
                ::HIR::Struct(mv$(args), ::HIR::Struct::Repr::C, ::HIR::Struct::Data(mv$(fields)))
                );
            tr.m_vtable_path = item_path;
            DEBUG("Vtable structure created - " << item_path);
            ::HIR::GenericPath  path( mv$(item_path), mv$(params) );

            tr.m_values.insert( ::std::make_pair(
                "vtable#",
                ::HIR::TraitValueItem(::HIR::Static( ::HIR::Linkage(), false, ::HIR::TypeRef::new_path( mv$(path), {} ), {} ))
                ) );
        }

        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            static Span sp;
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            //auto _ = this->m_resolve.set_impl_generics(impl.m_params);

            ::HIR::Visitor::visit_trait_impl(trait_path, impl);

            #if 0
            // Check if the trait has a vtable, and if it does emit an associated static for it.
            const auto& tr = m_crate.get_trait_by_path(sp, trait_path);
            if(tr.m_value_indexes.size() > 0)
            {
                auto monomorph_cb_trait = monomorphise_type_get_cb(sp, &impl.m_type, &impl.m_trait_args, nullptr);
                auto trait_gpath = ::HIR::GenericPath(trait_path, impl.m_trait_args.clone());

                ::std::vector< ::HIR::Literal>  vals;
                vals.resize( tr.m_value_indexes.size() );
                for(const auto& m : tr.m_value_indexes)
                {
                    //ASSERT_BUG(sp, tr.m_values.at(m.first).is_Function(), "TODO: Handle generating vtables with non-function items");
                    DEBUG("- " << m.second.first << " = " << m.second.second << " :: " << m.first);
                    auto gpath = monomorphise_genericpath_with(sp, m.second.second, monomorph_cb_trait, false);
                    vals.at(m.second.first) = ::HIR::Literal::make_BorrowOf( ::HIR::Path(impl.m_type.clone(), mv$(gpath), m.first) );
                }

                auto vtable_sp = trait_path;
                vtable_sp.m_components.back() += "#vtable";
                auto vtable_params = impl.m_trait_args.clone();
                for(const auto& ty : tr.m_type_indexes) {
                    ::HIR::Path path( impl.m_type.clone(), mv$(trait_gpath), ty.first );
                    vtable_params.m_types.push_back( ::HIR::TypeRef( mv$(path) ) );
                }

                const auto& vtable_ref = m_crate.get_struct_by_path(sp, vtable_sp);
                impl.m_statics.insert(::std::make_pair( "vtable#", ::HIR::TraitImpl::ImplEnt<::HIR::Static> { true, ::HIR::Static {
                    ::HIR::Linkage(),
                    false,
                    ::HIR::TypeRef::new_path(::HIR::GenericPath(mv$(vtable_sp), mv$(vtable_params)), &vtable_ref),
                    {},
                    ::HIR::Literal::make_List( mv$(vals) )
                    } } ));
            }
            #endif
        }
    };
}

void HIR_Expand_VTables(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

