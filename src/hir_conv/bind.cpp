/*
 * Set binding pointers in TypeRef and Pattern
 */
#include "main_bindings.hpp"
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <algorithm>    // std::find_if

namespace {

    
    enum class Target {
        TypeItem,
        Struct,
        Enum,
        EnumVariant,
    };
    const void* get_type_pointer(const Span& sp, const ::HIR::Crate& crate, const ::HIR::SimplePath& path, Target t)
    {
        if( path.m_crate_name != "" ) {
            TODO(sp, "Handle extern crates");
        }
        
        const ::HIR::Module*    mod = &crate.m_root_module;
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
    
    class Visitor:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;

    public:
        Visitor(const ::HIR::Crate& crate):
            m_crate(crate)
        {}
        
        void visit_pattern(::HIR::Pattern& pat) override
        {
            static Span _sp = Span();
            const Span& sp = _sp;

            ::HIR::Visitor::visit_pattern(pat);
            
            TU_MATCH_DEF(::HIR::Pattern::Data, (pat.m_data), (e),
            (
                ),
            (StructTuple,
                const auto& str = *reinterpret_cast< const ::HIR::Struct*>( get_type_pointer(sp, m_crate, e.path.m_path, Target::Struct) );
                TU_IFLET(::HIR::Struct::Data, str.m_data, Tuple, _,
                    // All good
                )
                else {
                    ERROR(sp, E0000, "Struct tuple pattern on non-tuple struct " << e.path);
                }
                
                fix_type_params(sp, str.m_params,  e.path.m_params);
                e.binding = &str;
                ),
            (Struct,
                const auto& str = *reinterpret_cast< const ::HIR::Struct*>( get_type_pointer(sp, m_crate, e.path.m_path, Target::Struct) );
                TU_IFLET(::HIR::Struct::Data, str.m_data, Named, _,
                    // All good
                )
                else {
                    ERROR(sp, E0000, "Struct pattern on field-less struct " << e.path);
                }
                fix_type_params(sp, str.m_params,  e.path.m_params);
                e.binding = &str;
                ),
            (EnumTuple,
                const auto& enm = *reinterpret_cast< const ::HIR::Enum*>( get_type_pointer(sp, m_crate, e.path.m_path, Target::EnumVariant) );
                const auto& des_name = e.path.m_path.m_components.back();
                unsigned int idx = ::std::find_if( enm.m_variants.begin(), enm.m_variants.end(), [&](const auto& x) { return x.first == des_name; }) - enm.m_variants.begin();
                if( idx == enm.m_variants.size() ) {
                    ERROR(sp, E0000, "Couldn't find enum variant " << e.path);
                }
                const auto& var = enm.m_variants[idx].second;
                TU_IFLET(::HIR::Enum::Variant, var, Tuple, _,
                    // All good
                )
                else {
                    ERROR(sp, E0000, "Enum tuple pattern on non-tuple variant " << e.path);
                }
                fix_type_params(sp, enm.m_params,  e.path.m_params);
                e.binding_ptr = &enm;
                e.binding_idx = idx;
                ),
            (EnumStruct,
                const auto& enm = *reinterpret_cast< const ::HIR::Enum*>( get_type_pointer(sp, m_crate, e.path.m_path, Target::EnumVariant) );
                const auto& des_name = e.path.m_path.m_components.back();
                unsigned int idx = ::std::find_if( enm.m_variants.begin(), enm.m_variants.end(), [&](const auto& x) { return x.first == des_name; }) - enm.m_variants.begin();
                if( idx == enm.m_variants.size() ) {
                    ERROR(sp, E0000, "Couldn't find enum variant " << e.path);
                }
                const auto& var = enm.m_variants[idx].second;
                TU_IFLET(::HIR::Enum::Variant, var, Struct, e,
                    // All good
                )
                else {
                    ERROR(sp, E0000, "Enum tuple pattern on non-tuple variant " << e.path);
                }
                fix_type_params(sp, enm.m_params,  e.path.m_params);
                e.binding_ptr = &enm;
                e.binding_idx = idx;
                )
            )
        }
        void visit_type(::HIR::TypeRef& ty) override
        {
            static Span _sp = Span();
            const Span& sp = _sp;

            ::HIR::Visitor::visit_type(ty);
            
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Path, e,
                TU_IFLET( ::HIR::Path::Data, e.path.m_data, Generic, e2,
                    const auto& item = *reinterpret_cast< const ::HIR::TypeItem*>( get_type_pointer(sp, m_crate, e2.m_path, Target::TypeItem) );
                    TU_MATCH_DEF( ::HIR::TypeItem, (item), (e3),
                    (
                        ERROR(sp, E0000, "Unexpected item type returned for " << e2.m_path << " - " << item.tag_str());
                        ),
                    (Struct,
                        e.binding = ::HIR::TypeRef::TypePathBinding::make_Struct(&e3);
                        ),
                    (Enum,
                        e.binding = ::HIR::TypeRef::TypePathBinding::make_Enum(&e3);
                        ),
                    (Trait,
                        ::std::vector< ::HIR::GenericPath>  traits;
                        traits.push_back( mv$(e2) );
                        ty.m_data = ::HIR::TypeRef::Data::make_TraitObject({ mv$(traits), {} });
                        )
                    )
                )
                else {
                    e.binding = ::HIR::TypeRef::TypePathBinding::make_Opaque({});
                }
            )
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
                    upper_visitor.visit_type(node.m_type);
                    ::HIR::ExprVisitorDef::visit(node);
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
            
            if( &*expr != nullptr )
            {
                ExprVisitor v { *this };
                (*expr).visit(v);
            }
        }
    };
}

void ConvertHIR_Bind(::HIR::Crate& crate)
{
    Visitor exp { crate };
    exp.visit_crate( crate );
}
