/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/visitor.cpp
 * - HIR Visitor default implementation
 */
#include <hir/hir.hpp>
#include <hir/visitor.hpp>

::HIR::Visitor::~Visitor()
{
}

namespace {
    template<typename T>
    void visit_impls(::HIR::Crate::ImplGroup<T>& g, ::std::function<void(T&)> cb) {
        for( auto& impl_group : g.named )
        {
            for( auto& impl : impl_group.second )
            {
                cb(*impl);
            }
        }
        for( auto& impl : g.non_named )
        {
            cb(*impl);
        }
        for( auto& impl : g.generic )
        {
            cb(*impl);
        }
    }
}

void ::HIR::Visitor::visit_crate(::HIR::Crate& crate)
{
    this->visit_module(::HIR::ItemPath(crate.m_crate_name), crate.m_root_module );

    visit_impls<::HIR::TypeImpl>(crate.m_type_impls, [&](::HIR::TypeImpl& ty_impl){ this->visit_type_impl(ty_impl); });
    for( auto& impl_group : crate.m_trait_impls )
    {
        visit_impls<::HIR::TraitImpl>(impl_group.second, [&](::HIR::TraitImpl& ty_impl){ this->visit_trait_impl(impl_group.first, ty_impl); });
    }
    for( auto& impl_group : crate.m_marker_impls )
    {
        visit_impls<::HIR::MarkerImpl>(impl_group.second, [&](::HIR::MarkerImpl& ty_impl){ this->visit_marker_impl(impl_group.first, ty_impl); });
    }
}

void ::HIR::Visitor::visit_module(::HIR::ItemPath p, ::HIR::Module& mod)
{
    TRACE_FUNCTION_FR(p,p);
    for( auto& named : mod.m_mod_items )
    {
        const auto& name = named.first;
        auto& item = named.second->ent;
        TU_MATCH(::HIR::TypeItem, (item), (e),
        (Import, ),
        (Module,
            DEBUG("mod " << name);
            this->visit_module(p + name, e);
            ),
        (TypeAlias,
            DEBUG("type " << name);
            this->visit_type_alias(p + name, e);
            ),
        (ExternType,
            DEBUG("extern type " << name);
            ),
        (Enum,
            DEBUG("enum " << name);
            this->visit_enum(p + name, e);
            ),
        (Struct,
            DEBUG("struct " << name);
            this->visit_struct(p + name, e);
            ),
        (Union,
            DEBUG("union " << name);
            this->visit_union(p + name, e);
            ),
        (Trait,
            DEBUG("trait " << name);
            this->visit_trait(p + name, e);
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
            this->visit_constant(p + name, e);
            ),
        (Static,
            DEBUG("static " << name);
            this->visit_static(p + name, e);
            ),
        (StructConstant,
            // Just a path
            ),
        (Function,
            DEBUG("fn " << name);
            this->visit_function(p + name, e);
            ),
        (StructConstructor,
            // Just a path
            )
        )
    }
}


void ::HIR::Visitor::visit_type_impl(::HIR::TypeImpl& impl)
{
    ::HIR::ItemPath    p { impl.m_type };
    TRACE_FUNCTION_F("impl.m_type=" << impl.m_type);
    this->visit_params(impl.m_params);
    this->visit_type(impl.m_type);

    for(auto& method : impl.m_methods) {
        DEBUG("method " << method.first);
        this->visit_function(p + method.first, method.second.data);
    }
    for(auto& ent : impl.m_constants) {
        DEBUG("const " << ent.first);
        this->visit_constant(p + ent.first, ent.second.data);
    }
}
void ::HIR::Visitor::visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl)
{
    ::HIR::ItemPath    p( impl.m_type, trait_path, impl.m_trait_args );
    TRACE_FUNCTION_F(p);
    this->visit_params(impl.m_params);
    // - HACK: Create a generic path to visit (so that proper checks are performed)
    {
        ::HIR::GenericPath  gp { trait_path, mv$(impl.m_trait_args) };
        this->visit_generic_path(gp, PathContext::TRAIT);
        impl.m_trait_args = mv$(gp.m_params);
    }
    this->visit_type(impl.m_type);

    for(auto& ent : impl.m_methods) {
        DEBUG("method " << ent.first);
        this->visit_function(p + ent.first, ent.second.data);
    }
    for(auto& ent : impl.m_constants) {
        DEBUG("const " << ent.first);
        this->visit_constant(p + ent.first, ent.second.data);
    }
    for(auto& ent : impl.m_statics) {
        DEBUG("static " << ent.first);
        this->visit_static(p + ent.first, ent.second.data);
    }
    for(auto& ent : impl.m_types) {
        DEBUG("type " << ent.first);
        this->visit_type(ent.second.data);
    }
}
void ::HIR::Visitor::visit_marker_impl(const ::HIR::SimplePath& trait_path, ::HIR::MarkerImpl& impl)
{
    this->visit_params(impl.m_params);
    this->visit_path_params(impl.m_trait_args);
    this->visit_type(impl.m_type);
}

void ::HIR::Visitor::visit_type_alias(::HIR::ItemPath p, ::HIR::TypeAlias& item)
{
    this->visit_params(item.m_params);
    this->visit_type(item.m_type);
}
void ::HIR::Visitor::visit_trait(::HIR::ItemPath p, ::HIR::Trait& item)
{
    ::HIR::SimplePath trait_sp = p.get_simple_path();
    ItemPath    trait_ip(trait_sp);
    TRACE_FUNCTION;

    this->visit_params(item.m_params);
    for(auto& par : item.m_parent_traits) {
        this->visit_trait_path(par);
    }
    for(auto& par : item.m_all_parent_traits) {
        this->visit_trait_path(par);
    }
    for(auto& i : item.m_types) {
        auto item_path = ::HIR::ItemPath(trait_ip, i.first.c_str());
        DEBUG("type " << i.first);
        this->visit_associatedtype(item_path, i.second);
    }
    for(auto& i : item.m_values) {
        auto item_path = ::HIR::ItemPath(trait_ip, i.first.c_str());
        TU_MATCH(::HIR::TraitValueItem, (i.second), (e),
        //(None, ),
        (Constant,
            DEBUG("constant " << i.first);
            this->visit_constant(item_path, e);
            ),
        (Static,
            DEBUG("static " << i.first);
            this->visit_static(item_path, e);
            ),
        (Function,
            DEBUG("method " << i.first);
            this->visit_function(item_path, e);
            )
        )
    }
}
void ::HIR::Visitor::visit_struct(::HIR::ItemPath p, ::HIR::Struct& item)
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
void ::HIR::Visitor::visit_enum(::HIR::ItemPath p, ::HIR::Enum& item)
{
    this->visit_params(item.m_params);
    TU_MATCHA( (item.m_data), (e),
    (Value,
        for(auto& var : e.variants)
        {
            this->visit_expr(var.expr);
        }
        ),
    (Data,
        for(auto& var : e)
        {
            this->visit_type(var.type);
        }
        )
    )
}
void ::HIR::Visitor::visit_union(::HIR::ItemPath p, ::HIR::Union& item)
{
    TRACE_FUNCTION_F(p);
    this->visit_params(item.m_params);
    for(auto& var : item.m_variants)
        this->visit_type(var.second.ent);
}
void ::HIR::Visitor::visit_associatedtype(ItemPath p, ::HIR::AssociatedType& item)
{
    TRACE_FUNCTION_F(p);
    for(auto& bound : item.m_trait_bounds)
        this->visit_trait_path(bound);
    this->visit_type(item.m_default);
}
void ::HIR::Visitor::visit_function(::HIR::ItemPath p, ::HIR::Function& item)
{
    TRACE_FUNCTION_F(p);
    this->visit_params(item.m_params);
    for(auto& arg : item.m_args)
    {
        this->visit_pattern(arg.first);
        this->visit_type(arg.second);
    }
    this->visit_type(item.m_return);
    this->visit_expr(item.m_code);
}
void ::HIR::Visitor::visit_static(::HIR::ItemPath p, ::HIR::Static& item)
{
    TRACE_FUNCTION_F(p);
    this->visit_type(item.m_type);
    this->visit_expr(item.m_value);
}
void ::HIR::Visitor::visit_constant(::HIR::ItemPath p, ::HIR::Constant& item)
{
    TRACE_FUNCTION_F(p);
    this->visit_params(item.m_params);
    this->visit_type(item.m_type);
    this->visit_expr(item.m_value);
}

void ::HIR::Visitor::visit_params(::HIR::GenericParams& params)
{
    TRACE_FUNCTION_F(params.fmt_args() << params.fmt_bounds());
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
            this->visit_trait_path(e.trait);
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
        if( e.m_trait.m_path != ::HIR::SimplePath() ) {
            this->visit_trait_path(e.m_trait);
        }
        for(auto& trait : e.m_markers) {
            this->visit_generic_path(trait, ::HIR::Visitor::PathContext::TYPE);
        }
        ),
    (ErasedType,
        if( e.m_origin != ::HIR::SimplePath() ) {
            this->visit_path(e.m_origin, ::HIR::Visitor::PathContext::VALUE);
        }
        for(auto& trait : e.m_traits) {
            this->visit_trait_path(trait);
        }
        ),
    (Array,
        this->visit_type( *e.inner );
        if( auto* se = e.size.opt_Unevaluated() )
            this->visit_expr( **se );
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
        ),
    (Closure,
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
    (SplitTuple,
        for(auto& sp : e.leading)
            this->visit_pattern(sp);
        for(auto& sp : e.trailing)
            this->visit_pattern(sp);
        ),
    (StructValue,
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
    (EnumValue,
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
    (Float,
        ),
    (String,
        ),
    (ByteString,
        ),
    (Named,
        this->visit_path(e.path, ::HIR::Visitor::PathContext::VALUE);
        )
    )
}
void ::HIR::Visitor::visit_trait_path(::HIR::TraitPath& p)
{
    this->visit_generic_path(p.m_path, ::HIR::Visitor::PathContext::TYPE);
    for(auto& assoc : p.m_type_bounds)
        this->visit_type(assoc.second);
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
        this->visit_path_params(e.impl_params);
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
