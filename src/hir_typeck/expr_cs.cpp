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

#include "helpers.hpp"
#include "expr_visit.hpp"

// PLAN: Build up a set of conditions that are easier to solve
struct Context
{
    struct Binding
    {
        ::std::string   name;
        ::HIR::TypeRef  ty;
        //unsigned int ivar;
    };
    
    /// Inferrence variable equalities
    struct Coercion
    {
        ::HIR::TypeRef  left_ty;
        ::HIR::ExprNodeP& right_node_ptr;
    };
    struct Associated
    {
        ::HIR::TypeRef  left_ty;
        
        ::HIR::SimplePath   trait;
        ::std::vector< ::HIR::TypeRef>  params;
        ::HIR::TypeRef  impl_ty;
        const char* name;   // if "", no type is used (and left is ignored) - Just does trait selection
    };
    
    const ::HIR::Crate& m_crate;
    const ::HIR::GenericParams* m_impl_params;
    const ::HIR::GenericParams* m_item_params;
    
    ::std::vector<Binding>  m_bindings;
    HMTypeInferrence    m_ivars;
    
    ::std::vector<Coercion> link_coerce;
    ::std::vector<Associated> link_assoc;
    /// Nodes that need revisiting (e.g. method calls when the receiver isn't known)
    ::std::vector< ::HIR::ExprNode*>    to_visit;
    
    Context(const ::HIR::Crate& crate, const ::HIR::GenericParams* impl_params, const ::HIR::GenericParams* item_params):
        m_crate(crate),
        m_impl_params( impl_params ),
        m_item_params( item_params )
    {
    }
    
    void dump() const;
    
    bool take_changed() { return m_ivars.take_changed(); }
    bool has_rules() const {
        return link_coerce.size() > 0 || link_assoc.size() > 0 || to_visit.size() > 0;
    }
    
    void add_ivars(::HIR::TypeRef& ty);
    // - Equate two types, with no possibility of coercion
    //  > Errors if the types are incompatible.
    //  > Forces types if one side is an infer
    void equate_types(const Span& sp, const ::HIR::TypeRef& l, const ::HIR::TypeRef& r);
    // - Equate two types, allowing inferrence
    void equate_types_coerce(const Span& sp, const ::HIR::TypeRef& l, ::HIR::ExprNodeP& node_ptr);
    // - Equate 
    void equate_types_assoc(const Span& sp, const ::HIR::TypeRef& l,  const ::HIR::SimplePath& trait, ::std::vector< ::HIR::TypeRef> ty_args, const ::HIR::TypeRef& impl_ty, const char *name);
    
    // - Add a pattern binding (forcing the type to match)
    void add_binding(const Span& sp, ::HIR::Pattern& pat, ::HIR::TypeRef& type);
    
    void add_var(unsigned int index, const ::std::string& name, ::HIR::TypeRef type);
    const ::HIR::TypeRef& get_var(const Span& sp, unsigned int idx) const;
    
    // - Add a revisit entry
    void add_revisit(::HIR::ExprNode& node);

    const ::HIR::TypeRef& get_type(const ::HIR::TypeRef& ty) const { return m_ivars.get_type(ty); }
    
private:
    void add_ivars_params(::HIR::PathParams& params);
};

static void fix_param_count(const Span& sp, Context& context, const ::HIR::Path& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params);
static void fix_param_count(const Span& sp, Context& context, const ::HIR::GenericPath& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params);

class ExprVisitor_Enum:
    public ::HIR::ExprVisitor
{
    Context& context;
    const ::HIR::TypeRef&   ret_type;
    
    // TEMP: List of in-scope traits for buildup
    ::HIR::t_trait_list m_traits;
public:
    ExprVisitor_Enum(Context& context, ::HIR::t_trait_list base_traits, const ::HIR::TypeRef& ret_type):
        context(context),
        ret_type(ret_type),
        m_traits( mv$(base_traits) )
    {
    }
    
    void visit(::HIR::ExprNode_Block& node) override
    {
        TRACE_FUNCTION_F("{ ... }");
        this->push_traits( node.m_traits );
        
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
        
        this->pop_traits( node.m_traits );
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
        
        this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
        
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
        
        // TODO: Depending on the form of the result type, it can lead to links between the input and output
        
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

    void add_ivars_generic_path(const Span& sp, ::HIR::GenericPath& gp) {
        for(auto& ty : gp.m_params.m_types)
            this->context.add_ivars(ty);
    }
    void add_ivars_path(const Span& sp, ::HIR::Path& path) {
        TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
        (Generic,
            this->add_ivars_generic_path(sp, e);
            ),
        (UfcsKnown,
            this->context.add_ivars(*e.type);
            this->add_ivars_generic_path(sp, e.trait);
            for(auto& ty : e.params.m_types)
                this->context.add_ivars(ty);
            ),
        (UfcsUnknown,
            TODO(sp, "Hit a UfcsUnknown (" << path << ") - Is this an error?");
            ),
        (UfcsInherent,
            this->context.add_ivars(*e.type);
            for(auto& ty : e.params.m_types)
                this->context.add_ivars(ty);
            )
        )
    }
    
    ::HIR::TypeRef get_structenum_ty(const Span& sp, bool is_struct, ::HIR::GenericPath& gp)
    {
        if( is_struct )
        {
            const auto& str = this->context.m_crate.get_struct_by_path(sp, gp.m_path);
            fix_param_count(sp, this->context, gp, str.m_params, gp.m_params);
            
            return ::HIR::TypeRef::new_path( gp.clone(), ::HIR::TypeRef::TypePathBinding::make_Struct(&str) );
        }
        else
        {
            auto s_path = gp.m_path;
            s_path.m_components.pop_back();
            
            const auto& enm = this->context.m_crate.get_enum_by_path(sp, s_path);
            fix_param_count(sp, this->context, gp, enm.m_params, gp.m_params);
            
            return ::HIR::TypeRef::new_path( ::HIR::GenericPath(mv$(s_path), gp.m_params.clone()), ::HIR::TypeRef::TypePathBinding::make_Enum(&enm) );
        }
    }
    
    void visit(::HIR::ExprNode_TupleVariant& node) override
    {
        TRACE_FUNCTION_F(node.m_path << "(...) [" << (node.m_is_struct ? "struct" : "enum") << "]");
        for( auto& val : node.m_args ) {
            this->context.add_ivars( val->m_res_type );
        }
        
        // - Create ivars in path, and set result type
        const auto ty = this->get_structenum_ty(node.span(), node.m_is_struct, node.m_path);
        this->context.equate_types(node.span(), node.m_res_type, ty);
        
        const ::HIR::t_tuple_fields* fields_ptr = nullptr;
        TU_MATCH(::HIR::TypeRef::TypePathBinding, (ty.m_data.as_Path().binding), (e),
        (Unbound, ),
        (Opaque, ),
        (Enum,
            const auto& var_name = node.m_path.m_path.m_components.back();
            const auto& enm = *e;
            auto it = ::std::find_if(enm.m_variants.begin(), enm.m_variants.end(), [&](const auto&v)->auto{ return v.first == var_name; });
            assert(it != enm.m_variants.end());
            fields_ptr = &it->second.as_Tuple();
            ),
        (Struct,
            fields_ptr = &e->m_data.as_Tuple();
            )
        )
        assert(fields_ptr);
        const ::HIR::t_tuple_fields& fields = *fields_ptr;
        if( fields.size() != node.m_args.size() ) {
            ERROR(node.span(), E0000, "");
        }
        
        // Bind fields with type params (coercable)
        for( unsigned int i = 0; i < node.m_args.size(); i ++ )
        {
            const auto& des_ty_r = fields[i].ent;
            if( monomorphise_type_needed(des_ty_r) ) {
                TODO(node.span(), "Monomorphise tuple variant type");
            }
            else {
                this->context.equate_types_coerce(node.span(), des_ty_r,  node.m_args[i]);
            }
        }
        
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
        
        // - Create ivars in path, and set result type
        const auto ty = this->get_structenum_ty(node.span(), node.m_is_struct, node.m_path);
        this->context.equate_types(node.span(), node.m_res_type, ty);
        
        const ::HIR::t_struct_fields* fields_ptr = nullptr;
        TU_MATCH(::HIR::TypeRef::TypePathBinding, (ty.m_data.as_Path().binding), (e),
        (Unbound, ),
        (Opaque, ),
        (Enum,
            const auto& var_name = node.m_path.m_path.m_components.back();
            const auto& enm = *e;
            auto it = ::std::find_if(enm.m_variants.begin(), enm.m_variants.end(), [&](const auto&v)->auto{ return v.first == var_name; });
            assert(it != enm.m_variants.end());
            fields_ptr = &it->second.as_Struct();
            ),
        (Struct,
            fields_ptr = &e->m_data.as_Named();
            )
        )
        assert(fields_ptr);
        const ::HIR::t_struct_fields& fields = *fields_ptr;
        
        // Bind fields with type params (coercable)
        for( auto& val : node.m_values)
        {
            const auto& name = val.first;
            auto it = ::std::find_if(fields.begin(), fields.end(), [&](const auto& v)->bool{ return v.first == name; });
            assert(it != fields.end());
            const auto& des_ty_r = it->second.ent;
            
            if( monomorphise_type_needed(des_ty_r) ) {
                TODO(node.span(), "Monomorphise struct variant type");
            }
            else {
                this->context.equate_types_coerce(node.span(), des_ty_r,  val.second);
            }
        }
        
        for( auto& val : node.m_values ) {
            val.second->visit( *this );
        }
    }
    void visit(::HIR::ExprNode_UnitVariant& node) override
    {
        TRACE_FUNCTION_F(node.m_path << " [" << (node.m_is_struct ? "struct" : "enum") << "]");
        
        // - Create ivars in path, and set result type
        const auto ty = this->get_structenum_ty(node.span(), node.m_is_struct, node.m_path);
        this->context.equate_types(node.span(), node.m_res_type, ty);
    }

private:
    /// (HELPER) Populate the cache for nodes that use visit_call
    void visit_call_populate_cache(const Span& sp, ::HIR::Path& path, ::HIR::ExprCallCache& cache) const
    {
        assert(cache.m_arg_types.size() == 0);
        
        const ::HIR::Function*  fcn_ptr = nullptr;
        ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)>    monomorph_cb;
        
        TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
        (Generic,
            const auto& fcn = this->context.m_crate.get_function_by_path(sp, e.m_path);
            fix_param_count(sp, this->context, path, fcn.m_params,  e.m_params);
            fcn_ptr = &fcn;
            cache.m_fcn_params = &fcn.m_params;
            
            //const auto& params_def = fcn.m_params;
            const auto& path_params = e.m_params;
            monomorph_cb = [&](const auto& gt)->const auto& {
                    const auto& e = gt.m_data.as_Generic();
                    if( e.name == "Self" || e.binding == 0xFFFF )
                        TODO(sp, "Handle 'Self' when monomorphising");
                    if( e.binding < 256 ) {
                        BUG(sp, "Impl-level parameter on free function (#" << e.binding << " " << e.name << ")");
                    }
                    else if( e.binding < 512 ) {
                        auto idx = e.binding - 256;
                        if( idx >= path_params.m_types.size() ) {
                            BUG(sp, "Generic param out of input range - " << idx << " '"<<e.name<<"' >= " << path_params.m_types.size());
                        }
                        return this->context.get_type(path_params.m_types[idx]);
                    }
                    else {
                        BUG(sp, "Generic bounding out of total range");
                    }
                };
            ),
        (UfcsKnown,
            const auto& trait = this->context.m_crate.get_trait_by_path(sp, e.trait.m_path);
            fix_param_count(sp, this->context, path, trait.m_params, e.trait.m_params);
            if( trait.m_values.count(e.item) == 0 ) {
                BUG(sp, "Method '" << e.item << "' of trait " << e.trait.m_path << " doesn't exist");
            }
            const auto& fcn = trait.m_values.at(e.item).as_Function();
            fix_param_count(sp, this->context, path, fcn.m_params,  e.params);
            cache.m_fcn_params = &fcn.m_params;
            cache.m_top_params = &trait.m_params;
            
            // TODO: Check/apply trait bounds (apply = closure arguments or fixed trait args)
            
            fcn_ptr = &fcn;
            
            const auto& trait_params = e.trait.m_params;
            const auto& path_params = e.params;
            monomorph_cb = [&](const auto& gt)->const auto& {
                    const auto& ge = gt.m_data.as_Generic();
                    if( ge.binding == 0xFFFF ) {
                        return *e.type;
                    }
                    else if( ge.binding < 256 ) {
                        auto idx = ge.binding;
                        if( idx >= trait_params.m_types.size() ) {
                            BUG(sp, "Generic param (impl) out of input range - " << idx << " '"<<ge.name<<"' >= " << trait_params.m_types.size());
                        }
                        return this->context.get_type(trait_params.m_types[idx]);
                    }
                    else if( ge.binding < 512 ) {
                        auto idx = ge.binding - 256;
                        if( idx >= path_params.m_types.size() ) {
                            BUG(sp, "Generic param out of input range - " << idx << " '"<<ge.name<<"' >= " << path_params.m_types.size());
                        }
                        return this->context.get_type(path_params.m_types[idx]);
                    }
                    else {
                        BUG(sp, "Generic bounding out of total range");
                    }
                };
            ),
        (UfcsUnknown,
            TODO(sp, "Hit a UfcsUnknown (" << path << ") - Is this an error?");
            ),
        (UfcsInherent,
            // TODO: What if this types has ivars?
            // - Locate function (and impl block)
            const ::HIR::TypeImpl* impl_ptr = nullptr;
            this->context.m_crate.find_type_impls(*e.type, [&](const auto& ty)->const auto& {
                    if( ty.m_data.is_Infer() )
                        return this->context.get_type(ty);
                    else
                        return ty;
                },
                [&](const auto& impl) {
                    DEBUG("- impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                    auto it = impl.m_methods.find(e.item);
                    if( it == impl.m_methods.end() )
                        return false;
                    fcn_ptr = &it->second;
                    impl_ptr = &impl;
                    return true;
                });
            if( !fcn_ptr ) {
                ERROR(sp, E0000, "Failed to locate function " << path);
            }
            assert(impl_ptr);
            fix_param_count(sp, this->context, path, fcn_ptr->m_params,  e.params);
            cache.m_fcn_params = &fcn_ptr->m_params;
            
            
            // If the impl block has parameters, figure out what types they map to
            // - The function params are already mapped (from fix_param_count)
            auto& impl_params = cache.m_ty_impl_params;
            if( impl_ptr->m_params.m_types.size() > 0 ) {
                impl_params.m_types.resize( impl_ptr->m_params.m_types.size() );
                impl_ptr->m_type.match_generics(sp, *e.type, this->context.m_ivars.callback_resolve_infer(), [&](auto idx, const auto& ty) {
                    assert( idx < impl_params.m_types.size() );
                    impl_params.m_types[idx] = ty.clone();
                    });
                for(const auto& ty : impl_params.m_types)
                    assert( !( ty.m_data.is_Infer() && ty.m_data.as_Infer().index == ~0u) );
            }
            
            // Create monomorphise callback
            const auto& fcn_params = e.params;
            monomorph_cb = [&](const auto& gt)->const auto& {
                    const auto& ge = gt.m_data.as_Generic();
                    if( ge.binding == 0xFFFF ) {
                        return this->context.get_type(*e.type);
                    }
                    else if( ge.binding < 256 ) {
                        auto idx = ge.binding;
                        if( idx >= impl_params.m_types.size() ) {
                            BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << impl_params.m_types.size());
                        }
                        return this->context.get_type(impl_params.m_types[idx]);
                    }
                    else if( ge.binding < 512 ) {
                        auto idx = ge.binding - 256;
                        if( idx >= fcn_params.m_types.size() ) {
                            BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << fcn_params.m_types.size());
                        }
                        return this->context.get_type(fcn_params.m_types[idx]);
                    }
                    else {
                        BUG(sp, "Generic bounding out of total range");
                    }
                };
            )
        )

        assert( fcn_ptr );
        const auto& fcn = *fcn_ptr;
        
        // --- Monomorphise the argument/return types (into current context)
        for(const auto& arg : fcn.m_args) {
            if( monomorphise_type_needed(arg.second) ) {
                cache.m_arg_types.push_back( /*this->context.expand_associated_types(sp, */monomorphise_type_with(sp, arg.second,  monomorph_cb)/*)*/ );
            }
            else {
                cache.m_arg_types.push_back( arg.second.clone() );
            }
        }
        if( monomorphise_type_needed(fcn.m_return) ) {
            cache.m_arg_types.push_back( /*this->context.expand_associated_types(sp, */monomorphise_type_with(sp, fcn.m_return,  monomorph_cb)/*)*/ );
        }
        else {
            cache.m_arg_types.push_back( fcn.m_return.clone() );
        }
        
        cache.m_monomorph_cb = mv$(monomorph_cb);
    }
public:
    void visit(::HIR::ExprNode_CallPath& node) override
    {
        TRACE_FUNCTION_F(node.m_path << "(...)");
        for( auto& val : node.m_args ) {
            this->context.add_ivars( val->m_res_type );
        }
        
        // Populate cache
        {
            this->visit_call_populate_cache(node.span(), node.m_path, node.m_cache);
            assert( node.m_cache.m_arg_types.size() >= 1);
            
            if( node.m_args.size() != node.m_cache.m_arg_types.size() - 1 ) {
                ERROR(node.span(), E0000, "Incorrect number of arguments to " << node.m_path);
            }
        }
        
        // Link arguments
        for(unsigned int i = 0; i < node.m_args.size(); i ++)
        {
            this->context.equate_types_coerce(node.span(), node.m_cache.m_arg_types[i], node.m_args[i]);
        }
        this->context.equate_types(node.span(), node.m_res_type,  node.m_cache.m_arg_types.back());

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
        
        // Nothing can be done until type is known
        this->context.add_revisit(node);

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
        
        // - Search in-scope trait list for traits that provide a method of this name
        const ::std::string& method_name = node.m_method;
        ::HIR::t_trait_list    possible_traits;
        for(const auto& trait_ref : ::reverse(m_traits))
        {
            if( trait_ref.first == nullptr )
                break;
            
            // TODO: Search supertraits too
            auto it = trait_ref.second->m_values.find(method_name);
            if( it == trait_ref.second->m_values.end() )
                continue ;
            if( !it->second.is_Function() )
                continue ;
            possible_traits.push_back( trait_ref );
        }
        //  > Store the possible set of traits for later
        node.m_traits = mv$(possible_traits);
        
        // Resolution can't be done until lefthand type is known.
        // > Has to be done during iteraton
        this->context.add_revisit( node );
        
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
        
        ::std::vector< ::HIR::TypeRef>  tuple_tys;
        for(const auto& val : node.m_vals ) {
            // Can these coerce? Assuming not
            tuple_tys.push_back( val->m_res_type.clone() );
        }
        this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef(mv$(tuple_tys)));
        
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
        
        // Cleanly equate into array (with coercions)
        // - Result type already set, just need to extract ivar
        const auto& inner_ty = *node.m_res_type.m_data.as_Array().inner;
        for( auto& val : node.m_vals ) {
            this->context.equate_types_coerce(node.span(), inner_ty,  val);
        }
        
        for( auto& val : node.m_vals ) {
            val->visit( *this );
        }
    }
    void visit(::HIR::ExprNode_ArraySized& node) override
    {
        TRACE_FUNCTION_F("[...; "<<node.m_size_val<<"]");
        this->context.add_ivars( node.m_val->m_res_type );
        this->context.add_ivars( node.m_size->m_res_type );
        
        // Create result type (can't be known until after const expansion)
        // - Should it be created in const expansion?
        auto ty = ::HIR::TypeRef::new_array( ::HIR::TypeRef(), node.m_size_val );
        this->context.add_ivars(ty);
        this->context.equate_types(node.span(), node.m_res_type, ty);
        // Equate with coercions
        const auto& inner_ty = *ty.m_data.as_Array().inner;
        this->context.equate_types_coerce(node.span(), inner_ty, node.m_val);
        this->context.equate_types(node.span(), ::HIR::TypeRef(::HIR::CoreType::Usize), node.m_size->m_res_type);
        
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
            DEBUG(" ( " << (e ? "true" : "false") << ")");
            ),
        (String,
            ),
        (ByteString,
            )
        )
    }
    void visit(::HIR::ExprNode_PathValue& node) override
    {
        const auto& sp = node.span();
        TRACE_FUNCTION_F(node.m_path);
        
        this->add_ivars_path(node.span(), node.m_path);
        
        TU_MATCH(::HIR::Path::Data, (node.m_path.m_data), (e),
        (Generic,
            switch(node.m_target) {
            case ::HIR::ExprNode_PathValue::UNKNOWN:
                BUG(sp, "Unknown target PathValue encountered with Generic path");
            case ::HIR::ExprNode_PathValue::FUNCTION: {
                const auto& f = this->context.m_crate.get_function_by_path(sp, e.m_path);
                ::HIR::FunctionType ft {
                    f.m_unsafe,
                    f.m_abi,
                    box$( f.m_return.clone() ),
                    {}
                    };
                for( const auto& arg : f.m_args )
                    ft.m_arg_types.push_back( arg.second.clone() );
                auto ty = ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Function(mv$(ft)) );
                this->context.equate_types(sp, node.m_res_type, ty);
                } break;
            case ::HIR::ExprNode_PathValue::STATIC: {
                const auto& v = this->context.m_crate.get_static_by_path(sp, e.m_path);
                DEBUG("static v.m_type = " << v.m_type);
                this->context.equate_types(sp, node.m_res_type, v.m_type);
                } break;
            case ::HIR::ExprNode_PathValue::CONSTANT: {
                const auto& v = this->context.m_crate.get_constant_by_path(sp, e.m_path);
                DEBUG("const"<<v.m_params.fmt_args()<<" v.m_type = " << v.m_type);
                if( v.m_params.m_types.size() > 0 ) {
                    TODO(sp, "Support generic constants in typeck");
                }
                this->context.equate_types(sp, node.m_res_type, v.m_type);
                } break;
            }
            ),
        (UfcsUnknown,
            BUG(sp, "Encountered UfcsUnknown");
            ),
        (UfcsKnown,
            TODO(sp, "Look up associated constants/statics (trait)");
            ),
        (UfcsInherent,
            // TODO: If ivars are valid within the type of this UFCS, then resolution has to be deferred until iteration
            // - If they're not valid, then resolution can be done here.
            TODO(sp, "Handle associated constants/functions in type - Can the type be infer?");
            
            #if 0
            // - Locate function (and impl block)
            const ::HIR::Function* fcn_ptr = nullptr;
            const ::HIR::TypeImpl* impl_ptr = nullptr;
            this->context.m_crate.find_type_impls(*e.type, [&](const auto& ty)->const auto& {
                    if( ty.m_data.is_Infer() )
                        return this->context.get_type(ty);
                    else
                        return ty;
                },
                [&](const auto& impl) {
                    DEBUG("- impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                    auto it = impl.m_methods.find(e.item);
                    if( it == impl.m_methods.end() )
                        return false;
                    fcn_ptr = &it->second;
                    impl_ptr = &impl;
                    return true;
                });
            if( !fcn_ptr ) {
                ERROR(sp, E0000, "Failed to locate function " << path);
            }
            assert(impl_ptr);
            fix_param_count(sp, this->context, path, fcn_ptr->m_params,  e.params);
            
            // If the impl block has parameters, figure out what types they map to
            // - The function params are already mapped (from fix_param_count)
            ::HIR::PathParams   impl_params;
            if( impl_ptr->m_params.m_types.size() > 0 ) {
                impl_params.m_types.resize( impl_ptr->m_params.m_types.size() );
                impl_ptr->m_type.match_generics(sp, *e.type, this->context.callback_resolve_infer(), [&](auto idx, const auto& ty) {
                    assert( idx < impl_params.m_types.size() );
                    impl_params.m_types[idx] = ty.clone();
                    });
                for(const auto& ty : impl_params.m_types)
                    assert( !( ty.m_data.is_Infer() && ty.m_data.as_Infer().index == ~0u) );
            }
            
            // Create monomorphise callback
            const auto& fcn_params = e.params;
            auto monomorph_cb = [&](const auto& gt)->const auto& {
                    const auto& ge = gt.m_data.as_Generic();
                    if( ge.binding == 0xFFFF ) {
                        return this->context.get_type(*e.type);
                    }
                    else if( ge.binding < 256 ) {
                        auto idx = ge.binding;
                        if( idx >= impl_params.m_types.size() ) {
                            BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << impl_params.m_types.size());
                        }
                        return this->context.get_type(impl_params.m_types[idx]);
                    }
                    else if( ge.binding < 512 ) {
                        auto idx = ge.binding - 256;
                        if( idx >= fcn_params.m_types.size() ) {
                            BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << fcn_params.m_types.size());
                        }
                        return this->context.get_type(fcn_params.m_types[idx]);
                    }
                    else {
                        BUG(sp, "Generic bounding out of total range");
                    }
                };
            
            ::HIR::FunctionType ft {
                fcn_ptr->m_unsafe, fcn_ptr->m_abi,
                box$( monomorphise_type_with(sp, fcn_ptr->m_return,  monomorph_cb) ),
                {}
                };
            for(const auto& arg : fcn_ptr->m_args)
                ft.m_arg_types.push_back( monomorphise_type_with(sp, arg.second,  monomorph_cb) );
            auto ty = ::HIR::TypeRef(mv$(ft));
            
            this->context.equate_types(node.span(), node.m_res_type, ty);
            #endif
            )
        )
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
    
private:
    void push_traits(const ::HIR::t_trait_list& list) {
        this->m_traits.insert( this->m_traits.end(), list.begin(), list.end() );
    }
    void pop_traits(const ::HIR::t_trait_list& list) {
        this->m_traits.erase( this->m_traits.end() - list.size(), this->m_traits.end() );
    }
};


void Context::dump() const {
    m_ivars.dump();
    DEBUG("CS Context - " << link_coerce.size() << " Coercions, " << link_assoc.size() << " associated, " << to_visit.size() << " nodes");
    for(const auto& v : link_coerce) {
        DEBUG(v.left_ty << " := " << &*v.right_node_ptr << " (" << v.right_node_ptr->m_res_type << ")");
    }
    for(const auto& v : link_assoc) {
        DEBUG(v.left_ty << " = " << "<" << v.impl_ty << " as " << v.trait << "<" << v.params << ">>::" << v.name);
    }
    for(const auto& v : to_visit) {
        DEBUG(&v << " " << typeid(*v).name());
    }
}
void Context::add_ivars(::HIR::TypeRef& ty) {
    TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
    (Infer,
        if( e.index == ~0u ) {
            e.index = this->m_ivars.new_ivar();
            this->m_ivars.get_type(ty).m_data.as_Infer().ty_class = e.ty_class;
        }
        ),
    (Diverge,
        ),
    (Primitive,
        ),
    (Path,
        // Iterate all arguments
        TU_MATCH(::HIR::Path::Data, (e.path.m_data), (e2),
        (Generic,
            this->add_ivars_params(e2.m_params);
            ),
        (UfcsKnown,
            this->add_ivars(*e2.type);
            this->add_ivars_params(e2.trait.m_params);
            this->add_ivars_params(e2.params);
            ),
        (UfcsUnknown,
            this->add_ivars(*e2.type);
            this->add_ivars_params(e2.params);
            ),
        (UfcsInherent,
            this->add_ivars(*e2.type);
            this->add_ivars_params(e2.params);
            )
        )
        ),
    (Generic,
        ),
    (TraitObject,
        // Iterate all paths
        ),
    (Array,
        add_ivars(*e.inner);
        ),
    (Slice,
        add_ivars(*e.inner);
        ),
    (Tuple,
        for(auto& ty : e)
            add_ivars(ty);
        ),
    (Borrow,
        add_ivars(*e.inner);
        ),
    (Pointer,
        add_ivars(*e.inner);
        ),
    (Function,
        // No ivars allowed
        // TODO: Check?
        ),
    (Closure,
        // Shouldn't be possible
        )
    )
}
void Context::add_ivars_params(::HIR::PathParams& params) {
    for(auto& arg : params.m_types)
        add_ivars(arg);
}

void Context::equate_types(const Span& sp, const ::HIR::TypeRef& li, const ::HIR::TypeRef& ri) {
    // Instantly apply equality
    const auto& l_t = this->m_ivars.get_type(li);
    const auto& r_t = this->m_ivars.get_type(ri);
    
    DEBUG("- l_t = " << l_t << ", r_t = " << r_t);
    TU_IFLET(::HIR::TypeRef::Data, r_t.m_data, Infer, r_e,
        TU_IFLET(::HIR::TypeRef::Data, l_t.m_data, Infer, l_e,
            // If both are infer, unify the two ivars (alias right to point to left)
            this->m_ivars.ivar_unify(l_e.index, r_e.index);
        )
        else {
            // Righthand side is infer, alias it to the left
            this->m_ivars.set_ivar_to(r_e.index, l_t.clone());
        }
    )
    else {
        TU_IFLET(::HIR::TypeRef::Data, l_t.m_data, Infer, l_e,
            // Lefthand side is infer, alias it to the right
            this->m_ivars.set_ivar_to(l_e.index, r_t.clone());
        )
        else {
        }
    }
}
void Context::add_binding(const Span& sp, ::HIR::Pattern& pat, ::HIR::TypeRef& type)
{
    TRACE_FUNCTION_F("pat = " << pat << ", type = " << type);
    
    if( pat.m_binding.is_valid() ) {
        const auto& pb = pat.m_binding;
        
        assert( pb.is_valid() );
        switch( pb.m_type )
        {
        case ::HIR::PatternBinding::Type::Move:
            this->add_var( pb.m_slot, pb.m_name, type.clone() );
            break;
        case ::HIR::PatternBinding::Type::Ref:
            this->add_var( pb.m_slot, pb.m_name, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, type.clone()) );
            break;
        case ::HIR::PatternBinding::Type::MutRef:
            this->add_var( pb.m_slot, pb.m_name, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, type.clone()) );
            break;
        }
        // TODO: Can there be bindings within a bound pattern?
        //return ;
    }
    
    // 
    TU_MATCH(::HIR::Pattern::Data, (pat.m_data), (e),
    (Any,
        // Just leave it, the pattern says nothing
        ),
    (Value,
        TODO(sp, "Value pattern");
        ),
    (Range,
        TODO(sp, "Range pattern");
        ),
    (Box,
        TODO(sp, "Box pattern");
        ),
    (Ref,
        if( type.m_data.is_Infer() ) {
            this->equate_types(sp, type, ::HIR::TypeRef::new_borrow( e.type, this->m_ivars.new_ivar_tr() ));
            type = this->get_type(type).clone();
        }
        // Type must be a &-ptr
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Pattern-type mismatch, expected &-ptr, got " << type);
            ),
        (Infer, throw "";),
        (Borrow,
            if( te.type != e.type ) {
                ERROR(sp, E0000, "Pattern-type mismatch, expected &-ptr, got " << type);
            }
            this->add_binding(sp, *e.sub, *te.inner );
            )
        )
        ),
    (Tuple,
        if( type.m_data.is_Infer() ) {
            ::std::vector< ::HIR::TypeRef>  sub_types;
            for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                sub_types.push_back( this->m_ivars.new_ivar_tr() );
            this->equate_types(sp, type, ::HIR::TypeRef( mv$(sub_types) ));
            type = this->get_type(type).clone();
        }
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Pattern-type mismatch, expected tuple, got " << type);
            ),
        (Infer, throw ""; ),
        (Tuple,
            if( te.size() != e.sub_patterns.size() ) {
                ERROR(sp, E0000, "Pattern-type mismatch, expected " << e.sub_patterns.size() << "-tuple, got " << type);
            }
            for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                this->add_binding(sp, e.sub_patterns[i], te[i] );
            )
        )
        ),
    (Slice,
        if( type.m_data.is_Infer() ) {
            this->equate_types(sp, type, ::HIR::TypeRef::new_slice( this->m_ivars.new_ivar_tr() ));
            type = this->get_type(type).clone();
        }
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Pattern-type mismatch, expected slice, got " << type);
            ),
        (Infer, throw""; ),
        (Slice,
            for(auto& sub : e.sub_patterns)
                this->add_binding(sp, sub, *te.inner );
            )
        )
        ),
    (SplitSlice,
        if( type.m_data.is_Infer() ) {
            this->equate_types(sp, type, ::HIR::TypeRef::new_slice( this->m_ivars.new_ivar_tr() ));
            type = this->get_type(type).clone();
        }
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Pattern-type mismatch, expected slice, got " << type);
            ),
        (Infer, throw ""; ),
        (Slice,
            for(auto& sub : e.leading)
                this->add_binding( sp, sub, *te.inner );
            for(auto& sub : e.trailing)
                this->add_binding( sp, sub, *te.inner );
            if( e.extra_bind.is_valid() ) {
                this->add_var( e.extra_bind.m_slot, e.extra_bind.m_name, type.clone() );
            }
            )
        )
        ),
    
    // - Enums/Structs
    (StructTuple,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            this->equate_types( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)) );
            type = this->get_type(type).clone();
        }
        assert(e.binding);
        const auto& str = *e.binding;
        // - assert check from earlier pass
        assert( str.m_data.is_Tuple() );
        const auto& sd = str.m_data.as_Tuple();
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Pattern-type mismatch, expected struct, got " << type);
            ),
        (Infer, throw ""; ),
        (Path,
            if( ! te.binding.is_Struct() || te.binding.as_Struct() != &str ) {
                ERROR(sp, E0000, "Type mismatch in struct pattern - " << type << " is not " << e.path);
            }
            // NOTE: Must be Generic for the above to have passed
            auto& gp = te.path.m_data.as_Generic();
            
            if( e.sub_patterns.size() != sd.size() ) { 
                ERROR(sp, E0000, "Tuple struct pattern with an incorrect number of fields");
            }
            for( unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
            {
                const auto& field_type = sd[i].ent;
                if( monomorphise_type_needed(field_type) ) {
                    auto var_ty = monomorphise_type(sp, str.m_params, gp.m_params,  field_type);
                    this->add_binding(sp, e.sub_patterns[i], var_ty);
                }
                else {
                    // SAFE: Can't have _ as monomorphise_type_needed checks for that
                    this->add_binding(sp, e.sub_patterns[i], const_cast< ::HIR::TypeRef&>(field_type));
                }
            }
            )
        )
        ),
    (StructTupleWildcard,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            this->equate_types( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)) );
            type = this->get_type(type).clone();
        }
        assert(e.binding);
        const auto& str = *e.binding;
        // - assert check from earlier pass
        assert( str.m_data.is_Tuple() );
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Type mismatch in struct pattern - " << type << " is not " << e.path);
            ),
        (Infer, throw ""; ),
        (Path,
            if( ! te.binding.is_Struct() || te.binding.as_Struct() != &str ) {
                ERROR(sp, E0000, "Type mismatch in struct pattern - " << type << " is not " << e.path);
            }
            )
        )
        ),
    (Struct,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            this->equate_types( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)) );
            type = this->get_type(type).clone();
        }
        assert(e.binding);
        const auto& str = *e.binding;
        // - assert check from earlier pass
        assert( str.m_data.is_Named() );
        const auto& sd = str.m_data.as_Named();
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Type mismatch in struct pattern - " << type << " is not " << e.path);
            ),
        (Infer, throw ""; ),
        (Path,
            if( ! te.binding.is_Struct() || te.binding.as_Struct() != &str ) {
                ERROR(sp, E0000, "Type mismatch in struct pattern - " << type << " is not " << e.path);
            }
            // NOTE: Must be Generic for the above to have passed
            auto& gp = te.path.m_data.as_Generic();
            for( auto& field_pat : e.sub_patterns )
            {
                unsigned int f_idx = ::std::find_if( sd.begin(), sd.end(), [&](const auto& x){ return x.first == field_pat.first; } ) - sd.begin();
                if( f_idx == sd.size() ) {
                    ERROR(sp, E0000, "Struct " << e.path << " doesn't have a field " << field_pat.first);
                }
                const ::HIR::TypeRef& field_type = sd[f_idx].second.ent;
                if( monomorphise_type_needed(field_type) ) {
                    auto field_type_mono = monomorphise_type(sp, str.m_params, gp.m_params,  field_type);
                    this->add_binding(sp, field_pat.second, field_type_mono);
                }
                else {
                    // SAFE: Can't have _ as monomorphise_type_needed checks for that
                    this->add_binding(sp, field_pat.second, const_cast< ::HIR::TypeRef&>(field_type));
                }
            }
            )
        )
        ),
    (EnumTuple,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            auto path = e.path.clone();
            path.m_path.m_components.pop_back();

            this->equate_types( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)) );
            type = this->get_type(type).clone();
        }
        assert(e.binding_ptr);
        const auto& enm = *e.binding_ptr;
        const auto& var = enm.m_variants[e.binding_idx].second;
        assert(var.is_Tuple());
        const auto& tup_var = var.as_Tuple();
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
            ),
        (Infer, throw ""; ),
        (Path,
            if( ! te.binding.is_Enum() || te.binding.as_Enum() != &enm ) {
                ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
            }
            // NOTE: Must be Generic for the above to have passed
            auto& gp = te.path.m_data.as_Generic();
            if( e.sub_patterns.size() != tup_var.size() ) { 
                ERROR(sp, E0000, "Enum pattern with an incorrect number of fields - " << e.path << " - expected " << tup_var.size() << ", got " << e.sub_patterns.size());
            }
            for( unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
            {
                if( monomorphise_type_needed(tup_var[i].ent) ) {
                    auto var_ty = monomorphise_type(sp, enm.m_params, gp.m_params,  tup_var[i].ent);
                    this->add_binding(sp, e.sub_patterns[i], var_ty);
                }
                else {
                    // SAFE: Can't have a _ (monomorphise_type_needed checks for that)
                    this->add_binding(sp, e.sub_patterns[i], const_cast< ::HIR::TypeRef&>(tup_var[i].ent));
                }
            }
            )
        )
        ),
    (EnumTupleWildcard,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            auto path = e.path.clone();
            path.m_path.m_components.pop_back();

            this->equate_types( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)) );
            type = this->get_type(type).clone();
        }
        assert(e.binding_ptr);
        const auto& enm = *e.binding_ptr;
        const auto& var = enm.m_variants[e.binding_idx].second;
        assert(var.is_Tuple());
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
            ),
        (Infer, throw ""; ),
        (Path,
            if( ! te.binding.is_Enum() || te.binding.as_Enum() != &enm ) {
                ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
            }
            )
        )
        ),
    (EnumStruct,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            auto path = e.path.clone();
            path.m_path.m_components.pop_back();

            this->equate_types( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)) );
            type = this->get_type(type).clone();
        }
        assert(e.binding_ptr);
        const auto& enm = *e.binding_ptr;
        const auto& var = enm.m_variants[e.binding_idx].second;
        assert(var.is_Struct());
        const auto& tup_var = var.as_Struct();
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (type.m_data), (te),
        (
            ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
            ),
        (Infer, throw ""; ),
        (Path,
            if( ! te.binding.is_Enum() || te.binding.as_Enum() != &enm ) {
                ERROR(sp, E0000, "Type mismatch in enum pattern - " << type << " is not " << e.path);
            }
            // NOTE: Must be Generic for the above to have passed
            auto& gp = te.path.m_data.as_Generic();
            
            for( auto& field_pat : e.sub_patterns )
            {
                unsigned int f_idx = ::std::find_if( tup_var.begin(), tup_var.end(), [&](const auto& x){ return x.first == field_pat.first; } ) - tup_var.begin();
                if( f_idx == tup_var.size() ) {
                    ERROR(sp, E0000, "Enum variant " << e.path << " doesn't have a field " << field_pat.first);
                }
                const ::HIR::TypeRef& field_type = tup_var[f_idx].second.ent;
                if( monomorphise_type_needed(field_type) ) {
                    auto field_type_mono = monomorphise_type(sp, enm.m_params, gp.m_params,  field_type);
                    this->add_binding(sp, field_pat.second, field_type_mono);
                }
                else {
                    // SAFE: Can't have _ as monomorphise_type_needed checks for that
                    this->add_binding(sp, field_pat.second, const_cast< ::HIR::TypeRef&>(field_type));
                }
            }
            )
        )
        )
    )
}
void Context::equate_types_coerce(const Span& sp, const ::HIR::TypeRef& l, ::HIR::ExprNodeP& node_ptr)
{
    // - Just record the equality
    this->link_coerce.push_back(Coercion {
        l.clone(), node_ptr
        });
}
void Context::equate_types_assoc(const Span& sp, const ::HIR::TypeRef& l,  const ::HIR::SimplePath& trait, ::std::vector< ::HIR::TypeRef> ty_args, const ::HIR::TypeRef& impl_ty, const char *name)
{
    this->link_assoc.push_back(Associated {
        l.clone(),
        
        trait.clone(),
        mv$(ty_args),
        impl_ty.clone(),
        name
        });
}
void Context::add_revisit(::HIR::ExprNode& node) {
    this->to_visit.push_back( &node );
}

void Context::add_var(unsigned int index, const ::std::string& name, ::HIR::TypeRef type) {
    if( m_bindings.size() <= index )
        m_bindings.resize(index+1);
    m_bindings[index] = Binding { name, mv$(type) };
}

const ::HIR::TypeRef& Context::get_var(const Span& sp, unsigned int idx) const {
    if( idx < this->m_bindings.size() ) {
        return this->m_bindings[idx].ty;
    }
    else {
        BUG(sp, "get_var - Binding index out of range");
    }
}


template<typename T>
void fix_param_count_(const Span& sp, Context& context, const T& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params)
{
    if( params.m_types.size() == param_defs.m_types.size() ) {
        // Nothing to do, all good
        return ;
    }
    
    if( params.m_types.size() == 0 ) {
        for(const auto& typ : param_defs.m_types) {
            (void)typ;
            params.m_types.push_back( ::HIR::TypeRef() );
            context.add_ivars( params.m_types.back() );
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
void fix_param_count(const Span& sp, Context& context, const ::HIR::Path& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params) {
    fix_param_count_(sp, context, path, param_defs, params);
}
void fix_param_count(const Span& sp, Context& context, const ::HIR::GenericPath& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params) {
    fix_param_count_(sp, context, path, param_defs, params);
}


void Typecheck_Code_CS(const typeck::ModuleState& ms, t_args& args, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr)
{
    Context context { ms.m_crate, ms.m_impl_generics, ms.m_item_generics };
    
    for( auto& arg : args ) {
        context.add_binding( Span(), arg.first, arg.second );
    }

    ExprVisitor_Enum    visitor(context, ms.m_traits, result_type);
    context.add_ivars(expr->m_res_type);
    expr->visit(visitor);
    
    context.equate_types(expr->span(), result_type, expr->m_res_type);
    
    context.dump();
    while( context.take_changed() && context.has_rules() )
    {
        // TODO: Run
        TODO(Span(), "Typecheck_Code_CS");
    }
}

