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
    class Visitor:
        public ::HIR::Visitor
    {
        ::HIR::Crate& crate;
        StaticTraitResolve  m_resolve;

        bool m_in_expr = false;
        ::HIR::GenericParams* m_cur_params = nullptr;
        unsigned m_cur_params_level = 0;
        ::std::vector< ::HIR::LifetimeRef* >    m_current_lifetime;

    public:
        Visitor(::HIR::Crate& crate)
            : crate(crate)
            , m_resolve(crate)
        {
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

            auto saved_liftime_depth = m_current_lifetime.size();
            auto saved_params = std::make_pair(m_cur_params, m_cur_params_level);

            // Lifetime elision logic!

            if(auto* e = ty.data_mut().opt_Borrow()) {
                visit_lifetime(sp, e->lifetime);
                m_current_lifetime.push_back(&e->lifetime);
            }
            if(auto* e = ty.data_mut().opt_Function()) {
                m_current_lifetime.push_back(nullptr);
                m_cur_params = &e->hrls;
                m_cur_params_level = 3;
            }
            if(auto* e = ty.data_mut().opt_TraitObject()) {
                // TODO: Create? but what if it's not used?
                if( e->m_trait.m_path.m_hrls )
                {
                    m_current_lifetime.push_back(nullptr);
                    m_cur_params = &*e->m_trait.m_path.m_hrls;
                    m_cur_params_level = 3;
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
                    }
                }
            }

            ::HIR::Visitor::visit_type(ty);

            m_cur_params       = saved_params.first ;
            m_cur_params_level = saved_params.second;
            while(m_current_lifetime.size() > saved_liftime_depth)
                m_current_lifetime.pop_back();

            {
                bool pushed = false;
                if( m_current_lifetime.empty() || !m_current_lifetime.back() ) {
                    // Push `'static` (if not in expression mode AND; this is a trait object OR we're not in arguments)
                    if( !m_in_expr && (!m_cur_params || ty.data().is_TraitObject()) ) {
                        static HIR::LifetimeRef static_lifetime = HIR::LifetimeRef::new_static();
                        m_current_lifetime.push_back(&static_lifetime);
                        pushed = true;
                    }
                }
                if(auto* e = ty.data_mut().opt_TraitObject()) {
                    if( e->m_lifetime.binding == HIR::LifetimeRef::INFER ||  e->m_lifetime.binding == HIR::LifetimeRef::UNKNOWN )
                    {
                        // https://doc.rust-lang.org/reference/lifetime-elision.html#default-trait-object-lifetimes
                        // If the trait object is used as a type argument of a generic type then the containing type is first used to try to infer a bound.
                        // - If there is a unique bound from the containing type then that is the default
                        // - If there is more than one bound from the containing type then an explicit bound must be specified
                        // If neither of those rules apply, then the bounds on the trait are used:
                        // - If the trait is defined with a single lifetime bound then that bound is used.
                        // - If 'static is used for any lifetime bound then 'static is used.
                        // - If the trait has no lifetime bounds, then the lifetime is inferred in expressions and is 'static outside of expressions.

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
                        }
                        else {
                            if( h.lifetimes.size() == 1 || h.lifetimes.back() == HIR::LifetimeRef::new_static() ) {
                                e->m_lifetime = h.lifetimes.front();
                            }
                            else {
                                // Error?
                            }
                        }
                    }
                    visit_lifetime(sp, e->m_lifetime);
                }
                if(auto* e = ty.data_mut().opt_ErasedType()) {
                    // TODO: For an erased type, check if there's a lifetime within any of the ATYs
                    // - If so, use that [citation needed]
                    // https://rust-lang.github.io/rfcs/1951-expand-impl-trait.html#scoping-for-type-and-lifetime-parameters
                    // Any mentioned lifetimes within the trait are considered as "captured"
                    // - So, enumerate the mentioned lifetimes and create a composite for it.
                    if( e->m_lifetime.binding == HIR::LifetimeRef::UNKNOWN ) {
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
                                }
                                if(const auto* tep = ty.data().opt_TraitObject()) {
                                    add_lifetime(tep->m_lifetime);
                                    // Push HRLs?
                                }
                                if(const auto* tep = ty.data().opt_ErasedType()) {
                                    //TODO(Span(), "Recursive erased type?");
                                }
                                HIR::Visitor::visit_type(ty);
                            }
                        } v;
                        v.visit_type(ty);
                        // If there is a lifetime on the stack (that wasn't from a `'static` pushed above), then use it
                        if( !m_current_lifetime.empty() && m_current_lifetime.back() && !pushed ) {
                            DEBUG("ErasedType: Use wrapping lifetime");
                            e->m_lifetime = *m_current_lifetime.back();
                        }
                        else if( v.lfts.empty() ) {
                            // No contained lifetimes, it's `'static`?
                            DEBUG("No inner lifetimes, will be `'static`");
                        }
                        else if( v.lfts.size() == 1) {
                            // Easy, just assign this lifetime
                            DEBUG("ErasedType: Use contained lifetime " << *v.lfts.begin());
                            e->m_lifetime = *v.lfts.begin();
                        }
                        else {
                            // If in arguments: Create a new input lifetime with a union of these lifetimes.
                            if( m_cur_params ) {
                                e->m_lifetime = HIR::LifetimeRef(m_cur_params_level * 256 + m_cur_params->m_lifetimes.size());
                                m_cur_params->m_lifetimes.push_back(HIR::LifetimeDef { });
                                for(const auto& l : v.lfts) {
                                    m_cur_params->m_bounds.push_back(HIR::GenericBound::make_Lifetime({ e->m_lifetime, l }));
                                }
                            }
                            else {
                                TODO(sp, "Encountered multiple lifetimes, which to use? in " << ty << ": " << v.lfts);
                            }
                        }
                    }

                    // If in arguments, don't visit an omitted lifetime (so we don't add an elided lifetime for something that will be generic)
                    if( e->m_lifetime.binding == HIR::LifetimeRef::UNKNOWN && m_cur_params ) {
                    }
                    else {
                        visit_lifetime(sp, e->m_lifetime);
                    }
                }
                if(pushed) {
                    m_current_lifetime.pop_back();
                }
            }
        }


        void visit_trait_path(::HIR::TraitPath& tp) override
        {
            const Span  sp;
            TRACE_FUNCTION_FR(tp, tp);

            auto has_apply_elision = [](::HIR::GenericPath& gp, bool& created_hrls)->bool {
                bool was_paren_trait_object = gp.m_hrls && gp.m_hrls->m_lifetimes.size() >= 1 && gp.m_hrls->m_lifetimes.back().m_name == "#apply_elision";
                created_hrls = false;
                if( was_paren_trait_object )
                {
                    if(!gp.m_hrls) {
                        gp.m_hrls = std::make_unique<HIR::GenericParams>();
                        created_hrls = true;
                    }
                    if(was_paren_trait_object) {
                        gp.m_hrls->m_lifetimes.pop_back();
                    }
                    return true;
                }
                else {
                    return false;
                }
                };

            // Handle a hack from lowering pass added when the path is `Foo()`
            bool created_hrls = false;
            if( has_apply_elision(tp.m_path, created_hrls) )
            {
                m_current_lifetime.push_back(nullptr);

                // Visit the trait args (as inputs)
                auto saved_params_ptr = m_cur_params;
                auto saved_params_lvl = m_cur_params_level;
                m_cur_params = tp.m_path.m_hrls.get();
                m_cur_params_level = 3;

                this->visit_generic_path(tp.m_path, ::HIR::Visitor::PathContext::TYPE);
                DEBUG(tp.m_path);

                m_cur_params = saved_params_ptr;
                m_cur_params_level = saved_params_lvl;

                // Fix the source paths in ATYs
                const auto& trait = m_resolve.m_crate.get_trait_by_path(sp, tp.m_path.m_path);
                struct H {
                    const HIR::Crate& m_crate;
                    H(const HIR::Crate& crate): m_crate(crate) {
                    }
                    bool enum_supertraits(const Span& sp, const HIR::Trait& tr, const HIR::GenericPath& tr_path, ::std::function<bool(HIR::GenericPath)> cb) {
                        static const HIR::TypeRef   self = HIR::TypeRef("Self", GENERIC_Self);
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
                auto fix_path = [this,&has_apply_elision](HIR::GenericPath& gp) {
                    bool created_hrls;
                    if( has_apply_elision(gp, created_hrls) )
                    {
                        m_current_lifetime.push_back(nullptr);

                        auto saved_params_ptr = m_cur_params;
                        auto saved_params_lvl = m_cur_params_level;
                        m_cur_params = gp.m_hrls.get();
                        m_cur_params_level = 3;

                        DEBUG("[fix_path] >> " << gp);
                        this->visit_generic_path(gp, ::HIR::Visitor::PathContext::TYPE);

                        m_cur_params = saved_params_ptr;
                        m_cur_params_level = saved_params_lvl;

                        m_current_lifetime.pop_back();
                        if(created_hrls && gp.m_hrls->is_empty()) {
                            gp.m_hrls.reset();
                        }
                    }
                    else {
                        m_current_lifetime.push_back(nullptr);

                        auto saved_params_ptr = m_cur_params;
                        auto saved_params_lvl = m_cur_params_level;
                        m_cur_params = gp.m_hrls.get();
                        m_cur_params_level = 3;

                        DEBUG("[fix_path] >> " << gp);
                        this->visit_generic_path(gp, ::HIR::Visitor::PathContext::TYPE);

                        m_cur_params = saved_params_ptr;
                        m_cur_params_level = saved_params_lvl;

                        m_current_lifetime.pop_back();
                    }
                };
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
                if( tp.m_path.m_hrls->m_lifetimes.size() == 1 ) {
                    m_current_lifetime.pop_back();
                    m_current_lifetime.push_back(&output_lifetime);
                }
                else {
                    // No output lifetime
                }

                // Visit the rest (associated types mostly), using the output lifetime from above
                ::HIR::Visitor::visit_trait_path(tp);

                m_current_lifetime.pop_back();

                if(created_hrls && tp.m_path.m_hrls->is_empty()) {
                    tp.m_path.m_hrls.reset();
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
            auto _ = m_resolve.set_impl_generics(impl.m_params);

            // Pre-visit so lifetime elision can work
            {
                m_cur_params = &impl.m_params;
                m_cur_params_level = 0;
                this->visit_type(impl.m_type);
                m_cur_params = nullptr;
            }

            ::HIR::Visitor::visit_type_impl(impl);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << impl.m_trait_args << " for " << impl.m_type);
            auto _ = m_resolve.set_impl_generics(impl.m_params);

            // Pre-visit so lifetime elision can work
            {
                m_cur_params = &impl.m_params;
                m_cur_params_level = 0;
                this->visit_type(impl.m_type);
                this->visit_path_params(impl.m_trait_args);
                m_cur_params = nullptr;
            }

            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << impl.m_trait_args << " for " << impl.m_type << " { }");
            auto _ = m_resolve.set_impl_generics(impl.m_params);

            // Pre-visit so lifetime elision can work
            {
                m_cur_params = &impl.m_params;
                m_cur_params_level = 0;
                this->visit_type(impl.m_type);
                this->visit_path_params(impl.m_trait_args);
                m_cur_params = nullptr;
            }

            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
        }

        void visit_struct(::HIR::ItemPath p, ::HIR::Struct& item) override
        {
            auto _ = m_resolve.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_struct(p, item);
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override
        {
            auto _ = m_resolve.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_enum(p, item);
        }
        void visit_union(::HIR::ItemPath p, ::HIR::Union& item) override
        {
            auto _ = m_resolve.set_impl_generics(item.m_params);
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

            // Visit arguments to get the input lifetimes
            m_cur_params = &item.m_params;
            m_cur_params_level = 1;
            for(auto& arg : item.m_args)
            {
                TRACE_FUNCTION_FR("ARG " << arg, "ARG " << arg);
                visit_type(arg.second);
            }
            m_cur_params = nullptr;

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

            if( elided_output_lifetime != HIR::LifetimeRef() ) {
                m_current_lifetime.pop_back();
            }
            assert(m_current_lifetime.empty());

            ::HIR::Visitor::visit_function(p, item);
        }
    };
}

void ConvertHIR_LifetimeElision(::HIR::Crate& crate)
{
    Visitor v { crate };
    v.visit_crate(crate);
}
