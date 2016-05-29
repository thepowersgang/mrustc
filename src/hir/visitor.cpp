
#include <hir/hir.hpp>
#include <hir/visitor.hpp>

::HIR::Visitor::~Visitor()
{
}

void ::HIR::Visitor::visit_crate(::HIR::Crate& crate)
{
    this->visit_module( crate.m_root_module );
    
    for( auto& ty_impl : crate.m_type_impls )
    {
        this->visit_type_impl(ty_impl);
    }
    for( auto& impl : crate.m_trait_impls )
    {
        this->visit_trait_impl(impl.first, impl.second);
    }
    for( auto& impl : crate.m_marker_impls )
    {
        this->visit_marker_impl(impl.first, impl.second);
    }
}

void ::HIR::Visitor::visit_module(::HIR::Module& mod)
{
    TRACE_FUNCTION;
    for( auto& named : mod.m_mod_items )
    {
        const auto& name = named.first;
        auto& item = named.second->ent;
        TU_MATCH(::HIR::TypeItem, (item), (e),
        (Import, ),
        (Module,
            DEBUG("mod " << name);
            this->visit_module(e);
            ),
        (TypeAlias,
            DEBUG("type " << name);
            this->visit_type_alias(e);
            ),
        (Enum,
            DEBUG("enum " << name);
            this->visit_enum(e);
            ),
        (Struct,
            DEBUG("struct " << name);
            this->visit_struct(e);
            ),
        (Trait,
            DEBUG("trait " << name);
            this->visit_trait(e);
            )
        )
    }
    for( auto& named : mod.m_value_items )
    {
        const auto& name = named.first;
        auto& item = named.second->ent;
        TU_MATCH(::HIR::ValueItem, (item), (e),
        (Import,
            // SimplePath - no visitor
            ),
        (Constant,
            DEBUG("const " << name);
            this->visit_constant(e);
            ),
        (Static,
            DEBUG("static " << name);
            this->visit_static(e);
            ),
        (StructConstant,
            // Just a path
            ),
        (Function,
            DEBUG("fn " << name);
            this->visit_function(e);
            ),
        (StructConstructor,
            // Just a path
            )
        )
    }
}


void ::HIR::Visitor::visit_type_impl(::HIR::TypeImpl& impl)
{
    this->visit_params(impl.m_params);
    this->visit_type(impl.m_type);
    
    for(auto& method : impl.m_methods) {
        this->visit_function(method.second);
    }
}
void ::HIR::Visitor::visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl)
{
    this->visit_params(impl.m_params);
    this->visit_path_params(impl.m_trait_args);
    this->visit_type(impl.m_type);
    
    for(auto& ent : impl.m_methods) {
        this->visit_function(ent.second);
    }
    for(auto& ent : impl.m_constants) {
        this->visit_expr(ent.second);
    }
    for(auto& ent : impl.m_types) {
        this->visit_type(ent.second);
    }
}
void ::HIR::Visitor::visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl)
{
    this->visit_params(impl.m_params);
    this->visit_path_params(impl.m_trait_args);
    this->visit_type(impl.m_type);
}

void ::HIR::Visitor::visit_type_alias(::HIR::TypeAlias& item)
{
    this->visit_params(item.m_params);
    this->visit_type(item.m_type);
}
void ::HIR::Visitor::visit_trait(::HIR::Trait& item)
{
    this->visit_params(item.m_params);
    for(auto& par : item.m_parent_traits) {
        this->visit_generic_path(par, ::HIR::Visitor::PathContext::TYPE);
    }
    for(auto& i : item.m_types) {
        this->visit_params(i.second.m_params);
        this->visit_type(i.second.m_default);
    }
    for(auto& i : item.m_values) {
        TU_MATCH(::HIR::TraitValueItem, (i.second), (e),
        (None, ),
        (Constant,
            this->visit_constant(e);
            ),
        (Static,
            this->visit_static(e);
            ),
        (Function,
            this->visit_function(e);
            )
        )
    }
}
void ::HIR::Visitor::visit_struct(::HIR::Struct& item)
{
    this->visit_params(item.m_params);
    TU_MATCH(::HIR::Struct::Data, (item.m_data), (e),
    (Unit,
        ),
    (Tuple,
        for(auto& ty : e) {
            this->visit_type(ty.ent);
        }
        ),
    (Named,
        for(auto& field : e) {
            this->visit_type(field.second.ent);
        }
        )
    )
}
void ::HIR::Visitor::visit_enum(::HIR::Enum& item)
{
    this->visit_params(item.m_params);
    for(auto& var : item.m_variants)
    {
        TU_MATCH(::HIR::Enum::Variant, (var.second), (v),
        (Unit,
            ),
        (Value,
            this->visit_expr(v);
            ),
        (Tuple,
            for(auto& ty : v) {
                this->visit_type(ty);
            }
            ),
        (Struct,
            for(auto& field : v) {
                this->visit_type(field.second);
            }
            )
        )
    }
}
void ::HIR::Visitor::visit_function(::HIR::Function& item)
{
    this->visit_params(item.m_params);
    for(auto& arg : item.m_args)
    {
        this->visit_pattern(arg.first);
        this->visit_type(arg.second);
    }
    this->visit_type(item.m_return);
    this->visit_expr(item.m_code);
}
void ::HIR::Visitor::visit_static(::HIR::Static& item)
{
    this->visit_type(item.m_type);
    this->visit_expr(item.m_value);
}
void ::HIR::Visitor::visit_constant(::HIR::Constant& item)
{
    this->visit_params(item.m_params);
    this->visit_type(item.m_type);
    this->visit_expr(item.m_value);
}

void ::HIR::Visitor::visit_params(::HIR::GenericParams& params)
{
    for(auto& tps : params.m_types)
        this->visit_type( tps.m_default );
    for(auto& bound : params.m_bounds )
    {
        TU_MATCH(::HIR::GenericBound, (bound), (e),
        (Lifetime,
            ),
        (TypeLifetime,
            this->visit_type(e.type);
            ),
        (TraitBound,
            this->visit_type(e.type);
            this->visit_generic_path(e.trait.m_path, ::HIR::Visitor::PathContext::TYPE);
            ),
        //(NotTrait, struct {
        //    ::HIR::TypeRef  type;
        //    ::HIR::GenricPath    trait;
        //    }),
        (TypeEquality,
            this->visit_type(e.type);
            this->visit_type(e.other_type);
            )
        )
    }
}
void ::HIR::Visitor::visit_type(::HIR::TypeRef& ty)
{
    TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
    (Infer,
        ),
    (Diverge,
        ),
    (Primitive,
        ),
    (Path,
        this->visit_path(e.path, ::HIR::Visitor::PathContext::TYPE);
        ),
    (Generic,
        ),
    (TraitObject,
        for(auto& trait : e.m_traits) {
            this->visit_generic_path(trait, ::HIR::Visitor::PathContext::TYPE);
        }
        ),
    (Array,
        this->visit_type( *e.inner );
        this->visit_expr( e.size );
        ),
    (Slice,
        this->visit_type( *e.inner );
        ),
    (Tuple,
        for(auto& t : e) {
            this->visit_type(t);
        }
        ),
    (Borrow,
        this->visit_type( *e.inner );
        ),
    (Pointer,
        this->visit_type( *e.inner );
        ),
    (Function,
        for(auto& t : e.m_arg_types) {
            this->visit_type(t);
        }
        this->visit_type(*e.m_rettype);
        )
    )
}
void ::HIR::Visitor::visit_pattern(::HIR::Pattern& pat)
{
    TU_MATCH(::HIR::Pattern::Data, (pat.m_data), (e),
    (Any,
        ),
    (Box,
        this->visit_pattern( *e.sub );
        ),
    (Ref,
        this->visit_pattern( *e.sub );
        ),
    (Tuple,
        for(auto& sp : e.sub_patterns)
            this->visit_pattern(sp);
        ),
    (StructTupleWildcard,
        this->visit_generic_path(e.path, ::HIR::Visitor::PathContext::TYPE);
        ),
    (StructTuple,
        this->visit_generic_path(e.path, ::HIR::Visitor::PathContext::TYPE);
        for(auto& sp : e.sub_patterns)
            this->visit_pattern(sp);
        ),
    (Struct,
        this->visit_generic_path(e.path, ::HIR::Visitor::PathContext::TYPE);
        for(auto& sp : e.sub_patterns)
            this->visit_pattern(sp.second);
        ),
    // Refutable
    (Value,
        this->visit_pattern_val(e.val);
        ),
    (Range,
        this->visit_pattern_val(e.start);
        this->visit_pattern_val(e.end);
        ),
    (EnumTupleWildcard,
        this->visit_generic_path(e.path, ::HIR::Visitor::PathContext::TYPE);
        ),
    (EnumTuple,
        this->visit_generic_path(e.path, ::HIR::Visitor::PathContext::TYPE);
        for(auto& sp : e.sub_patterns)
            this->visit_pattern(sp);
        ),
    (EnumStruct,
        this->visit_generic_path(e.path, ::HIR::Visitor::PathContext::TYPE);
        for(auto& sp : e.sub_patterns)
            this->visit_pattern(sp.second);
        ),
    (Slice,
        for(auto& sp : e.sub_patterns)
            this->visit_pattern(sp);
        ),
    (SplitSlice,
        for(auto& sp : e.leading)
            this->visit_pattern(sp);
        for(auto& sp : e.trailing)
            this->visit_pattern(sp);
        )
    )
}
void ::HIR::Visitor::visit_pattern_val(::HIR::Pattern::Value& val)
{
    TU_MATCH(::HIR::Pattern::Value, (val), (e),
    (Integer,
        ),
    (String,
        ),
    (Named,
        this->visit_path(e, ::HIR::Visitor::PathContext::VALUE);
        )
    )
}
void ::HIR::Visitor::visit_path(::HIR::Path& p, ::HIR::Visitor::PathContext pc)
{
    TU_MATCH(::HIR::Path::Data, (p.m_data), (e),
    (Generic,
        this->visit_generic_path(e, pc);
        ),
    (UfcsInherent,
        this->visit_type(*e.type);
        this->visit_path_params(e.params);
        ),
    (UfcsKnown,
        this->visit_type(*e.type);
        this->visit_generic_path(e.trait, ::HIR::Visitor::PathContext::TYPE);
        this->visit_path_params(e.params);
        ),
    (UfcsUnknown,
        this->visit_type(*e.type);
        this->visit_path_params(e.params);
        )
    )
}
void ::HIR::Visitor::visit_path_params(::HIR::PathParams& p)
{
    for( auto& ty : p.m_types )
    {
        this->visit_type(ty);
    }
}
void ::HIR::Visitor::visit_generic_path(::HIR::GenericPath& p, ::HIR::Visitor::PathContext /*pc*/)
{
    this->visit_path_params(p.m_params);
}
void ::HIR::Visitor::visit_expr(::HIR::ExprPtr& exp)
{
    // Do nothing, leave expression stuff for user
}
