/*
 */
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <hir_typeck/static.hpp>
#include "main_bindings.hpp"
#include <algorithm>

namespace {
    typedef ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >   t_args;
    // -----------------------------------------------------------------------
    // Enumeration visitor
    // 
    // Iterates the HIR expression tree and extracts type "equations"
    // -----------------------------------------------------------------------
    class ExprVisitor_Validate:
        public ::HIR::ExprVisitor
    {
        const StaticTraitResolve&  m_resolve;
        const t_args&   m_args;
        const ::HIR::TypeRef&   ret_type;
        ::std::vector< const ::HIR::TypeRef*>   closure_ret_types;
        
    public:
        ExprVisitor_Validate(const StaticTraitResolve& res, const t_args& args, const ::HIR::TypeRef& ret_type):
            m_resolve(res),
            m_args(args),
            ret_type(ret_type)
        {
        }
        
        void visit_root(::HIR::ExprNode& node)
        {
            node.visit(*this);
            check_types_equal(node.span(), ret_type, node.m_res_type);
        }
        
        void visit(::HIR::ExprNode_Block& node) override
        {
            for(auto& n : node.m_nodes)
            {
                n->visit(*this);
            }
            if( node.m_nodes.size() > 0 )
            {
                check_types_equal(node.span(), node.m_res_type, node.m_nodes.back()->m_res_type);
            }
        }
        void visit(::HIR::ExprNode_Return& node) override
        {
            // TODO: Check against return type
            const auto& ret_ty = ( this->closure_ret_types.size() > 0 ? *this->closure_ret_types.back() : this->ret_type );
            check_types_equal(ret_ty, node.m_value);
            node.m_value->visit(*this);
        }
        void visit(::HIR::ExprNode_Loop& node) override
        {
            node.m_code->visit(*this);
        }
        void visit(::HIR::ExprNode_LoopControl& node) override
        {
        }
        void visit(::HIR::ExprNode_Let& node) override
        {
            if(node.m_value)
            {
                check_types_equal(node.span(), node.m_type, node.m_value->m_res_type);
                node.m_value->visit(*this);
            }
        }
        void visit(::HIR::ExprNode_Match& node) override
        {
            node.m_value->visit(*this);
            for(auto& arm : node.m_arms)
            {
                check_types_equal(node.span(), node.m_res_type, arm.m_code->m_res_type);
                arm.m_code->visit( *this );
            }
        }
        void visit(::HIR::ExprNode_If& node) override
        {
            node.m_cond->visit( *this );
            check_types_equal(node.span(), node.m_res_type, node.m_true->m_res_type);
            if( node.m_false )
            {
                check_types_equal(node.span(), node.m_res_type, node.m_false->m_res_type);
            }
        }
        void visit(::HIR::ExprNode_Assign& node) override
        {
            if( node.m_op == ::HIR::ExprNode_Assign::Op::None ) {
                check_types_equal(node.span(), node.m_slot->m_res_type, node.m_value->m_res_type);
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
                const auto& trait_path = this->get_lang_item_path(node.span(), lang_item);
                
                check_associated_type(node.span(),  ::HIR::TypeRef(),  trait_path, ::make_vec1(node.m_value->m_res_type.clone()), node.m_slot->m_res_type,  "");
            }
            
            node.m_slot->visit( *this );
            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_BinOp& node) override
        {
            switch(node.m_op)
            {
            case ::HIR::ExprNode_BinOp::Op::CmpEqu:
            case ::HIR::ExprNode_BinOp::Op::CmpNEqu:
            case ::HIR::ExprNode_BinOp::Op::CmpLt:
            case ::HIR::ExprNode_BinOp::Op::CmpLtE:
            case ::HIR::ExprNode_BinOp::Op::CmpGt:
            case ::HIR::ExprNode_BinOp::Op::CmpGtE: {
                check_types_equal(node.span(), ::HIR::TypeRef(::HIR::CoreType::Bool), node.m_res_type);
                
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
                const auto& op_trait = this->get_lang_item_path(node.span(), item_name);
                
                check_associated_type(node.span(),  ::HIR::TypeRef(),  op_trait, ::make_vec1(node.m_right->m_res_type.clone()), node.m_left->m_res_type,  "");
                break; }
            
            case ::HIR::ExprNode_BinOp::Op::BoolAnd:
            case ::HIR::ExprNode_BinOp::Op::BoolOr:
                // No validation needed, result forced in typeck
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
                const auto& op_trait = this->get_lang_item_path(node.span(), item_name);
                
                check_associated_type(node.span(),  node.m_res_type,  op_trait, ::make_vec1(node.m_right->m_res_type.clone()), node.m_left->m_res_type,  "Output");
                break; }
            }
            
            node.m_left ->visit( *this );
            node.m_right->visit( *this );
        }
        
        void visit(::HIR::ExprNode_UniOp& node) override
        {
            switch(node.m_op)
            {
            case ::HIR::ExprNode_UniOp::Op::Ref:
                check_types_equal(node.span(), node.m_res_type, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, node.m_value->m_res_type.clone()));
                break;
            case ::HIR::ExprNode_UniOp::Op::RefMut:
                check_types_equal(node.span(), node.m_res_type, ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, node.m_value->m_res_type.clone()));
                break;
            case ::HIR::ExprNode_UniOp::Op::Invert:
                check_associated_type(node.span(), node.m_res_type,  this->get_lang_item_path(node.span(), "not"), {}, node.m_value->m_res_type, "Output");
                break;
            case ::HIR::ExprNode_UniOp::Op::Negate:
                check_associated_type(node.span(), node.m_res_type,  this->get_lang_item_path(node.span(), "neg"), {}, node.m_value->m_res_type, "Output");
                break;
            }
            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_Index& node) override
        {
            check_associated_type(node.span(),
                node.m_res_type,
                this->get_lang_item_path(node.span(), "index"), ::make_vec1(node.m_index->m_res_type.clone()), node.m_value->m_res_type, "Target"
                );
            
            node.m_value->visit( *this );
            node.m_index->visit( *this );
        }
        
        void visit(::HIR::ExprNode_Cast& node) override
        {
            // TODO: Check castability
            
            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_Unsize& node) override
        {
            const Span& sp = node.span();
            
            // TODO: Check unsizability
            TU_MATCH_DEF(::HIR::TypeRef::Data, (node.m_res_type.m_data), (e),
            (
                ERROR(sp, E0000, "Invalid unsizing operation");
                ),
            (TraitObject,
                ),
            (Slice,
                )
            )
            
            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_Deref& node) override
        {
            check_associated_type(node.span(),
                node.m_res_type,
                this->get_lang_item_path(node.span(), "deref"), {}, node.m_value->m_res_type, "Target"
                );

            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_TupleVariant& node) override
        {
            const auto& sp = node.span();
            
            // - Create ivars in path, and set result type
            const auto& ty = node.m_res_type;
            
            const ::HIR::t_tuple_fields* fields_ptr = nullptr;
            ASSERT_BUG(sp, ty.m_data.is_Path(), "Result type of _TupleVariant isn't Path");
            TU_MATCH(::HIR::TypeRef::TypePathBinding, (ty.m_data.as_Path().binding), (e),
            (Unbound, ),
            (Opaque, ),
            (Enum,
                const auto& var_name = node.m_path.m_path.m_components.back();
                const auto& enm = *e;
                auto it = ::std::find_if(enm.m_variants.begin(), enm.m_variants.end(), [&](const auto&v)->auto{ return v.first == var_name; });
                assert(it != enm.m_variants.end());
                ASSERT_BUG(sp, it->second.is_Tuple(), "Pointed variant of TupleVariant (" << node.m_path << ") isn't a Tuple");
                fields_ptr = &it->second.as_Tuple();
                ),
            (Struct,
                ASSERT_BUG(sp, e->m_data.is_Tuple(), "Pointed struct in TupleVariant (" << node.m_path << ") isn't a Tuple");
                fields_ptr = &e->m_data.as_Tuple();
                )
            )
            assert(fields_ptr);
            const ::HIR::t_tuple_fields& fields = *fields_ptr;
            ASSERT_BUG(sp, fields.size() == node.m_args.size(), "");
            
            // Bind fields with type params (coercable)
            for( unsigned int i = 0; i < node.m_args.size(); i ++ )
            {
                const auto& des_ty_r = fields[i].ent;
                const auto* des_ty = &des_ty_r;
                if( monomorphise_type_needed(des_ty_r) ) {
                    assert( node.m_arg_types[i] != ::HIR::TypeRef() );
                    des_ty = &node.m_arg_types[i];
                }
                
                check_types_equal(*des_ty, node.m_args[i]);
            }
            
            for( auto& val : node.m_args ) {
                val->visit( *this );
            }
        }
        void visit(::HIR::ExprNode_StructLiteral& node) override
        {
            const auto& sp = node.span();
            if( node.m_base_value) {
                check_types_equal( node.m_base_value->span(), node.m_res_type, node.m_base_value->m_res_type );
            }
            
            // - Create ivars in path, and set result type
            const auto& ty = node.m_res_type;
            ASSERT_BUG(sp, ty.m_data.is_Path(), "Result type of _StructLiteral isn't Path");
            
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
                auto& des_ty_cache = node.m_value_types[it - fields.begin()];
                const auto* des_ty = &des_ty_r;
                
                DEBUG(name << " : " << des_ty_r);
                if( monomorphise_type_needed(des_ty_r) ) {
                    assert( des_ty_cache != ::HIR::TypeRef() );
                    des_ty = &des_ty_cache;
                }
                check_types_equal(*des_ty, val.second);
            }
            
            for( auto& val : node.m_values ) {
                val.second->visit( *this );
            }
            if( node.m_base_value ) {
                node.m_base_value->visit( *this );
            }
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override
        {
            const auto& sp = node.span();
            const auto& ty = node.m_res_type;
            ASSERT_BUG(sp, ty.m_data.is_Path(), "Result type of _StructLiteral isn't Path");
            
            TU_MATCH(::HIR::TypeRef::TypePathBinding, (ty.m_data.as_Path().binding), (e),
            (Unbound, ),
            (Opaque, ),
            (Enum,
                const auto& var_name = node.m_path.m_path.m_components.back();
                const auto& enm = *e;
                auto it = ::std::find_if(enm.m_variants.begin(), enm.m_variants.end(), [&](const auto&v)->auto{ return v.first == var_name; });
                assert(it != enm.m_variants.end());
                assert( it->second.is_Unit() || it->second.is_Value() );
                ),
            (Struct,
                assert( e->m_data.is_Unit() );
                )
            )
        }

        void visit(::HIR::ExprNode_CallPath& node) override
        {
            // Link arguments
            for(unsigned int i = 0; i < node.m_args.size(); i ++)
            {
                check_types_equal(node.span(), node.m_cache.m_arg_types[i], node.m_args[i]->m_res_type);
            }
            check_types_equal(node.span(), node.m_res_type, node.m_cache.m_arg_types.back());

            for( auto& val : node.m_args ) {
                val->visit( *this );
            }
        }
        void visit(::HIR::ExprNode_CallValue& node) override
        {
            ASSERT_BUG(node.span(), node.m_arg_types.size() > 0, "CallValue cache not populated");
            for(unsigned int i = 0; i < node.m_args.size(); i ++)
            {
                check_types_equal(node.span(), node.m_arg_types[i], node.m_args[i]->m_res_type);
            }
            check_types_equal(node.span(), node.m_res_type, node.m_arg_types.back());
            
            // Don't bother checking for a FnOnce impl, if the cache is populated it was found
            
            node.m_value->visit( *this );
            for( auto& val : node.m_args ) {
                val->visit( *this );
            }
        }
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            ASSERT_BUG(node.span(), node.m_cache.m_arg_types.size() > 0, "CallMethod cache not populated");
            ASSERT_BUG(node.span(), node.m_cache.m_arg_types.size() == 1 + node.m_args.size() + 1, "CallMethod cache mis-sized");
            check_types_equal(node.m_cache.m_arg_types[0], node.m_value);
            for(unsigned int i = 0; i < node.m_args.size(); i ++)
            {
                check_types_equal(node.m_cache.m_arg_types[1+i], node.m_args[i]);
            }
            check_types_equal(node.span(), node.m_res_type, node.m_cache.m_arg_types.back());
            
            node.m_value->visit( *this );
            for( auto& val : node.m_args ) {
                val->visit( *this );
            }
        }
        
        void visit(::HIR::ExprNode_Field& node) override
        {
            const auto& sp = node.span();
            
            const auto& str_ty = node.m_value->m_res_type;
            ASSERT_BUG(sp, str_ty.m_data.is_Path(), "Value type of _Field isn't Path");
            const auto& ty_e = str_ty.m_data.as_Path();
            ASSERT_BUG(sp, ty_e.binding.is_Struct(), "Value type of _Field isn't a Struct");
            //const auto& str = *ty_e.binding.as_Struct();
            
            // TODO: Triple-check result, but that probably isn't needed
            
            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_Tuple& node) override
        {
            TRACE_FUNCTION_F(&node << " (...,)");
            const auto& tys = node.m_res_type.m_data.as_Tuple();
            
            ASSERT_BUG(node.span(), tys.size() == node.m_vals.size(), "Bad element count in tuple literal");
            for(unsigned int i = 0; i < node.m_vals.size(); i ++)
            {
                check_types_equal(node.span(), tys[i], node.m_vals[i]->m_res_type);
            }
            
            for( auto& val : node.m_vals ) {
                val->visit( *this );
            }
        }
        void visit(::HIR::ExprNode_ArrayList& node) override
        {
            // Cleanly equate into array (with coercions)
            const auto& inner_ty = *node.m_res_type.m_data.as_Array().inner;
            for( auto& val : node.m_vals ) {
                check_types_equal(val->span(), inner_ty, val->m_res_type);
            }
            
            for( auto& val : node.m_vals ) {
                val->visit( *this );
            }
        }
        void visit(::HIR::ExprNode_ArraySized& node) override
        {
            //check_types_equal(node.m_size->span(), ::HIR::TypeRef(::HIR::Primitive::Usize), node.m_size->m_res_type);
            const auto& inner_ty = *node.m_res_type.m_data.as_Array().inner;
            check_types_equal(node.m_val->span(), inner_ty, node.m_val->m_res_type);
            
            node.m_val->visit( *this );
            node.m_size->visit( *this );
        }
        
        void visit(::HIR::ExprNode_Literal& node) override
        {
            // No validation needed
        }
        void visit(::HIR::ExprNode_PathValue& node) override
        {
            const auto& sp = node.span();
            
            TU_MATCH(::HIR::Path::Data, (node.m_path.m_data), (e),
            (Generic,
                switch(node.m_target)
                {
                case ::HIR::ExprNode_PathValue::UNKNOWN:
                    BUG(sp, "Unknown target PathValue encountered with Generic path");
                case ::HIR::ExprNode_PathValue::FUNCTION:
                    // TODO: Is validate needed?
                    assert( node.m_res_type.m_data.is_Function() );
                    break;
                case ::HIR::ExprNode_PathValue::STRUCT_CONSTR: {
                    } break;
                case ::HIR::ExprNode_PathValue::STATIC: {
                    } break;
                case ::HIR::ExprNode_PathValue::CONSTANT: {
                    } break;
                }
                ),
            (UfcsUnknown,
                BUG(sp, "Encountered UfcsUnknown");
                ),
            (UfcsKnown,
                check_associated_type(sp, ::HIR::TypeRef(),  e.trait.m_path, mv$(e.trait.m_params.clone().m_types), e.type->clone(), "");
                
                const auto& trait = this->m_resolve.m_crate.get_trait_by_path(sp, e.trait.m_path);
                auto it = trait.m_values.find( e.item );
                if( it == trait.m_values.end() || it->second.is_None() ) {
                    ERROR(sp, E0000, "`" << e.item << "` is not a value member of trait " << e.trait.m_path);
                }
                TU_MATCH( ::HIR::TraitValueItem, (it->second), (ie),
                (None, throw ""; ),
                (Constant,
                    TODO(sp, "Monomorpise associated constant type - " << ie.m_type);
                    ),
                (Static,
                    TODO(sp, "Monomorpise associated static type - " << ie.m_type);
                    ),
                (Function,
                    assert( node.m_res_type.m_data.is_Function() );
                    )
                )
                ),
            (UfcsInherent,
                )
            )
        }
        
        void visit(::HIR::ExprNode_Variable& node) override
        {
            // TODO: Check against variable slot? Nah.
        }
        
        void visit(::HIR::ExprNode_Closure& node) override
        {
            check_types_equal(node.m_code->span(), node.m_return, node.m_code->m_res_type);
            
            this->closure_ret_types.push_back( &node.m_return );
            node.m_code->visit( *this );
            this->closure_ret_types.pop_back( );
        }
        
    private:
        void check_types_equal(const ::HIR::TypeRef& l, const ::HIR::ExprNodeP& node) const
        {
            check_types_equal(node->span(), l, node->m_res_type);
        }
        void check_types_equal(const Span& sp, const ::HIR::TypeRef& l, const ::HIR::TypeRef& r) const
        {
            if( l != r ) {
                ERROR(sp, E0000, "Type mismatch - " << l << " != " << r);
            }
        }
        void check_associated_type(const Span& sp,
                const ::HIR::TypeRef& res,
                const ::HIR::SimplePath& trait, const ::std::vector< ::HIR::TypeRef>& params, const ::HIR::TypeRef& ity, const char* name
            ) const
        {
        }
        
        const ::HIR::SimplePath& get_lang_item_path(const Span& sp, const char* name) const
        {
            return m_resolve.m_crate.get_lang_item_path(sp, name);
        }
    };
    
    
    class OuterVisitor:
        public ::HIR::Visitor
    {
        StaticTraitResolve  m_resolve;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_resolve(crate)
        {}
        
        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            ::HIR::Visitor::visit_module(p, mod);
        }
        
        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) {
            TODO(Span(), "visit_expr");
        }
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                this->visit_type( *e.inner );
                DEBUG("Array size " << ty);
                t_args  tmp;
                if( e.size ) {
                    ExprVisitor_Validate    ev(m_resolve, {}, ::HIR::TypeRef(::HIR::CoreType::Usize));
                    ev.visit_root( *e.size );
                }
            )
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }
        // ------
        // Code-containing items
        // ------
        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override {
            //auto _ = this->m_ms.set_item_generics(item.m_params);
            if( item.m_code )
            {
                DEBUG("Function code " << p);
                ExprVisitor_Validate    ev(m_resolve, item.m_args, item.m_return);
                ev.visit_root( *item.m_code );
            }
            else
            {
                DEBUG("Function code " << p << " (none)");
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override {
            if( item.m_value )
            {
                t_args  tmp;
                ExprVisitor_Validate    ev(m_resolve, tmp, item.m_type);
                ev.visit_root(*item.m_value);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value )
            {
                t_args  tmp;
                ExprVisitor_Validate    ev(m_resolve, tmp, item.m_type);
                ev.visit_root(*item.m_value);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            //auto _ = this->m_ms.set_item_generics(item.m_params);
            // TODO: Use a different type depding on repr()
            auto enum_type = ::HIR::TypeRef(::HIR::CoreType::Isize);
            
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    DEBUG("Enum value " << p << " - " << var.first);
                    
                    t_args  tmp;
                    ExprVisitor_Validate    ev(m_resolve, tmp, enum_type);
                    ev.visit_root(*e);
                )
            }
        }
    };
}

void Typecheck_Expressions_Validate(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

