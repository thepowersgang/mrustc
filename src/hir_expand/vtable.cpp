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
#include <hir_typeck/static.hpp>    // visit_ty_with
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

            StaticTraitResolve  resolve { m_crate };
            resolve.set_impl_generics_raw(MetadataType::Unknown, tr.m_params);
            ::HIR::GenericPath  trait_path( p.get_simple_path(), tr.m_params.make_nop_params(0) );

            ::std::unordered_map< ::std::string,unsigned int>  assoc_type_indexes;
            struct Foo {
                ::HIR::Trait*   trait_ptr;
                ::HIR::GenericParams    params;
                bool has_conflict;
                void add_types_from_trait(const HIR::GenericPath& path, const HIR::Trait& tr, const HIR::TraitPath::assoc_list_t& assoc) {
                    for(const auto& ty : tr.m_types) {
                        bool is_known = false;
                        for(const auto& ent : assoc) {
                            if( ent.first == ty.first ) {
                                DEBUG(ty.first << " = " << ent.second.type);
                                is_known = true;
                                break;
                            }
                        }
                        if( !is_known ) {
                            auto i = params.m_types.size();
                            DEBUG(ty.first << " #" << i << " (from " << path << ")");
                            auto rv = trait_ptr->m_type_indexes.insert( ::std::make_pair(ty.first, static_cast<unsigned>(i)) );
                            if(rv.second == false) {
                                // NOTE: Some traits have multiple parents with the same ATY name
                                // E.g. `::"rustc_data_structures-0_0_0"::graph::ControlFlowGraph`
                                DEBUG("Conflicting ATY name " << ty.first);
                                rv.first->second = UINT_MAX;
                                this->has_conflict = true;
                            }
                            else {
                                params.m_types.push_back( ::HIR::TypeParamDef { RcString::new_interned(FMT("a#" << ty.first)), {}, ty.second.is_sized } );
                            }
                        }
                    }
                }
            };
            Foo visitor { &tr, {}, false };
            for(const auto& tp : tr.m_params.m_types) {
                visitor.params.m_types.push_back( ::HIR::TypeParamDef { tp.m_name, {}, tp.m_is_sized } );
            }
            visitor.add_types_from_trait(trait_path, tr, {});
            for(const auto& st : tr.m_all_parent_traits)
            {
                assert(st.m_trait_ptr);
                visitor.add_types_from_trait(st.m_path, *st.m_trait_ptr, st.m_type_bounds);
            }
            bool has_conflicting_aty_name = visitor.has_conflict;
            auto args = mv$(visitor.params);

            struct VtableConstruct {
                const OuterVisitor* m_outer;
                const StaticTraitResolve* m_resolve_ptr;
                ::HIR::Trait*   trait_ptr;
                ::HIR::t_struct_fields fields;

                bool add_ents_from_trait(const ::HIR::Trait& tr, const ::HIR::GenericPath& trait_path, std::vector<bool>* supertrait_flags)
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
                                bool is_self = (pe.type == ::HIR::TypeRef::new_self());
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
                        if( t == ::HIR::TypeRef::new_self() ) {
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
                                    && b.as_TraitBound().type == ::HIR::TypeRef::new_self()
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

                            ::HIR::TypeRef  tmp;

                            ::HIR::TypeData_FunctionPointer ft;
                            ft.hrls.m_lifetimes = ve.m_params.m_lifetimes;
                            ft.is_unsafe = ve.m_unsafe;
                            ft.is_variadic = ve.m_variadic;
                            ft.m_abi = ve.m_abi;
                            ft.m_rettype = m_resolve_ptr->monomorph_expand(sp, ve.m_return, m);
                            ft.m_arg_types.reserve( ve.m_args.size() );
                            ft.m_arg_types.push_back( clone_ty_with(sp, m_resolve_ptr->monomorph_expand_opt(sp, tmp, ve.m_args[0].second, m), clone_self_cb) );
                            if( ve.m_receiver == ::HIR::Function::Receiver::Value ) {
                                ft.m_arg_types[0] = HIR::TypeRef::new_borrow(HIR::BorrowType::Owned, mv$(ft.m_arg_types[0]));
                            }
                            for(unsigned int i = 1; i < ve.m_args.size(); i ++)
                                ft.m_arg_types.push_back( m_resolve_ptr->monomorph_expand(sp, ve.m_args[i].second, m) );
                            // Clear the first argument (the receiver)
                            ::HIR::TypeRef  fcn_type( mv$(ft) );

                            // Detect use of `Self` and don't create the vtable if there is.
                            // NOTE: Associated types where replaced by clone_ty_with
                            if( visit_ty_with(fcn_type, [&](const auto& t){ return (t == ::HIR::TypeRef::new_self()); }) )
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
                    if( supertrait_flags ) {
                        supertrait_flags->reserve(tr.m_all_parent_traits.size());
                        for(const auto& st : tr.m_all_parent_traits) {
                            auto self = ::HIR::TypeRef::new_self();
                            auto st_mono = MonomorphStatePtr(&self, &trait_path.m_params, nullptr).monomorph_traitpath(sp, st, false);
                            // NOTE: Doesn't trigger non-object-safe
                            supertrait_flags->push_back( add_ents_from_trait(*st.m_trait_ptr, st.m_path, nullptr) );
                        }
                    }
                    return true;
                }
            };

            VtableConstruct vtc { this, &resolve, &tr, {} };
            // - Drop glue pointer
            ::HIR::TypeData_FunctionPointer ft;
            ft.is_unsafe = false;
            ft.is_variadic = false;
            ft.m_abi = RcString::new_interned(ABI_RUST);
            ft.m_rettype = ::HIR::TypeRef::new_unit();
            ft.m_arg_types.push_back( ::HIR::TypeRef::new_pointer(::HIR::BorrowType::Owned, ::HIR::TypeRef::new_unit()) );
            vtc.fields.push_back(::std::make_pair( RcString::new_interned("#drop_glue"), ::HIR::VisEnt<::HIR::TypeRef> { ::HIR::Publicity::new_none(), ::HIR::TypeRef(mv$(ft)) } ));
            // - Size of data
            vtc.fields.push_back(::std::make_pair( RcString::new_interned("#size"), ::HIR::VisEnt<::HIR::TypeRef> { ::HIR::Publicity::new_none(), ::HIR::CoreType::Usize } ));
            // - Alignment of data
            vtc.fields.push_back(::std::make_pair( RcString::new_interned("#align"), ::HIR::VisEnt<::HIR::TypeRef> { ::HIR::Publicity::new_none(), ::HIR::CoreType::Usize } ));
            // - Add methods
            ::std::vector<bool> supertrait_flags;
            if( ! vtc.add_ents_from_trait(tr, trait_path, &supertrait_flags) || has_conflicting_aty_name )
            {
                tr.m_value_indexes.clear();
                tr.m_type_indexes.clear();
                return ;
            }
            tr.m_vtable_parent_traits_start = vtc.fields.size();
            // Add parent vtables too.
            for(size_t i = 0; i < tr.m_all_parent_traits.size(); i ++ )
            {
                const auto& pt = tr.m_all_parent_traits[i];
                auto parent_vtable_spath = pt.m_path.m_path;
                parent_vtable_spath.update_last_component( RcString::new_interned(FMT( parent_vtable_spath.components().back().c_str() << "#vtable" )) );
                auto parent_vtable_path = ::HIR::GenericPath(mv$(parent_vtable_spath), pt.m_path.m_params.clone());
                auto ty = true || supertrait_flags[i]
                    ? ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::TypeRef::new_path(mv$(parent_vtable_path), {}) )
                    : ::HIR::TypeRef::new_unit()
                    ;
                vtc.fields.push_back(::std::make_pair(
                    RcString::new_interned(FMT("#parent_" << i)),
                    ::HIR::VisEnt<::HIR::TypeRef> { ::HIR::Publicity::new_none(), mv$(ty) }
                    ));
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
                    ::HIR::Path path( ::HIR::TypeRef::new_self(), trait_path.clone(), ty.first );
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
                RcString::new_interned("vtable#"),
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
                impl.m_statics.insert(::std::make_pair( RcString::new_interned("vtable#"), ::HIR::TraitImpl::ImplEnt<::HIR::Static> { true, ::HIR::Static {
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

    class FixupVisitor:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
    public:
        FixupVisitor(const ::HIR::Crate& crate):
            m_crate(crate)
        {
        }

        void visit_struct(HIR::ItemPath ip, HIR::Struct& str)
        {
            static Span sp;
            auto p = std::strchr(ip.name, '#');
            if( p && std::strcmp(p, "#vtable") == 0 )
            {
                auto trait_path = ip.parent->get_simple_path();
                trait_path += RcString::new_interned(ip.name, p - ip.name);
                const auto& trait = m_crate.get_trait_by_path(sp, trait_path);

                auto& fields = str.m_data.as_Named();
                for(size_t i = 0; i < trait.m_all_parent_traits.size(); i ++)
                {
                    const auto& pt = trait.m_all_parent_traits[i];
                    const auto& parent_trait = *pt.m_trait_ptr;
                    auto& fld_ty = fields[trait.m_vtable_parent_traits_start + i].second.ent;
                    DEBUG(pt << " " << fld_ty);

                    if( parent_trait.m_vtable_path == HIR::SimplePath() ) {
                        // Not object safe, so clear this entry
                        fld_ty = ::HIR::TypeRef::new_unit();
                    }
                    else {
                        auto& te = fld_ty.data_mut().as_Borrow().inner.data_mut().as_Path();
                        auto& vtable_gpath = te.path.m_data.as_Generic();
                        te.binding = &m_crate.get_struct_by_path(sp, vtable_gpath.m_path);

                        for(const auto& aty_idx : parent_trait.m_type_indexes)
                        {
                            if( vtable_gpath.m_params.m_types.size() <= aty_idx.second ) {
                                vtable_gpath.m_params.m_types.resize( aty_idx.second+1 );
                            }
                            auto& slot = vtable_gpath.m_params.m_types[aty_idx.second];
                            // If this associated type is in the trait path `pt`
                            auto it = pt.m_type_bounds.find( aty_idx.first );
                            if( it != pt.m_type_bounds.end() ) {
                                slot = it->second.type.clone();
                            }
                            // If this type is not in the trait path, then check if it has a defined generic
                            else if( trait.m_type_indexes.count(aty_idx.first) != 0 ) {
                                slot = HIR::TypeRef(RcString(), trait.m_type_indexes.at(aty_idx.first));
                            }
                            else {
                                // Otherwise, it has to have been defined in another parent trait
                                const HIR::GenericPath* gp = nullptr;
                                for( const auto& pptrait_path : parent_trait.m_all_parent_traits ) {
                                    if( pptrait_path.m_trait_ptr->m_types.count(aty_idx.first) != 0 ) {
                                        // Found the trait that defined this ATY
                                        DEBUG("Found " << aty_idx.first << " in " << pptrait_path);
                                        gp = &pptrait_path.m_path;
                                    }
                                }
                                ASSERT_BUG(sp, gp, "Failed to a find trait that defined " << aty_idx.first << " in " << pt.m_path.m_path);

                                // Monomorph into the top trait
                                auto gp_mono = MonomorphStatePtr(nullptr, &pt.m_path.m_params, nullptr).monomorph_genericpath(sp, *gp);
                                // Search the parent list
                                const HIR::TraitPath* p = nullptr;
                                for(const auto& pt : trait.m_all_parent_traits) {
                                    if( pt.m_path == gp_mono ) {
                                        p = &pt;
                                    }
                                }
                                ASSERT_BUG(sp, p, "Failed to find " << gp_mono << " in parent trait list for " << trait_path);
                                auto it = p->m_type_bounds.find( aty_idx.first );
                                ASSERT_BUG(sp, it != p->m_type_bounds.end(), "Failed to find " << aty_idx.first << " in " << *p);
                                slot = it->second.type.clone();
                            }
                        }
                    }
                }
            }
        }
    };
} // namespace

void HIR_Expand_VTables(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );

    FixupVisitor fv(crate);
    fv.visit_crate(crate);
}

