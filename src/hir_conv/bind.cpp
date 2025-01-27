/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_conv/bind.cpp
 * - Set binding pointers in HIR structures
 * - Also fixes parameter counts.
 */
#include "main_bindings.hpp"
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <mir/mir.hpp>
#include <algorithm>    // std::find_if

#include <mir/helpers.hpp>

#include <hir_typeck/static.hpp>
#include <hir_typeck/expr_visit.hpp>    // For ModuleState
#include <hir/expr_state.hpp>

void ConvertHIR_Bind(::HIR::Crate& crate);

namespace {

    static const ::HIR::TypeRef  ty_Self = ::HIR::TypeRef::new_self();

    enum class Target {
        TypeItem,
        Struct,
        Enum,
        EnumVariant,
    };
    const void* get_type_pointer(const Span& sp, const ::HIR::Crate& crate, const ::HIR::SimplePath& path, Target t)
    {
        if( t == Target::EnumVariant )
        {
            return &crate.get_typeitem_by_path(sp, path, /*ignore_crate_name=*/false, /*ignore_last_node=*/true).as_Enum();
        }
        else
        {
            const auto& ti = crate.get_typeitem_by_path(sp, path);
            switch(t)
            {
            case Target::TypeItem:  return &ti;
            case Target::EnumVariant:   throw "";

            case Target::Struct:
                TU_IFLET(::HIR::TypeItem, ti, Struct, e2,
                    return &e2;
                )
                else {
                    ERROR(sp, E0000, "Expected a struct at " << path << ", got a " << ti.tag_str());
                }
                break;
            case Target::Enum:
                TU_IFLET(::HIR::TypeItem, ti, Enum, e2,
                    return &e2;
                )
                else {
                    ERROR(sp, E0000, "Expected a enum at " << path << ", got a " << ti.tag_str());
                }
                break;
            }
            throw "";
        }
    }

    void fix_type_params(const Span& sp, const ::HIR::GenericParams& params_def, ::HIR::PathParams& params)
    {
        #if 1
        if( params.m_lifetimes.size() == 0 ) {
            params.m_lifetimes.resize( params_def.m_lifetimes.size() );
        }
        if( params.m_lifetimes.size() != params_def.m_lifetimes.size() ) {
            ERROR(sp, E0000, "Incorrect lifetime param count, expected " << params_def.m_lifetimes.size() << ", got " << params.m_lifetimes.size());
        }

        if( params.m_types.size() == 0 ) {
            params.m_types.resize( params_def.m_types.size() );
            // TODO: Optionally fill in the defaults?
        }
        if( params.m_types.size() != params_def.m_types.size() ) {
            ERROR(sp, E0000, "Incorrect parameter count, expected " << params_def.m_types.size() << ", got " << params.m_types.size());
        }

        if( params.m_values.size() == 0 ) {
            params.m_values.resize( params_def.m_values.size() );
        }
        if( params.m_values.size() != params_def.m_values.size() ) {
            ERROR(sp, E0000, "Incorrect value parameter count, expected " << params_def.m_values.size() << ", got " << params.m_values.size());
        }
        #endif
    }
    void fix_param_count(
        const Span& sp,
        const ::HIR::GenericPath& path, const ::HIR::GenericParams& param_defs, ::HIR::PathParams& params,
        bool fill_infer=true, const ::HIR::TypeRef* self_ty=nullptr
        )
    {
        if( params.m_lifetimes.size() != param_defs.m_lifetimes.size() )
        {
            if( params.m_lifetimes.size() == 0 && fill_infer ) {
                params.m_lifetimes.resize( param_defs.m_lifetimes.size() );
            }
        }
        if( params.m_types.size() != param_defs.m_types.size() )
        {
            TRACE_FUNCTION_FR(path, params);

            if( params.m_types.size() == 0 && fill_infer ) {
                params.m_types.resize( param_defs.m_types.size() );
            }
            else if( params.m_types.size() > param_defs.m_types.size() ) {
                ERROR(sp, E0000, "Too many type parameters passed to " << path);
            }
            else {
                while( params.m_types.size() < param_defs.m_types.size() ) {
                    const auto& typ = param_defs.m_types[params.m_types.size()];
                    if( typ.m_default.data().is_Infer() ) {
                        ERROR(sp, E0000, "Omitted type parameter with no default in " << path);
                    }
                    else {
                        // TODO: Does expanding defaults need a custom monomorphiser that can handle later defaults?
                        MonomorphStatePtr   ms(self_ty, &params, nullptr);
                        auto ty = ms.monomorph_type(sp, typ.m_default);
                        params.m_types.push_back( mv$(ty) );
                    }
                }
            }
        }
        if( params.m_values.size() != param_defs.m_values.size() )
        {
            if( params.m_values.size() == 0 && fill_infer ) {
                params.m_values.reserve(param_defs.m_values.size());
                for(const auto& val : param_defs.m_values) {
                    if( val.m_default ) {
                        // NOTE: Can't just copy, as Unevaluated may not have had its params set yet
                        TODO(sp, "Value generic defaults");
                    }
                    else {
                        params.m_values.push_back( ::HIR::ConstGeneric::make_Infer({}) );
                    }
                }
            }
        }
    }

    class Visitor:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;

        typeck::ModuleState m_ms;

        struct CurMod {
            const ::HIR::Module* ptr;
            const ::HIR::ItemPath*  path;
        } m_cur_module;

        unsigned m_in_expr;

    public:
        Visitor(const ::HIR::Crate& crate):
            m_crate(crate),
            m_ms(crate),
            m_in_expr(0)
        {
            static ::HIR::ItemPath  root_path("");
            m_cur_module.ptr = &crate.m_root_module;
            m_cur_module.path = &root_path;
        }

        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            auto parent_mod = m_cur_module;
            m_cur_module.ptr = &mod;
            m_cur_module.path = &p;

            m_ms.push_traits(p, mod);
            ::HIR::Visitor::visit_module(p, mod);
            m_ms.pop_traits(mod);

            m_cur_module = parent_mod;
        }

        void visit_trait_path(::HIR::TraitPath& p) override
        {
            static Span sp;
            p.m_trait_ptr = &m_crate.get_trait_by_path(sp, p.m_path.m_path);

            ::HIR::Visitor::visit_trait_path(p);
        }

        void visit_literal(const Span& sp, EncodedLiteral& lit)
        {
            for(auto& r : lit.relocations)
            {
                if(r.p)
                    visit_path(*r.p, ::HIR::Visitor::PathContext::VALUE);
            }
        }

        void visit_pattern_Value(const Span& sp, ::HIR::Pattern& pat, ::HIR::Pattern::Value& val)
        {
            bool is_single_value = pat.m_data.is_Value();

            if( auto* ve = val.opt_Named() )
            {
                if(auto* pe = ve->path.m_data.opt_Generic())
                {
                    const auto& path = pe->m_path;
                    const auto& pc = path.components().back();
                    const ::HIR::Module*  mod = nullptr;
                    if( path.components().size() == 1 )
                    {
                        mod = &m_crate.get_mod_by_path(sp, path, true);
                    }
                    else
                    {
                        const auto& ti = m_crate.get_typeitem_by_path(sp, path, /*ignore_crate_name=*/false, /*ignore_last_node=*/true);
                        if( const auto& enm = ti.opt_Enum() )
                        {
                            if( !is_single_value ) {
                                ERROR(sp, E0000, "Enum variant in range pattern - " << pat);
                            }

                            // Enum variant
                            auto idx = enm->find_variant(pc);
                            if( idx == SIZE_MAX ) {
                                BUG(sp, "'" << pc << "' isn't a variant in path " << path);
                            }
                            HIR::GenericPath path = std::move(*pe);
                            fix_type_params(sp, enm->m_params,  path.m_params);
                            pat.m_data = ::HIR::Pattern::Data::make_PathValue({
                                mv$(path),
                                ::HIR::Pattern::PathBinding::make_Enum({ enm, static_cast<unsigned>(idx) })
                                });
                        }
                        else if( (mod = ti.opt_Module()) )
                        {
                            mod = &ti.as_Module();
                        }
                        else
                        {
                            BUG(sp, "Node " << path.components().size()-2 << " of path " << ve->path << " wasn't a module");
                        }
                    }

                    if( mod )
                    {
                        auto it = mod->m_value_items.find( path.components().back() );
                        if( it == mod->m_value_items.end() ) {
                            BUG(sp, "Couldn't find final component of " << path);
                        }
                        // Unit-like struct match or a constant
                        TU_MATCH_HDRA( (it->second->ent), { )
                        default:
                            ERROR(sp, E0000, "Value pattern " << pat << " pointing to unexpected item type - " << it->second->ent.tag_str());
                        TU_ARMA(Constant, e2) {
                            // Store reference to this item for later use
                            ve->binding = &e2;
                            }
                        TU_ARMA(StructConstant, e2) {
                            const auto& str = mod->m_mod_items.find(pc)->second->ent.as_Struct();
                            // Convert into a dedicated pattern type
                            if( !is_single_value ) {
                                ERROR(sp, E0000, "Struct in range pattern - " << pat);
                            }
                            auto path = mv$(*pe);
                            fix_type_params(sp, str.m_params,  path.m_params);
                            pat.m_data = ::HIR::Pattern::Data::make_PathValue({
                                mv$(path),
                                &str
                                });
                            }
                        }
                    }
                }
                else {
                    // NOTE: Defer until Resolve UFCS (saves duplicating logic)
                }
            }
        }


        void visit_pattern(::HIR::Pattern& pat) override
        {
            static Span _sp = Span();
            const Span& sp = _sp;

            ::HIR::Visitor::visit_pattern(pat);

            TU_MATCH_HDRA( (pat.m_data), {)
            default:
                // Nothing
            TU_ARMA(Value, e) {
                this->visit_pattern_Value(sp, pat, e.val);
                }
            TU_ARMA(Range, e) {
                if(e.start) this->visit_pattern_Value(sp, pat, *e.start);
                if(e.end  ) this->visit_pattern_Value(sp, pat, *e.end);
                }
            TU_ARMA(PathValue, e) {
                }
            TU_ARMA(PathTuple, e) {
                }
            TU_ARMA(PathNamed, e) {
                }
            }
        }
        void visit_path_params(::HIR::PathParams& params) override
        {
            HIR::Visitor::visit_path_params(params);
            for(auto& v : params.m_values)
            {
                if(auto* ve = v.opt_Unevaluated())
                {
                    if( m_ms.m_impl_generics ) {
                        (*ve)->params_impl = m_ms.m_impl_generics->make_nop_params(0);
                    }
                    if( m_ms.m_item_generics ) {
                        (*ve)->params_item = m_ms.m_item_generics->make_nop_params(1);
                    }
                }
            }
        }
        void visit_params(::HIR::GenericParams& params) override
        {
            static Span sp;
            for(auto& bound : params.m_bounds)
            {
                if(auto* be = bound.opt_TraitBound())
                {
                    {
                        const auto& trait = m_crate.get_trait_by_path(sp, be->trait.m_path.m_path);
                        fix_param_count(sp, be->trait.m_path, trait.m_params, be->trait.m_path.m_params, /*fill_infer=*/false, &be->type);
                    }
                    // Also ensure that the defaults are filled in the source traits
                    // - Is there a better solution to this? It feels like it would give the wrong answer (filling defaults incorrectly)
                    for(auto& aty : be->trait.m_type_bounds) {
                        const auto& trait = m_crate.get_trait_by_path(sp, aty.second.source_trait.m_path);
                        fix_param_count(sp, be->trait.m_path, trait.m_params, aty.second.source_trait.m_params, /*fill_infer=*/false, &be->type);
                    }
                    for(auto& aty : be->trait.m_type_bounds) {
                        const auto& trait = m_crate.get_trait_by_path(sp, aty.second.source_trait.m_path);
                        fix_param_count(sp, be->trait.m_path, trait.m_params, aty.second.source_trait.m_params, /*fill_infer=*/false, &be->type);
                    }
                }
            }

            ::HIR::Visitor::visit_params(params);
        }
        void visit_associatedtype(HIR::ItemPath p, ::HIR::AssociatedType& item) override
        {
            static Span sp;
            HIR::Visitor::visit_associatedtype(p, item);
            HIR::TypeRef    ty = ::HIR::TypeRef::new_path(p.get_full_path(), {});
            for(auto& bound : item.m_trait_bounds) {
                const auto& trait = m_crate.get_trait_by_path(sp, bound.m_path.m_path);
                fix_param_count(sp, bound.m_path, trait.m_params, bound.m_path.m_params, /*fill_infer=*/false, &ty);
            }
        }
        void visit_type(::HIR::TypeRef& ty) override
        {
            visit_type_inner(ty);
        }
        void visit_type_inner(::HIR::TypeRef& ty, bool do_bind=true)
        {
            //TRACE_FUNCTION_F(ty);
            static Span sp;

            if(auto* e = ty.data_mut().opt_Path())
            {
                TU_MATCH_HDRA( (e->path.m_data), {)
                TU_ARMA(Generic, pe) {
                    if(!do_bind)
                        break;
                    const auto& item = *reinterpret_cast< const ::HIR::TypeItem*>( get_type_pointer(sp, m_crate, pe.m_path, Target::TypeItem) );
                    TU_MATCH_DEF( ::HIR::TypeItem, (item), (e3),
                    (
                        ERROR(sp, E0000, "Unexpected item type returned for " << pe.m_path << " - " << item.tag_str());
                        ),
                    (TypeAlias,
                        BUG(sp, "TypeAlias encountered after `Resolve Type Aliases` - " << ty);
                        // Assume it'll be filled out, with the correct binding
                        ),
                    (ExternType,
                        e->binding = ::HIR::TypePathBinding::make_ExternType(&e3);
                        DEBUG("- " << ty);
                        ),
                    (Struct,
                        fix_param_count(sp, pe, e3.m_params,  pe.m_params, /*fill_infer=*/m_in_expr!=0);
                        e->binding = ::HIR::TypePathBinding::make_Struct(&e3);
                        DEBUG("- " << ty);
                        ),
                    (Union,
                        fix_param_count(sp, pe, e3.m_params,  pe.m_params, /*fill_infer=*/m_in_expr!=0);
                        e->binding = ::HIR::TypePathBinding::make_Union(&e3);
                        DEBUG("- " << ty);
                        ),
                    (Enum,
                        fix_param_count(sp, pe, e3.m_params,  pe.m_params, /*fill_infer=*/m_in_expr!=0);
                        e->binding = ::HIR::TypePathBinding::make_Enum(&e3);
                        DEBUG("- " << ty);
                        ),
                    (Trait,
                        // TODO: Should this reassign instead?
                        ty.data_mut() = ::HIR::TypeData::make_TraitObject({ ::HIR::TraitPath { {}, mv$(pe), {}, {} }, {}, {} });
                        )
                    )
                    }
                TU_ARMA(UfcsUnknown, pe) {
                    //TODO(sp, "Should UfcsKnown be encountered here?");
                    }
                TU_ARMA(UfcsInherent, pe) {
                    }
                TU_ARMA(UfcsKnown, pe) {

                    const auto& trait = m_crate.get_trait_by_path(sp, pe.trait.m_path);
                    fix_param_count(sp, pe.trait, trait.m_params, pe.trait.m_params, /*fill_infer=*/false, &pe.type);

                    if( pe.type.data().is_Path() && pe.type.data().as_Path().binding.is_Opaque() ) {
                        // - Opaque type, opaque result
                        e->binding = ::HIR::TypePathBinding::make_Opaque({});
                    }
                    else if( pe.type.data().is_Generic() ) {
                        // - Generic type, opaque resut. (TODO: Sometimes these are known - via generic bounds)
                        e->binding = ::HIR::TypePathBinding::make_Opaque({});
                    }
                    else {
                        //bool found = find_impl(sp, m_crate, pe.trait.m_path, pe.trait.m_params, *pe.type, [&](const auto& impl_params, const auto& impl) {
                        //    DEBUG("TODO");
                        //    return false;
                        //    });
                        //if( found ) {
                        //}
                        //TODO(sp, "Resolve known UfcsKnown - " << ty);
                    }
                    }
                }
            }
            else if( auto* te = ty.data_mut().opt_ErasedType() )
            {
                HIR::TypeRef    ty_eself { "ErasedSelf", GENERIC_ErasedSelf };
                for(auto& t : te->m_traits)
                {
                    const auto& trait = m_crate.get_trait_by_path(sp, t.m_path.m_path);
                    fix_param_count(sp, t.m_path, trait.m_params, t.m_path.m_params, /*fill_infer=*/m_in_expr, &ty_eself);
                }
            }
            else if( auto* te = ty.data_mut().opt_TraitObject() )
            {
                HIR::TypeRef    ty_eself { "ErasedSelf", GENERIC_ErasedSelf };
                if( te->m_trait.m_path.m_path != HIR::SimplePath() )
                {
                    const auto& trait = m_crate.get_trait_by_path(sp, te->m_trait.m_path.m_path);
                    fix_param_count(sp, te->m_trait.m_path, trait.m_params, te->m_trait.m_path.m_params, /*fill_infer=*/m_in_expr, nullptr);
                }
                for(auto& m : te->m_markers) {
                    const auto& trait = m_crate.get_trait_by_path(sp, m.m_path);
                    fix_param_count(sp, m, trait.m_params, m.m_params, /*fill_infer=*/m_in_expr, nullptr);
                }
                DEBUG("- " << ty);
            }

            ::HIR::Visitor::visit_type(ty);
        }

        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type << " - from " << impl.m_src_module);
            auto _ = this->m_ms.set_impl_generics(impl.m_params);

            auto mod_ip = ::HIR::ItemPath(impl.m_src_module);
            const auto* mod = (impl.m_src_module != ::HIR::SimplePath() ? &this->m_ms.m_crate.get_mod_by_path(Span(), impl.m_src_module) : nullptr);
            if(mod) {
                m_ms.push_traits(impl.m_src_module, *mod);
                m_cur_module.ptr = mod;
                m_cur_module.path = &mod_ip;
            }
            ::HIR::Visitor::visit_type_impl(impl);
            if(mod)
                m_ms.pop_traits(*mod);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            auto _ = this->m_ms.set_impl_generics(impl.m_params);

            auto mod_ip = ::HIR::ItemPath(impl.m_src_module);
            const auto* mod = (impl.m_src_module != ::HIR::SimplePath() ? &this->m_ms.m_crate.get_mod_by_path(Span(), impl.m_src_module) : nullptr);
            if(mod) {
                m_ms.push_traits(impl.m_src_module, *mod);
                m_cur_module.ptr = mod;
                m_cur_module.path = &mod_ip;
            }
            m_ms.m_traits.push_back( ::std::make_pair( &trait_path, &this->m_ms.m_crate.get_trait_by_path(Span(), trait_path) ) );
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
            m_ms.m_traits.pop_back( );
            if(mod)
                m_ms.pop_traits(*mod);
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type << " { }");
            auto _ = this->m_ms.set_impl_generics(impl.m_params);

            auto mod_ip = ::HIR::ItemPath(impl.m_src_module);
            const auto* mod = (impl.m_src_module != ::HIR::SimplePath() ? &this->m_ms.m_crate.get_mod_by_path(Span(), impl.m_src_module) : nullptr);
            if(mod) {
                m_ms.push_traits(impl.m_src_module, *mod);
                m_cur_module.ptr = mod;
                m_cur_module.path = &mod_ip;
            }
            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
            if(mod)
                m_ms.pop_traits(*mod);
        }

        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override
        {
            auto _ = this->m_ms.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
        }

        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override
        {
            auto _ = this->m_ms.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_enum(p, item);
        }
        void visit_struct(::HIR::ItemPath p, ::HIR::Struct& item) override
        {
            auto _ = this->m_ms.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_struct(p, item);
        }
        void visit_union(::HIR::ItemPath p, ::HIR::Union& item) override
        {
            auto _ = this->m_ms.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_union(p, item);
        }

        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override
        {
            auto _ = this->m_ms.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_function(p, item);
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override
        {
            //auto _ = this->m_ms.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_static(p, item);
            visit_literal(Span(), item.m_value_res);
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override
        {
            auto _ = this->m_ms.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_constant(p, item);
            visit_literal(Span(), item.m_value_res);
        }

        // Actual expressions
        void visit_expr(::HIR::ExprPtr& expr) override
        {
            struct ExprVisitor:
                public ::HIR::ExprVisitorDef
            {
                Visitor& upper_visitor;

                ExprVisitor(Visitor& uv):
                    upper_visitor(uv)
                {}

                void visit_generic_path(::HIR::Visitor::PathContext pc, ::HIR::GenericPath& p) override
                {
                    upper_visitor.visit_generic_path(p, pc);
                }
                void visit_type(::HIR::TypeRef& ty) override
                {
                    upper_visitor.visit_type_inner(ty, true);
                }

                void visit_node_ptr(::HIR::ExprNodeP& node_ptr) override
                {
                    upper_visitor.visit_type(node_ptr->m_res_type);
                    ::HIR::ExprVisitorDef::visit_node_ptr(node_ptr);
                }
                void visit(::HIR::ExprNode_Let& node) override
                {
                    upper_visitor.visit_type(node.m_type);
                    upper_visitor.visit_pattern(node.m_pattern);
                    ::HIR::ExprVisitorDef::visit(node);
                }
                void visit(::HIR::ExprNode_Match& node) override
                {
                    for(auto& arm : node.m_arms)
                    {
                        for(auto& pat : arm.m_patterns)
                            upper_visitor.visit_pattern(pat);
                        for(auto& g : arm.m_guards)
                            upper_visitor.visit_pattern(g.pat);
                    }
                    ::HIR::ExprVisitorDef::visit(node);
                }

                void visit(::HIR::ExprNode_PathValue& node) override
                {
                    upper_visitor.visit_path(node.m_path, ::HIR::Visitor::PathContext::VALUE);
                }
                void visit(::HIR::ExprNode_CallPath& node) override
                {
                    upper_visitor.visit_path(node.m_path, ::HIR::Visitor::PathContext::VALUE);
                    ::HIR::ExprVisitorDef::visit(node);

                    // #[rustc_legacy_const_generics] - A backwards compatability hack added between 1.39 and 1.54 to be backwards compatible with the x86 intrinsics
                    // - Rewrites some literal arguments into const generics
                    if(auto* e = node.m_path.m_data.opt_Generic())
                    {
                        auto& fcn = upper_visitor.m_crate.get_function_by_path(node.span(), e->m_path);
                        if(!fcn.m_markings.rustc_legacy_const_generics.empty() )
                        {
                            if( node.m_args.size() == fcn.m_args.size() ) {
                                // Acceptable
                            }
                            else if( node.m_args.size() == fcn.m_args.size() + fcn.m_markings.rustc_legacy_const_generics.size() ) {
                                for(auto idx : fcn.m_markings.rustc_legacy_const_generics)
                                {
                                    auto& arg_node = node.m_args.at(idx);
                                    assert(arg_node);
                                    // TODO: Check that the expression is a valid const (no locals referenced, no function calls?)
                                    // - Allow: Arithmatic, casts, literals
                                    //if( !dynamic_cast<const HIR::ExprNode_Literal*>(arg_node.get()) )
                                    //    ERROR(arg_node->span(), E0000, "Argument " << idx << " must be a literal for #[rustc_legacy_const_generics] tagged function");
                                    HIR::ExprPtr    ep { std::move(arg_node) };
                                    e->m_params.m_values.push_back( HIR::ConstGeneric( std::make_unique<HIR::ConstGeneric_Unevaluated>(std::move(ep)) ));
                                    // - Visit to ensure that the expr state gets filled
                                    upper_visitor.visit_constgeneric(e->m_params.m_values.back());
                                }
                                auto new_end = std::remove_if(node.m_args.begin(), node.m_args.end(), [](const HIR::ExprNodeP& np){ return !np; });
                                node.m_args.erase(new_end, node.m_args.end());
                            }
                            else {
                                // Will error downstream
                            }
                        }
                    }
                }
                void visit(::HIR::ExprNode_CallMethod& node) override
                {
                    upper_visitor.visit_path_params(node.m_params);
                    ::HIR::ExprVisitorDef::visit(node);
                }

                void visit(::HIR::ExprNode_StructLiteral& node) override
                {
                    upper_visitor.visit_type_inner(node.m_type, false);

                    ::HIR::ExprVisitorDef::visit(node);
                }
                void visit(::HIR::ExprNode_ArraySized& node) override
                {
                    auto& as = node.m_size;
                    if( as.is_Unevaluated() )
                    {
                        upper_visitor.visit_constgeneric(as.as_Unevaluated());
                    }
                    ::HIR::ExprVisitorDef::visit(node);
                }

                void visit(::HIR::ExprNode_Closure& node) override
                {
                    upper_visitor.visit_type(node.m_return);
                    for(auto& arg : node.m_args) {
                        upper_visitor.visit_pattern(arg.first);
                        upper_visitor.visit_type(arg.second);
                    }
                    ::HIR::ExprVisitorDef::visit(node);
                }
            };

            for(auto& ty : expr.m_erased_types)
                visit_type(ty);

            // Set up the module state
            {
                expr.m_state = ::HIR::ExprStatePtr(::HIR::ExprState(*m_cur_module.ptr, m_cur_module.path->get_simple_path()));
                expr.m_state->m_traits = m_ms.m_traits; // TODO: Only obtain the current module's set
                expr.m_state->m_impl_generics = m_ms.m_impl_generics;
                expr.m_state->m_item_generics = m_ms.m_item_generics;
            }

            // Local expression
            if( expr.get() != nullptr )
            {
                // TODO: Disable type param defaults for this scope
                this->m_in_expr ++;

                ExprVisitor v { *this };
                (*expr).visit(v);

                this->m_in_expr --;
            }
            // External expression (has MIR)
            else if( auto* mir = expr.get_ext_mir_mut() )
            {
                for(auto& ty : mir->locals)
                    this->visit_type(ty);
                struct MirVisitor: public ::MIR::visit::VisitorMut
                {
                    Visitor& upper_visitor;
                    MirVisitor(Visitor& upper_visitor):
                        upper_visitor(upper_visitor)
                    {
                    }
                    void visit_type(::HIR::TypeRef& t) override {
                        upper_visitor.visit_type(t);
                    }
                    void visit_path(::HIR::Path& p) override {
                        upper_visitor.visit_path(p, ::HIR::Visitor::PathContext::VALUE);
                    }
                    bool visit_lvalue(::MIR::LValue& lv, ::MIR::visit::ValUsage u) override {
                        if( lv.m_root.is_Static() ) {
                            upper_visitor.visit_path(lv.m_root.as_Static(), ::HIR::Visitor::PathContext::VALUE);
                        }
                        return false;
                    }
                };
                MirVisitor  mv(*this);
                for(auto& block : mir->blocks)
                {
                    for(auto& stmt : block.statements)
                    {
                        mv.visit_stmt(stmt);
                    }
                    mv.visit_terminator(block.terminator);
                }
            }
            else
            {
            }
        }
    };

    class Visitor_EnumSuperTraits:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;

    public:
        Visitor_EnumSuperTraits(const ::HIR::Crate& m_crate):
            m_crate(m_crate)
        {
        }

        void visit_trait(::HIR::ItemPath ip, ::HIR::Trait& tr) override
        {
            static Span sp;
            TRACE_FUNCTION_F(ip);

            // Enumerate supertraits and save for later stages
            struct Enumerate
            {
                ::std::vector< ::HIR::TraitPath>    supertraits;
                ::std::vector<const ::HIR::TraitPath*>  tp_stack;

                void enum_supertraits_in(const ::HIR::Trait& tr, ::HIR::TraitPath path)
                {
                    TRACE_FUNCTION_F(path);
                    tp_stack.push_back(&path);
                    auto& params = path.m_path.m_params;

                    // Fill defaulted parameters.
                    // NOTE: Doesn't do much error checking.
                    fix_param_count(sp, path.m_path, tr.m_params, path.m_path.m_params, false, &ty_Self);

                    auto monomorph_cb = MonomorphStatePtr(&ty_Self, &params, nullptr);
                    auto monomorph_tp = [&](const HIR::TraitPath& tp)->HIR::TraitPath {
                        // TODO: if `path.m_path` has HRLs, then this needs HRLs (only if the HRLs get used?)
                        if( (tp.m_hrtbs && !tp.m_hrtbs->is_empty()) && (path.m_hrtbs && !path.m_hrtbs->is_empty()) ) {
                            // TODO: How to determine which to use?
                            // - May need to combine them.
                            TODO(sp, "Trait path and outer path both have HRLs, how to handle?");
                            return monomorph_cb.monomorph_traitpath(sp, tp, false);
                        }
                        else if( path.m_hrtbs && !path.m_hrtbs->is_empty() ) {
                            auto rv = monomorph_cb.monomorph_traitpath(sp, tp, false);
                            rv.m_hrtbs = box$(path.m_hrtbs->clone());
                            return rv;
                        }
                        else {
                            return monomorph_cb.monomorph_traitpath(sp, tp, false);
                        }
                        };
                    if( tr.m_all_parent_traits.size() > 0 )
                    {
                        for(const auto& pt : tr.m_all_parent_traits)
                        {
                            supertraits.push_back( monomorph_tp(pt) );
                            fill_type_aliases( supertraits.back() );
                        }
                    }
                    else
                    {
                        // Recurse into parent traits
                        for(const auto& pt : tr.m_parent_traits)
                        {
                            enum_supertraits_in(*pt.m_trait_ptr, monomorph_tp(pt));
                        }
                        // - Bound parent traits
                        for(const auto& b : tr.m_params.m_bounds)
                        {
                            if( !b.is_TraitBound() )
                                continue;
                            const auto& be = b.as_TraitBound();
                            if( be.type != ty_Self )
                                continue;
                            const auto& pt = be.trait;
                            if( pt.m_path.m_path == path.m_path.m_path )
                                continue ;

                            enum_supertraits_in(*pt.m_trait_ptr, monomorph_tp(pt));
                        }
                    }


                    // Build output path.
                    ::HIR::TraitPath    out_path;
                    out_path.m_hrtbs = mv$(path.m_hrtbs);
                    out_path.m_path = mv$(path.m_path);
                    out_path.m_trait_ptr = &tr;
                    fill_type_aliases(out_path);
                    // TODO: HRLs?
                    supertraits.push_back( mv$(out_path) );
                    tp_stack.pop_back();
                }

                void fill_type_aliases(HIR::TraitPath& out_path) const
                {
                    const HIR::Trait& tr = *out_path.m_trait_ptr;
                    // - Locate associated types for this trait
                    for(const auto& ty : tr.m_types)
                    {
                        if( out_path.m_type_bounds.count(ty.first) == 0 )
                        {
                            HIR::TypeRef    v;

                            for(auto oit = tp_stack.rbegin(); oit != tp_stack.rend(); ++oit)
                            {
                                auto it = (*oit)->m_type_bounds.find(ty.first);
                                if( it != (*oit)->m_type_bounds.end() ) {
                                    // TODO: Check the source trait
                                    v = it->second.type.clone();
                                    break;
                                }
                            }
                            // TODO: What if there's multiple?
                            DEBUG(ty.first << " = " << v);

                            if( v != ::HIR::TypeRef() )
                            {
                                out_path.m_type_bounds.insert( ::std::make_pair(ty.first, ::HIR::TraitPath::AtyEqual { out_path.m_path.clone(), mv$(v) }) );
                            }
                        }

                        if( out_path.m_trait_bounds.count(ty.first) == 0 )
                        {
                            std::vector<HIR::TraitPath> traits;
                            for(auto oit = tp_stack.rbegin(); oit != tp_stack.rend(); ++oit)
                            {
                                auto it = (*oit)->m_trait_bounds.find(ty.first);
                                if( it != (*oit)->m_trait_bounds.end() )
                                {
                                    // TODO: Check the source trait
                                    for(const auto& t : it->second.traits)
                                        traits.push_back(t.clone());
                                }
                            }
                            DEBUG(ty.first << ": " << traits);
                            if( !traits.empty() )
                            {
                                out_path.m_trait_bounds.insert( ::std::make_pair(ty.first, ::HIR::TraitPath::AtyBound { out_path.m_path.clone(), mv$(traits) }) );
                            }
                        }
                    }
                }
            };

            auto this_path = ip.get_simple_path();
            this_path.update_crate_name( m_crate.m_crate_name );

            Enumerate   e;
            for(const auto& pt : tr.m_parent_traits)
            {
                e.enum_supertraits_in(*pt.m_trait_ptr, pt.clone());
            }
            for(const auto& b : tr.m_params.m_bounds)
            {
                if( !b.is_TraitBound() )
                    continue;
                const auto& be = b.as_TraitBound();
                if( be.type != ty_Self )
                    continue;
                const auto& pt = be.trait;

                // TODO: Remove this along with the from_ast.cpp hack
                if( pt.m_path.m_path == this_path )
                {
                    // TODO: Should this restrict based on the parameters
                    continue ;
                }

                e.enum_supertraits_in(*be.trait.m_trait_ptr, be.trait.clone());
            }

            ::std::sort(e.supertraits.begin(), e.supertraits.end());
            DEBUG("supertraits = " << e.supertraits);
            if( e.supertraits.size() > 0 )
            {
                bool dedeup_done = false;
                auto prev = e.supertraits.begin();
                for(auto it = e.supertraits.begin()+1; it != e.supertraits.end(); )
                {
                    if( prev->m_path == it->m_path )
                    {
                        if( *prev == *it ) {
                        }
                        else if( prev->m_type_bounds.size() == 0 ) {
                            ::std::swap(*prev, *it);
                        }
                        else if( it->m_type_bounds.size() == 0 ) {
                        }
                        else {
                            for(auto& e : it->m_type_bounds ) {
                                if( prev->m_type_bounds.count(e.first) ) {
                                    ASSERT_BUG(sp, prev->m_type_bounds[e.first].type == e.second.type,
                                        "TODO: Handle mismatched type bounds in merging supertrait ATY bounds: " << e.first << " =\n " <<
                                        prev->m_type_bounds[e.first] << "\n " << e.second.type);
                                }
                                prev->m_type_bounds.insert(std::move(e));
                            }
                        }
                        it = e.supertraits.erase(it);
                        dedeup_done = true;
                    }
                    else
                    {
                        ++ it;
                        ++ prev;
                    }
                }
                if( dedeup_done ) {
                    DEBUG("supertraits dd = " << e.supertraits);
                }
            }
            tr.m_all_parent_traits = mv$(e.supertraits);
        }
    };

    class Visitor_Post:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;

        typeck::ModuleState m_ms;

    public:
        Visitor_Post(const ::HIR::Crate& crate)
            : m_crate(crate)
            , m_ms(crate)
        {
        }

        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            m_ms.push_traits(p, mod);
            ::HIR::Visitor::visit_module(p, mod);
            m_ms.pop_traits(mod);
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            visit_type_inner(ty);
        }
        void visit_type_inner(::HIR::TypeRef& ty, bool do_bind=true)
        {
            //TRACE_FUNCTION_F(ty);
            static Span sp;

            if( auto* te = ty.data_mut().opt_NamedFunction() )
            {
                if(te->def.is_Function() && te->def.as_Function() == nullptr)
                {
                    StaticTraitResolve  resolve { m_crate };
                    resolve.set_both_generics_raw(m_ms.m_impl_generics, m_ms.m_item_generics);
                    MonomorphState  unused_ms;
                    const auto& v = resolve.get_value(sp, te->path, unused_ms, true);

                    TU_MATCH_HDRA( (v), {)
                    default:
                        TODO(sp, "Resolve external NamedFunction type - " << te->path << " : " << v.tag_str());
                    TU_ARMA(Function, e) {
                        te->def = e;
                        }
                    TU_ARMA(StructConstructor, e) {
                        te->def = e.s;
                        }
                    TU_ARMA(EnumConstructor, e) {
                        te->def = ::HIR::TypeData_NamedFunction_Ty::make_EnumConstructor({ e.e, e.v });
                        }
                    }
                }
            }

            ::HIR::Visitor::visit_type(ty);
        }

        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type << " - from " << impl.m_src_module);
            auto _ = this->m_ms.set_impl_generics(impl.m_params);

            const auto* mod = (impl.m_src_module != ::HIR::SimplePath() ? &this->m_ms.m_crate.get_mod_by_path(Span(), impl.m_src_module) : nullptr);
            if(mod)
                m_ms.push_traits(impl.m_src_module, *mod);
            ::HIR::Visitor::visit_type_impl(impl);
            if(mod)
                m_ms.pop_traits(*mod);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            auto _ = this->m_ms.set_impl_generics(impl.m_params);

            const auto* mod = (impl.m_src_module != ::HIR::SimplePath() ? &this->m_ms.m_crate.get_mod_by_path(Span(), impl.m_src_module) : nullptr);
            if(mod)
                m_ms.push_traits(impl.m_src_module, *mod);
            m_ms.m_traits.push_back( ::std::make_pair( &trait_path, &this->m_ms.m_crate.get_trait_by_path(Span(), trait_path) ) );
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
            m_ms.m_traits.pop_back( );
            if(mod)
                m_ms.pop_traits(*mod);
        }
        void visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type << " { }");
            auto _ = this->m_ms.set_impl_generics(impl.m_params);

            const auto* mod = (impl.m_src_module != ::HIR::SimplePath() ? &this->m_ms.m_crate.get_mod_by_path(Span(), impl.m_src_module) : nullptr);
            if(mod) {
                m_ms.push_traits(impl.m_src_module, *mod);
            }
            ::HIR::Visitor::visit_marker_impl(trait_path, impl);
            if(mod)
                m_ms.pop_traits(*mod);
        }

        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override
        {
            auto _ = this->m_ms.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
        }

        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override
        {
            auto _ = this->m_ms.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_enum(p, item);
        }
        void visit_struct(::HIR::ItemPath p, ::HIR::Struct& item) override
        {
            auto _ = this->m_ms.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_struct(p, item);
        }
        void visit_union(::HIR::ItemPath p, ::HIR::Union& item) override
        {
            auto _ = this->m_ms.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_union(p, item);
        }

        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override
        {
            auto _ = this->m_ms.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_function(p, item);
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override
        {
            //auto _ = this->m_ms.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_static(p, item);
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override
        {
            auto _ = this->m_ms.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_constant(p, item);
        }

        // Actual expressions
        void visit_expr(::HIR::ExprPtr& expr) override
        {
            struct ExprVisitor:
                public ::HIR::ExprVisitorDef
            {
                Visitor_Post& upper_visitor;

                ExprVisitor(Visitor_Post& uv):
                    upper_visitor(uv)
                {}

                void visit_generic_path(::HIR::Visitor::PathContext pc, ::HIR::GenericPath& p) override
                {
                    upper_visitor.visit_generic_path(p, pc);
                }
                void visit_type(::HIR::TypeRef& ty) override
                {
                    upper_visitor.visit_type_inner(ty, true);
                }

                void visit_node_ptr(::HIR::ExprNodeP& node_ptr) override
                {
                    upper_visitor.visit_type(node_ptr->m_res_type);
                    ::HIR::ExprVisitorDef::visit_node_ptr(node_ptr);
                }
                void visit(::HIR::ExprNode_Let& node) override
                {
                    upper_visitor.visit_type(node.m_type);
                    upper_visitor.visit_pattern(node.m_pattern);
                    ::HIR::ExprVisitorDef::visit(node);
                }
                void visit(::HIR::ExprNode_Match& node) override
                {
                    for(auto& arm : node.m_arms)
                    {
                        for(auto& pat : arm.m_patterns)
                            upper_visitor.visit_pattern(pat);
                        for(auto& g : arm.m_guards)
                            upper_visitor.visit_pattern(g.pat);
                    }
                    ::HIR::ExprVisitorDef::visit(node);
                }

                void visit(::HIR::ExprNode_PathValue& node) override
                {
                    upper_visitor.visit_path(node.m_path, ::HIR::Visitor::PathContext::VALUE);
                }
                void visit(::HIR::ExprNode_CallPath& node) override
                {
                    upper_visitor.visit_path(node.m_path, ::HIR::Visitor::PathContext::VALUE);
                    ::HIR::ExprVisitorDef::visit(node);
                }
                void visit(::HIR::ExprNode_CallMethod& node) override
                {
                    upper_visitor.visit_path_params(node.m_params);
                    ::HIR::ExprVisitorDef::visit(node);
                }

                void visit(::HIR::ExprNode_StructLiteral& node) override
                {
                    upper_visitor.visit_type_inner(node.m_type, false);

                    ::HIR::ExprVisitorDef::visit(node);
                }
                void visit(::HIR::ExprNode_ArraySized& node) override
                {
                    auto& as = node.m_size;
                    if( as.is_Unevaluated() )
                    {
                        upper_visitor.visit_constgeneric(as.as_Unevaluated());
                    }
                    ::HIR::ExprVisitorDef::visit(node);
                }

                void visit(::HIR::ExprNode_Closure& node) override
                {
                    upper_visitor.visit_type(node.m_return);
                    for(auto& arg : node.m_args) {
                        upper_visitor.visit_pattern(arg.first);
                        upper_visitor.visit_type(arg.second);
                    }
                    ::HIR::ExprVisitorDef::visit(node);
                }
            };

            for(auto& ty : expr.m_erased_types)
                visit_type(ty);

            // Local expression
            if( expr.get() != nullptr )
            {
                ExprVisitor v { *this };
                (*expr).visit(v);
            }
            // External expression (has MIR)
            else if( auto* mir = expr.get_ext_mir_mut() )
            {
                for(auto& ty : mir->locals)
                    this->visit_type(ty);
                struct MirVisitor: public ::MIR::visit::VisitorMut
                {
                    Visitor_Post& upper_visitor;
                    MirVisitor(Visitor_Post& upper_visitor):
                        upper_visitor(upper_visitor)
                    {
                    }
                    void visit_type(::HIR::TypeRef& t) override {
                        upper_visitor.visit_type(t);
                    }
                    void visit_path(::HIR::Path& p) override {
                        upper_visitor.visit_path(p, ::HIR::Visitor::PathContext::VALUE);
                    }
                    bool visit_lvalue(::MIR::LValue& lv, ::MIR::visit::ValUsage u) override {
                        if( lv.m_root.is_Static() ) {
                            upper_visitor.visit_path(lv.m_root.as_Static(), ::HIR::Visitor::PathContext::VALUE);
                        }
                        return false;
                    }
                };
                MirVisitor  mv(*this);
                for(auto& block : mir->blocks)
                {
                    for(auto& stmt : block.statements)
                    {
                        mv.visit_stmt(stmt);
                    }
                    mv.visit_terminator(block.terminator);
                }
            }
            else
            {
            }
        }
    };
}

void ConvertHIR_Bind(::HIR::Crate& crate)
{
    {
        Visitor exp { crate };
        // Also visit extern crates to update their pointers
        for(auto& ec : crate.m_ext_crates)
        {
            exp.visit_crate( *ec.second.m_data );
        }
        exp.visit_crate( crate );
    }

    {
        Visitor_Post v { crate };
        for(auto& ec : crate.m_ext_crates)
        {
            v.visit_crate( *ec.second.m_data );
        }
        v.visit_crate(crate);
    }


    // Populate supertrait list
    Visitor_EnumSuperTraits(crate).visit_crate(crate);
}
