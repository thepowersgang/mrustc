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

#include <hir_typeck/static.hpp>
#include <hir_typeck/expr_visit.hpp>    // For ModuleState
#include <hir/expr_state.hpp>

void ConvertHIR_Bind(::HIR::Crate& crate);

namespace {


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
            return &crate.get_typeitem_by_path(sp, path, false, true).as_Enum();
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
        if( params.m_types.size() == 0 ) {
            params.m_types.resize( params_def.m_types.size() );
        }
        if( params.m_types.size() != params_def.m_types.size() ) {
            ERROR(sp, E0000, "Incorrect parameter count, expected " << params_def.m_types.size() << ", got " << params.m_types.size());
        }
        #endif
    }

    const ::HIR::Struct& get_struct_ptr(const Span& sp, const ::HIR::Crate& crate, ::HIR::GenericPath& path) {
        const auto& str = *reinterpret_cast< const ::HIR::Struct*>( get_type_pointer(sp, crate, path.m_path, Target::Struct) );
        fix_type_params(sp, str.m_params,  path.m_params);
        return str;
    }
    ::std::pair< const ::HIR::Enum*, unsigned int> get_enum_ptr(const Span& sp, const ::HIR::Crate& crate, ::HIR::GenericPath& path) {
        const auto& enm = *reinterpret_cast< const ::HIR::Enum*>( get_type_pointer(sp, crate, path.m_path, Target::EnumVariant) );
        const auto& des_name = path.m_path.m_components.back();
        auto idx = enm.find_variant(des_name);
        if( idx == SIZE_MAX ) {
            ERROR(sp, E0000, "Couldn't find enum variant " << path);
        }

        fix_type_params(sp, enm.m_params,  path.m_params);
        return ::std::make_pair( &enm, static_cast<unsigned>(idx) );
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

    public:
        Visitor(const ::HIR::Crate& crate):
            m_crate(crate),
            m_ms(crate)
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

        void visit_literal(const Span& sp, ::HIR::Literal& lit)
        {
            TU_MATCH(::HIR::Literal, (lit), (e),
            (Invalid,
                ),
            (Defer,
                // Shouldn't happen here, but ...
                ),
            (List,
                for(auto& val : e) {
                    visit_literal(sp, val);
                }
                ),
            (Variant,
                visit_literal(sp, *e.val);
                ),
            (Integer,
                ),
            (Float,
                ),
            (BorrowPath,
                visit_path(e, ::HIR::Visitor::PathContext::VALUE);
                ),
            (BorrowData,
                visit_literal(sp, *e);
                ),
            (String,
                )
            )
        }

        void visit_pattern_Value(const Span& sp, ::HIR::Pattern& pat, ::HIR::Pattern::Value& val)
        {
            bool is_single_value = pat.m_data.is_Value();

            TU_IFLET( ::HIR::Pattern::Value, val, Named, ve,
                TU_IFLET( ::HIR::Path::Data, ve.path.m_data, Generic, pe,
                    const auto& path = pe.m_path;
                    const auto& pc = path.m_components.back();
                    const ::HIR::Module*  mod = nullptr;
                    if( path.m_components.size() == 1 )
                    {
                        mod = &m_crate.get_mod_by_path(sp, path, true);
                    }
                    else
                    {
                        const auto& ti = m_crate.get_typeitem_by_path(sp, path, false, true);
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
                            auto path = mv$(pe);
                            fix_type_params(sp, enm->m_params,  path.m_params);
                            pat.m_data = ::HIR::Pattern::Data::make_EnumValue({
                                mv$(path),
                                enm,
                                static_cast<unsigned>(idx)
                                });
                        }
                        else if( (mod = ti.opt_Module()) )
                        {
                            mod = &ti.as_Module();
                        }
                        else
                        {
                            BUG(sp, "Node " << path.m_components.size()-2 << " of path " << ve.path << " wasn't a module");
                        }
                    }

                    if( mod )
                    {
                        auto it = mod->m_value_items.find( path.m_components.back() );
                        if( it == mod->m_value_items.end() ) {
                            BUG(sp, "Couldn't find final component of " << path);
                        }
                        // Unit-like struct match or a constant
                        TU_MATCH_DEF( ::HIR::ValueItem, (it->second->ent), (e2),
                        (
                            ERROR(sp, E0000, "Value pattern " << pat << " pointing to unexpected item type - " << it->second->ent.tag_str())
                            ),
                        (Constant,
                            // Store reference to this item for later use
                            ve.binding = &e2;
                            ),
                        (StructConstant,
                            const auto& str = mod->m_mod_items.find(pc)->second->ent.as_Struct();
                            // Convert into a dedicated pattern type
                            if( !is_single_value ) {
                                ERROR(sp, E0000, "Struct in range pattern - " << pat);
                            }
                            auto path = mv$(pe);
                            fix_type_params(sp, str.m_params,  path.m_params);
                            pat.m_data = ::HIR::Pattern::Data::make_StructValue({
                                mv$(path),
                                &str
                                });
                            )
                        )
                    }
                )
                else {
                    // NOTE: Defer until Resolve UFCS (saves duplicating logic)
                }
            )
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
                this->visit_pattern_Value(sp, pat, e.start);
                this->visit_pattern_Value(sp, pat, e.end);
                }
            TU_ARMA(StructValue, e) {
                const auto& str = get_struct_ptr(sp, m_crate, e.path);
                TU_IFLET(::HIR::Struct::Data, str.m_data, Unit, _,
                    e.binding = &str;
                )
                else {
                    ERROR(sp, E0000, "Struct value pattern on non-unit struct " << e.path);
                }
                }
            TU_ARMA(StructTuple, e) {
                const auto& str = get_struct_ptr(sp, m_crate, e.path);
                TU_IFLET(::HIR::Struct::Data, str.m_data, Tuple, _,
                    e.binding = &str;
                )
                else {
                    ERROR(sp, E0000, "Struct tuple pattern on non-tuple struct " << e.path);
                }
                }
            TU_ARMA(Struct, e) {
                const auto& str = get_struct_ptr(sp, m_crate, e.path);
                if(str.m_data.is_Named() ) {
                }
                else if( str.m_data.is_Unit() && e.sub_patterns.size() == 0 ) {
                }
                else if( str.m_data.is_Tuple() && str.m_data.as_Tuple().empty() && e.sub_patterns.size() == 0 ) {
                }
                else {
                    ERROR(sp, E0000, "Struct pattern `" << pat << "` on field-less struct " << e.path);
                }
                e.binding = &str;
                }
            TU_ARMA(EnumValue, e) {
                auto p = get_enum_ptr(sp, m_crate, e.path);
                if( p.first->m_data.is_Data() )
                {
                    const auto& var = p.first->m_data.as_Data()[p.second];
                    if( var.is_struct || var.type != ::HIR::TypeRef::new_unit() )
                        ERROR(sp, E0000, "Enum value pattern on non-unit variant " << e.path);
                }
                e.binding_ptr = p.first;
                e.binding_idx = p.second;
                }
            TU_ARMA(EnumTuple, e) {
                auto p = get_enum_ptr(sp, m_crate, e.path);
                if( !p.first->m_data.is_Data() )
                    ERROR(sp, E0000, "Enum tuple pattern on non-tuple variant " << e.path);
                const auto& var = p.first->m_data.as_Data()[p.second];
                if( var.is_struct )
                    ERROR(sp, E0000, "Enum tuple pattern on non-tuple variant " << e.path);
                e.binding_ptr = p.first;
                e.binding_idx = p.second;
                }
            TU_ARMA(EnumStruct, e) {
                auto p = get_enum_ptr(sp, m_crate, e.path);
                if( !e.is_exhaustive && e.sub_patterns.empty() )
                {
                    if( !p.first->m_data.is_Data() ) {
                        pat.m_data = ::HIR::Pattern::Data::make_EnumValue({
                                ::std::move(e.path), p.first, p.second
                                });
                    }
                    else {
                        const auto& var = p.first->m_data.as_Data()[p.second];
                        if( var.type == ::HIR::TypeRef::new_unit() )
                        {
                            pat.m_data = ::HIR::Pattern::Data::make_EnumValue({
                                    ::std::move(e.path), p.first, p.second
                                    });
                        }
                        else if( !var.is_struct )
                        {
                            ASSERT_BUG(sp, var.type.m_data.is_Path(), "");
                            ASSERT_BUG(sp, var.type.m_data.as_Path().binding.is_Struct(), "EnumStruct pattern on unexpected variant " << e.path << " with " << var.type.m_data.as_Path().binding.tag_str());
                            const auto& str = *var.type.m_data.as_Path().binding.as_Struct();
                            ASSERT_BUG(sp, str.m_data.is_Tuple(), "");
                            const auto& flds = str.m_data.as_Tuple();
                            ::std::vector<HIR::Pattern> subpats;
                            for(size_t i = 0; i < flds.size(); i ++)
                                subpats.push_back(::HIR::Pattern { });
                            pat.m_data = ::HIR::Pattern::Data::make_EnumTuple({
                                    ::std::move(e.path), p.first, p.second, mv$(subpats)
                                    });
                        }
                        else
                        {
                            // Keep as a struct pattern
                        }
                    }
                }
                else
                {
                    if( !p.first->m_data.is_Data() )
                    {
                        ERROR(sp, E0000, "Enum struct pattern `" << pat << "` on non-struct variant " << e.path);
                    }
                    else
                    {
                        const auto& var = p.first->m_data.as_Data()[p.second];
                        if( !var.is_struct )
                            ERROR(sp, E0000, "Enum struct pattern `" << pat << "` on non-struct variant " << e.path);
                    }
                }
                e.binding_ptr = p.first;
                e.binding_idx = p.second;
                }
            }
        }
        static void fix_param_count(const Span& sp, const ::HIR::GenericPath& path, const ::HIR::GenericParams& param_defs, ::HIR::PathParams& params, bool fill_infer=true, const ::HIR::TypeRef* self_ty=nullptr)
        {
            if( params.m_types.size() == param_defs.m_types.size() ) {
                // Nothing to do, all good
                return ;
            }

            TRACE_FUNCTION_F(path);

            if( params.m_types.size() == 0 && fill_infer ) {
                for(const auto& typ : param_defs.m_types) {
                    (void)typ;
                    params.m_types.push_back( ::HIR::TypeRef() );
                }
            }
            else if( params.m_types.size() > param_defs.m_types.size() ) {
                ERROR(sp, E0000, "Too many type parameters passed to " << path);
            }
            else {
                while( params.m_types.size() < param_defs.m_types.size() ) {
                    const auto& typ = param_defs.m_types[params.m_types.size()];
                    if( typ.m_default.m_data.is_Infer() ) {
                        ERROR(sp, E0000, "Omitted type parameter with no default in " << path);
                    }
                    else {
                        // Clone, replacing `self` if a replacement was provided.
                        auto ty = clone_ty_with(sp, typ.m_default, [&](const auto& ty, auto& out){
                            if(const auto* te = ty.m_data.opt_Generic() )
                            {
                                if( te->binding == GENERIC_Self ) {
                                    if( !self_ty )
                                        TODO(sp, "Self enountered in default params, but no Self available - " << ty << " in " << typ.m_default << " for " << path);
                                    out = self_ty->clone();
                                }
                                // NOTE: Should only be seeing impl-level params here. Method-level ones are only seen in expression context.
                                else if( (te->binding >> 8) == 0 ) {
                                    auto idx = te->binding & 0xFF;
                                    ASSERT_BUG(sp, idx < params.m_types.size(), "TODO: Handle use of latter types in defaults");
                                    out = params.m_types[idx].clone();
                                }
                                else {
                                    TODO(sp, "Monomorphise in fix_param_count - encountered " << ty << " in " << typ.m_default);
                                }
                                return true;
                            }
                            return false;
                            });
                        params.m_types.push_back( mv$(ty) );
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
                    const auto& trait = m_crate.get_trait_by_path(sp, be->trait.m_path.m_path);
                    fix_param_count(sp, be->trait.m_path, trait.m_params, be->trait.m_path.m_params, /*fill_infer=*/false, &be->type);
                }
            }

            ::HIR::Visitor::visit_params(params);
        }
        void visit_type(::HIR::TypeRef& ty) override
        {
            visit_type_inner(ty);
        }
        void visit_type_inner(::HIR::TypeRef& ty, bool do_bind=true)
        {
            //TRACE_FUNCTION_F(ty);
            static Span sp;

            if(auto* e = ty.m_data.opt_Path())
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
                        ),
                    (ExternType,
                        e->binding = ::HIR::TypePathBinding::make_ExternType(&e3);
                        DEBUG("- " << ty);
                        ),
                    (Struct,
                        fix_param_count(sp, pe, e3.m_params,  pe.m_params);
                        e->binding = ::HIR::TypePathBinding::make_Struct(&e3);
                        DEBUG("- " << ty);
                        ),
                    (Union,
                        fix_param_count(sp, pe, e3.m_params,  pe.m_params);
                        e->binding = ::HIR::TypePathBinding::make_Union(&e3);
                        DEBUG("- " << ty);
                        ),
                    (Enum,
                        fix_param_count(sp, pe, e3.m_params,  pe.m_params);
                        e->binding = ::HIR::TypePathBinding::make_Enum(&e3);
                        DEBUG("- " << ty);
                        ),
                    (Trait,
                        // TODO: Should this reassign instead?
                        ty.m_data = ::HIR::TypeData::make_TraitObject({ ::HIR::TraitPath { mv$(pe), {}, {} }, {}, {} });
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
                    fix_param_count(sp, pe.trait, trait.m_params, pe.trait.m_params, /*fill_infer=*/false, &*pe.type);

                    if( pe.type->m_data.is_Path() && pe.type->m_data.as_Path().binding.is_Opaque() ) {
                        // - Opaque type, opaque result
                        e->binding = ::HIR::TypePathBinding::make_Opaque({});
                    }
                    else if( pe.type->m_data.is_Generic() ) {
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
        // TODO: Are generics for types "item" or "impl"?
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override
        {
            auto _ = this->m_ms.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_enum(p, item);
        }
        void visit_struct(::HIR::ItemPath p, ::HIR::Struct& item) override
        {
            auto _ = this->m_ms.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_struct(p, item);
        }
        void visit_union(::HIR::ItemPath p, ::HIR::Union& item) override
        {
            auto _ = this->m_ms.set_item_generics(item.m_params);
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
                    }
                    ::HIR::ExprVisitorDef::visit(node);
                }
                void visit(::HIR::ExprNode_Cast& node) override
                {
                    upper_visitor.visit_type(node.m_res_type);
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
                    upper_visitor.visit_expr(node.m_size);
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
                ExprVisitor v { *this };
                (*expr).visit(v);
            }
            // External expression (has MIR)
            else if( auto* mir = expr.get_ext_mir_mut() )
            {
                struct H {
                    static void visit_lvalue(Visitor& upper_visitor, ::MIR::LValue& lv)
                    {
                        if( lv.m_root.is_Static() ) {
                            upper_visitor.visit_path(lv.m_root.as_Static(), ::HIR::Visitor::PathContext::VALUE);
                        }
                    }
                    static void visit_constant(Visitor& upper_visitor, ::MIR::Constant& e)
                    {
                        TU_MATCHA( (e), (ce),
                        (Int, ),
                        (Uint,),
                        (Float, ),
                        (Bool, ),
                        (Bytes, ),
                        (StaticString, ),  // String
                        (Const,
                            upper_visitor.visit_path(*ce.p, ::HIR::Visitor::PathContext::VALUE);
                            ),
                        (Generic,
                            ),
                        (ItemAddr,
                            upper_visitor.visit_path(*ce, ::HIR::Visitor::PathContext::VALUE);
                            )
                        )
                    }
                    static void visit_param(Visitor& upper_visitor, ::MIR::Param& p)
                    {
                        TU_MATCHA( (p), (e),
                        (LValue, H::visit_lvalue(upper_visitor, e);),
                        (Constant,
                            H::visit_constant(upper_visitor, e);
                            )
                        )
                    }
                };
                for(auto& ty : mir->locals)
                    this->visit_type(ty);
                for(auto& block : mir->blocks)
                {
                    for(auto& stmt : block.statements)
                    {
                        TU_IFLET(::MIR::Statement, stmt, Assign, se,
                            H::visit_lvalue(*this, se.dst);
                            TU_MATCHA( (se.src), (e),
                            (Use,
                                H::visit_lvalue(*this, e);
                                ),
                            (Constant,
                                H::visit_constant(*this, e);
                                ),
                            (SizedArray,
                                H::visit_param(*this, e.val);
                                ),
                            (Borrow,
                                H::visit_lvalue(*this, e.val);
                                ),
                            (Cast,
                                H::visit_lvalue(*this, e.val);
                                this->visit_type(e.type);
                                ),
                            (BinOp,
                                H::visit_param(*this, e.val_l);
                                H::visit_param(*this, e.val_r);
                                ),
                            (UniOp,
                                H::visit_lvalue(*this, e.val);
                                ),
                            (DstMeta,
                                H::visit_lvalue(*this, e.val);
                                ),
                            (DstPtr,
                                H::visit_lvalue(*this, e.val);
                                ),
                            (MakeDst,
                                H::visit_param(*this, e.ptr_val);
                                H::visit_param(*this, e.meta_val);
                                ),
                            (Tuple,
                                for(auto& val : e.vals)
                                    H::visit_param(*this, val);
                                ),
                            (Array,
                                for(auto& val : e.vals)
                                    H::visit_param(*this, val);
                                ),
                            (Variant,
                                H::visit_param(*this, e.val);
                                ),
                            (Struct,
                                for(auto& val : e.vals)
                                    H::visit_param(*this, val);
                                )
                            )
                        )
                        else TU_IFLET(::MIR::Statement, stmt, Drop, se,
                            H::visit_lvalue(*this, se.slot);
                        )
                        else {
                        }
                    }
                    TU_MATCHA( (block.terminator), (te),
                    (Incomplete, ),
                    (Return, ),
                    (Diverge, ),
                    (Goto, ),
                    (Panic, ),
                    (If,
                        H::visit_lvalue(*this, te.cond);
                        ),
                    (Switch,
                        H::visit_lvalue(*this, te.val);
                        ),
                    (SwitchValue,
                        H::visit_lvalue(*this, te.val);
                        ),
                    (Call,
                        H::visit_lvalue(*this, te.ret_val);
                        TU_MATCHA( (te.fcn), (e2),
                        (Value,
                            H::visit_lvalue(*this, e2);
                            ),
                        (Path,
                            visit_path(e2, ::HIR::Visitor::PathContext::VALUE);
                            ),
                        (Intrinsic,
                            visit_path_params(e2.params);
                            )
                        )
                        for(auto& arg : te.args)
                            H::visit_param(*this, arg);
                        )
                    )
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
    Visitor exp { crate };

    // Also visit extern crates to update their pointers
    for(auto& ec : crate.m_ext_crates)
    {
        exp.visit_crate( *ec.second.m_data );
    }

    exp.visit_crate( crate );
}
