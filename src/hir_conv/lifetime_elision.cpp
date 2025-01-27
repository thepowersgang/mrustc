/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_conv/lifetime_elision.cpp
 * - 
 */
#include <hir/hir.hpp>
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>
#include <hir/expr.hpp> // ExprVisitor
#include "main_bindings.hpp"

namespace
{
    /// <summary>
    /// A class that acts like StaticTraitResolve, but only holds params
    /// </summary>
    struct MiniResolve
    {
        const HIR::Crate& m_crate;
        const HIR::GenericParams* m_impl_generics;
        const HIR::GenericParams* m_item_generics;

        MiniResolve(const HIR::Crate& crate)
            : m_crate(crate)
        {
        }

        NullOnDrop<const ::HIR::GenericParams> set_impl_generics(const ::HIR::GenericParams& gps) {
            m_impl_generics = &gps;
            return NullOnDrop<const ::HIR::GenericParams>(m_impl_generics);
        }

        NullOnDrop<const ::HIR::GenericParams> set_item_generics(const ::HIR::GenericParams& gps) {
            m_item_generics = &gps;
            return NullOnDrop<const ::HIR::GenericParams>(m_item_generics);
        }
    };
    class Visitor:
        public ::HIR::Visitor
    {
        ::HIR::Crate& crate;
        MiniResolve  m_resolve;

        bool m_in_expr = false;
        bool m_create_elided = false;
        ::HIR::GenericParams* m_cur_params = nullptr;
        unsigned m_cur_params_level = 0;
        ::std::vector< ::HIR::LifetimeRef* >    m_current_lifetime;

        unsigned m_current_depth = 0;
        std::vector<std::pair<unsigned, ::HIR::LifetimeRef*>> m_trait_object_rule;

    public:
        Visitor(::HIR::Crate& crate)
            : crate(crate)
            , m_resolve(crate)
        {
        }

    private:

        struct SavedParams {
            Visitor* parent;
            bool m_create_elided;
            ::HIR::GenericParams* m_cur_params;
            unsigned m_cur_params_level;
            SavedParams(Visitor& parent)
                : parent(&parent)
                , m_create_elided(parent.m_create_elided)
                , m_cur_params(parent.m_cur_params)
                , m_cur_params_level(parent.m_cur_params_level)
            {
            }
            SavedParams(const SavedParams&) = delete;
            SavedParams(SavedParams&& x)
                : parent(x.parent)
                , m_create_elided(x.m_create_elided)
                , m_cur_params(x.m_cur_params)
                , m_cur_params_level(x.m_cur_params_level)
            {
                x.parent = nullptr;
            }
            ~SavedParams() {
                restore();
            }
            void restore() {
                if(parent) {
                    parent->m_create_elided = m_create_elided;
                    parent->m_cur_params = m_cur_params;
                    parent->m_cur_params_level = m_cur_params_level;
                    parent = nullptr;
                }
            }
        };
        SavedParams save_params() {
            return SavedParams(*this);
        }
        void set_params(::HIR::GenericParams* params, unsigned level) {
            m_create_elided = true;
            m_cur_params = params;
            m_cur_params_level = level;
        }
        SavedParams push_params(::HIR::GenericParams& params, unsigned level) {
            auto rv = save_params();
            set_params(&params, level);
            return rv;
        }
        SavedParams push_params(::HIR::GenericParams* params, unsigned level) {
            auto rv = save_params();
            set_params(params, level);
            return rv;
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
                        DEBUG("Use stack: " << lft);
                    }
                    // Otherwise, try to make a new one
                    else if( m_cur_params && m_create_elided ) {
                        auto idx = m_cur_params->m_lifetimes.size();
                        m_cur_params->m_lifetimes.push_back(HIR::LifetimeDef { RcString::new_interned(FMT("elided#" << idx)) });
                        lft.binding = m_cur_params_level * 256 + idx;
                        DEBUG("Create elided lifetime: " << lft << " " << m_cur_params->m_lifetimes.back().m_name);
                    }
                    else if ( m_in_expr ) {
                        // Allow
                    }
                    else {
                        // TODO: Would error here, but there's places where it doesn't quite work.
                        // - E.g. `-> impl Foo` with no input lifetime
                        ERROR(sp, E0000, "Unspecified lifetime in outer context");
                    }
                    break;
                default:
                    BUG(sp, "Unexpected lifetime binding - " << lft);
                }
            }
            else
            {
                // Add implicit bound
                if( m_cur_params ) {
                    if( !m_current_lifetime.empty() && m_current_lifetime.back() && m_current_lifetime.back()->is_param() ) {
                        const auto& outer = *m_current_lifetime.back();
                        //DEBUG("maybe add " << lft << ": " << outer);
                        if( lft != outer
                            && lft.as_param().group() < 2 // I.e. an impl or method param, not HRL or placeholder
                            && outer.as_param().group() < 2
                            // One of the two lifetimes must be from this block?
                            && (lft.as_param().group() == m_cur_params_level || outer.as_param().group() == m_cur_params_level)
                            )
                        {
                            // Add `'this: 'outer`
                            bool found = false;
                            // Only if not a duplicate
                            for(const auto& b : m_cur_params->m_bounds) {
                                if( const auto* be = b.opt_Lifetime() ) {
                                    if( be->test == lft && be->valid_for == outer) {
                                        found = true;
                                        break;
                                    }
                                }
                            }
                            if( !found ) {
                                DEBUG("Push bound " << lft << ": " << outer);
                                m_cur_params->m_bounds.push_back(::HIR::GenericBound::make_Lifetime({ lft, outer }));
                            }
                        }
                    }
                    else {
                        if( m_current_lifetime.empty() ) {
                        }
                        else if( m_current_lifetime.back() ) {
                            //DEBUG("No bound " << lft << ": " << *m_current_lifetime.back());
                        }
                        else {
                            //DEBUG("No bound " << lft << ": nullptr");
                        }
                    }
                }
            }
        }
        bool bound_exists(const HIR::LifetimeRef& test, const HIR::LifetimeRef& valid_for) const {
            if( m_resolve.m_impl_generics ) {
                for(const auto& b : m_resolve.m_impl_generics->m_bounds ) {
                    if(b.is_Lifetime()) DEBUG(b);
                    if( b.is_Lifetime() && b.as_Lifetime().test == test && b.as_Lifetime().valid_for == valid_for ) {
                        return true;
                    }
                }
            }
            if( m_resolve.m_item_generics ) {
                for(const auto& b : m_resolve.m_item_generics->m_bounds ) {
                    if(b.is_Lifetime()) DEBUG(b);
                    if( b.is_Lifetime() && b.as_Lifetime().test == test && b.as_Lifetime().valid_for == valid_for ) {
                        return true;
                    }
                }
            }
            return false;
        }

        void visit_params(::HIR::GenericParams& params) override
        {
            TRACE_FUNCTION_F(params.fmt_args() << params.fmt_bounds());
            for(auto& tps : params.m_types)
                this->visit_type( tps.m_default );
            for(auto& val : params.m_values)
                this->visit_type(val.m_type);
            // The bounds list can grow as inferred lifetime bounds are added, so iterate manually and move the bound in/out to maintain pointer stability
            for(size_t i = 0; i < params.m_bounds.size(); i++) {
                auto bound = std::move(params.m_bounds[i]);
                params.m_bounds[i] = HIR::GenericBound::make_Lifetime({ HIR::LifetimeRef::new_static(), HIR::LifetimeRef::new_static() });
                visit_generic_bound(bound);
                params.m_bounds[i] = std::move(bound);
            }
        }

        void visit_generic_path(::HIR::GenericPath& p, ::HIR::Visitor::PathContext pc) override
        {
            const static Span   sp;
            // Get the type definition and fill in omitted lifetimes
            const HIR::GenericParams* gp = nullptr;
            if( p.m_path.components().size() > 1 ) {
                if( const auto* e = m_resolve.m_crate.get_typeitem_by_path(sp, p.m_path, false, true).opt_Enum() ) {
                    gp = &e->m_params;
                }
            }
            if( !gp ) {
                switch(pc)
                {
                case HIR::Visitor::PathContext::TYPE:
                case HIR::Visitor::PathContext::TRAIT: {
                    const auto& ti = m_resolve.m_crate.get_typeitem_by_path(sp, p.m_path);
                    TU_MATCH_HDRA( (ti), {)
                    TU_ARMA(Import, e) BUG(sp, "Unexpected reference to import - " << p);
                    TU_ARMA(Module, e) BUG(sp, "Unexpected reference to module - " << p);
                    TU_ARMA(TypeAlias, e) { gp = &e.m_params; }
                    TU_ARMA(TraitAlias, e) { gp = &e.m_params; }
                    TU_ARMA(ExternType, e) { gp = nullptr; }
                    TU_ARMA(Enum  , e) { gp = &e.m_params; }
                    TU_ARMA(Struct, e) { gp = &e.m_params; }
                    TU_ARMA(Union , e) { gp = &e.m_params; }
                    TU_ARMA(Trait , e) { gp = &e.m_params; }
                    }
                    } break;
                case HIR::Visitor::PathContext::VALUE: {
                    const auto& vi = m_resolve.m_crate.get_valitem_by_path(sp, p.m_path);
                    TU_MATCH_HDRA( (vi), { )
                    TU_ARMA(Import, e) BUG(sp, "Unexpected reference to import - " << p);
                    TU_ARMA(Constant, e) { gp = nullptr; }
                    TU_ARMA(Static  , e) { gp = nullptr; }
                    TU_ARMA(Function, e) { gp = &e.m_params; }
                    TU_ARMA(StructConstant   , e) { gp = &m_resolve.m_crate.get_struct_by_path(sp, e.ty).m_params; }
                    TU_ARMA(StructConstructor, e) { gp = &m_resolve.m_crate.get_struct_by_path(sp, e.ty).m_params; }
                    }
                    } break;
                }
            }
            if( p.m_params.m_lifetimes.size() < (gp ? gp->m_lifetimes.size() : 0) && m_current_lifetime.size() && m_current_lifetime.back() )
            {
                assert(gp); // Should be non-null because `.size()` is unsigned, and the above is `.size() < 0` if `gp` is null
                DEBUG(p);
                p.m_params.m_lifetimes.resize( gp->m_lifetimes.size() );
                DEBUG(p);
            }
            HIR::Visitor::visit_generic_path(p, pc);
        }

        void visit_path_params(::HIR::PathParams& pp) override
        {
            DEBUG(pp);
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

            auto saved_m_trait_object_rule = m_trait_object_rule.size();
            auto saved_liftime_depth = m_current_lifetime.size();
            auto saved_params = save_params();
            if(m_current_depth == 0 ) {
                DEBUG("> " << ty);
            }
            m_current_depth += 1;

            // Lifetime elision logic!

            if(auto* e = ty.data_mut().opt_Borrow()) {
                visit_lifetime(sp, e->lifetime);
                m_current_lifetime.push_back(&e->lifetime);
                m_trait_object_rule.push_back(::std::make_pair(m_current_depth, &e->lifetime));
            }
            if(auto* e = ty.data_mut().opt_Function()) {
                m_current_lifetime.push_back(nullptr);
                set_params(&e->hrls, 3);
            }
            if(auto* e = ty.data_mut().opt_TraitObject()) {
                // TODO: Create? but what if it's not used?
                if( e->m_trait.m_hrtbs )
                {
                    m_current_lifetime.push_back(nullptr);
                    set_params(&*e->m_trait.m_hrtbs, 3);
                }


                // If neither of those rules apply, then the bounds on the trait are used:
                // - If the trait is defined with a single lifetime bound then that bound is used.
                // - If 'static is used for any lifetime bound then 'static is used.
                // - If the trait has no lifetime bounds, then the lifetime is inferred in expressions and is 'static outside of expressions.
                if( e->m_lifetime.binding == HIR::LifetimeRef::INFER ||  e->m_lifetime.binding == HIR::LifetimeRef::UNKNOWN )
                {
                    struct H {
                        const Span& sp;
                        const HIR::Crate& crate;
                        std::vector<HIR::LifetimeRef>   lifetimes;
                        void visit_trait(const HIR::SimplePath& p, const HIR::PathParams& params) {
                            const auto& t = crate.get_trait_by_path(sp, p);
                            DEBUG(p << " " << t.m_lifetime);
                            if( t.m_lifetime != HIR::LifetimeRef() )
                            {
                                if( t.m_lifetime == HIR::LifetimeRef::new_static() ) {
                                    lifetimes.push_back(t.m_lifetime);
                                    // Early return on 'static, no need to check anything else
                                    return ;
                                }
                                else {
                                    // TODO: Parameters
                                }
                            }
                            // TODO: Monomorph? (for lifetime parameters)
                            for(const auto& st : t.m_parent_traits) {
                                visit_trait(st.m_path.m_path, st.m_path.m_params);
                            }
                        }
                    } h { sp, m_resolve.m_crate };
                    if( e->m_trait.m_path.m_path != HIR::SimplePath() ) {
                        h.visit_trait(e->m_trait.m_path.m_path, e->m_trait.m_path.m_params);
                    }
                    std::sort(h.lifetimes.begin(), h.lifetimes.end());
                    auto new_end = std::unique(h.lifetimes.begin(), h.lifetimes.end());
                    h.lifetimes.erase(new_end, h.lifetimes.end());
                    if( h.lifetimes.empty() ) {
                        // Apply normal elision rules?
                        DEBUG("TraitObject: No available bounds");
                    }
                    else {
                        if( h.lifetimes.size() == 1 || h.lifetimes.back() == HIR::LifetimeRef::new_static() ) {
                            DEBUG("TraitObject: Set lifetime " << h.lifetimes.front() << " from bounds");
                            e->m_lifetime = h.lifetimes.back();
                        }
                        else {
                            // Error?
                            DEBUG("TraitObject: Multiple bounded lifetimes");
                        }
                    }
                }

                // https://doc.rust-lang.org/reference/lifetime-elision.html#default-trait-object-lifetimes
                // If the trait object is used as a type argument of a generic type then the containing type is first used to try to infer a bound.
                // - If there is a unique bound from the containing type then that is the default
                // - If there is more than one bound from the containing type then an explicit bound must be specified

                bool was_static_rule = false;
                // If the lifetime is omitted, or '_
                // ... AND this is within prototype (not in an expression)
                if( (e->m_lifetime.binding == HIR::LifetimeRef::UNKNOWN /*|| e->m_lifetime.binding == HIR::LifetimeRef::INFER*/)
                    //&& m_cur_params
                    //&& m_create_elided    // In arguments
                    && !m_in_expr   // Not in expression
                    )
                {
                    if( !m_trait_object_rule.empty() )
                    {
                        DEBUG("TraitObject: cur=" << m_current_depth << " back.first=" << m_trait_object_rule.back().first);
                        if( m_trait_object_rule.back().first == m_current_depth-1) {
                            if( m_trait_object_rule.back().second ) {
                                const auto& lft = *m_trait_object_rule.back().second;
                                e->m_lifetime = lft;
                                was_static_rule = (lft.binding == HIR::LifetimeRef::STATIC);
                                DEBUG("TraitObject: Set lifetime " << e->m_lifetime << " - trait object rule");
                            }
                        }
                    }
                }
                if( (was_static_rule || e->m_lifetime.binding == HIR::LifetimeRef::UNKNOWN /*|| e->m_lifetime.binding == HIR::LifetimeRef::INFER*/)
                    && !m_in_expr   // Not in expression
                    )
                {
                    // HACK: If the trait has a lifeime param, use that
                    if( !e->m_trait.m_hrtbs && e->m_trait.m_path.m_params.m_lifetimes.size() == 1 ) {
                        e->m_lifetime = e->m_trait.m_path.m_params.m_lifetimes[0];
                        DEBUG("TraitObject: Set to first/only lifetime param of data trait: " << e->m_lifetime);
                    }
                }
                // If there is no available rule (i.e. not in a borrow), and the lifetime was omitted (not just '_), then fill in 'static
                if( false && m_trait_object_rule.empty() && e->m_lifetime.binding == HIR::LifetimeRef::UNKNOWN && !m_in_expr && !(m_create_elided && m_cur_params) )
                {
                    e->m_lifetime = HIR::LifetimeRef::new_static();
                    DEBUG("TraitObject: Set lifetime " << e->m_lifetime << " - hack");
                }
            }

            if(auto* e = ty.data_mut().opt_Path()) {
                // Expand default lifetime params
                if( auto* p = e->path.m_data.opt_Generic() )
                {
                    const HIR::TypeItem& ti = m_resolve.m_crate.get_typeitem_by_path(sp, p->m_path);
                    const HIR::GenericParams* gp = nullptr;
                    TU_MATCH_HDRA( (ti), { )
                    TU_ARMA(Import, v) { BUG(sp, "Unexpected import: " << p->m_path); }
                    TU_ARMA(Module, v) { BUG(sp, "Unexpected module: " << p->m_path); }
                    TU_ARMA(TypeAlias, v) { gp = &v.m_params; }
                    TU_ARMA(TraitAlias, v) { gp = &v.m_params; }
                    TU_ARMA(ExternType, v) { gp = nullptr; }
                    TU_ARMA(Enum, v) { gp = &v.m_params; }
                    TU_ARMA(Struct, v) { gp = &v.m_params; }
                    TU_ARMA(Union, v) { gp = &v.m_params; }
                    TU_ARMA(Trait, v) { gp = &v.m_params; }
                    }
                    if(gp) {
                        p->m_params.m_lifetimes.resize( gp->m_lifetimes.size() );

                        // Inherit bounds.
                        if( m_cur_params ) {
                            TRACE_FUNCTION_FR("INHERIT BOUNDS: " << *p, "INHERIT BOUNDS");
                            // Visit lifeitmes first - so they're un-elided
                            for(auto& l : p->m_params.m_lifetimes) {
                                visit_lifetime(sp, l);
                            }
                            // Then make a monomorph state, and find lifetime bounds
                            MonomorphStatePtr   ms(nullptr, &p->m_params, nullptr);
                            for(const auto& b : gp->m_bounds) {
                                TU_MATCH_HDRA((b), {)
                                TU_ARMA(Lifetime, be) {
                                    ASSERT_BUG(sp, be.test.is_param(), b);
                                    ASSERT_BUG(sp, be.valid_for.binding != HIR::LifetimeRef::UNKNOWN, b);
                                    m_cur_params->m_bounds.push_back(HIR::GenericBound::make_Lifetime({
                                        ms.monomorph_lifetime(sp, be.test),
                                        ms.monomorph_lifetime(sp, be.valid_for)
                                        }));
                                    const auto& nbe = m_cur_params->m_bounds.back().as_Lifetime();
                                    if( nbe.test.is_param() ) {
                                        ASSERT_BUG(sp, nbe.test.is_param(),
                                            b << " -> " << m_cur_params->m_bounds.back());
                                        ASSERT_BUG(sp, nbe.valid_for.binding != HIR::LifetimeRef::UNKNOWN,
                                            b << " -> " << m_cur_params->m_bounds.back());
                                        if( (nbe.test.is_param() && nbe.test.as_param().group() == 3)
                                         || (nbe.valid_for.is_param() && nbe.valid_for.as_param().group() == 3) ) {
                                            m_cur_params->m_bounds.pop_back();
                                        }
                                        else {
                                            DEBUG("INHERIT " << m_cur_params->m_bounds.back());
                                        }
                                    }
                                    else {
                                        // The monomorphised lifetime wasn't a parameter - had to be `'static` but not checking
                                        // - Remove the new bound, if it was bad then there should be an error later on?
                                        m_cur_params->m_bounds.pop_back();
                                    }
                                    }
                                TU_ARMA(TypeLifetime, be) {
                                    // TODO: Should type lifetimes be inferred too?
                                    }
                                TU_ARMA(TraitBound, _be) {}
                                TU_ARMA(TypeEquality, _be) {}
                                }
                            }
                        }
                    }

                    if( p->m_params.m_lifetimes.size() == 0 ) {
                        // Mark such that contained trait objects use `'static`
                        static ::HIR::LifetimeRef   static_lifetime = ::HIR::LifetimeRef::new_static();
                        m_trait_object_rule.push_back(std::make_pair(m_current_depth, &static_lifetime));
                    }
                    else if( p->m_params.m_lifetimes.size() == 1 ) {
                        // Mark such that contained trait objects use this lifetime
                        m_trait_object_rule.push_back(std::make_pair(m_current_depth, &p->m_params.m_lifetimes[0]));
                    }
                    else {
                        // Mark such that contained trait objects require an explicit annotation
                        m_trait_object_rule.push_back(std::make_pair(m_current_depth, nullptr));
                    }
                }
                else if( auto* p = e->path.m_data.opt_UfcsKnown() )
                {
                    // Get trait, check if the type has ATCs
                    const auto& trait = m_resolve.m_crate.get_trait_by_path(sp, p->trait.m_path);
                    const auto& aty = trait.m_types.at(p->item);

                    if( p->params.m_lifetimes.size() < aty.m_generics.m_lifetimes.size() )//&& m_current_lifetime.size() && m_current_lifetime.back() )
                    {
                        p->params.m_lifetimes.resize( aty.m_generics.m_lifetimes.size() );
                    }
                }
            }

            ::HIR::Visitor::visit_type(ty);

            saved_params.restore();
            while(m_current_lifetime.size() > saved_liftime_depth)
                m_current_lifetime.pop_back();
            while(m_trait_object_rule.size() > saved_m_trait_object_rule)
                m_trait_object_rule.pop_back();
            m_current_depth -= 1;

            {
                bool pushed = false;
                if( m_current_lifetime.empty() || !m_current_lifetime.back() ) {
                    // Push `'static` (if not in expression mode AND; this is a trait object OR we're not in arguments)
                    if( !m_in_expr ) {
                        static HIR::LifetimeRef static_lifetime = HIR::LifetimeRef::new_static();
                        if( !(m_cur_params && m_create_elided) ) {
                            // In the return type, so we don't want to make a new parameter - push `'static`
                            m_current_lifetime.push_back(&static_lifetime);
                            pushed = true;
                        }
                        else if( ty.data().is_TraitObject() && ty.data().as_TraitObject().m_lifetime == HIR::LifetimeRef() ) {
                            // `dyn Foo` as vs `dyn Foo+'_`
                            m_current_lifetime.push_back(&static_lifetime);
                            pushed = true;
                        }
                    }
                }
                if(auto* e = ty.data_mut().opt_TraitObject()) {
                    // TODO: The following are different
                    // - `fn foo(&self) -> Box<dyn Foo>`      -> `fn foo<'a>(&'a self) -> Box<dyn Foo + 'static>`
                    // - `fn foo(&self) -> Box<dyn Foo + '_>` -> `fn foo<'a>(&'a self) -> Box<dyn Foo + 'a>`
                    // BUT
                    // - `fn foo(&self) -> &dyn Foo` -> `fn foo<'a>(&'a self) -> &'a (dyn Foo + 'a)`
                    // - `fn foo(&self) -> &(dyn Foo + '_)` -> `fn foo<'a>(&'a self) -> &'a (dyn Foo + 'a)`
                    // TODO: What about in structs?

                    visit_lifetime(sp, e->m_lifetime);
                    DEBUG("TraitObject: Final lifetime " << e->m_lifetime);
                }
                if(auto* e = ty.data_mut().opt_ErasedType()) {
                    // For an erased type, check if there's a lifetime within any of the ATYs
                    // - If so, use that [citation needed]
                    // https://rust-lang.github.io/rfcs/1951-expand-impl-trait.html#scoping-for-type-and-lifetime-parameters
                    // Any mentioned lifetimes within the trait are considered as "captured"
                    // - So, enumerate the mentioned lifetimes and create a composite for it.
                    if( e->m_lifetimes.empty() ) {
                        // If there is no lifetime assigned, then grab all mentioned lifetimes?
                        struct V: public HIR::Visitor {
                            std::set<HIR::LifetimeRef>  lfts;
                            void visit_path_params(HIR::PathParams& pp) override {
                                for(auto& lft : pp.m_lifetimes) {
                                    add_lifetime(lft);
                                }

                                HIR::Visitor::visit_path_params(pp);
                            }
                            void add_lifetime(const HIR::LifetimeRef& lft) {
                                if( lft.is_hrl() ) {
                                    // HRL - ignore
                                    return;
                                }
                                this->lfts.insert(lft);
                            }
                            void visit_type(HIR::TypeRef& ty) override {
                                if(const auto* tep = ty.data().opt_Borrow()) {
                                    add_lifetime(tep->lifetime);
                                }
                                if(const auto* tep = ty.data().opt_Function()) {
                                    // Push HRLs?
                                    (void)tep;
                                }
                                if(const auto* tep = ty.data().opt_TraitObject()) {
                                    add_lifetime(tep->m_lifetime);
                                    // Push HRLs?
                                }
                                if(const auto* tep = ty.data().opt_ErasedType()) {
                                    for(const auto& lft : tep->m_lifetimes) {
                                        add_lifetime(lft);
                                    }
                                }
                                HIR::Visitor::visit_type(ty);
                            }
                        } v;
                        v.visit_type(ty);
                        // If there is a lifetime on the stack (that wasn't from a `'static` pushed above), then use it
                        if( v.lfts.empty() && !m_current_lifetime.empty() && m_current_lifetime.back() && !pushed ) {
                            DEBUG("ErasedType: Use wrapping lifetime");
                            e->m_lifetimes.push_back( *m_current_lifetime.back() );
                        }
                        else if( v.lfts.empty() ) {
                            // No contained lifetimes, it's `'static`?
                            DEBUG("No inner lifetimes, will be `'static`");
                            e->m_lifetimes.push_back( HIR::LifetimeRef::new_static() );
                        }
                        else if( v.lfts.size() == 1) {
                            // Easy, just assign this lifetime
                            DEBUG("ErasedType: Use contained lifetime " << *v.lfts.begin());
                            e->m_lifetimes.push_back( *v.lfts.begin() );
                        }
                        else {
                            // If in arguments: Create a new input lifetime with a union of these lifetimes.
                            if( m_cur_params && m_create_elided ) {
                                e->m_lifetimes.push_back( HIR::LifetimeRef(m_cur_params_level * 256 + m_cur_params->m_lifetimes.size()) );
                                m_cur_params->m_lifetimes.push_back(HIR::LifetimeDef { });
                                for(const auto& l : v.lfts) {
                                    m_cur_params->m_bounds.push_back(HIR::GenericBound::make_Lifetime({ e->m_lifetimes[0], l }));
                                }
                            }
                            // In return: Save the list?
                            else {
                                e->m_lifetimes.clear();
                                for(const auto& lft : v.lfts) {
                                    e->m_lifetimes.push_back( lft );
                                }
                            }
                        }
                    }

                    // If in arguments, don't visit an omitted lifetime (so we don't add an elided lifetime for something that will be generic)
                    if( (!e->m_lifetimes.empty() && e->m_lifetimes.front().binding == HIR::LifetimeRef::UNKNOWN) && (m_cur_params && m_create_elided) ) {
                    }
                    else {
                        for(auto& lft : e->m_lifetimes)
                            visit_lifetime(sp, lft);
                    }
                }
                if(pushed) {
                    m_current_lifetime.pop_back();
                }
            }

            if(m_current_depth == 0 ) {
                DEBUG("< " << ty);
            }
        }


        void visit_trait_path(::HIR::TraitPath& tp) override
        {
            const Span  sp;
            TRACE_FUNCTION_FR(tp, tp);

            auto has_apply_elision = [](::HIR::TraitPath& tp, bool& created_hrls)->bool {
                bool was_paren_trait_object = tp.m_hrtbs && tp.m_hrtbs->m_lifetimes.size() >= 1 && tp.m_hrtbs->m_lifetimes.back().m_name == "#apply_elision";
                created_hrls = false;
                if( was_paren_trait_object )
                {
                    if(!tp.m_hrtbs) {
                        tp.m_hrtbs = std::make_unique<HIR::GenericParams>();
                        created_hrls = true;
                    }
                    if(was_paren_trait_object) {
                        tp.m_hrtbs->m_lifetimes.pop_back();
                    }
                    return true;
                }
                else {
                    return false;
                }
                };

            // Handle a hack from lowering pass added when the path is `Foo()`
            bool created_hrls = false;
            if( has_apply_elision(tp, created_hrls) )
            {
                m_current_lifetime.push_back(nullptr);

                // Visit the trait args (as inputs)
                auto saved_params = push_params(tp.m_hrtbs.get(), 3);

                this->visit_generic_path(tp.m_path, ::HIR::Visitor::PathContext::TYPE);
                DEBUG(tp.m_path);
                if( tp.m_hrtbs ) {
                    DEBUG("for " << tp.m_hrtbs->fmt_args());
                }

                #if 1
                HIR::LifetimeRef    lft;
                if( tp.m_hrtbs && tp.m_hrtbs->m_lifetimes.size() == 1 )
                {
                    lft = HIR::LifetimeRef( /*tp.m_hrtbs->m_lifetimes[0].m_name,*/ 3*256+0 );
                }
                if( lft == HIR::LifetimeRef() )
                {
                    // If there wasn't an elided lifetime in the input, then get a lifetime from that as the output.
                    // - If there's only one lifetime in `tp.m_path`, use that
                    struct V: public HIR::Visitor {
                        HIR::LifetimeRef out;
                        unsigned n_found = 0;
                        void add_lifetime(const HIR::LifetimeRef& lft) {
                            if( lft.is_hrl() ) {
                                // HRL - ignore
                                return;
                            }
                            n_found += 1;
                            out = lft;
                        }
                        void visit_path_params(HIR::PathParams& pp) override {
                            for(auto& lft : pp.m_lifetimes) {
                                add_lifetime(lft);
                            }

                            HIR::Visitor::visit_path_params(pp);
                        }
                        void visit_type(HIR::TypeRef& ty) override {
                            if(const auto* tep = ty.data().opt_Borrow()) {
                                add_lifetime(tep->lifetime);
                            }
                            if(const auto* tep = ty.data().opt_Function()) {
                                // Push HRLs?
                                (void)tep;
                            }
                            if(const auto* tep = ty.data().opt_TraitObject()) {
                                add_lifetime(tep->m_lifetime);
                                // Push HRLs?
                            }
                            if(const auto* tep = ty.data().opt_ErasedType()) {
                                for(const auto& lft : tep->m_lifetimes) {
                                    add_lifetime(lft);
                                }
                            }
                            HIR::Visitor::visit_type(ty);
                        }
                    } v;
                    v.visit_path_params(tp.m_path.m_params);
                    if( v.n_found == 1 ) {
                        lft = v.out;
                    }
                }
                if( lft != HIR::LifetimeRef() )
                {
                    m_current_lifetime.push_back( &lft );
                    for(auto& assoc : tp.m_type_bounds)
                    {
                        this->visit_generic_path(assoc.second.source_trait, ::HIR::Visitor::PathContext::TYPE);
                        this->visit_type(assoc.second.type);
                    }
                    for(auto& assoc : tp.m_trait_bounds)
                    {
                        this->visit_generic_path(assoc.second.source_trait, ::HIR::Visitor::PathContext::TYPE);
                        for(auto& trait : assoc.second.traits)
                            this->visit_trait_path(trait);
                    }
                    m_current_lifetime.pop_back();
                }
                #endif

                saved_params.restore();

                // Fix the source paths in ATYs
                const auto& trait = m_resolve.m_crate.get_trait_by_path(sp, tp.m_path.m_path);
                struct H {
                    const HIR::Crate& m_crate;
                    H(const HIR::Crate& crate): m_crate(crate) {
                    }
                    bool enum_supertraits(const Span& sp, const HIR::Trait& tr, const HIR::GenericPath& tr_path, ::std::function<bool(HIR::GenericPath)> cb) {
                        static const HIR::TypeRef   self = ::HIR::TypeRef::new_self();
                        MonomorphStatePtr   ms(&self, &tr_path.m_params, nullptr);

                        if( tr.m_all_parent_traits.size() > 0 ) {
                            // Externals will have this populated
                            for(const auto& supertrait : tr.m_all_parent_traits) {
                                auto m = ms.monomorph_genericpath(sp, supertrait.m_path, false);
                                if( cb(std::move(m)) )
                                    return true;
                            }
                        }
                        else {
                            // This runs before bind, so locals won't have the main list populated
                            for(const auto& pt : tr.m_parent_traits)
                            {
                                auto m = ms.monomorph_genericpath(sp, pt.m_path, false);
                                DEBUG("- " << m);
                                if( enum_supertraits(sp, m_crate.get_trait_by_path(sp, m.m_path), m, cb) )
                                    return true;
                                if( cb(std::move(m)) )
                                    return true;
                            }
                            for(const auto& b : tr.m_params.m_bounds)
                            {
                                if( !b.is_TraitBound() )
                                    continue;
                                const auto& be = b.as_TraitBound();
                                if( be.type != self )
                                    continue;
                                const auto& pt = be.trait;
                                if( pt.m_path.m_path == tr_path.m_path )
                                    continue ;

                                auto m = ms.monomorph_genericpath(sp, pt.m_path, false);
                                DEBUG("- " << m);
                                if( enum_supertraits(sp, m_crate.get_trait_by_path(sp, m.m_path), m, cb) )
                                    return true;
                                if( cb(std::move(m)) )
                                    return true;
                            }
                        }
                        return false;
                    }
                } h(m_resolve.m_crate);
                auto fix_source = [&](HIR::GenericPath& gp, const RcString& name) {
                    //fix_path(gp);
                    DEBUG("[fix_source] >> " << gp);
                    // NOTE: The HRLs of this path have been edited! (they were `<'#apply_elision,>`, now blank)
                    if( gp.m_path == tp.m_path.m_path && gp.m_params == tp.m_path.m_params ) {
                        gp = tp.m_path.clone();
                        return ;
                    }
                    if( h.enum_supertraits(sp, trait, tp.m_path, [&](HIR::GenericPath m) {
                        DEBUG("[fix_source] ?? " << m);
                        if( m == gp ) { // Equality ignores lifetimes
                            gp = std::move(m);
                            return true;
                        }
                        return false;
                        }) )
                    {
                        return ;
                    }
                    BUG(sp, "Failed to find " << gp << " in parent trait list of " << tp.m_path);
                    };
                for(auto& assoc : tp.m_type_bounds) {
                    fix_source(assoc.second.source_trait, assoc.first);
                }
                for(auto& assoc : tp.m_trait_bounds) {
                    fix_source(assoc.second.source_trait, assoc.first);
                }

                // Set the output lifetime (if present)
                auto output_lifetime = HIR::LifetimeRef(3*256 + 0);
                if( tp.m_hrtbs->m_lifetimes.size() == 1 ) {
                    m_current_lifetime.pop_back();
                    m_current_lifetime.push_back(&output_lifetime);
                }
                else {
                    // No output lifetime
                }

                // Visit the rest (associated types mostly), using the output lifetime from above
                ::HIR::Visitor::visit_trait_path(tp);

                m_current_lifetime.pop_back();

                if(created_hrls && tp.m_hrtbs->is_empty()) {
                    tp.m_hrtbs.reset();
                }
            }
            else
            {
                ::HIR::Visitor::visit_trait_path(tp);
            }
        }

        void visit_expr(::HIR::ExprPtr& ep) override
        {
            struct EV: public HIR::ExprVisitorDef {
                Visitor& parent;
                EV(Visitor& parent): parent(parent) {}
                void visit_type(HIR::TypeRef& ty) {
                    parent.visit_type(ty);
                }
            } v { *this };

            auto s = m_in_expr;
            m_in_expr = true;
            if(ep) {
                ep->visit(v);
            }
            m_in_expr = s;
        }

        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            auto _ = m_resolve.set_impl_generics(/*impl.m_type,*/ impl.m_params);

            // Pre-visit so lifetime elision can work
            {
                auto _ = push_params(impl.m_params, 0);
                this->visit_type(impl.m_type);
            }

            ::HIR::Visitor::visit_type_impl(impl);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << impl.m_trait_args << " for " << impl.m_type);
            auto _ = m_resolve.set_impl_generics(/*impl.m_type,*/ impl.m_params);

            // Pre-visit so lifetime elision can work
            {
                auto _ = push_params(impl.m_params, 0);
                this->visit_type(impl.m_type);
                this->visit_path_params(impl.m_trait_args);
            }

            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << impl.m_trait_args << " for " << impl.m_type << " { }");
            auto _ = m_resolve.set_impl_generics(/*impl.m_type,*/ impl.m_params);

            // Pre-visit so lifetime elision can work
            {
                auto _ = push_params(impl.m_params, 0);
                this->visit_type(impl.m_type);
                this->visit_path_params(impl.m_trait_args);
            }

            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
        }

        void visit_struct(::HIR::ItemPath p, ::HIR::Struct& item) override
        {
            auto _ = m_resolve.set_impl_generics(/*item.m_struct_markings.dst_type,*/ item.m_params);
            auto _2 = push_params(item.m_params, 0); m_create_elided = false;
            ::HIR::Visitor::visit_struct(p, item);
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override
        {
            auto _ = m_resolve.set_impl_generics(/*MetadataType::None,*/ item.m_params);
            auto _2 = push_params(item.m_params, 0); m_create_elided = false;
            ::HIR::Visitor::visit_enum(p, item);
        }
        void visit_union(::HIR::ItemPath p, ::HIR::Union& item) override
        {
            auto _ = m_resolve.set_impl_generics(/*MetadataType::None,*/ item.m_params);
            auto _2 = push_params(item.m_params, 0); m_create_elided = false;
            ::HIR::Visitor::visit_union(p, item);
        }

        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override
        {
            auto lft = HIR::LifetimeRef::new_static();
            m_current_lifetime.push_back(&lft);
            visit_type(item.m_type);
            m_current_lifetime.pop_back(/*&lft*/);

            ::HIR::Visitor::visit_constant(p, item);
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override
        {
            auto lft = HIR::LifetimeRef::new_static();
            m_current_lifetime.push_back(&lft);
            visit_type(item.m_type);
            m_current_lifetime.pop_back(/*&lft*/);

            ::HIR::Visitor::visit_static(p, item);
        }

        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override
        {
            TRACE_FUNCTION_F(p);
            auto _ = m_resolve.set_item_generics(item.m_params);
            // NOTE: Superfluous... except that it makes the params valid for the return type.
            visit_params(item.m_params);

            auto first_elided_lifetime_idx = item.m_params.m_lifetimes.size();

            // TODO: Add lifetime bounds from argument types!
            // - While visiting the argument types, find path types and inherit the lifetime bounds

            // Visit arguments to get the input lifetimes
            auto saved_params = push_params(item.m_params, 1);
            for(auto& arg : item.m_args)
            {
                TRACE_FUNCTION_FR("ARG " << arg, "ARG " << arg);
                visit_type(arg.second);
            }
            m_create_elided = false;

            // Get output lifetime
            // - Try `&self`'s lifetime (if it was an elided lifetime)
            HIR::LifetimeRef    elided_output_lifetime;
            if( item.m_receiver != HIR::Function::Receiver::Free ) {
                if( const auto* b = item.m_args[0].second.data().opt_Borrow() ) {
                    // If this was an elided lifetime.
                    if( b->lifetime.is_param() && (b->lifetime.binding >> 8) == 1 && (b->lifetime.binding & 0xFF) >= first_elided_lifetime_idx ) {
                        elided_output_lifetime = b->lifetime;
                        DEBUG("Elided 'self");
                    }
                    // Also allow 'static self (see lazy_static 1.0.2)
                    if( b->lifetime.binding == HIR::LifetimeRef::STATIC ) {
                        elided_output_lifetime = b->lifetime;
                        DEBUG("Static 'self");
                    }
                    // OR, just always use `'self` if present
                    if(true) {
                        DEBUG("'self specified");
                        elided_output_lifetime = b->lifetime;
                    }
                }
            }
            // - OR, look for only one elided lifetime
            if( elided_output_lifetime == HIR::LifetimeRef() ) {
                if( item.m_params.m_lifetimes.size() == first_elided_lifetime_idx + 1 ) {
                    elided_output_lifetime = HIR::LifetimeRef(256 + first_elided_lifetime_idx);
                    DEBUG("Elided 'only");
                }
            }
            if( elided_output_lifetime == HIR::LifetimeRef() ) {
                if( item.m_params.m_lifetimes.size() == 1 ) {
                    elided_output_lifetime = HIR::LifetimeRef(256 + 0);
                    DEBUG("Elided 'single");
                }
            }
            // TODO: Search for an explicit lifetime in the input, and use that if there was only one?
            if( elided_output_lifetime == HIR::LifetimeRef() ) {
                struct V: public HIR::Visitor {
                    HIR::LifetimeRef out;
                    unsigned n_found = 0;
                    void add_lifetime(const HIR::LifetimeRef& lft) {
                        if( lft.is_hrl() ) {
                            // HRL - ignore
                            return;
                        }
                        n_found += 1;
                        out = lft;
                    }
                    void visit_path_params(HIR::PathParams& pp) override {
                        for(auto& lft : pp.m_lifetimes) {
                            add_lifetime(lft);
                        }

                        HIR::Visitor::visit_path_params(pp);
                    }
                    void visit_type(HIR::TypeRef& ty) override {
                        if(const auto* tep = ty.data().opt_Borrow()) {
                            add_lifetime(tep->lifetime);
                        }
                        if(const auto* tep = ty.data().opt_Function()) {
                            // Push HRLs?
                            (void)tep;
                        }
                        if(const auto* tep = ty.data().opt_TraitObject()) {
                            add_lifetime(tep->m_lifetime);
                            // Push HRLs?
                        }
                        if(const auto* tep = ty.data().opt_ErasedType()) {
                            for(const auto& lft : tep->m_lifetimes) {
                                add_lifetime(lft);
                            }
                        }
                        HIR::Visitor::visit_type(ty);
                    }
                } v;
                for(auto& a : item.m_args) {
                    v.visit_type(a.second);
                }
                if( v.n_found == 1 ) {
                    elided_output_lifetime = v.out;
                    DEBUG("Explicit 'single (recurse)");
                }
            }
            if( elided_output_lifetime == HIR::LifetimeRef() ) {
                // TODO: If the only argument is a `'static`, use that? (or if there's only one borrow in the arguments, use that)
                if( item.m_args.size() == 1 && item.m_args.front().second.data().is_Borrow() ) {
                    elided_output_lifetime = item.m_args.front().second.data().as_Borrow().lifetime;
                    DEBUG("Explicit 'single");
                }
            }
            // If present, set it (push to the stack)
            assert(m_current_lifetime.empty());
            if( elided_output_lifetime != HIR::LifetimeRef() ) {
                m_current_lifetime.push_back(&elided_output_lifetime);
            }

            // Visit return type (populates path for `impl Trait` in return position
            {
                TRACE_FUNCTION_FR("RET " << item.m_return, "RET " << item.m_return);
                visit_type(item.m_return);
            }
            // - Unset params for the expression
            saved_params.restore();

            if( elided_output_lifetime != HIR::LifetimeRef() ) {
                m_current_lifetime.pop_back();
            }
            assert(m_current_lifetime.empty());

            DEBUG("Output: " << item.m_params.fmt_args() << item.m_params.fmt_bounds());

            ::HIR::Visitor::visit_function(p, item);
        }
    };
}

void ConvertHIR_LifetimeElision(::HIR::Crate& crate)
{
    Visitor v { crate };
    v.visit_crate(crate);
}
