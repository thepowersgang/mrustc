/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/expr.cpp
 * - HIR expression helper code
 */
#include <hir/expr.hpp>

::HIR::ExprNode::~ExprNode()
{
}

#define DEF_VISIT(nt, n, code)   void ::HIR::nt::visit(ExprVisitor& nv) { nv.visit_node(*this); nv.visit(*this); } void ::HIR::ExprVisitorDef::visit(::HIR::nt& n) { code }

void ::HIR::ExprVisitor::visit_node_ptr(::std::unique_ptr< ::HIR::ExprNode>& node_ptr) {
    assert(node_ptr);
    node_ptr->visit(*this);
}
void ::HIR::ExprVisitor::visit_node(::HIR::ExprNode& node) {
}
void ::HIR::ExprVisitorDef::visit_node_ptr(::std::unique_ptr< ::HIR::ExprNode>& node_ptr) {
    assert(node_ptr);
    TRACE_FUNCTION_F(&*node_ptr << " " << typeid(*node_ptr).name());
    node_ptr->visit(*this);
    visit_type(node_ptr->m_res_type);
}
DEF_VISIT(ExprNode_Block, node,
    for(auto& subnode : node.m_nodes) {
        visit_node_ptr(subnode);
    }
    if( node.m_value_node )
        visit_node_ptr(node.m_value_node);
)
DEF_VISIT(ExprNode_Asm, node,
    for(auto& v : node.m_outputs)
        visit_node_ptr(v.value);
    for(auto& v : node.m_inputs)
        visit_node_ptr(v.value);
)
DEF_VISIT(ExprNode_Return, node,
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_Let, node,
    visit_pattern(node.span(), node.m_pattern);
    visit_type(node.m_type);
    if( node.m_value ) {
        visit_node_ptr(node.m_value);
    }
)
DEF_VISIT(ExprNode_Loop, node,
    visit_node_ptr(node.m_code);
)
DEF_VISIT(ExprNode_LoopControl, node,
    if( node.m_value ) {
        visit_node_ptr(node.m_value);
    }
)
DEF_VISIT(ExprNode_Match, node,
    visit_node_ptr(node.m_value);
    for(auto& arm : node.m_arms)
    {
        for(auto& pat : arm.m_patterns)
            visit_pattern(node.span(), pat);
        if( arm.m_cond )
            visit_node_ptr(arm.m_cond);
        visit_node_ptr(arm.m_code);
    }
)
DEF_VISIT(ExprNode_If, node,
    visit_node_ptr(node.m_cond);
    visit_node_ptr(node.m_true);
    if( node.m_false )
        visit_node_ptr(node.m_false);
)

DEF_VISIT(ExprNode_Assign, node,
    visit_node_ptr(node.m_slot);
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_BinOp, node,
    visit_node_ptr(node.m_left);
    visit_node_ptr(node.m_right);
)
DEF_VISIT(ExprNode_UniOp, node,
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_Borrow, node,
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_Cast, node,
    TRACE_FUNCTION_F("_Cast " << node.m_res_type);
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_Unsize, node,
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_Index, node,
    visit_node_ptr(node.m_value);
    visit_node_ptr(node.m_index);
)
DEF_VISIT(ExprNode_Deref, node,
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_Emplace, node,
    if( node.m_place )
        visit_node_ptr(node.m_place);
    visit_node_ptr(node.m_value);
)

DEF_VISIT(ExprNode_TupleVariant, node,
    visit_generic_path(::HIR::Visitor::PathContext::VALUE, node.m_path);

    for(auto& ty : node.m_arg_types)
        visit_type(ty);

    for(auto& arg : node.m_args)
        visit_node_ptr(arg);
)
DEF_VISIT(ExprNode_CallPath, node,
    TRACE_FUNCTION_F("_CallPath: " << node.m_path);
    for(auto& ty : node.m_cache.m_arg_types)
        visit_type(ty);

    visit_path(::HIR::Visitor::PathContext::VALUE, node.m_path);
    for(auto& arg : node.m_args)
        visit_node_ptr(arg);
)
DEF_VISIT(ExprNode_CallValue, node,
    TRACE_FUNCTION_F("_CallValue:");
    for(auto& ty : node.m_arg_types)
        visit_type(ty);

    visit_node_ptr(node.m_value);
    for(auto& arg : node.m_args)
        visit_node_ptr(arg);
)
DEF_VISIT(ExprNode_CallMethod, node,
    TRACE_FUNCTION_F("_CallMethod: " << node.m_method);
    for(auto& ty : node.m_cache.m_arg_types)
        visit_type(ty);

    visit_path(::HIR::Visitor::PathContext::VALUE, node.m_method_path);

    visit_node_ptr(node.m_value);
    for(auto& arg : node.m_args)
        visit_node_ptr(arg);
)
DEF_VISIT(ExprNode_Field, node,
    visit_node_ptr(node.m_value);
)

DEF_VISIT(ExprNode_Literal, , )
DEF_VISIT(ExprNode_UnitVariant, node,
    visit_generic_path(::HIR::Visitor::PathContext::VALUE, node.m_path);
)
DEF_VISIT(ExprNode_PathValue, node,
    visit_path(::HIR::Visitor::PathContext::VALUE, node.m_path);
)
DEF_VISIT(ExprNode_Variable, , )
DEF_VISIT(ExprNode_ConstParam, , )
DEF_VISIT(ExprNode_StructLiteral, node,
    visit_type(node.m_type);
    if( node.m_base_value )
        visit_node_ptr(node.m_base_value);
    for(auto& val : node.m_values)
        visit_node_ptr(val.second);

    visit_generic_path(::HIR::Visitor::PathContext::TYPE, node.m_real_path);
)
DEF_VISIT(ExprNode_UnionLiteral, node,
    visit_generic_path(::HIR::Visitor::PathContext::TYPE, node.m_path);
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_Tuple, node,
    for(auto& val : node.m_vals)
        visit_node_ptr(val);
)
DEF_VISIT(ExprNode_ArrayList, node,
    for(auto& val : node.m_vals)
        visit_node_ptr(val);
)
DEF_VISIT(ExprNode_ArraySized, node,
    visit_node_ptr(node.m_val);
    //visit_node_ptr(node.m_size);
)

DEF_VISIT(ExprNode_Closure, node,
    for(auto& arg : node.m_args) {
        visit_pattern(node.span(), arg.first);
        visit_type(arg.second);
    }
    visit_type(node.m_return);
    if(node.m_code)
    {
        visit_node_ptr(node.m_code);
    }
    else
    {
        for(auto& cap : node.m_captures)
            visit_node_ptr(cap);
    }
)

#undef DEF_VISIT

// TODO: Merge this with the stuff in ::HIR::Visitor
void ::HIR::ExprVisitorDef::visit_pattern(const Span& sp, ::HIR::Pattern& pat)
{
    TU_MATCH(::HIR::Pattern::Data, (pat.m_data), (e),
    (Any,
        ),
    (Box,
        this->visit_pattern(sp, *e.sub);
        ),
    (Ref,
        this->visit_pattern(sp, *e.sub);
        ),
    (Tuple,
        for(auto& subpat : e.sub_patterns)
            this->visit_pattern(sp, subpat);
        ),
    (SplitTuple,
        for(auto& subpat : e.leading)
            this->visit_pattern(sp, subpat);
        for(auto& subpat : e.trailing)
            this->visit_pattern(sp, subpat);
        ),
    (StructValue,
        // Nothing.
        ),
    (StructTuple,
        for(auto& subpat : e.sub_patterns)
            this->visit_pattern(sp, subpat);
        ),
    (Struct,
        for(auto& fld_pat : e.sub_patterns)
        {
            this->visit_pattern(sp, fld_pat.second);
        }
        ),
    // Refutable
    (Value,
        ),
    (Range,
        ),
    (EnumValue,
        ),
    (EnumTuple,
        for(auto& subpat : e.sub_patterns)
            this->visit_pattern(sp, subpat);
        ),
    (EnumStruct,
        for(auto& fld_pat : e.sub_patterns)
            this->visit_pattern(sp, fld_pat.second);
        ),
    (Slice,
        for(auto& subpat : e.sub_patterns)
            this->visit_pattern(sp, subpat);
        ),
    (SplitSlice,
        for(auto& subpat : e.leading)
            this->visit_pattern(sp, subpat);
        for(auto& subpat : e.trailing)
            this->visit_pattern(sp, subpat);
        )
    )
}
void ::HIR::ExprVisitorDef::visit_type(::HIR::TypeRef& ty)
{
    TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
    (Infer,
        ),
    (Diverge,
        ),
    (Primitive,
        ),
    (Path,
        this->visit_path(::HIR::Visitor::PathContext::TYPE, e.path);
        ),
    (Generic,
        ),
    (TraitObject,
        this->visit_trait_path(e.m_trait);
        for(auto& trait : e.m_markers) {
            this->visit_generic_path(::HIR::Visitor::PathContext::TYPE, trait);
        }
        ),
    (ErasedType,
        this->visit_path(::HIR::Visitor::PathContext::TYPE, e.m_origin);
        for(auto& trait : e.m_traits) {
            this->visit_trait_path(trait);
        }
        ),
    (Array,
        this->visit_type( *e.inner );
        //this->visit_expr( e.size );
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
void ::HIR::ExprVisitorDef::visit_path_params(::HIR::PathParams& pp)
{
    for(auto& ty : pp.m_types)
    {
        visit_type(ty);
    }
}
void ::HIR::ExprVisitorDef::visit_trait_path(::HIR::TraitPath& p)
{
    this->visit_generic_path(::HIR::Visitor::PathContext::TYPE, p.m_path);
    for(auto& assoc : p.m_type_bounds)
        this->visit_type(assoc.second);
}
void ::HIR::ExprVisitorDef::visit_path(::HIR::Visitor::PathContext pc, ::HIR::Path& path)
{
    TU_MATCHA( (path.m_data), (e),
    (Generic,
        visit_generic_path(pc, e);
        ),
    (UfcsKnown,
        visit_type(*e.type);
        visit_generic_path(pc, e.trait);
        visit_path_params(e.params);
        ),
    (UfcsUnknown,
        visit_type(*e.type);
        visit_path_params(e.params);
        ),
    (UfcsInherent,
        visit_type(*e.type);
        visit_path_params(e.params);
        visit_path_params(e.impl_params);
        )
    )
}
void ::HIR::ExprVisitorDef::visit_generic_path(::HIR::Visitor::PathContext pc, ::HIR::GenericPath& path)
{
    visit_path_params(path.m_params);
}

