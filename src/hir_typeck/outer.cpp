/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/output.cpp
 * - Typechecking at the module level (no inferrence)
 */
#include <hir/hir.hpp>
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>

namespace {

    const ::HIR::GenericParams& get_params_for_item(const Span& sp, const ::HIR::Crate& crate, const ::HIR::SimplePath& path, ::HIR::Visitor::PathContext pc)
    {
        // Support for enum variants
        if( path.components().size() > 1 )
        {
            const auto& pitem = crate.get_typeitem_by_path(sp, path, false, true);
            if(pitem.is_Enum() ) {
                return pitem.as_Enum().m_params;
            }
        }

        switch( pc )
        {
        case ::HIR::Visitor::PathContext::VALUE: {
            const auto& item = crate.get_valitem_by_path(sp, path);

            TU_MATCH( ::HIR::ValueItem, (item), (e),
            (Import,
                BUG(sp, "Value path pointed to import - " << path << " = " << e.path);
                ),
            (Function,
                return e.m_params;
                ),
            (Constant,
                return e.m_params;
                ),
            (Static,
                // TODO: Return an empty set?
                BUG(sp, "Attepted to get parameters for static " << path);
                ),
            (StructConstructor,
                return get_params_for_item(sp, crate, e.ty, ::HIR::Visitor::PathContext::TYPE);
                ),
            (StructConstant,
                return get_params_for_item(sp, crate, e.ty, ::HIR::Visitor::PathContext::TYPE);
                )
            )
            } break;
        case ::HIR::Visitor::PathContext::TRAIT:
            // TODO: treat PathContext::TRAIT differently
        case ::HIR::Visitor::PathContext::TYPE: {
            const auto& item = crate.get_typeitem_by_path(sp, path);

            TU_MATCH( ::HIR::TypeItem, (item), (e),
            (Import,
                BUG(sp, "Type path pointed to import - " << path);
                ),
            (TypeAlias,
                BUG(sp, "Type path pointed to type alias - " << path);
                ),
            (TraitAlias,
                BUG(sp, "Type path pointed to trait alias - " << path);
                ),
            (ExternType,
                static ::HIR::GenericParams empty_params;
                return empty_params;
                ),
            (Module,
                BUG(sp, "Type path pointed to module - " << path);
                ),
            (Struct,
                return e.m_params;
                ),
            (Enum,
                return e.m_params;
                ),
            (Union,
                return e.m_params;
                ),
            (Trait,
                return e.m_params;
                )
            )
            } break;
        }
        throw "";

    }

    class Visitor:
        public ::HIR::Visitor
    {
        ::HIR::Crate& crate;
        StaticTraitResolve  m_resolve;

        const ::HIR::Trait* m_current_trait = nullptr;
        const ::HIR::ItemPath* m_current_trait_path = nullptr;


        ::HIR::GenericParams* m_cur_params = nullptr;
        unsigned m_cur_params_level = 0;
        ::HIR::ItemPath* m_fcn_path = nullptr;
        ::HIR::Function* m_fcn_ptr = nullptr;
        unsigned int m_fcn_erased_count = 0;

        ::std::vector< ::HIR::TypeRef* >    m_self_types;
        ::std::vector< ::HIR::LifetimeRef* >    m_current_lifetime;

        typedef ::std::vector< ::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >   t_trait_imports;
        t_trait_imports m_traits;
    public:
        Visitor(::HIR::Crate& crate):
            crate(crate),
            m_resolve(crate)
        {
        }

    private:
        struct ModTraitsGuard {
            Visitor* v;
            t_trait_imports old_imports;

            ~ModTraitsGuard() {
                this->v->m_traits = mv$(this->old_imports);
            }
        };
        ModTraitsGuard push_mod_traits(const ::HIR::Module& mod) {
            static Span sp;
            DEBUG("");
            auto rv = ModTraitsGuard {  this, mv$(this->m_traits)  };
            for( const auto& trait_path : mod.m_traits ) {
                DEBUG("- " << trait_path);
                m_traits.push_back( ::std::make_pair( &trait_path, &this->crate.get_trait_by_path(sp, trait_path) ) );
            }
            return rv;
        }

#if 0
        void update_self_type(const Span& sp, ::HIR::TypeRef& ty) const
        {
            struct H {
                static void handle_pathparams(const Visitor& self, const Span& sp, ::HIR::PathParams& pp) {
                    for(auto& typ : pp.m_types)
                        self.update_self_type(sp, typ);
                }
            };

            TU_MATCH(::HIR::TypeData, (ty.data_mut()), (e),
            (Generic,
                if(e.name == "Self" || e.binding == GENERIC_Self) {
                    if( m_self_types.empty() )
                        ERROR(sp, E0000, "Self appeared in unexpected location");
                    if( !m_self_types.back() )
                        ERROR(sp, E0000, "Self appeared in unexpected location");
                    ty = m_self_types.back()->clone();
                    return;
                }
                ),

            (Infer,
                ),
            (Diverge,
                ),
            (Primitive,
                ),
            (Path,
                TU_MATCHA( (e.path.m_data), (pe),
                (Generic,
                    H::handle_pathparams(*this, sp, pe.m_params);
                    ),
                (UfcsKnown,
                    update_self_type(sp, pe.type);
                    H::handle_pathparams(*this, sp, pe.trait.m_params);
                    H::handle_pathparams(*this, sp, pe.params);
                    ),
                (UfcsInherent,
                    update_self_type(sp, pe.type);
                    H::handle_pathparams(*this, sp, pe.params);
                    ),
                (UfcsUnknown,
                    update_self_type(sp, pe.type);
                    H::handle_pathparams(*this, sp, pe.params);
                    )
                )
                ),
            (TraitObject,
                // NOTE: Can't mention Self anywhere
                TODO(sp, "TraitObject - " << ty);
                ),
            (ErasedType,
                TODO(sp, "update_self_type - ErasedType");
                ),
            (Tuple,
                for(auto& sty : e)
                    update_self_type(sp, sty);
                ),
            (Array,
                update_self_type(sp, e.inner);
                ),
            (Slice,
                update_self_type(sp, e.inner);
                ),
            (Borrow,
                update_self_type(sp, e.inner);
                ),
            (Pointer,
                update_self_type(sp, e.inner);
                ),
            (Function,
                TODO(sp, "update_self_type - Function");
                ),
            (Closure,
                TODO(sp, "update_self_type - Closure");
                ),
            (Generator,
                TODO(sp, "update_self_type - Generator?");
                )
            )
        }
#endif
        void check_parameters(const Span& sp, const ::HIR::GenericParams& param_def,  ::HIR::PathParams& param_vals)
        {
            MonomorphStatePtr ms(m_self_types.empty() ? nullptr : m_self_types.back(), &param_vals, nullptr);


            if( param_vals.m_lifetimes.size() == 0 ) {
                param_vals.m_lifetimes.resize( param_def.m_lifetimes.size() );
            }
            if( param_vals.m_lifetimes.size() != param_def.m_lifetimes.size() ) {
                ERROR(sp, E0000, "Incorrect lifetime param count, expected " << param_def.m_lifetimes.size() << ", got " << param_vals.m_lifetimes.size());
            }

            while( param_vals.m_types.size() < param_def.m_types.size() ) {
                unsigned int i = param_vals.m_types.size();
                const auto& ty_def = param_def.m_types[i];
                if( ty_def.m_default.data().is_Infer() ) {
                    ERROR(sp, E0000, "Unspecified parameter with no default - " << param_def.fmt_args() << " with " << param_vals);
                }

                // Replace and expand
                param_vals.m_types.push_back( ms.monomorph_type(sp, ty_def.m_default) );
                DEBUG("Add missing param (using default): " << param_vals.m_types.back());
            }

            if( param_vals.m_types.size() != param_def.m_types.size() ) {
                ERROR(sp, E0000, "Incorrect number of parameters - expected " << param_def.m_types.size() << ", got " << param_vals.m_types.size());
            }

            for(unsigned int i = 0; i < param_vals.m_types.size(); i ++)
            {
                if( param_vals.m_types[i] == ::HIR::TypeRef() ) {
                    // TODO: Why is this pulling in the default? Why not just leave it as-is

                    //if( param_def.m_types[i].m_default == ::HIR::TypeRef() )
                    //    ERROR(sp, E0000, "Unspecified parameter with no default");
                    // TODO: Monomorphise?
                    param_vals.m_types[i] = ms.monomorph_type(sp, param_def.m_types[i].m_default);
                    DEBUG("Update `_` param (using default): " << param_def.m_types[i].m_default << " -> " << param_vals.m_types[i]);
                }
            }

            // TODO: Check generic bounds
            for( const auto& bound : param_def.m_bounds )
            {
                TU_MATCH(::HIR::GenericBound, (bound), (e),
                (Lifetime,
                    ),
                (TypeLifetime,
                    ),
                (TraitBound,
                    // TODO: Check for an implementation of this trait
                    DEBUG("TODO: Check bound " << e.type << " : " << e.trait.m_path);
                    //DEBUG("- " << monomorph_type_with(sp, e.type, monomorph_cb) << " : " << monomorphise_traitpath_with(sp, e.trait, monomorph_cb));
                    ),
                (TypeEquality,
                    // TODO: Check that two types are equal in this case
                    DEBUG("TODO: Check equality bound " << e.type << " == " << e.other_type);
                    )
                )
            }
        }

    public:
        void visit_lifetime(const Span& sp, HIR::LifetimeRef& lft)
        {
            if( !lft.is_param() )
            {
                switch(lft.binding)
                {
                case HIR::LifetimeRef::STATIC:  // 'static
                    break;
                case HIR::LifetimeRef::INFER:   // '_
                    //TODO(sp, "Handle explicitly elided lifetimes");
                    //break;
                case HIR::LifetimeRef::UNKNOWN: // <none>
                    // If there's a current liftime (i.e. we're within a borrow), then use that
                    if( !m_current_lifetime.empty() && m_current_lifetime.back() ) {
                        lft = *m_current_lifetime.back();
                    }
                    // Otherwise, try to make a new one
                    else if( m_cur_params ) {
                        auto idx = m_cur_params->m_lifetimes.size();
                        m_cur_params->m_lifetimes.push_back(HIR::LifetimeDef { RcString::new_interned(FMT("elided#" << idx)) });
                        lft.binding = m_cur_params_level * 256 + idx;
                    }
                    else {
                        //ERROR(sp, E0000, "Unspecified lifetime in outer context");
                        // TODO: Would error here, but don't fully support HKTs (e.g. `Fn(&i32)`)
                    }
                    break;
                default:
                    BUG(sp, "Unexpected lifetime binding - " << lft);
                }
            }
        }
        void visit_path_params(::HIR::PathParams& pp) override
        {
            static Span _sp;
            const Span& sp = _sp;

            for(auto& lft : pp.m_lifetimes) {
                visit_lifetime(sp, lft);
            }

            HIR::Visitor::visit_path_params(pp);
        }
        void visit_type(::HIR::TypeRef& ty) override
        {
            static Span _sp;
            const Span& sp = _sp;

            // Lifetime elision logic!
            if(auto* e = ty.data_mut().opt_Borrow())
            {
                visit_lifetime(sp, e->lifetime);
                m_current_lifetime.push_back(&e->lifetime);
            }

            auto self = ::HIR::TypeRef::new_self();
            if( ty.data().is_ErasedType() ) {
                m_self_types.push_back(&self);
            }

            auto saved_params = std::make_pair(m_cur_params, m_cur_params_level);
            if(auto* e = ty.data_mut().opt_Function()) {
                m_cur_params = &e->hrls;
                m_cur_params_level = 3;
            }

            ::HIR::Visitor::visit_type(ty);

            m_cur_params       = saved_params.first ;
            m_cur_params_level = saved_params.second;

            if( ty.data().is_ErasedType() ) {
                m_self_types.pop_back();
            }

            if(/*const auto* e =*/ ty.data().opt_Borrow())
            {
                m_current_lifetime.pop_back();
            }

            #if 0
            if( const auto* te = ty.m_data.opt_Generic() )
            {
                if(te->name == "Self" && te->binding == GENERIC_Self) {
                    if( m_self_types.empty() )
                        ERROR(sp, E0000, "Self appeared in unexpected location");
                    if( !m_self_types.back() )
                        ERROR(sp, E0000, "Self appeared in unexpected location");
                    ty = m_self_types.back()->clone();
                }
            }
            #endif

            if(auto* e = ty.data().opt_Path())
            {
                TU_MATCH( ::HIR::Path::Data, (e->path.m_data), (pe),
                (Generic,
                    ),
                (UfcsUnknown,
                    TODO(sp, "Should UfcsKnown be encountered here?");
                    ),
                (UfcsInherent,
                    TODO(sp, "Locate impl block for UFCS Inherent");
                    ),
                (UfcsKnown,
                    TRACE_FUNCTION_FR("UfcsKnown - " << ty, ty);
                    m_resolve.expand_associated_types(sp, ty);
                    )
                )
            }


            if(auto* e = ty.data_mut().opt_TraitObject()) {
                visit_lifetime(sp, e->m_lifetime);
            }

            // If an ErasedType is encountered, check if it has an origin set.
            if(auto* e = ty.data_mut().opt_ErasedType())
            {
                for(auto& lft : e->m_lifetimes)
                    visit_lifetime(sp, lft);
                if( auto* ee = e->m_inner.opt_Fcn() )
                {
                    DEBUG("Set origin of ErasedType - " << ty);
                    DEBUG("&ty.data() = " << &ty.data());
                    // If not, figure out what to do with it

                    // If the function path is set, we're processing the return type of a function
                    // - Add this to the list of erased types associated with the function
                    if( ee->m_origin != HIR::SimplePath() )
                    {
                        // Already set, somehow (maybe we're visiting the function after expansion)
                    }
                    else if( m_fcn_path )
                    {
                        assert(m_fcn_ptr);
                        DEBUG(*m_fcn_path << " " << m_fcn_erased_count);

                        ::HIR::PathParams    params = m_fcn_ptr->m_params.make_nop_params(1);
                        // Populate with function path
                        ee->m_origin = m_fcn_path->get_full_path();
                        TU_MATCH_HDRA( (ee->m_origin.m_data), {)
                        TU_ARMA(Generic, e2) {
                            e2.m_params = mv$(params);
                            }
                        TU_ARMA(UfcsInherent, e2) {
                            e2.params = mv$(params);
                            // Impl params, just directly references the parameters.
                            // - Downstream monomorph will fix that
                            e2.impl_params = m_resolve.m_impl_generics->make_nop_params(0);
                            }
                        TU_ARMA(UfcsKnown, e2) {
                            e2.params = mv$(params);
                            }
                        TU_ARMA(UfcsUnknown, e2) {
                            throw "";
                            }
                        }
                        ee->m_index = m_fcn_erased_count++;
                    }
                    // If the function _pointer_ is set (but not the path), then we're in the function arguments
                    // - Add a un-namable generic parameter (TODO: Prevent this from being explicitly set when called)
                    else if( m_fcn_ptr )
                    {
                        size_t idx = m_fcn_ptr->m_params.m_types.size();
                        auto name = RcString::new_interned(FMT("erased$" << idx));
                        auto new_ty = ::HIR::TypeRef( name, 256 + idx );
                        m_fcn_ptr->m_params.m_types.push_back({ name, ::HIR::TypeRef(), e->m_is_sized });
                        for( auto& trait : e->m_traits )
                        {
                            struct M: MonomorphiserNop {
                                const HIR::TypeRef& new_ty;
                                M(const HIR::TypeRef& ty): new_ty(ty) {}
                                ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& ty) const override {
                                    if( ty.binding == GENERIC_ErasedSelf ) {
                                        return new_ty.clone();
                                    }
                                    return HIR::TypeRef(ty);
                                }
                            } m { new_ty };
                            // TODO: Monomorph the trait to replace `Self` with this generic?
                            // - Except, that should it be?
                            m_fcn_ptr->m_params.m_bounds.push_back(::HIR::GenericBound::make_TraitBound({
                                nullptr,
                                new_ty.clone(),
                                m.monomorph_traitpath(sp, trait, false)
                                }));
                        }
                        for(const auto& lft : e->m_lifetimes)
                        {
                            m_fcn_ptr->m_params.m_bounds.push_back(::HIR::GenericBound::make_TypeLifetime({
                                new_ty.clone(),
                                lft
                                }));
                        }
                        ty = ::std::move(new_ty);
                    }
                    else
                    {
                        // TODO: If we're in a top-level `type`, then it must be used as the return type of a function.
                        // https://rust-lang.github.io/rfcs/2515-type_alias_impl_trait.html#type-alias
                        ERROR(sp, E0000, "Use of an erased type outside of a function return - " << ty);
                    }
                }
            }
        }

        void visit_generic_path(::HIR::GenericPath& p, PathContext pc) override
        {
            static Span sp;
            TRACE_FUNCTION_F("p = " << p);
            const auto& params = get_params_for_item(sp, crate, p.m_path, pc);
            auto& args = p.m_params;

            check_parameters(sp, params, args);
            DEBUG("p = " << p);

            ::HIR::Visitor::visit_generic_path(p, pc);
        }

    private:
        bool locate_trait_item_in_bounds(const Span& sp, ::HIR::Visitor::PathContext pc,  const ::HIR::TypeRef& tr, const ::HIR::GenericParams& params,  ::HIR::Path::Data& pd) {
            //const auto& name = pd.as_UfcsUnknown().item;
            for(const auto& b : params.m_bounds)
            {
                TU_IFLET(::HIR::GenericBound, b, TraitBound, e,
                    DEBUG("- " << e.type << " : " << e.trait.m_path);
                    if( e.type == tr ) {
                        DEBUG(" - Match");
                        if( locate_in_trait_and_set(sp, pc, e.trait.m_path, this->crate.get_trait_by_path(sp, e.trait.m_path.m_path),  pd) ) {
                            return true;
                        }
                    }
                );
                // -
            }
            return false;
        }
        static ::HIR::Path::Data get_ufcs_known(::HIR::Path::Data::Data_UfcsUnknown e,  ::HIR::GenericPath trait_path, const ::HIR::Trait& trait)
        {
            return ::HIR::Path::Data::make_UfcsKnown({ mv$(e.type), mv$(trait_path), mv$(e.item), mv$(e.params)} );
        }
        static bool locate_item_in_trait(::HIR::Visitor::PathContext pc, const ::HIR::Trait& trait,  ::HIR::Path::Data& pd)
        {
            const auto& e = pd.as_UfcsUnknown();

            switch(pc)
            {
            case ::HIR::Visitor::PathContext::VALUE:
                if( trait.m_values.find( e.item ) != trait.m_values.end() ) {
                    return true;
                }
                break;
            case ::HIR::Visitor::PathContext::TRAIT:
                break;
            case ::HIR::Visitor::PathContext::TYPE:
                if( trait.m_types.find( e.item ) != trait.m_types.end() ) {
                    return true;
                }
                break;
            }
            return false;
        }
        bool locate_in_trait_and_set(const Span& sp, ::HIR::Visitor::PathContext pc, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait,  ::HIR::Path::Data& pd) {
            if( locate_item_in_trait(pc, trait,  pd) ) {
                pd = get_ufcs_known(mv$(pd.as_UfcsUnknown()), make_generic_path(trait_path.m_path, trait), trait);
                return true;
            }
            // Search all supertraits
            for( const auto& pt : trait.m_all_parent_traits )
            {
                if( locate_item_in_trait(pc, *pt.m_trait_ptr, pd) )
                {
                    pd = get_ufcs_known(mv$(pd.as_UfcsUnknown()), make_generic_path(trait_path.m_path, trait), trait);
                    return true;
                }
            }
            return false;
        }
        bool set_from_impl(const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait,  ::HIR::Path::Data& pd)
        {
            auto& e = pd.as_UfcsUnknown();
            const auto& type = e.type;
            return this->crate.find_trait_impls(trait_path.m_path, type, HIR::ResolvePlaceholdersNop(), [&](const auto& impl) {
                DEBUG("FOUND impl" << impl.m_params.fmt_args() << " " << trait_path.m_path << impl.m_trait_args << " for " << impl.m_type);
                // TODO: Check bounds
                for(const auto& bound : impl.m_params.m_bounds) {
                    DEBUG("- TODO: Bound " << bound);
                    return false;
                }
                pd = get_ufcs_known(mv$(e), make_generic_path(trait_path.m_path, trait), trait);
                return true;
                });
        }
        bool locate_in_trait_impl_and_set(::HIR::Visitor::PathContext pc, const ::HIR::GenericPath& trait_path, const ::HIR::Trait& trait,  ::HIR::Path::Data& pd) {
            auto& e = pd.as_UfcsUnknown();
            if( this->locate_item_in_trait(pc, trait,  pd) ) {
                return this->set_from_impl(trait_path, trait, pd);
            }
            else {
                DEBUG("- Item " << e.item << " not in trait " << trait_path.m_path);
            }


            // Search supertraits (recursively)
            for( const auto& pt : trait.m_all_parent_traits )
            {
                if( this->locate_item_in_trait(pc, *pt.m_trait_ptr, pd) )
                {
                    // TODO: Monomorphise params?
                    return set_from_impl(pt.m_path, *pt.m_trait_ptr, pd);
                }
                else
                {
                }
            }
            return false;
        }
        static ::HIR::GenericPath make_generic_path(::HIR::SimplePath sp, const ::HIR::Trait& trait)
        {
            auto trait_path_g = ::HIR::GenericPath( mv$(sp) );
            for(unsigned int i = 0; i < trait.m_params.m_types.size(); i ++ ) {
                trait_path_g.m_params.m_types.push_back( ::HIR::TypeRef(trait.m_params.m_types[i].m_name, i) );
            }
            return trait_path_g;
        }
        ::HIR::GenericPath get_current_trait_gp() const
        {
            assert(m_current_trait_path);
            assert(m_current_trait);
            auto trait_path = ::HIR::GenericPath( m_current_trait_path->get_simple_path() );
            for(unsigned int i = 0; i < m_current_trait->m_params.m_types.size(); i ++ ) {
                trait_path.m_params.m_types.push_back( ::HIR::TypeRef(m_current_trait->m_params.m_types[i].m_name, i) );
            }
            return trait_path;
        }
        void visit_path_UfcsUnknown(const Span& sp, ::HIR::Path& p, ::HIR::Visitor::PathContext pc)
        {
            TRACE_FUNCTION_FR("UfcsUnknown - p=" << p, p);
            auto& e = p.m_data.as_UfcsUnknown();

            this->visit_type( e.type );
            this->visit_path_params( e.params );

            // Search for matching impls in current generic blocks
            if( m_resolve.m_item_generics != nullptr && locate_trait_item_in_bounds(sp, pc, e.type, *m_resolve.m_item_generics,  p.m_data) ) {
                return ;
            }
            if( m_resolve.m_impl_generics != nullptr && locate_trait_item_in_bounds(sp, pc, e.type, *m_resolve.m_impl_generics,  p.m_data) ) {
                return ;
            }

            if(const auto* te = e.type.data().opt_Generic())
            {
                // If processing a trait, and the type is 'Self', search for the type/method on the trait
                // - TODO: This could be encoded by a `Self: Trait` bound in the generics, but that may have knock-on issues?
                if( te->name == "Self" && m_current_trait ) {
                    auto trait_path = this->get_current_trait_gp();
                    if( this->locate_in_trait_and_set(sp, pc, trait_path, *m_current_trait,  p.m_data) ) {
                        // Success!
                        return ;
                    }
                }
                ERROR(sp, E0000, "Failed to find impl with '" << e.item << "' for " << e.type);
                return ;
            }
            else {
                // 1. Search for applicable inherent methods (COMES FIRST!)
                if( this->crate.find_type_impls(e.type, HIR::ResolvePlaceholdersNop(), [&](const auto& impl) {
                    DEBUG("- matched inherent impl " << e.type);
                    // Search for item in this block
                    switch( pc )
                    {
                    case ::HIR::Visitor::PathContext::VALUE:
                        if( impl.m_methods.find(e.item) == impl.m_methods.end() ) {
                            return false;
                        }
                        // Found it, just keep going (don't care about details here)
                        break;
                    case ::HIR::Visitor::PathContext::TRAIT:
                    case ::HIR::Visitor::PathContext::TYPE:
                        return false;
                    }

                    return true;
                    }) )
                {
                    auto new_data = ::HIR::Path::Data::make_UfcsInherent({ mv$(e.type), mv$(e.item), mv$(e.params)} );
                    p.m_data = mv$(new_data);
                    DEBUG("- Resolved, replace with " << p);
                    return ;
                }
                // 2. Search all impls of in-scope traits for this method on this type
                for( const auto& trait_info : m_traits )
                {
                    const auto& trait = *trait_info.second;

                    switch(pc)
                    {
                    case ::HIR::Visitor::PathContext::VALUE:
                        if( trait.m_values.find(e.item) == trait.m_values.end() )
                            continue ;
                        break;
                    case ::HIR::Visitor::PathContext::TRAIT:
                    case ::HIR::Visitor::PathContext::TYPE:
                        if( trait.m_types.find(e.item) == trait.m_types.end() )
                            continue ;
                        break;
                    }
                    DEBUG("- Trying trait " << *trait_info.first);

                    auto trait_path = ::HIR::GenericPath( *trait_info.first );
                    for(unsigned int i = 0; i < trait.m_params.m_types.size(); i ++ ) {
                        trait_path.m_params.m_types.push_back( ::HIR::TypeRef() );
                    }

                    // TODO: Search supertraits
                    // TODO: Should impls be searched first, or item names?
                    // - Item names add complexity, but impls are slower
                    if( this->locate_in_trait_impl_and_set(pc, mv$(trait_path), trait,  p.m_data) ) {
                        return ;
                    }
                }
            }

            // Couldn't find it
            ERROR(sp, E0000, "Failed to find impl with '" << e.item << "' for " << e.type << " (in " << p << ")");
        }

    public:
        void visit_expr(HIR::ExprPtr& exp) override
        {
            // No-op
        }

        void visit_path(::HIR::Path& p, ::HIR::Visitor::PathContext pc) override
        {
            //assert(pc == ::HIR::Visitor::PathContext::TYPE);
            TU_MATCH(::HIR::Path::Data, (p.m_data), (e),
            (Generic,
                this->visit_generic_path(e, pc);
                ),
            (UfcsKnown,
                this->visit_type(e.type);
                m_self_types.push_back(&e.type);
                this->visit_generic_path(e.trait, ::HIR::Visitor::PathContext::TRAIT);
                m_self_types.pop_back();
                // TODO: Locate impl block and check parameters
                ),
            (UfcsInherent,
                this->visit_type(e.type);
                // TODO: Locate impl block and check parameters
                ),
            (UfcsUnknown,
                BUG(Span(), "Encountered unknown-trait UFCS path during outer typeck - " << p);
                )
            )
        }

        void visit_params(::HIR::GenericParams& params) override
        {
            TRACE_FUNCTION_F(params.fmt_args());
            for(auto& tps : params.m_types)
                this->visit_type( tps.m_default );

            for(auto& bound : params.m_bounds )
            {
                TU_MATCH_HDRA( (bound), {)
                TU_ARMA(Lifetime, e) {
                    }
                TU_ARMA(TypeLifetime, e) {
                    this->visit_type(e.type);
                    }
                TU_ARMA(TraitBound, e) {
                    this->visit_type(e.type);
                    m_self_types.push_back(&e.type);
                    this->visit_trait_path(e.trait);
                    m_self_types.pop_back();
                    }
                //(NotTrait, e) {
                //    ::HIR::TypeRef  type;
                //    ::HIR::GenricPath    trait;
                //    }),
                TU_ARMA(TypeEquality, e) {
                    this->visit_type(e.type);
                    this->visit_type(e.other_type);
                    }
                }
            }
        }

        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            auto _ = this->push_mod_traits( mod );
            ::HIR::Visitor::visit_module(p, mod);
        }

        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override
        {
            m_current_trait = &item;
            m_current_trait_path = &p;

            auto _ = m_resolve.set_impl_generics(MetadataType::TraitObject, item.m_params);
            auto self = ::HIR::TypeRef::new_self();
            m_self_types.push_back(&self);
            ::HIR::Visitor::visit_trait(p, item);
            m_self_types.pop_back();

            m_current_trait = nullptr;
        }
        void visit_struct(::HIR::ItemPath p, ::HIR::Struct& item) override
        {
            auto _ = m_resolve.set_impl_generics(item.m_struct_markings.dst_type, item.m_params);
            ::HIR::Visitor::visit_struct(p, item);
        }
        void visit_union(::HIR::ItemPath p, ::HIR::Union& item) override
        {
            auto _ = m_resolve.set_impl_generics(MetadataType::None, item.m_params);
            ::HIR::Visitor::visit_union(p, item);
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override
        {
            auto _ = m_resolve.set_impl_generics(MetadataType::None, item.m_params);
            ::HIR::Visitor::visit_enum(p, item);
        }
        void visit_associatedtype(::HIR::ItemPath p, ::HIR::AssociatedType& item) override
        {
            // Push `Self = <Self as CurTrait>::Type` for processing defaults in the bounds.
            auto path_aty = ::HIR::Path( ::HIR::TypeRef::new_self(), this->get_current_trait_gp(), p.get_name() );
            auto ty_aty = ::HIR::TypeRef::new_path( mv$(path_aty), ::HIR::TypePathBinding::make_Opaque({}) );
            m_self_types.push_back(&ty_aty);

            ::HIR::Visitor::visit_associatedtype(p, item);

            m_self_types.pop_back();
        }

        void visit_type_alias(::HIR::ItemPath p, ::HIR::TypeAlias& item) override
        {
            // Ignore type aliases, they don't have to typecheck.
        }


        void add_lifetime_bounds_for_impl_type(const Span& sp, HIR::GenericParams& dst, const ::HIR::TypeRef& ty)
        {
            // REF: rustc-1.29.0-src/src/vendor/clap/src/args/arg.rs:54 - Omitted lifetime bounds

            // https://rust-lang.github.io/rfcs/2089-implied-bounds.html ?
            // HACK: Just grab the lifetime bounds from a path type
            if( ty.data().is_Path() && ty.data().as_Path().path.m_data.is_Generic() )
            {
                const auto& gp = ty.data().as_Path().path.m_data.as_Generic();
                const auto& ti = m_resolve.m_crate.get_typeitem_by_path(sp, gp.m_path);

                const HIR::GenericParams* params = nullptr;
                if(const auto* e = ti.opt_Struct()) {
                    params = &e->m_params;
                }
                else if(const auto* e = ti.opt_Enum()) {
                    params = &e->m_params;
                }
                else if(const auto* e = ti.opt_Union()) {
                    params = &e->m_params;
                }
                else {
                    DEBUG("TODO: Obtain bounds from " << ti.tag_str());
                }

                if(params)
                {
                    MonomorphStatePtr   ms(nullptr, &gp.m_params, nullptr);
                    for(const auto& b : params->m_bounds)
                    {
                        if( const auto* be = b.opt_Lifetime() ) {
                            dst.m_bounds.push_back(HIR::GenericBound::make_Lifetime({ ms.monomorph_lifetime(sp, be->test), ms.monomorph_lifetime(sp, be->valid_for) }));
                        }
                    }
                }
            }
        }

        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            auto _ = m_resolve.set_impl_generics(impl.m_type, impl.m_params);
            m_self_types.push_back( &impl.m_type );

            // Pre-visit so lifetime elision can work
            {
                m_cur_params = &impl.m_params;
                m_cur_params_level = 0;
                this->visit_type(impl.m_type);
                m_cur_params = nullptr;
            }

            // Propagate bounds from the type
            add_lifetime_bounds_for_impl_type(Span(), impl.m_params, impl.m_type);

            ::HIR::Visitor::visit_type_impl(impl);
            // TODO: Check that the type is valid

            m_self_types.pop_back();
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            static Span sp;
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type);
            auto _ = m_resolve.set_impl_generics(impl.m_type, impl.m_params);
            m_self_types.push_back( &impl.m_type );

            // Pre-visit so lifetime elision can work
            {
                m_cur_params = &impl.m_params;
                m_cur_params_level = 0;
                this->visit_type(impl.m_type);
                this->visit_path_params(impl.m_trait_args);
                m_cur_params = nullptr;
            }

            // Propagate bounds from the type
            add_lifetime_bounds_for_impl_type(Span(), impl.m_params, impl.m_type);

            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
            m_self_types.pop_back();

            // TODO: Check that the type+trait is valid
            // - And fix bad elided liftimes (match annotations if they were elided)
            {
                const auto& trait = m_resolve.m_crate.get_trait_by_path(sp, trait_path);
                for(auto& e : impl.m_methods)
                {
                    auto _ = m_resolve.set_item_generics(e.second.data.m_params);

                    const auto v_it = trait.m_values.find(e.first);
                    if( v_it == trait.m_values.end() || !v_it->second.is_Function() ) {
                        ERROR(sp, E0000, "Trait " << trait_path << " doesn't have a method named " << e.first);
                    }
                    auto& impl_fcn = e.second.data;
                    const auto& trait_fcn = v_it->second.as_Function();

                    auto fcn_params = impl_fcn.m_params.make_nop_params(1);
                    MonomorphStatePtr   ms { &impl.m_type, &impl.m_trait_args, &fcn_params };
                    HIR::TypeRef    tmp;
                    auto maybe_monomorph = [&](const HIR::TypeRef& ty)->const HIR::TypeRef& {
                        if(monomorphise_type_needed(ty)) {
                            tmp = ms.monomorph_type(sp, ty);
                            m_resolve.expand_associated_types(sp, tmp);
                            return tmp;
                        }
                        else {
                            return ty;
                        }
                        };

                    // Check signature
                    // - Includes fixing incorrectly elided lifetimes
                    // ```
                    // trait Foo<T> {
                    //   fn foo(&self, bar: T);
                    // }
                    // impl Foo<&Bar> for Baz {
                    //   fn foo(&self, bar: &Bar) { }
                    // }

                    std::vector<std::string> failures;
                    // -- Generics
                    if( impl_fcn.m_params.m_types.size() != trait_fcn.m_params.m_types.size() ) {
                        failures.push_back(FMT("Mismatched type param count (expected " << trait_fcn.m_params.m_types.size() << ", got " << impl_fcn.m_params.m_types.size() << ")"));
                    }
                    // Different logic for lifetimes, only want to check un-elided lifetimes
                    // - Well, elided lifetimes can overlap non-elided ones (as long as they're identical)
#if 0
                    {
                        auto find_first_elided_idx = [](const HIR::GenericParams& gp) {
                            return std::find_if(gp.m_lifetimes.begin(), gp.m_lifetimes.end(), [](const HIR::LifetimeDef& ld){ return ld.m_name.compare(0, 7, "elided#") == 0; }) - gp.m_lifetimes.begin();
                            };
                        size_t impl_n_unelided  = find_first_elided_idx(impl_fcn .m_params);
                        size_t trait_n_unelided = find_first_elided_idx(trait_fcn.m_params);
                        if( impl_n_unelided != trait_n_unelided ) {
                            failures.push_back(FMT("Mismatched lifetime param count (expected " << trait_n_unelided << ", got " << impl_n_unelided << ")"));
                        }
                    }
#endif
                    if( impl_fcn.m_params.m_values.size() != trait_fcn.m_params.m_values.size() ) {
                        failures.push_back(FMT("Mismatched const param count (expected " << trait_fcn.m_params.m_values.size() << ", got " << impl_fcn.m_params.m_values.size() << ")"));
                    }
                    // -- Arguments
                    if( impl_fcn.m_args.size() != trait_fcn.m_args.size() ) {
                        failures.push_back(FMT("Mismatched argument count (expected " << trait_fcn.m_args.size() << ", got " << impl_fcn.m_args.size() << ")"));
                    }
                    if( impl_fcn.m_receiver != trait_fcn.m_receiver ) {
                        failures.push_back(FMT("Receiver type")); //"(expected " << trait_fcn.m_receiver << ", got " << impl_fcn.m_receiver));
                    }
                    for( size_t i = 0; i < std::min(impl_fcn.m_args.size(), trait_fcn.m_args.size()); i ++ )
                    {
                        if( !(i == 0 && (trait_fcn.m_receiver == HIR::Function::Receiver::Free || impl_fcn.m_receiver == HIR::Function::Receiver::Free)) ) {
                            // Check the type.
                            // - Also, fix lifetime elision?
                            const auto& exp_ty = maybe_monomorph(trait_fcn.m_args[i].second);
                            /*const*/ auto& has_ty = impl_fcn.m_args[i].second;

                            if( exp_ty != has_ty ) {
                                failures.push_back(FMT("Argument " << 1+i << " mismatch - expected " << exp_ty << ", got " << has_ty));
                            }
                        }
                    }
                    const auto& exp_ret_ty = maybe_monomorph(trait_fcn.m_return);
                    if( impl_fcn.m_return != exp_ret_ty ) {
                        failures.push_back(FMT("Mismatched return type (expected " << exp_ret_ty << ", got " << impl_fcn.m_return << ")"));
                    }

                    //if( impl_fcn.m_params.m_lifetimes.size() != trait_fcn.m_params.m_lifetimes.size() ) {
                    //    failures.push_back(FMT("Mismatched lifetime param count (expected " << trait_fcn.m_params.m_lifetimes.size() << ", got " << impl_fcn.m_params.m_lifetimes.size() << ")"));
                    //}

                    if( !failures.empty() ) {
                        ERROR(sp, E0000, "Method " << e.first << " doesn't match trait:\n"
                            << FMT_CB(os, for(const auto& f : failures) os << "- " << f << "\n")
                            << "Trait:\n" << FMT_CB(os, {
                                os << "    fn " << e.first << trait_fcn.m_params.fmt_args() << "(";
                                for(const auto& a : trait_fcn.m_args)
                                {
                                    os << a.first << ": " << maybe_monomorph(a.second) << ", ";
                                }
                                os << ")\n";
                                os << "    -> " << maybe_monomorph(trait_fcn.m_return) << "\n";
                                os << "    " << trait_fcn.m_params.fmt_bounds();
                                }) << "\n"
                            << "Impl : " << FMT_CB(os, {
                                os << "    fn " << e.first << impl_fcn.m_params.fmt_args() << "(";
                                for(const auto& a : impl_fcn.m_args)
                                {
                                    os << a.first << ": " << a.second << ", ";
                                }
                                os << ")\n";
                                os << "    -> " << impl_fcn.m_return << "\n";
                                os << "    " << impl_fcn.m_params.fmt_bounds();
                                }) << "\n"
                            << "in impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type
                            );
                    }
                    // HACK: Replace all types (which should be functionally identical) so lifetimes match
                    // - This is needed for monomorphisation to work properly?
                    // REF: rustc-1.29.0/src/vendor/serde/src/private/de.rs:1379
                    // Counter-ref: rustc-1.54.0
                    // Update AFTER the checks
                    DEBUG("Replace generic block's lifetimes with " << trait_fcn.m_params.fmt_args());
                    impl_fcn.m_params.m_lifetimes = trait_fcn.m_params.m_lifetimes;
                    // Replace the lifetime bounds too (undoes some potential confusion from elision)
                    {
                        auto& bl = impl_fcn.m_params.m_bounds;
                        bl.erase( std::remove_if(bl.begin(), bl.end(), [](const HIR::GenericBound& b){ return b.is_Lifetime(); }), bl.end());
                    }
                    for(const auto& b : trait_fcn.m_params.m_bounds) {
                        TU_MATCH_HDRA( (b), { )
                        default:
                            break;
                        TU_ARMA(TypeLifetime, be) {
                            impl_fcn.m_params.m_bounds.push_back(::HIR::GenericBound::make_TypeLifetime({ ms.monomorph_type(sp, be.type), ms.monomorph_lifetime(sp, be.valid_for) }));
                            }
                        TU_ARMA(Lifetime, be) {
                            impl_fcn.m_params.m_bounds.push_back(::HIR::GenericBound::make_Lifetime({ ms.monomorph_lifetime(sp, be.test), ms.monomorph_lifetime(sp, be.valid_for) }));
                            }
                        }
                    }

                    impl_fcn.m_return = exp_ret_ty.clone();
                    // HACK: Clone the expected type, so the lifetimes match.
                    for( size_t i = 0; i < std::min(impl_fcn.m_args.size(), trait_fcn.m_args.size()); i ++ )
                    {
                        impl_fcn.m_args[i].second = m_resolve.monomorph_expand(sp, trait_fcn.m_args[i].second, ms);
                    }
                    DEBUG("Updated < " << impl.m_type << " as " << trait_path << impl.m_trait_args << " >::" << e.first);

                    DEBUG(FMT_CB(os, {
                        os << "fn " << e.first << impl_fcn.m_params.fmt_args() << "(";
                        for(const auto& a : impl_fcn.m_args)
                        {
                            os << a.first << ": " << a.second << ", ";
                        }
                        os << ")";
                        os << impl_fcn.m_params.fmt_bounds();
                        }));
                }
                for(const auto& e : impl.m_constants)
                {
                    const auto& vi = trait.m_values.at(e.first);
                    if(!vi.is_Constant()) {
                        ERROR(sp, E0000, "Trait " << trait_path << " doesn't have a constant named " << e.first);
                    }
                    const auto& impl_const = e.second.data;
                    const auto& trait_const = vi.as_Constant();

                    // Check type
                }
                for(const auto& e : impl.m_statics)
                {
                    const auto& vi = trait.m_values.at(e.first);
                    if(!vi.is_Static()) {
                        ERROR(sp, E0000, "Trait " << trait_path << " doesn't have a static named " << e.first);
                    }
                    const auto& impl_static = e.second.data;
                    const auto& trait_static = vi.as_Static();

                    // Check type
                }
                for(const auto& e : trait.m_types)
                {
                    const auto& trait_type = trait.m_types.at(e.first);
                    const auto& impl_type = e.second;

                    // Check that the bounds fit
                }
            }
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type << " { }");
            auto _ = m_resolve.set_impl_generics(impl.m_type, impl.m_params);
            m_self_types.push_back( &impl.m_type );

            // Pre-visit so lifetime elision can work
            {
                m_cur_params = &impl.m_params;
                m_cur_params_level = 0;
                this->visit_type(impl.m_type);
                this->visit_path_params(impl.m_trait_args);
                m_cur_params = nullptr;
            }

            // Propagate bounds from the type/trait
            add_lifetime_bounds_for_impl_type(Span(), impl.m_params, impl.m_type);

            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
            // TODO: Check that the type+trait is valid

            m_self_types.pop_back();
        }

        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override {
            TRACE_FUNCTION_F(p);

            auto _ = m_resolve.set_item_generics(item.m_params);
            // NOTE: Superfluous... except that it makes the params valid for the return type.
            visit_params(item.m_params);

            m_fcn_ptr = &item;
            auto first_elided_lifetime_idx = item.m_params.m_lifetimes.size();

            // Visit arguments
            // - Used to convert `impl Trait` in argument position into generics
            // - Done first so the path in return-position `impl Trait` is valid
            m_cur_params = &item.m_params;
            m_cur_params_level = 1;
            for(auto& arg : item.m_args)
            {
                TRACE_FUNCTION_F("ARG " << arg);
                visit_type(arg.second);
            }
            m_cur_params = nullptr;

            // Get output lifetime
            // - Try `&self`'s lifetime (if it was an elided lifetime)
            HIR::LifetimeRef    elided_output_lifetime;
            if( item.m_receiver != HIR::Function::Receiver::Free ) {
                if( const auto* b = item.m_args[0].second.data().opt_Borrow() ) {
                    // If this was an elided lifetime.
                    if( b->lifetime.is_param() && (b->lifetime.binding >> 8) == 1 && (b->lifetime.binding & 0xFF) > first_elided_lifetime_idx ) {
                        elided_output_lifetime = b->lifetime;
                    }
                }
            }
            // - OR, look for only one elided lifetime
            if( elided_output_lifetime == HIR::LifetimeRef() ) {
                if( item.m_params.m_lifetimes.size() == first_elided_lifetime_idx + 1 ) {
                    elided_output_lifetime = HIR::LifetimeRef(256 + first_elided_lifetime_idx);
                }
            }
            // If present, set it (push to the stack)
            assert(m_current_lifetime.empty());
            if( elided_output_lifetime != HIR::LifetimeRef() ) {
                m_current_lifetime.push_back(&elided_output_lifetime);
            }

            // Visit return type (populates path for `impl Trait` in return position
            m_fcn_path = &p;
            m_fcn_erased_count = 0;
            {
                TRACE_FUNCTION_F("RET " << item.m_return);
                visit_type(item.m_return);
            }
            m_fcn_path = nullptr;
            m_fcn_ptr = nullptr;

            if( elided_output_lifetime != HIR::LifetimeRef() ) {
                m_current_lifetime.pop_back();
            }
            assert(m_current_lifetime.empty());

            if( item.m_receiver == HIR::Function::Receiver::Custom ) {
                this->visit_type(item.m_receiver_type);
            }
            ::HIR::Visitor::visit_function(p, item);
        }
    };
}


void Typecheck_ModuleLevel(::HIR::Crate& crate)
{
    Visitor v { crate };
    v.visit_crate(crate);
}

