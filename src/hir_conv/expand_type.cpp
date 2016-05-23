/*
 * Expand `type` aliases in HIR
 */
#include <hir/hir.hpp>
#include <hir/expr.hpp>

void ConvertHIR_ExpandAliases_Type(const ::HIR::Crate& crate, ::HIR::TypeRef& ty);

::HIR::TypeRef ConvertHIR_ExpandAliases_GetExpansion(const ::HIR::Crate& crate, const ::HIR::Path& path)
{
    TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
    (Generic,
        const ::HIR::Module* mod = &crate.m_root_module;
        assert( e.m_path.m_crate_name == "" && "TODO: Handle extern crates" );
        for( unsigned int i = 0; i < e.m_path.m_components.size() - 1; i ++ )
        {
            const auto& pc = e.m_path.m_components[i];
            auto it = mod->m_mod_items.find( pc );
            if( it == mod->m_mod_items.end() ) {
                BUG(Span(), "Couldn't find component " << i << " of " << e.m_path);
            }
            TU_MATCH_DEF( ::HIR::TypeItem, (it->second->ent), (e2),
            (
                BUG(Span(), "Node " << i << " of path " << e.m_path << " wasn't a module");
                ),
            (Module,
                mod = &e2;
                )
            )
        }
        auto it = mod->m_mod_items.find( e.m_path.m_components.back() );
        if( it == mod->m_mod_items.end() ) {
            BUG(Span(), "Could not find type name in " << e.m_path);
        }
        
        TU_MATCH_DEF( ::HIR::TypeItem, (it->second->ent), (e2),
        (
            ),
        (TypeAlias,
            if( e2.m_params.m_types.size() > 0 ) {
                TODO(Span(), "Replace type params in type alias");
            }
            return e2.m_type.clone();
            )
        )
        ),
    (UfcsInherent,
        DEBUG("TODO: Locate impl blocks for types - path=" << path);
        ),
    (UfcsKnown,
        DEBUG("TODO: Locate impl blocks for traits on types - path=" << path);
        ),
    (UfcsUnknown,
        DEBUG("TODO: Locate impl blocks for traits on types - path=" << path);
        )
    )
    return ::HIR::TypeRef();
}

void ConvertHIR_ExpandAliases_PathParams(const ::HIR::Crate& crate, ::HIR::PathParams& p)
{
    for(auto& ty : p.m_types)
    {
        ConvertHIR_ExpandAliases_Type(crate, ty);
    }
}
void ConvertHIR_ExpandAliases_Path(const ::HIR::Crate& crate, ::HIR::GenericPath& p)
{
    ConvertHIR_ExpandAliases_PathParams(crate, p.m_params);
}

void ConvertHIR_ExpandAliases_Path(const ::HIR::Crate& crate, ::HIR::Path& p)
{
    TU_MATCH(::HIR::Path::Data, (p.m_data), (e),
    (Generic,
        ConvertHIR_ExpandAliases_Path(crate, e);
        ),
    (UfcsInherent,
        ConvertHIR_ExpandAliases_Type(crate, *e.type);
        ConvertHIR_ExpandAliases_PathParams(crate, e.params);
        ),
    (UfcsKnown,
        ConvertHIR_ExpandAliases_Type(crate, *e.type);
        ConvertHIR_ExpandAliases_Path(crate, e.trait);
        ConvertHIR_ExpandAliases_PathParams(crate, e.params);
        ),
    (UfcsUnknown,
        ConvertHIR_ExpandAliases_Type(crate, *e.type);
        ConvertHIR_ExpandAliases_PathParams(crate, e.params);
        )
    )
}

void ConvertHIR_ExpandAliases_Type(const ::HIR::Crate& crate, ::HIR::TypeRef& ty)
{
    TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
    (Infer,
        ),
    (Diverge,
        ),
    (Primitive,
        ),
    (Path,
        ConvertHIR_ExpandAliases_Path(crate, e);
        
        auto new_type = ConvertHIR_ExpandAliases_GetExpansion(crate, e);
        if( ! new_type.m_data.is_Infer() ) {
            DEBUG("Replacing " << ty << " with " << new_type);
        }
        ),
    (Generic,
        ),
    (TraitObject,
        for(auto& trait : e.m_traits) {
            ConvertHIR_ExpandAliases_Path(crate, trait);
        }
        ),
    (Array,
        ConvertHIR_ExpandAliases_Type(crate, *e.inner);
        // TODO: Expression?
        ),
    (Tuple,
        for(auto& t : e) {
            ConvertHIR_ExpandAliases_Type(crate, t);
        }
        ),
    (Borrow,
        ConvertHIR_ExpandAliases_Type(crate, *e.inner);
        ),
    (Pointer,
        ConvertHIR_ExpandAliases_Type(crate, *e.inner);
        ),
    (Function,
        for(auto& t : e.m_arg_types) {
            ConvertHIR_ExpandAliases_Type(crate, t);
        }
        ConvertHIR_ExpandAliases_Type(crate, *e.m_rettype);
        )
    )
}
void ConvertHIR_ExpandAliases_Expr(const ::HIR::Crate& crate, ::HIR::ExprPtr& ep)
{
    struct Visitor:
        public ::HIR::ExprVisitorDef
    {
        const ::HIR::Crate& crate;
        
        Visitor(const ::HIR::Crate& crate):
            crate(crate)
        {}
        
        void visit(::HIR::ExprNode_Let& node) override
        {
            ConvertHIR_ExpandAliases_Type(crate, node.m_type);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_Cast& node) override
        {
            ConvertHIR_ExpandAliases_Type(crate, node.m_type);
            ::HIR::ExprVisitorDef::visit(node);
        }
        
        void visit(::HIR::ExprNode_CallPath& node) override
        {
            ConvertHIR_ExpandAliases_Path(crate, node.m_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            ConvertHIR_ExpandAliases_PathParams(crate, node.m_params);
            ::HIR::ExprVisitorDef::visit(node);
        }
        
        void visit(::HIR::ExprNode_Closure& node) override
        {
            ConvertHIR_ExpandAliases_Type(crate, node.m_return);
            for(auto& arg : node.m_args)
                ConvertHIR_ExpandAliases_Type(crate, arg.second);
            ::HIR::ExprVisitorDef::visit(node);
        }
    };
    
    if( &*ep != nullptr )
    {
        Visitor v { crate };
        (*ep).visit(v);
    }
}
void ConvertHIR_ExpandAliases_GenericParams(const ::HIR::Crate& crate, ::HIR::GenericParams& gp)
{
}

void ConvertHIR_ExpandAliases_Mod(const ::HIR::Crate& crate, ::HIR::Module& mod)
{
    for( auto& named : mod.m_mod_items )
    {
        auto& item = named.second->ent;
        TU_MATCH(::HIR::TypeItem, (item), (e),
        (Import, ),
        (Module,
            ConvertHIR_ExpandAliases_Mod(crate, e);
            ),
        (TypeAlias,
            ConvertHIR_ExpandAliases_GenericParams(crate, e.m_params);
            ConvertHIR_ExpandAliases_Type(crate, e.m_type);
            ),
        (Enum,
            ConvertHIR_ExpandAliases_GenericParams(crate, e.m_params);
            for(auto& var : e.m_variants)
            {
                TU_MATCH(::HIR::Enum::Variant, (var.second), (v),
                (Unit,
                    ),
                (Value,
                    ConvertHIR_ExpandAliases_Expr(crate, v);
                    ),
                (Tuple,
                    for(auto& ty : v) {
                        ConvertHIR_ExpandAliases_Type(crate, ty);
                    }
                    ),
                (Struct,
                    for(auto& field : v) {
                        ConvertHIR_ExpandAliases_Type(crate, field.second);
                    }
                    )
                )
            }
            ),
        (Struct,
            ConvertHIR_ExpandAliases_GenericParams(crate, e.m_params);
            TU_MATCH(::HIR::Struct::Data, (e.m_data), (e2),
            (Unit,
                ),
            (Tuple,
                for(auto& ty : e2) {
                    ConvertHIR_ExpandAliases_Type(crate, ty.ent);
                }
                ),
            (Named,
                for(auto& field : e2) {
                    ConvertHIR_ExpandAliases_Type(crate, field.second.ent);
                }
                )
            )
            ),
        (Trait,
            ConvertHIR_ExpandAliases_GenericParams(crate, e.m_params);
            )
        )
    }
    for( auto& named : mod.m_value_items )
    {
        auto& item = named.second->ent;
        TU_MATCH(::HIR::ValueItem, (item), (e),
        (Import, ),
        (Constant,
            ConvertHIR_ExpandAliases_GenericParams(crate, e.m_params);
            ConvertHIR_ExpandAliases_Type(crate, e.m_type);
            ConvertHIR_ExpandAliases_Expr(crate, e.m_value);
            ),
        (Static,
            ConvertHIR_ExpandAliases_Type(crate, e.m_type);
            ConvertHIR_ExpandAliases_Expr(crate, e.m_value);
            ),
        (StructConstant,
            // Just a path
            ),
        (Function,
            ConvertHIR_ExpandAliases_GenericParams(crate, e.m_params);
            for(auto& arg : e.m_args)
            {
                ConvertHIR_ExpandAliases_Type(crate, arg.second);
            }
            ConvertHIR_ExpandAliases_Type(crate, e.m_return);
            ConvertHIR_ExpandAliases_Expr(crate, e.m_code);
            ),
        (StructConstructor,
            // Just a path
            )
        )
    }
}

void ConvertHIR_ExpandAliases(::HIR::Crate& crate)
{
    ConvertHIR_ExpandAliases_Mod(crate, crate.m_root_module);
}
