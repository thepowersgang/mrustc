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

#define DEF_VISIT_H(nt, n)   void ::HIR::nt::visit(ExprVisitor& nv) { nv.visit_node(*this); nv.visit(*this); } void ::HIR::ExprVisitorDef::visit(::HIR::nt& n)
#define DEF_VISIT(nt, n, code)   DEF_VISIT_H(nt, n) { code }

const char* ::HIR::ExprNode::type_name() const
{
    return typeid(*this).name();
}

void ::HIR::ExprVisitor::visit_node_ptr(::std::unique_ptr< ::HIR::ExprNode>& node_ptr) {
    assert(node_ptr);
    node_ptr->visit(*this);
}
void ::HIR::ExprVisitor::visit_node(::HIR::ExprNode& node) {
}
void ::HIR::ExprVisitorDef::visit_node_ptr(::std::unique_ptr< ::HIR::ExprNode>& node_ptr) {
    assert(node_ptr);
    TRACE_FUNCTION_F(&*node_ptr << " " << node_ptr->type_name());
    node_ptr->visit(*this);
    visit_type(node_ptr->m_res_type);
}
DEF_VISIT_H(ExprNode_Block, node) {
    TRACE_FUNCTION_F("_Block");
    for(auto& subnode : node.m_nodes) {
        visit_node_ptr(subnode);
    }
    if( node.m_value_node )
        visit_node_ptr(node.m_value_node);
}
DEF_VISIT_H(ExprNode_ConstBlock, node) {
    TRACE_FUNCTION_F("_ConstBlock");
    visit_node_ptr(node.m_inner);
}
DEF_VISIT_H(ExprNode_Asm, node) {
    TRACE_FUNCTION_F("_Asm");
    for(auto& v : node.m_outputs)
        visit_node_ptr(v.value);
    for(auto& v : node.m_inputs)
        visit_node_ptr(v.value);
}
DEF_VISIT_H(ExprNode_Asm2, node) {
    TRACE_FUNCTION_F("_Asm2");
    for(auto& v : node.m_params)
    {
        TU_MATCH_HDRA( (v), { )
        TU_ARMA(Const, e) {
            visit_node_ptr(e);
            }
        TU_ARMA(Sym, e) {
            visit_path(::HIR::Visitor::PathContext::VALUE, e);
            }
        TU_ARMA(RegSingle, e) {
            visit_node_ptr(e.val);
            }
        TU_ARMA(Reg, e) {
            if(e.val_in)    visit_node_ptr(e.val_in);
            if(e.val_out)   visit_node_ptr(e.val_out);
            }
        }
    }
}
DEF_VISIT_H(ExprNode_Return, node) {
    TRACE_FUNCTION_F("_Return");
    visit_node_ptr(node.m_value);
}
DEF_VISIT_H(ExprNode_Yield, node) {
    TRACE_FUNCTION_F("_Yield");
    visit_node_ptr(node.m_value);
}
DEF_VISIT_H(ExprNode_Let, node) {
    TRACE_FUNCTION_F("_Let: " << node.m_pattern);
    // Visit the value FIRST as it's evaluated before the variable is defined
    if( node.m_value ) {
        visit_node_ptr(node.m_value);
    }
    visit_pattern(node.span(), node.m_pattern);
    visit_type(node.m_type);
}
DEF_VISIT_H(ExprNode_Loop, node) {
    TRACE_FUNCTION_F("_Loop");
    visit_node_ptr(node.m_code);
}
DEF_VISIT_H(ExprNode_LoopControl, node) {
    TRACE_FUNCTION_F("_LoopControl");
    if( node.m_value ) {
        visit_node_ptr(node.m_value);
    }
}
DEF_VISIT_H(ExprNode_Match, node) {
    TRACE_FUNCTION_F("_Match");
    visit_node_ptr(node.m_value);
    for(auto& arm : node.m_arms)
    {
        for(auto& pat : arm.m_patterns)
            visit_pattern(node.span(), pat);
        for(auto& c : arm.m_guards) {
            visit_pattern(node.span(), c.pat);
            visit_node_ptr(c.val);
        }
        visit_node_ptr(arm.m_code);
    }
}
DEF_VISIT_H(ExprNode_If, node) {
    TRACE_FUNCTION_F("_If");
    visit_node_ptr(node.m_cond);
    visit_node_ptr(node.m_true);
    if( node.m_false )
        visit_node_ptr(node.m_false);
}

DEF_VISIT(ExprNode_Assign, node,
    TRACE_FUNCTION_F("_Assign");
    visit_node_ptr(node.m_slot);
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_BinOp, node,
    TRACE_FUNCTION_F("_BinOp");
    visit_node_ptr(node.m_left);
    visit_node_ptr(node.m_right);
)
DEF_VISIT(ExprNode_UniOp, node,
    TRACE_FUNCTION_F("_UniOp");
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_Borrow, node,
    TRACE_FUNCTION_F("_Borrow");
    visit_node_ptr(node.m_value);
)
DEF_VISIT(ExprNode_RawBorrow, node,
    visit_node_ptr(node.m_value);
)
DEF_VISIT_H(ExprNode_Cast, node) {
    TRACE_FUNCTION_F("_Cast " << node.m_dst_type);
    visit_type(node.m_dst_type);
    visit_node_ptr(node.m_value);
}
DEF_VISIT_H(ExprNode_Unsize, node) {
    TRACE_FUNCTION_F("_Unsize " << node.m_dst_type);
    visit_type(node.m_dst_type);
    visit_node_ptr(node.m_value);
}
DEF_VISIT_H(ExprNode_Index, node) {
    TRACE_FUNCTION_F("_Index");
    visit_node_ptr(node.m_value);
    visit_node_ptr(node.m_index);
}
DEF_VISIT_H(ExprNode_Deref, node) {
    TRACE_FUNCTION_F("_Deref");
    visit_node_ptr(node.m_value);
}
DEF_VISIT_H(ExprNode_Emplace, node) {
    TRACE_FUNCTION_F("_Emplace");
    if( node.m_place )
        visit_node_ptr(node.m_place);
    visit_node_ptr(node.m_value);
}

DEF_VISIT_H(ExprNode_TupleVariant, node) {
    TRACE_FUNCTION_F("_TupleVariant: " << node.m_path);
    visit_generic_path(::HIR::Visitor::PathContext::VALUE, node.m_path);

    for(auto& ty : node.m_arg_types) {
        if( ty != HIR::TypeRef() )
            visit_type(ty);
    }

    for(auto& arg : node.m_args)
        visit_node_ptr(arg);
}
DEF_VISIT_H(ExprNode_CallPath, node) {
    TRACE_FUNCTION_F("_CallPath: " << node.m_path);
    for(auto& ty : node.m_cache.m_arg_types)
        visit_type(ty);

    visit_path(::HIR::Visitor::PathContext::VALUE, node.m_path);
    for(auto& arg : node.m_args)
        visit_node_ptr(arg);
}
DEF_VISIT_H(ExprNode_CallValue, node) {
    TRACE_FUNCTION_F("_CallValue:");
    for(auto& ty : node.m_arg_types)
        visit_type(ty);

    visit_node_ptr(node.m_value);
    for(auto& arg : node.m_args)
        visit_node_ptr(arg);
}
DEF_VISIT_H(ExprNode_CallMethod, node) {
    TRACE_FUNCTION_FR("_CallMethod: " << node.m_method, "_CallMethod: " << node.m_method);
    visit_path_params(node.m_params);
    for(auto& ty : node.m_cache.m_arg_types)
        visit_type(ty);

    visit_path(::HIR::Visitor::PathContext::VALUE, node.m_method_path);

    visit_node_ptr(node.m_value);
    for(auto& arg : node.m_args)
        visit_node_ptr(arg);
}
DEF_VISIT_H(ExprNode_Field, node) {
    TRACE_FUNCTION_F("_Field: " << node.m_field);
    visit_node_ptr(node.m_value);
}

DEF_VISIT(ExprNode_Literal, node,
    TRACE_FUNCTION_F("_Literal");
)
DEF_VISIT(ExprNode_UnitVariant, node,
    TRACE_FUNCTION_F("_UnitVariant: " << node.m_path);
    visit_generic_path(::HIR::Visitor::PathContext::VALUE, node.m_path);
)
DEF_VISIT(ExprNode_PathValue, node,
    TRACE_FUNCTION_F("_PathValue: " << node.m_path);
    visit_path(::HIR::Visitor::PathContext::VALUE, node.m_path);
)
DEF_VISIT(ExprNode_Variable, node,
    TRACE_FUNCTION_F("_Variable: #" << node.m_slot);
)
DEF_VISIT(ExprNode_ConstParam, node,
    TRACE_FUNCTION_F("_ConstParam");
)
DEF_VISIT_H(ExprNode_StructLiteral, node) {
    TRACE_FUNCTION_F("_StructLiteral: " << node.m_real_path);
    if(node.m_type != HIR::TypeRef())
        visit_type(node.m_type);
    if( node.m_base_value )
        visit_node_ptr(node.m_base_value);
    for(auto& val : node.m_values)
        visit_node_ptr(val.second);

    visit_generic_path(::HIR::Visitor::PathContext::TYPE, node.m_real_path);
}
DEF_VISIT_H(ExprNode_Tuple, node) {
    TRACE_FUNCTION_F("_Tuple");
    for(auto& val : node.m_vals)
        visit_node_ptr(val);
}
DEF_VISIT_H(ExprNode_ArrayList, node) {
    TRACE_FUNCTION_F("_ArrayList");
    for(auto& val : node.m_vals)
        visit_node_ptr(val);
}
DEF_VISIT(ExprNode_ArraySized, node,
    TRACE_FUNCTION_F("_ArraySized");
    visit_node_ptr(node.m_val);
    //visit_arraysize(node.m_size); // Don't do this, array sizes are not part of the normal expression tree
)

DEF_VISIT_H(ExprNode_Closure, node) {
    TRACE_FUNCTION_F("_Closure");
    if( node.m_obj_path != HIR::GenericPath() )
    {
        for(auto& cap : node.m_captures)
            visit_node_ptr(cap);
    }
    else
    {
        for(auto& arg : node.m_args) {
            visit_pattern(node.span(), arg.first);
            visit_type(arg.second);
        }
        visit_type(node.m_return);
        visit_node_ptr(node.m_code);
    }
}
namespace HIR {
::std::ostream& operator<<(::std::ostream& os, const ExprNode_Closure::AvuCache::Capture& x)
{
    os << "#" << x.root_slot;
    for(const auto& n : x.fields) {
        if( n == RcString() ) {
            os << ".*";
        }
        else {
            os << "." << n;
        }
    }
    os << "[" << x.usage << "]";
    return os;
}
}
DEF_VISIT_H(ExprNode_Generator, node) {
    TRACE_FUNCTION_F("_Generator");
    //for(auto& arg : node.m_args) {
    //    visit_pattern(node.span(), arg.first);
    //    visit_type(arg.second);
    //}
    visit_type(node.m_return);
    visit_type(node.m_yield_ty);
    visit_type(node.m_resume_ty);
    if(node.m_code)
    {
        visit_node_ptr(node.m_code);
    }
    else
    {
        for(auto& cap : node.m_captures)
            visit_node_ptr(cap);
    }
}
DEF_VISIT_H(ExprNode_GeneratorWrapper, node) {
    //for(auto& arg : node.m_args) {
    //    visit_pattern(node.span(), arg.first);
    //    visit_type(arg.second);
    //}
    visit_type(node.m_return);
    visit_type(node.m_yield_ty);
    if(node.m_code)
    {
        visit_node_ptr(node.m_code);
    }
}

#undef DEF_VISIT
#undef DEF_VISIT_H

// TODO: Merge this with the stuff in ::HIR::Visitor
void ::HIR::ExprVisitorDef::visit_pattern(const Span& sp, ::HIR::Pattern& pat)
{
    TU_MATCH_HDRA( (pat.m_data), {)
    TU_ARMA(Any, e) {
        }
    TU_ARMA(Box, e) {
        this->visit_pattern(sp, *e.sub);
        }
    TU_ARMA(Ref, e) {
        this->visit_pattern(sp, *e.sub);
        }
    TU_ARMA(Tuple, e) {
        for(auto& subpat : e.sub_patterns)
            this->visit_pattern(sp, subpat);
        }
    TU_ARMA(SplitTuple, e) {
        for(auto& subpat : e.leading)
            this->visit_pattern(sp, subpat);
        for(auto& subpat : e.trailing)
            this->visit_pattern(sp, subpat);
        }
    TU_ARMA(PathValue, e) {
        // Nothing.
        this->visit_path(HIR::Visitor::PathContext::VALUE, e.path);
        }
    TU_ARMA(PathTuple, e) {
        this->visit_path(HIR::Visitor::PathContext::VALUE, e.path);
        for(auto& subpat : e.leading)
            this->visit_pattern(sp, subpat);
        for(auto& subpat : e.trailing)
            this->visit_pattern(sp, subpat);
        }
    TU_ARMA(PathNamed, e) {
        this->visit_path(HIR::Visitor::PathContext::TYPE, e.path);
        for(auto& fld_pat : e.sub_patterns)
            this->visit_pattern(sp, fld_pat.second);
        }
    TU_ARMA(Value, e) {
        }
    TU_ARMA(Range, e) {
        }
    TU_ARMA(Slice, e) {
        for(auto& subpat : e.sub_patterns)
            this->visit_pattern(sp, subpat);
        }
    TU_ARMA(SplitSlice, e) {
        for(auto& subpat : e.leading)
            this->visit_pattern(sp, subpat);
        for(auto& subpat : e.trailing)
            this->visit_pattern(sp, subpat);
        }
    TU_ARMA(Or, e) {
        for(auto& subpat : e)
            this->visit_pattern(sp, subpat);
        }
    }
}
void ::HIR::ExprVisitorDef::visit_type(::HIR::TypeRef& ty)
{
    TU_MATCH(::HIR::TypeData, (ty.data_mut()), (e),
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
        for(auto& trait : e.m_traits) {
            this->visit_trait_path(trait);
        }
        TU_MATCH_HDRA( (e.m_inner), {)
        TU_ARMA(Known, ee) {
            this->visit_type(ee);
            }
        TU_ARMA(Fcn, ee) {
            this->visit_path(::HIR::Visitor::PathContext::TYPE, ee.m_origin);
            }
        TU_ARMA(Alias, ee) {
            }
        }
        ),
    (Array,
        this->visit_type( e.inner );
        ),
    (Slice,
        this->visit_type( e.inner );
        ),
    (Tuple,
        for(auto& t : e) {
            this->visit_type(t);
        }
        ),
    (Borrow,
        this->visit_type( e.inner );
        ),
    (Pointer,
        this->visit_type( e.inner );
        ),
    (NamedFunction,
        this->visit_path(::HIR::Visitor::PathContext::VALUE, e.path);
        ),
    (Function,
        for(auto& t : e.m_arg_types) {
            this->visit_type(t);
        }
        this->visit_type(e.m_rettype);
        ),
    (Closure,
        //for(auto& t : e.m_closure_arg_types) {
        //    this->visit_type(t);
        //}
        //this->visit_type(e.m_closure_rettype);
        ),
    (Generator,
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
        this->visit_type(assoc.second.type);
    for(auto& assoc : p.m_trait_bounds)
        for(auto& t : assoc.second.traits)
            this->visit_trait_path(t);
}
void ::HIR::ExprVisitorDef::visit_path(::HIR::Visitor::PathContext pc, ::HIR::Path& path)
{
    TU_MATCHA( (path.m_data), (e),
    (Generic,
        visit_generic_path(pc, e);
        ),
    (UfcsKnown,
        visit_type(e.type);
        visit_generic_path(pc, e.trait);
        visit_path_params(e.params);
        ),
    (UfcsUnknown,
        visit_type(e.type);
        visit_path_params(e.params);
        ),
    (UfcsInherent,
        visit_type(e.type);
        visit_path_params(e.params);
        visit_path_params(e.impl_params);
        )
    )
}
void ::HIR::ExprVisitorDef::visit_generic_path(::HIR::Visitor::PathContext pc, ::HIR::GenericPath& path)
{
    visit_path_params(path.m_params);
}

