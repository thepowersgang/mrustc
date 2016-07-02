/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_typeck/expr_cs.cpp
 * - Constraint Solver type inferrence
 */
#include "main_bindings.hpp"
#include <hir/expr.hpp>
#include <hir/hir.hpp>
#include <hir/visitor.hpp>
#include <algorithm>    // std::find_if

#include "expr.hpp"

// PLAN: Build up a set of conditions that are easier to solve
struct Context
{
    struct Binding
    {
        ::HIR::TypeRef  ty;
        unsigned int ivar;
    };
    struct IVar
    {
        //unsigned int    unified_with;
        ::std::unique_ptr< HIR::TypeRef>    known_type;
    };
    
    /// Inferrence variable equalities
    struct TyEq
    {
        unsigned int left_ivar;
        unsigned int right_ivar;
        /// If not nullptr, this points to the node that yeilded `right_ivar` and indicates that a coercion can happen here
        ::HIR::ExprNodeP* node_ptr_ptr;
    };
    struct TyConcrete
    {
        unsigned int left_ivar;
        const ::HIR::TypeRef&   type;
        ::HIR::ExprNodeP* node_ptr_ptr;
    };
    
    const ::HIR::Crate& m_crate;
    
    ::std::vector<Binding>  bindings;
    ::std::vector<IVar> ivars;
    
    ::std::vector<TyEq> links;
    /// Boundary conditions - places where the exact (or partial) type are known
    ::std::vector<TyConcrete>   boundaries;
    /// Nodes that need revisiting (e.g. method calls when the receiver isn't known)
    ::std::vector< ::HIR::ExprNode*>    to_visit;
    
    Context(const ::HIR::Crate& crate):
        m_crate(crate)
    {}
    
    void add_ivars(::HIR::TypeRef& ty);
    // - Equate two types, with no possibility of coercion
    //  > Errors if the types are incompatible.
    //  > Forces types if one side is an infer
    void equate_types(const Span& sp, const ::HIR::TypeRef& l, const ::HIR::TypeRef& r);
    // - Equate two types, allowing inferrence
    void equate_types_coerce(const Span& sp, const ::HIR::TypeRef& l, ::HIR::ExprNodeP& node_ptr);
    // - Equate 
    void equate_types_assoc(const Span& sp, const ::HIR::TypeRef& l,  const ::HIR::SimplePath& trait, const ::std::vector< ::HIR::TypeRef>& ty_args, const ::HIR::TypeRef& impl_ty, const char *name);
    
    // - Add a pattern binding (forcing the type to match)
    void add_binding(const Span& sp, ::HIR::Pattern& pat, ::HIR::TypeRef& type);
    const ::HIR::TypeRef& get_var(const Span& sp, unsigned int idx) const;
    
    // - Add a revisit entry
    void add_revisit(::HIR::ExprNode& node);

    typedef ::std::vector<::std::pair< const ::HIR::SimplePath*, const ::HIR::Trait* > >    t_trait_list;
    void push_traits(const t_trait_list& list);
    void pop_traits(const t_trait_list& list);
};


class ExprVisitor_Enum:
    public ::HIR::ExprVisitor
{
    Context& context;
    const ::HIR::TypeRef&   ret_type;
public:
    ExprVisitor_Enum(Context& context, const ::HIR::TypeRef& ret_type):
        context(context),
        ret_type(ret_type)
    {
    }
    
    void visit(::HIR::ExprNode_Block& node) override
    {
        TRACE_FUNCTION_F("{ ... }");
        this->context.push_traits( node.m_traits );
        
        for( unsigned int i = 0; i < node.m_nodes.size(); i ++ )
        {
            auto& snp = node.m_nodes[i];
            this->context.add_ivars( snp->m_res_type );
            if( i == node.m_nodes.size()-1 ) {
                this->context.equate_types(snp->span(), node.m_res_type, snp->m_res_type);
            }
            else {
                // TODO: Ignore? or force to ()? - Depends on inner
                // - Blocks (and block-likes) are forced to ()
                //  - What if they were '({});'? Then they're left dangling
            }
            snp->visit(*this);
        }
        
        this->context.pop_traits( node.m_traits );
    }
    void visit(::HIR::ExprNode_Return& node) override
    {
        TRACE_FUNCTION_F("return ...");
        this->context.add_ivars( node.m_value->m_res_type );

        this->context.equate_types_coerce(node.span(), this->ret_type, node.m_value);
        
        node.m_value->visit( *this );
    }
    
    void visit(::HIR::ExprNode_Loop& node) override
    {
        TRACE_FUNCTION_F("loop { ... }");
        
        // TODO: Node must return ()?
        
        node.m_code->visit( *this );
    }
    void visit(::HIR::ExprNode_LoopControl& node) override
    {
        TRACE_FUNCTION_F((node.m_continue ? "continue" : "break") << " '" << node.m_label);
        // Nothing
    }
    
    void visit(::HIR::ExprNode_Let& node) override
    {
        TRACE_FUNCTION_F("let " << node.m_pattern << ": " << node.m_type);
        
        this->context.add_ivars( node.m_type );
        this->context.add_binding(node.span(), node.m_pattern, node.m_type);
        
        this->context.add_ivars( node.m_value->m_res_type );
        this->context.equate_types_coerce( node.span(), node.m_type, node.m_value );
        
        node.m_value->visit( *this );
    }
    void visit(::HIR::ExprNode_Match& node) override
    {
        TRACE_FUNCTION_F("match ...");
        
        this->context.add_ivars(node.m_value->m_res_type);
        
        for(auto& arm : node.m_arms)
        {
            DEBUG("ARM " << arm.m_patterns);
            for(auto& pat : arm.m_patterns)
            {
                this->context.add_binding(node.span(), pat, node.m_value->m_res_type);
            }
            
            if( arm.m_cond )
            {
                this->context.add_ivars( arm.m_cond->m_res_type );
                this->context.equate_types_coerce(arm.m_cond->span(), ::HIR::TypeRef(::HIR::CoreType::Bool), arm.m_cond);
                arm.m_cond->visit( *this );
            }
            
            this->context.add_ivars( arm.m_code->m_res_type );
            this->context.equate_types_coerce(node.span(), node.m_res_type, arm.m_code);
            arm.m_code->visit( *this );
        }
        
        node.m_value->visit( *this );
    }
    
    void visit(::HIR::ExprNode_If& node) override
    {
        TRACE_FUNCTION_F("if ...");
        
        this->context.add_ivars( node.m_cond->m_res_type );
        this->context.equate_types_coerce(node.m_cond->span(), ::HIR::TypeRef(::HIR::CoreType::Bool), node.m_cond);
        
        this->context.add_ivars( node.m_true->m_res_type );
        this->context.equate_types_coerce(node.span(), node.m_res_type,  node.m_true);
        node.m_true->visit( *this );
        
        if( node.m_false ) {
            this->context.add_ivars( node.m_false->m_res_type );
            this->context.equate_types_coerce(node.span(), node.m_res_type,  node.m_false);
            node.m_false->visit( *this );
        }
        else {
            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
        }
    }
    
    
    void visit(::HIR::ExprNode_Assign& node) override
    {
        TRACE_FUNCTION_F("... = ...");
        this->context.add_ivars( node.m_slot ->m_res_type );
        this->context.add_ivars( node.m_value->m_res_type );
        
        // Plain assignment can't be overloaded, requires equal types
        if( node.m_op == ::HIR::ExprNode_Assign::Op::None ) {
            this->context.equate_types_coerce(node.span(), node.m_slot->m_res_type, node.m_value);
        }
        else {
            // Type inferrence using the +=
            // - "" as type name to indicate that it's just using the trait magic?
            const char *lang_item = nullptr;
            switch( node.m_op )
            {
            case ::HIR::ExprNode_Assign::Op::None:  throw "";
            case ::HIR::ExprNode_Assign::Op::Add: lang_item = "add_assign"; break;
            case ::HIR::ExprNode_Assign::Op::Sub: lang_item = "sub_assign"; break;
            case ::HIR::ExprNode_Assign::Op::Mul: lang_item = "mul_assign"; break;
            case ::HIR::ExprNode_Assign::Op::Div: lang_item = "div_assign"; break;
            case ::HIR::ExprNode_Assign::Op::Mod: lang_item = "rem_assign"; break;
            case ::HIR::ExprNode_Assign::Op::And: lang_item = "bitand_assign"; break;
            case ::HIR::ExprNode_Assign::Op::Or : lang_item = "bitor_assign" ; break;
            case ::HIR::ExprNode_Assign::Op::Xor: lang_item = "bitxor_assign"; break;
            case ::HIR::ExprNode_Assign::Op::Shr: lang_item = "shl_assign"; break;
            case ::HIR::ExprNode_Assign::Op::Shl: lang_item = "shr_assign"; break;
            }
            assert(lang_item);
            const auto& trait_path = this->context.m_crate.get_lang_item_path(node.span(), lang_item);
            
            this->context.equate_types_assoc(node.span(), ::HIR::TypeRef(), trait_path, ::make_vec1(node.m_value->m_res_type.clone()),  node.m_slot->m_res_type.clone(), "");
        }
        
        node.m_slot->visit( *this );
        node.m_value->visit( *this );
    }
    void visit(::HIR::ExprNode_BinOp& node) override
    {
        TRACE_FUNCTION_F("... "<<::HIR::ExprNode_BinOp::opname(node.m_op)<<" ...");
        this->context.add_ivars( node.m_left ->m_res_type );
        this->context.add_ivars( node.m_right->m_res_type );
        
        switch(node.m_op)
        {
        case ::HIR::ExprNode_BinOp::Op::CmpEqu:
        case ::HIR::ExprNode_BinOp::Op::CmpNEqu:
        case ::HIR::ExprNode_BinOp::Op::CmpLt:
        case ::HIR::ExprNode_BinOp::Op::CmpLtE:
        case ::HIR::ExprNode_BinOp::Op::CmpGt:
        case ::HIR::ExprNode_BinOp::Op::CmpGtE: {
            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef(::HIR::CoreType::Bool));
            
            const char* item_name = nullptr;
            switch(node.m_op)
            {
            case ::HIR::ExprNode_BinOp::Op::CmpEqu:  item_name = "eq";  break;
            case ::HIR::ExprNode_BinOp::Op::CmpNEqu: item_name = "eq";  break;
            case ::HIR::ExprNode_BinOp::Op::CmpLt:   item_name = "ord"; break;
            case ::HIR::ExprNode_BinOp::Op::CmpLtE:  item_name = "ord"; break;
            case ::HIR::ExprNode_BinOp::Op::CmpGt:   item_name = "ord"; break;
            case ::HIR::ExprNode_BinOp::Op::CmpGtE:  item_name = "ord"; break;
            default: break;
            }
            assert(item_name);
            const auto& op_trait = this->context.m_crate.get_lang_item_path(node.span(), item_name);

            this->context.equate_types_assoc(node.span(), ::HIR::TypeRef(),  op_trait, ::make_vec1(node.m_right->m_res_type.clone()), node.m_left->m_res_type.clone(), "");
            break; }
        
        case ::HIR::ExprNode_BinOp::Op::BoolAnd:
        case ::HIR::ExprNode_BinOp::Op::BoolOr:
            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef(::HIR::CoreType::Bool));
            this->context.equate_types(node.span(), node.m_left ->m_res_type, ::HIR::TypeRef(::HIR::CoreType::Bool));
            this->context.equate_types(node.span(), node.m_right->m_res_type, ::HIR::TypeRef(::HIR::CoreType::Bool));
            break;
        default: {
            const char* item_name = nullptr;
            switch(node.m_op)
            {
            case ::HIR::ExprNode_BinOp::Op::CmpEqu:  throw "";
            case ::HIR::ExprNode_BinOp::Op::CmpNEqu: throw "";
            case ::HIR::ExprNode_BinOp::Op::CmpLt:   throw "";
            case ::HIR::ExprNode_BinOp::Op::CmpLtE:  throw "";
            case ::HIR::ExprNode_BinOp::Op::CmpGt:   throw "";
            case ::HIR::ExprNode_BinOp::Op::CmpGtE:  throw "";
            case ::HIR::ExprNode_BinOp::Op::BoolAnd: throw "";
            case ::HIR::ExprNode_BinOp::Op::BoolOr:  throw "";

            case ::HIR::ExprNode_BinOp::Op::Add: item_name = "add"; break;
            case ::HIR::ExprNode_BinOp::Op::Sub: item_name = "sub"; break;
            case ::HIR::ExprNode_BinOp::Op::Mul: item_name = "mul"; break;
            case ::HIR::ExprNode_BinOp::Op::Div: item_name = "div"; break;
            case ::HIR::ExprNode_BinOp::Op::Mod: item_name = "rem"; break;
            
            case ::HIR::ExprNode_BinOp::Op::And: item_name = "bitand"; break;
            case ::HIR::ExprNode_BinOp::Op::Or:  item_name = "bitor";  break;
            case ::HIR::ExprNode_BinOp::Op::Xor: item_name = "bitxor"; break;
            
            case ::HIR::ExprNode_BinOp::Op::Shr: item_name = "shr"; break;
            case ::HIR::ExprNode_BinOp::Op::Shl: item_name = "shl"; break;
            }
            assert(item_name);
            const auto& op_trait = this->context.m_crate.get_lang_item_path(node.span(), item_name);
            
            this->context.equate_types_assoc(node.span(), node.m_res_type,  op_trait, ::make_vec1(node.m_right->m_res_type.clone()), node.m_left->m_res_type.clone(), "Output");
            break; }
        }
        node.m_left ->visit( *this );
        node.m_right->visit( *this );
    }
    void visit(::HIR::ExprNode_UniOp& node) override
    {
        TRACE_FUNCTION_F(::HIR::ExprNode_UniOp::opname(node.m_op) << "...");
        this->context.add_ivars( node.m_value->m_res_type );
        switch(node.m_op)
        {
        case ::HIR::ExprNode_UniOp::Op::Ref:
            // TODO: Can Ref/RefMut trigger coercions?
            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, node.m_value->m_res_type.clone()));
            break;
        case ::HIR::ExprNode_UniOp::Op::RefMut:
            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, node.m_value->m_res_type.clone()));
            break;
        case ::HIR::ExprNode_UniOp::Op::Invert:
            this->context.equate_types_assoc(node.span(), node.m_res_type,  this->context.m_crate.get_lang_item_path(node.span(), "not"), {}, node.m_value->m_res_type.clone(), "Output");
        case ::HIR::ExprNode_UniOp::Op::Negate:
            this->context.equate_types_assoc(node.span(), node.m_res_type,  this->context.m_crate.get_lang_item_path(node.span(), "minus"), {}, node.m_value->m_res_type.clone(), "Output");
            break;
        }
        node.m_value->visit( *this );
    }
    void visit(::HIR::ExprNode_Cast& node) override
    {
        TRACE_FUNCTION_F("... as " << node.m_res_type);
        this->context.add_ivars( node.m_value->m_res_type );
        // Depending on the form of the result type, it can lead to links between the input and output
        node.m_value->visit( *this );
    }
    void visit(::HIR::ExprNode_Unsize& node) override
    {
        BUG(node.span(), "Hit _Unsize");
    }
    void visit(::HIR::ExprNode_Index& node) override
    {
        TRACE_FUNCTION_F("... [ ... ]");
        this->context.add_ivars( node.m_value->m_res_type );
        this->context.add_ivars( node.m_index->m_res_type );
        
        const auto& op_trait = this->context.m_crate.get_lang_item_path(node.span(), "index");
        this->context.equate_types_assoc(node.span(), node.m_res_type,  op_trait, ::make_vec1(node.m_index->m_res_type.clone()), node.m_value->m_res_type.clone(), "Output");
        
        node.m_value->visit( *this );
        node.m_index->visit( *this );
    }
    void visit(::HIR::ExprNode_Deref& node) override
    {
        TRACE_FUNCTION_F("*...");
        this->context.add_ivars( node.m_value->m_res_type );
        
        const auto& op_trait = this->context.m_crate.get_lang_item_path(node.span(), "deref");
        this->context.equate_types_assoc(node.span(), node.m_res_type,  op_trait, {}, node.m_value->m_res_type.clone(), "Target");

        node.m_value->visit( *this );
    }
    
    void visit(::HIR::ExprNode_TupleVariant& node) override
    {
        TRACE_FUNCTION_F(node.m_path << "(...) [" << (node.m_is_struct ? "struct" : "enum") << "]");
        for( auto& val : node.m_args ) {
            this->context.add_ivars( val->m_res_type );
        }
        
        // TODO: Result type
        // TODO: Bind fields with type params
        
        for( auto& val : node.m_args ) {
            val->visit( *this );
        }
    }
    void visit(::HIR::ExprNode_StructLiteral& node) override
    {
        TRACE_FUNCTION_F(node.m_path << "{...} [" << (node.m_is_struct ? "struct" : "enum") << "]");
        for( auto& val : node.m_values ) {
            this->context.add_ivars( val.second->m_res_type );
        }
        
        // TODO: Result type
        // TODO: Bind fields with type params
        
        for( auto& val : node.m_values ) {
            val.second->visit( *this );
        }
    }
    void visit(::HIR::ExprNode_CallPath& node) override
    {
        TRACE_FUNCTION_F(node.m_path << "(...)");
        for( auto& val : node.m_args ) {
            this->context.add_ivars( val->m_res_type );
        }
        
        // TODO: Locate method and link arguments
        // - Beware problems with inferrence passthrough order! Argument can coerce

        for( auto& val : node.m_args ) {
            val->visit( *this );
        }
    }
    void visit(::HIR::ExprNode_CallValue& node) override
    {
        TRACE_FUNCTION_F("...(...)");
        this->context.add_ivars( node.m_value->m_res_type );
        for( auto& val : node.m_args ) {
            this->context.add_ivars( val->m_res_type );
        }
        
        // TODO: Locate method and link arguments

        node.m_value->visit( *this );
        for( auto& val : node.m_args ) {
            val->visit( *this );
        }
    }
    void visit(::HIR::ExprNode_CallMethod& node) override
    {
        TRACE_FUNCTION_F("(...)."<<node.m_method<<"(...)");
        this->context.add_ivars( node.m_value->m_res_type );
        for( auto& val : node.m_args ) {
            this->context.add_ivars( val->m_res_type );
        }
        
        // TODO: Locate method and link arguments
        
        node.m_value->visit( *this );
        for( auto& val : node.m_args ) {
            val->visit( *this );
        }
    }
    void visit(::HIR::ExprNode_Field& node) override
    {
        TRACE_FUNCTION_F("(...)."<<node.m_field);
        this->context.add_ivars( node.m_value->m_res_type );
        
        this->context.add_revisit( node );
        
        node.m_value->visit( *this );
    }
    
    void visit(::HIR::ExprNode_Tuple& node) override
    {
        TRACE_FUNCTION_F("(...,)");
        for( auto& val : node.m_vals ) {
            this->context.add_ivars( val->m_res_type );
        }
        
        // TODO: Cleanly equate into tuple (can it coerce?)
        
        for( auto& val : node.m_vals ) {
            val->visit( *this );
        }
    }
    void visit(::HIR::ExprNode_ArrayList& node) override
    {
        TRACE_FUNCTION_F("[...,]");
        for( auto& val : node.m_vals ) {
            this->context.add_ivars( val->m_res_type );
        }
        
        // TODO: Cleanly equate into array (with coercions)
        
        for( auto& val : node.m_vals ) {
            val->visit( *this );
        }
    }
    void visit(::HIR::ExprNode_ArraySized& node) override
    {
        TRACE_FUNCTION_F("[...; "<<node.m_size_val<<"]");
        this->context.add_ivars( node.m_val->m_res_type );
        this->context.add_ivars( node.m_size->m_res_type );
        
        // TODO: Cleanly equate into array (TODO: with coercions?)
        
        node.m_val->visit( *this );
        node.m_size->visit( *this );
    }
    
    void visit(::HIR::ExprNode_Literal& node) override
    {
        TU_MATCH(::HIR::ExprNode_Literal::Data, (node.m_data), (e),
        (Integer,
            DEBUG(" (: " << e.m_type << " = " << e.m_value << ")");
            ),
        (Float,
            DEBUG(" (: " << e.m_type << " = " << e.m_value << ")");
            ),
        (Boolean,
            ),
        (String,
            ),
        (ByteString,
            )
        )
    }
    void visit(::HIR::ExprNode_UnitVariant& node) override
    {
        TRACE_FUNCTION_F(node.m_path << " [" << (node.m_is_struct ? "struct" : "enum") << "]");
        
        // TODO: Infer/bind path
        // TODO: Result type
    }
    void visit(::HIR::ExprNode_PathValue& node) override
    {
        TRACE_FUNCTION_F(node.m_path);
        
        // TODO: Infer/bind path
        // TODO: Bind type
    }
    void visit(::HIR::ExprNode_Variable& node) override
    {
        TRACE_FUNCTION_F(node.m_name << "{" << node.m_slot << "}");
        
        this->context.equate_types(node.span(), node.m_res_type,  this->context.get_var(node.span(), node.m_slot));
    }
    
    void visit(::HIR::ExprNode_Closure& node) override
    {
        TRACE_FUNCTION_F("|...| ...");
        for(auto& arg : node.m_args) {
            this->context.add_ivars( arg.second );
            this->context.add_binding( node.span(), arg.first, arg.second );
        }
        this->context.add_ivars( node.m_return );
        this->context.add_ivars( node.m_code->m_res_type );
        
        // Closure result type
        ::HIR::TypeRef::Data::Data_Closure  ty_data;
        for(auto& arg : node.m_args) {
            ty_data.m_arg_types.push_back( arg.second.clone() );
        }
        ty_data.m_rettype = box$( node.m_return.clone() );
        this->context.equate_types( node.span(), node.m_res_type, ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Closure(mv$(ty_data)) ) );

        this->context.equate_types_coerce( node.span(), node.m_return, node.m_code );
        
        node.m_code->visit( *this );
    }
};


void Typecheck_Code_CS(Context context, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr)
{
    ExprVisitor_Enum    visitor(context, result_type);
    expr->visit(visitor);
    
    context.equate_types(expr->span(), result_type, expr->m_res_type);
}

void Context::add_ivars(::HIR::TypeRef& ty) {
}
void Context::equate_types(const Span& sp, const ::HIR::TypeRef& l, const ::HIR::TypeRef& r) {
}
void Context::equate_types_coerce(const Span& sp, const ::HIR::TypeRef& l, ::HIR::ExprNodeP& node_ptr) {
}
void Context::add_binding(const Span& sp, ::HIR::Pattern& pat, ::HIR::TypeRef& type) {
}
const ::HIR::TypeRef& Context::get_var(const Span& sp, unsigned int idx) const {
    if( idx < this->bindings.size() ) {
        return this->bindings[idx].ty;
    }
    else {
        BUG(sp, "get_var - Binding index out of range");
    }
}
void Context::add_revisit(::HIR::ExprNode& node) {
}
void Context::push_traits(const t_trait_list& list) {
}
void Context::pop_traits(const t_trait_list& list) {
}
void Context::equate_types_assoc(const Span& sp, const ::HIR::TypeRef& l,  const ::HIR::SimplePath& trait, const ::std::vector< ::HIR::TypeRef>& ty_args, const ::HIR::TypeRef& impl_ty, const char *name)
{
}
