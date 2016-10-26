/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_conv/bind.cpp
 * - Set binding pointers in HIR structures
 */
#include "main_bindings.hpp"
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <algorithm>    // std::find_if

#include <hir_typeck/static.hpp>

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
        // NOTE: Can't share with HIR::Crate::get_typeitem_by_path because it has to handle enum variants
        const ::HIR::Module*    mod;
        if( path.m_crate_name != "" ) {
            ASSERT_BUG(sp, crate.m_ext_crates.count(path.m_crate_name) > 0, "Crate '" << path.m_crate_name << "' not loaded");
            mod = &crate.m_ext_crates.at(path.m_crate_name)->m_root_module;
        }
        else {
            mod = &crate.m_root_module;
        }
        
        for( unsigned int i = 0; i < path.m_components.size()-1; i ++ )
        {
            const auto& pc = path.m_components[i];
            auto it = mod->m_mod_items.find( pc );
            if( it == mod->m_mod_items.end() ) {
                BUG(sp, "Couldn't find component " << i << " of " << path);
            }
            
            // If second-last, and an enum variant is desired, return the pointer to the enum
            if( i+1 == path.m_components.size()-1 && t == Target::EnumVariant )
            {
                TU_IFLET(::HIR::TypeItem, it->second->ent, Enum, e2,
                    return &e2;
                )
                else {
                    ERROR(sp, E0000, "Expected an enum at the penultimate node of " << path << ", got a " << it->second->ent.tag_str());
                }
            }
            else {
                TU_MATCH_DEF( ::HIR::TypeItem, (it->second->ent), (e2),
                (
                    BUG(sp, "Node " << i << " of path " << path << " wasn't a module");
                    ),
                (Module,
                    mod = &e2;
                    )
                )
            }
        }

        const auto& pc = path.m_components.back();
        auto it = mod->m_mod_items.find( pc );
        if( it == mod->m_mod_items.end() ) {
            BUG(sp, "Couldn't find final component of " << path);
        }
        
        switch(t)
        {
        case Target::TypeItem:  return &it->second->ent;
        case Target::EnumVariant:   throw "";
        
        case Target::Struct:
            TU_IFLET(::HIR::TypeItem, it->second->ent, Struct, e2,
                return &e2;
            )
            else {
                ERROR(sp, E0000, "Expected a struct at " << path << ", got a " << it->second->ent.tag_str());
            }
            break;
        case Target::Enum:
            TU_IFLET(::HIR::TypeItem, it->second->ent, Enum, e2,
                return &e2;
            )
            else {
                ERROR(sp, E0000, "Expected a enum at " << path << ", got a " << it->second->ent.tag_str());
            }
            break;
        }
        throw "";
    }
    
    void fix_type_params(const Span& sp, const ::HIR::GenericParams& params_def, ::HIR::PathParams& params)
    {
        if( params.m_types.size() == 0 ) {
            params.m_types.resize( params_def.m_types.size() );
        }
        if( params.m_types.size() != params_def.m_types.size() ) {
            ERROR(sp, E0000, "Incorrect parameter count, expected " << params_def.m_types.size() << ", got " << params.m_types.size());
        }
    }
    
    const ::HIR::Struct& get_struct_ptr(const Span& sp, const ::HIR::Crate& crate, ::HIR::GenericPath& path) {
        const auto& str = *reinterpret_cast< const ::HIR::Struct*>( get_type_pointer(sp, crate, path.m_path, Target::Struct) );
        fix_type_params(sp, str.m_params,  path.m_params);
        return str;
    }
    ::std::pair< const ::HIR::Enum*, unsigned int> get_enum_ptr(const Span& sp, const ::HIR::Crate& crate, ::HIR::GenericPath& path) {
        const auto& enm = *reinterpret_cast< const ::HIR::Enum*>( get_type_pointer(sp, crate, path.m_path, Target::EnumVariant) );
        const auto& des_name = path.m_path.m_components.back();
        unsigned int idx = ::std::find_if( enm.m_variants.begin(), enm.m_variants.end(), [&](const auto& x) { return x.first == des_name; }) - enm.m_variants.begin();
        if( idx == enm.m_variants.size() ) {
            ERROR(sp, E0000, "Couldn't find enum variant " << path);
        }
        
        fix_type_params(sp, enm.m_params,  path.m_params);
        return ::std::make_pair( &enm, idx );
    }
    
    
    class Visitor:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
        const ::HIR::SimplePath&    m_lang_CoerceUnsized;
        const ::HIR::SimplePath&    m_lang_Deref;

    public:
        Visitor(const ::HIR::Crate& crate):
            m_crate(crate),
            m_lang_CoerceUnsized(m_crate.get_lang_item_path_opt("coerce_unsized")),
            m_lang_Deref(m_crate.get_lang_item_path_opt("coerce_unsized"))
        {}
        
        void visit_trait_path(::HIR::TraitPath& p) override
        {
            static Span sp;
            p.m_trait_ptr = &m_crate.get_trait_by_path(sp, p.m_path.m_path);
            
            ::HIR::Visitor::visit_trait_path(p);
        }
        
        
        void visit_pattern_Value(const Span& sp, ::HIR::Pattern& pat, ::HIR::Pattern::Value& val)
        {
            bool is_single_value = pat.m_data.is_Value();
            
            TU_IFLET( ::HIR::Pattern::Value, val, Named, ve,
                TU_IFLET( ::HIR::Path::Data, ve.path.m_data, Generic, pe,
                    const ::HIR::Enum* enm = nullptr;
                    const auto& path = pe.m_path;
                    const ::HIR::Module*  mod;
                    if( path.m_crate_name != "" ) {
                        ASSERT_BUG(sp, m_crate.m_ext_crates.count(path.m_crate_name) > 0, "Crate '" << path.m_crate_name << "' not loaded");
                        mod = &m_crate.m_ext_crates.at(path.m_crate_name)->m_root_module;
                    }
                    else {
                        mod = &m_crate.m_root_module;
                    }
                    for(unsigned int i = 0; i < path.m_components.size() - 1; i ++ )
                    {
                        const auto& pc = path.m_components[i];
                        auto it = mod->m_mod_items.find( pc );
                        if( it == mod->m_mod_items.end() ) {
                            BUG(sp, "Couldn't find component " << i << " of " << path);
                        }
                        
                        if( i == path.m_components.size() - 2 ) {
                            // Here it's allowed to be either a module, or an enum.
                            TU_IFLET( ::HIR::TypeItem, it->second->ent, Module, e2,
                                mod = &e2;
                            )
                            else TU_IFLET( ::HIR::TypeItem, it->second->ent, Enum, e2,
                                enm = &e2;
                            )
                            else {
                                BUG(sp, "Node " << i << " of path " << ve.path << " wasn't a module or enum");
                            }
                        }
                        else {
                            TU_IFLET( ::HIR::TypeItem, it->second->ent, Module, e2,
                                mod = &e2;
                            )
                            else {
                                BUG(sp, "Node " << i << " of path " << ve.path << " wasn't a module");
                            }
                        }
                    }
                    const auto& pc = path.m_components.back();
                    if( enm ) {
                        if( !is_single_value ) {
                            ERROR(sp, E0000, "Enum variant in range pattern - " << pat);
                        }
                        
                        // Enum variant
                        auto it = ::std::find_if( enm->m_variants.begin(), enm->m_variants.end(), [&](const auto&v){ return v.first == pc; });
                        if( it == enm->m_variants.end() ) {
                            BUG(sp, "'" << pc << "' isn't a variant in path " << path);
                        }
                        unsigned int index = it - enm->m_variants.begin();
                        auto path = mv$(pe);
                        fix_type_params(sp, enm->m_params,  path.m_params);
                        pat.m_data = ::HIR::Pattern::Data::make_EnumValue({
                            mv$(path),
                            enm,
                            index
                            });
                    }
                    else {
                        auto it = mod->m_value_items.find( pc );
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
            
            TU_MATCH_DEF(::HIR::Pattern::Data, (pat.m_data), (e),
            (
                ),
            (Value,
                this->visit_pattern_Value(sp, pat, e.val);
                ),
            (Range,
                this->visit_pattern_Value(sp, pat, e.start);
                this->visit_pattern_Value(sp, pat, e.end);
                ),
            (StructTuple,
                const auto& str = get_struct_ptr(sp, m_crate, e.path);
                TU_IFLET(::HIR::Struct::Data, str.m_data, Tuple, _,
                    e.binding = &str;
                )
                else {
                    ERROR(sp, E0000, "Struct tuple pattern on non-tuple struct " << e.path);
                }
                ),
            (Struct,
                const auto& str = get_struct_ptr(sp, m_crate, e.path);
                TU_IFLET(::HIR::Struct::Data, str.m_data, Named, _,
                    e.binding = &str;
                )
                else {
                    ERROR(sp, E0000, "Struct pattern on field-less struct " << e.path);
                }
                ),
            (EnumTuple,
                auto p = get_enum_ptr(sp, m_crate, e.path);
                const auto& var = p.first->m_variants[p.second].second;
                TU_IFLET(::HIR::Enum::Variant, var, Tuple, _,
                    e.binding_ptr = p.first;
                    e.binding_idx = p.second;
                )
                else {
                    ERROR(sp, E0000, "Enum tuple pattern on non-tuple variant " << e.path);
                }
                ),
            (EnumStruct,
                auto p = get_enum_ptr(sp, m_crate, e.path);
                const auto& var = p.first->m_variants[p.second].second;
                TU_IFLET(::HIR::Enum::Variant, var, Struct, _,
                    // All good
                    e.binding_ptr = p.first;
                    e.binding_idx = p.second;
                )
                else {
                    ERROR(sp, E0000, "Enum tuple pattern on non-tuple variant " << e.path);
                }
                )
            )
        }
        static void fix_param_count(const Span& sp, const ::HIR::GenericPath& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params) {
            if( params.m_types.size() == param_defs.m_types.size() ) {
                // Nothing to do, all good
                return ;
            }
            
            if( params.m_types.size() == 0 ) {
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
                        // TODO: What if this contains a generic param? (is that valid? Self maybe, what about others?)
                        params.m_types.push_back( typ.m_default.clone() );
                    }
                }
            }
        }
        void visit_type(::HIR::TypeRef& ty) override
        {
            //TRACE_FUNCTION_F(ty);
            static Span _sp = Span();
            const Span& sp = _sp;
            
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Path, e,
                TU_MATCH( ::HIR::Path::Data, (e.path.m_data), (pe),
                (Generic,
                    const auto& item = *reinterpret_cast< const ::HIR::TypeItem*>( get_type_pointer(sp, m_crate, pe.m_path, Target::TypeItem) );
                    TU_MATCH_DEF( ::HIR::TypeItem, (item), (e3),
                    (
                        ERROR(sp, E0000, "Unexpected item type returned for " << pe.m_path << " - " << item.tag_str());
                        ),
                    (TypeAlias,
                        BUG(sp, "TypeAlias encountered after `Resolve Type Aliases` - " << ty);
                        ),
                    (Struct,
                        fix_param_count(sp, pe, e3.m_params,  pe.m_params);
                        e.binding = ::HIR::TypeRef::TypePathBinding::make_Struct(&e3);
                        DEBUG("- " << ty);
                        ),
                    (Enum,
                        fix_param_count(sp, pe, e3.m_params,  pe.m_params);
                        e.binding = ::HIR::TypeRef::TypePathBinding::make_Enum(&e3);
                        DEBUG("- " << ty);
                        ),
                    (Trait,
                        ty.m_data = ::HIR::TypeRef::Data::make_TraitObject({ ::HIR::TraitPath { mv$(pe), {}, {} }, {}, {} });
                        )
                    )
                    ),
                (UfcsUnknown,
                    //TODO(sp, "Should UfcsKnown be encountered here?");
                    ),
                (UfcsInherent,
                    ),
                (UfcsKnown,
                    if( pe.type->m_data.is_Path() && pe.type->m_data.as_Path().binding.is_Opaque() ) {
                        // - Opaque type, opaque result
                        e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                    }
                    else if( pe.type->m_data.is_Generic() ) {
                        // - Generic type, opaque resut. (TODO: Sometimes these are known - via generic bounds)
                        e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
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
                    )
                )
            )
            
            ::HIR::Visitor::visit_type(ty);
        }
        
        void visit_expr(::HIR::ExprPtr& expr) override
        {
            struct ExprVisitor:
                public ::HIR::ExprVisitorDef
            {
                Visitor& upper_visitor;
                
                ExprVisitor(Visitor& uv):
                    upper_visitor(uv)
                {}
                
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
            
            if( expr.get() != nullptr )
            {
                ExprVisitor v { *this };
                (*expr).visit(v);
            }
        }
        
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
            
            TU_IFLET(::HIR::TypeRef::Data, impl.m_type.m_data, Path, te,
                ::HIR::TraitMarkings*   markings = nullptr;
                TU_MATCHA( (te.binding), (tpb),
                (Unbound,
                    // Wut?
                    ),
                (Opaque,
                    // Just ignore, it's a wildcard... wait? is that valid?
                    ),
                (Struct,
                    markings = &const_cast<HIR::Struct*>(tpb)->m_markings;
                    ),
                (Enum,
                    markings = &const_cast<HIR::Enum*>(tpb)->m_markings;
                    )
                )
                
                if( markings )
                {
                    // CoerceUnsized - set flag to avoid needless searches later
                    if( trait_path == m_lang_CoerceUnsized ) {
                        markings->can_coerce = true;
                    }
                    // Deref - set flag to avoid needless searches later
                    else if( trait_path == m_lang_Deref ) {
                        markings->has_a_deref = true;
                    }
                }
            )
        }
    };
}

void ConvertHIR_Bind(::HIR::Crate& crate)
{
    Visitor exp { crate };
    exp.visit_crate( crate );
    
    // Also visit extern crates to update their pointers
    for(auto& ec : crate.m_ext_crates)
    {
        exp.visit_crate( *ec.second );
    }
}
