/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/ufcs_everything.cpp
 * - Expand all function calls (_CallMethod, and _CallValue) and operator overloads to _CallPath
 * - Also handles borrow-unsize-deref for _Unsize on arrays (see comment in _Unsize)
 */
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <hir_typeck/static.hpp>
#include <algorithm>
#include "main_bindings.hpp"

namespace {
    inline HIR::ExprNodeP mk_exprnodep(HIR::ExprNode* en, ::HIR::TypeRef ty){ en->m_res_type = mv$(ty); return HIR::ExprNodeP(en); }
}
#define NEWNODE(TY, CLASS, ...)  mk_exprnodep(new HIR::ExprNode_##CLASS(__VA_ARGS__), TY)

namespace {

    class ExprVisitor_Mutate:
        public ::HIR::ExprVisitorDef
    {
        const ::HIR::Crate& m_crate;
        ::HIR::ExprNodeP    m_replacement;
        ::HIR::SimplePath   m_lang_Box;

    public:
        ExprVisitor_Mutate(const ::HIR::Crate& crate):
            m_crate(crate)
        {
            if( crate.m_lang_items.count("owned_box") > 0 ) {
                m_lang_Box = crate.m_lang_items.at("owned_box");
            }
        }
        void visit_node_ptr(::HIR::ExprPtr& root) {
            const auto& node_ref = *root;
            const char* node_ty = typeid(node_ref).name();
            TRACE_FUNCTION_FR(&*root << " " << node_ty << " : " << root->m_res_type, node_ty);
            root->visit(*this);
            if( m_replacement ) {
                auto usage = root->m_usage;
                const auto* ptr = m_replacement.get();
                DEBUG("=> REPLACE " << ptr << " " << typeid(*ptr).name());
                root.reset( m_replacement.release() );
                root->m_usage = usage;
            }
        }

        void visit_node_ptr(::HIR::ExprNodeP& node) override {
            const auto& node_ref = *node;
            const char* node_ty = typeid(node_ref).name();
            TRACE_FUNCTION_FR(&*node << " " << node_ty << " : " << node->m_res_type, node_ty);
            assert( node );
            node->visit(*this);
            if( m_replacement ) {
                auto usage = node->m_usage;
                const auto* ptr = m_replacement.get();
                DEBUG("=> REPLACE " << ptr << " " << typeid(*ptr).name());
                node = mv$(m_replacement);
                node->m_usage = usage;
            }
        }

        // ----------
        // _CallValue
        // ----------
        // Replace with a UFCS call using the now-known type
        void visit(::HIR::ExprNode_CallValue& node) override
        {
            const auto& sp = node.span();

            ::HIR::ExprVisitorDef::visit(node);
            const auto& ty_val = node.m_value->m_res_type;

            // Calling a `fn` type should be kept as a _CallValue
            if( ty_val.data().is_Function() ) {
                return ;
            }

            // 1. Construct tuple type containing argument types for `Args`
            ::HIR::TypeRef  arg_tup_type;
            {
                ::std::vector< ::HIR::TypeRef>  arg_types;
                for(unsigned int i = 0; i < node.m_args.size(); i ++)
                    arg_types.push_back( node.m_args[i]->m_res_type.clone() );
                arg_tup_type = ::HIR::TypeRef::new_tuple( mv$(arg_types) );
            }
            // - Make the trait arguments.
            ::HIR::PathParams   trait_args;
            trait_args.m_types.push_back( arg_tup_type.clone() );

            // - If the called value is a local closure, figure out how it's being used.
            // TODO: You can call via &-ptrs, but that currently isn't handled in typeck
            if(const auto* e = node.m_value->m_res_type.data().opt_Closure() )
            {
                if( node.m_trait_used == ::HIR::ExprNode_CallValue::TraitUsed::Unknown )
                {
                    // NOTE: Closure node still exists, and will do until MIR construction deletes the HIR
                    switch(e->node->m_class)
                    {
                    case ::HIR::ExprNode_Closure::Class::Unknown:
                        BUG(sp, "References an ::Unknown closure");
                    case ::HIR::ExprNode_Closure::Class::NoCapture:
                    case ::HIR::ExprNode_Closure::Class::Shared:
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Fn;
                        break;
                    case ::HIR::ExprNode_Closure::Class::Mut:
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::FnMut;
                        break;
                    case ::HIR::ExprNode_Closure::Class::Once:
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::FnOnce;
                        break;
                    }
                }
            }

            // Use marking in node to determine trait to use
            ::HIR::TypeRef self_arg_type;
            ::HIR::Path   method_path(::HIR::SimplePath{});
            switch(node.m_trait_used)
            {
            case ::HIR::ExprNode_CallValue::TraitUsed::Fn:
                // Insert a borrow op.
                self_arg_type = ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ty_val.clone() );
                node.m_value = NEWNODE(self_arg_type.clone(), Borrow, sp,  ::HIR::BorrowType::Shared, mv$(node.m_value));
                method_path = ::HIR::Path(
                    ty_val.clone(),
                    ::HIR::GenericPath( m_crate.get_lang_item_path(sp, "fn"), mv$(trait_args) ),
                    "call"
                    );
                break;
            case ::HIR::ExprNode_CallValue::TraitUsed::FnMut:
                self_arg_type = ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Unique, ty_val.clone() );
                node.m_value = NEWNODE(self_arg_type.clone(), Borrow, sp,  ::HIR::BorrowType::Unique, mv$(node.m_value));
                method_path = ::HIR::Path(
                    ty_val.clone(),
                    ::HIR::GenericPath( m_crate.get_lang_item_path(sp, "fn_mut"), mv$(trait_args) ),
                    "call_mut"
                    );
                break;
            case ::HIR::ExprNode_CallValue::TraitUsed::FnOnce:
                self_arg_type = ty_val.clone();
                method_path = ::HIR::Path(
                    ty_val.clone(),
                    ::HIR::GenericPath( m_crate.get_lang_item_path(sp, "fn_once"), mv$(trait_args) ),
                    "call_once"
                    );
                break;

            //case ::HIR::ExprNode_CallValue::TraitUsed::Unknown:
            default:
                BUG(node.span(), "Encountered CallValue with TraitUsed::Unknown, ty=" << node.m_value->m_res_type);
            }
            assert(self_arg_type != ::HIR::TypeRef());


            // Construct argument list for the output
            ::std::vector< ::HIR::ExprNodeP>    args;
            args.reserve( 2 );
            args.push_back( mv$(node.m_value) );
            args.push_back(NEWNODE( arg_tup_type.clone(), Tuple, sp,  mv$(node.m_args) ));

            m_replacement = NEWNODE(mv$(node.m_res_type), CallPath, sp,
                mv$(method_path),
                mv$(args)
                );

            // Populate the cache for later passes
            auto& arg_types = dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement).m_cache.m_arg_types;
            arg_types.push_back( mv$(self_arg_type) );
            arg_types.push_back( mv$(arg_tup_type) );
            arg_types.push_back( m_replacement->m_res_type.clone() );
        }

        // ----------
        // _CallMethod
        // ----------
        // Simple replacement
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            const auto& sp = node.span();

            ::HIR::ExprVisitorDef::visit(node);

            ::std::vector< ::HIR::ExprNodeP>    args;
            args.reserve( 1 + node.m_args.size() );
            args.push_back( mv$(node.m_value) );
            for(auto& arg : node.m_args)
                args.push_back( mv$(arg) );

            // Replace using known function path
            m_replacement = NEWNODE(mv$(node.m_res_type), CallPath, sp,
                mv$(node.m_method_path),
                mv$(args)
                );
            // Populate the cache for later passes
            dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement).m_cache = mv$(node.m_cache);
        }


        static bool is_op_valid_shift(const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& ty_r)
        {
            // Integer with any other integer is valid, others go to overload resolution
            if( ty_l.data().is_Primitive() && ty_r.data().is_Primitive() ) {
                switch(ty_l.data().as_Primitive())
                {
                case ::HIR::CoreType::Char:
                case ::HIR::CoreType::Str:
                case ::HIR::CoreType::Bool:
                case ::HIR::CoreType::F32:
                case ::HIR::CoreType::F64:
                    break;
                default:
                    switch(ty_r.data().as_Primitive())
                    {
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Bool:
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        break;
                    default:
                        // RETURN early
                        return true;
                    }
                    break;
                }

            }
            return false;
        }
        static bool is_op_valid_bitmask(const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& ty_r)
        {
            // Equal integers and bool are valid
            if( ty_l == ty_r ) {
                if(const auto* e = ty_l.data().opt_Primitive())
                {
                    switch(*e)
                    {
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Str:
                        break;
                    default:
                        // RETURN early
                        return true;
                    }
                }
            }
            return false;
        }
        static bool is_op_valid_arith(const ::HIR::TypeRef& ty_l, const ::HIR::TypeRef& ty_r)
        {
            // Equal floats/integers are valid, others go to overload
            if( ty_l == ty_r ) {
                if(const auto* e = ty_l.data().opt_Primitive())
                {
                    switch(*e)
                    {
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Bool:
                        break;
                    default:
                        // RETURN early
                        return true;
                    }
                }
            }
            return false;
        }

        // -------
        // _Assign
        // -------
        // Replace with overload call if not a builtin supported operation
        void visit(::HIR::ExprNode_Assign& node) override
        {
            const auto& sp = node.span();
            ::HIR::ExprVisitorDef::visit(node);

            const auto& ty_slot = node.m_slot->m_res_type;
            const auto& ty_val  = node.m_value->m_res_type;

            const char* langitem = nullptr;
            const char* opname = nullptr;
            #define _(opname)   case ::HIR::ExprNode_Assign::Op::opname
            switch( node.m_op )
            {
            _(None):
                ASSERT_BUG(sp, ty_slot == ty_val, "Types must equal for non-operator assignment, " << ty_slot << " != " << ty_val);
                return ;
            _(Shr): {langitem = "shr_assign"; opname = "shr_assign"; } if(0)
            _(Shl): {langitem = "shl_assign"; opname = "shl_assign"; }
                if( is_op_valid_shift(ty_slot, ty_val) ) {
                    return ;
                }
                break;

            _(And): {langitem = "bitand_assign"; opname = "bitand_assign"; } if(0)
            _(Or ): {langitem = "bitor_assign" ; opname = "bitor_assign" ; } if(0)
            _(Xor): {langitem = "bitxor_assign"; opname = "bitxor_assign"; }
                if( is_op_valid_bitmask(ty_slot, ty_val) ) {
                    return ;
                }
                break;

            _(Add): {langitem = "add_assign"; opname = "add_assign"; } if(0)
            _(Sub): {langitem = "sub_assign"; opname = "sub_assign"; } if(0)
            _(Mul): {langitem = "mul_assign"; opname = "mul_assign"; } if(0)
            _(Div): {langitem = "div_assign"; opname = "div_assign"; } if(0)
            _(Mod): {langitem = "rem_assign"; opname = "rem_assign"; }
                if( is_op_valid_arith(ty_slot, ty_val) ) {
                    return ;
                }
                // - Fall down to overload replacement
                break;
            }
            #undef _
            assert( langitem );
            assert( opname );

            // Needs replacement, continue
            ::HIR::PathParams   trait_params;
            trait_params.m_types.push_back( ty_val.clone() );
            ::HIR::GenericPath  trait { m_crate.get_lang_item_path(node.span(), langitem), mv$(trait_params) };

            auto slot_type_refmut = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, ty_slot.clone());
            ::std::vector< ::HIR::ExprNodeP>    args;
            args.push_back(NEWNODE( slot_type_refmut.clone(), Borrow, sp,  ::HIR::BorrowType::Unique, mv$(node.m_slot) ));
            args.push_back( mv$(node.m_value) );
            m_replacement = NEWNODE(mv$(node.m_res_type), CallPath, sp,
                ::HIR::Path(ty_slot.clone(), mv$(trait), opname),
                mv$(args)
                );

            // Populate the cache for later passes
            auto& arg_types = dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement).m_cache.m_arg_types;
            arg_types.push_back( mv$(slot_type_refmut) );
            arg_types.push_back( ty_val.clone() );
            arg_types.push_back( ::HIR::TypeRef::new_unit() );
        }

        void visit(::HIR::ExprNode_BinOp& node) override
        {
            const auto& sp = node.span();
            ::HIR::ExprVisitorDef::visit(node);

            const auto& ty_l = node.m_left->m_res_type;
            const auto& ty_r  = node.m_right->m_res_type;

            const char* langitem = nullptr;
            const char* method = nullptr;
            switch(node.m_op)
            {
            case ::HIR::ExprNode_BinOp::Op::CmpEqu: { langitem = "eq"; method = "eq"; } if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpNEqu:{ langitem = "eq"; method = "ne"; } if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpLt:  { langitem = TARGETVER_LEAST_1_29 ? "partial_ord" : "ord"; method = "lt"; } if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpLtE: { langitem = TARGETVER_LEAST_1_29 ? "partial_ord" : "ord"; method = "le"; } if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpGt:  { langitem = TARGETVER_LEAST_1_29 ? "partial_ord" : "ord"; method = "gt"; } if(0)
            case ::HIR::ExprNode_BinOp::Op::CmpGtE: { langitem = TARGETVER_LEAST_1_29 ? "partial_ord" : "ord"; method = "ge"; }
                {
                // 1. Check if the types are valid for primitive comparison
                if( ty_l == ty_r ) {
                    TU_MATCH_DEF(::HIR::TypeData, (ty_l.data()), (e),
                    (
                        // Unknown - Overload
                        ),
                    (Pointer,
                        // Raw pointer, valid.
                        return ;
                        ),
                    // TODO: Should comparing &str be handled by the overload, or MIR?
                    (Primitive,
                        if( e != ::HIR::CoreType::Str ) {
                            return ;
                        }
                        )
                    )
                }
                // 2. If not, emit a call with params borrowed
                ::HIR::PathParams   trait_params;
                trait_params.m_types.push_back( ty_r.clone() );
                ::HIR::GenericPath  trait { m_crate.get_lang_item_path(node.span(), langitem), mv$(trait_params) };

                auto ty_l_ref = ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ty_l.clone() );
                auto ty_r_ref = ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ty_r.clone() );

                ::std::vector< ::HIR::ExprNodeP>    args;
                auto sp_left  = node.m_left ->span();
                auto sp_right = node.m_right->span();
                args.push_back(NEWNODE(ty_l_ref.clone(), Borrow, sp_left ,  ::HIR::BorrowType::Shared, mv$(node.m_left ) ));
                args.push_back(NEWNODE(ty_r_ref.clone(), Borrow, sp_right,  ::HIR::BorrowType::Shared, mv$(node.m_right) ));

                m_replacement = NEWNODE(mv$(node.m_res_type), CallPath, sp,
                    ::HIR::Path(ty_l.clone(), mv$(trait), method),
                    mv$(args)
                    );

                // Populate the cache for later passes
                auto& arg_types = dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement).m_cache.m_arg_types;
                arg_types.push_back( mv$(ty_l_ref) );
                arg_types.push_back( mv$(ty_r_ref) );
                arg_types.push_back( ::HIR::TypeRef( ::HIR::CoreType::Bool ) );
                return ;
                } break;

            case ::HIR::ExprNode_BinOp::Op::Xor: langitem = method = "bitxor"; if(0)
            case ::HIR::ExprNode_BinOp::Op::Or : langitem = method = "bitor" ; if(0)
            case ::HIR::ExprNode_BinOp::Op::And: langitem = method = "bitand";
                if( is_op_valid_bitmask(ty_l, ty_r) ) {
                    return ;
                }
                break;

            case ::HIR::ExprNode_BinOp::Op::Shr: langitem = method = "shr"; if(0)
            case ::HIR::ExprNode_BinOp::Op::Shl: langitem = method = "shl";
                if( is_op_valid_shift(ty_l, ty_r) ) {
                    return ;
                }
                break;

            case ::HIR::ExprNode_BinOp::Op::Add: langitem = method = "add"; if(0)
            case ::HIR::ExprNode_BinOp::Op::Sub: langitem = method = "sub"; if(0)
            case ::HIR::ExprNode_BinOp::Op::Mul: langitem = method = "mul"; if(0)
            case ::HIR::ExprNode_BinOp::Op::Div: langitem = method = "div"; if(0)
            case ::HIR::ExprNode_BinOp::Op::Mod: langitem = method = "rem";
                if( is_op_valid_arith(ty_l, ty_r) ) {
                    return ;
                }
                break;

            case ::HIR::ExprNode_BinOp::Op::BoolAnd:
            case ::HIR::ExprNode_BinOp::Op::BoolOr:
                ASSERT_BUG(sp, ty_l == ::HIR::TypeRef(::HIR::CoreType::Bool), "&& operator requires bool");
                ASSERT_BUG(sp, ty_r == ::HIR::TypeRef(::HIR::CoreType::Bool), "&& operator requires bool");
                return ;
            }
            assert(langitem);
            assert(method);

            // Needs replacement, continue
            ::HIR::PathParams   trait_params;
            trait_params.m_types.push_back( ty_r.clone() );
            ::HIR::GenericPath  trait { m_crate.get_lang_item_path(node.span(), langitem), mv$(trait_params) };

            ::std::vector< ::HIR::ExprNodeP>    args;
            args.push_back( mv$(node.m_left) );
            args.push_back( mv$(node.m_right) );

            m_replacement = NEWNODE(mv$(node.m_res_type), CallPath, sp,
                ::HIR::Path(ty_l.clone(), mv$(trait), method),
                mv$(args)
                );

            // Populate the cache for later passes
            auto& arg_types = dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement).m_cache.m_arg_types;
            arg_types.push_back( ty_l.clone() );
            arg_types.push_back( ty_r.clone() );
            arg_types.push_back( m_replacement->m_res_type.clone() );
        }

        void visit(::HIR::ExprNode_UniOp& node) override
        {
            const auto& sp = node.span();
            ::HIR::ExprVisitorDef::visit(node);

            const auto& ty_val = node.m_value->m_res_type;

            const char* langitem = nullptr;
            const char* method = nullptr;
            switch(node.m_op)
            {
            case ::HIR::ExprNode_UniOp::Op::Invert:
                // Check if the operation is valid in the MIR.
                if( ty_val.data().is_Primitive() ) {
                    switch( ty_val.data().as_Primitive() )
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        break;
                    default:
                        return;
                    }
                }
                else {
                    // Not valid, replace with call
                }
                langitem = method = "not";
                break;
            case ::HIR::ExprNode_UniOp::Op::Negate:
                if( ty_val.data().is_Primitive() ) {
                    switch( ty_val.data().as_Primitive() )
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                    case ::HIR::CoreType::Bool:
                        break;
                    case ::HIR::CoreType::U8:
                    case ::HIR::CoreType::U16:
                    case ::HIR::CoreType::U32:
                    case ::HIR::CoreType::U64:
                    case ::HIR::CoreType::Usize:
                        ERROR(node.span(), E0000, "`-` operator on unsigned integer - " << ty_val);
                        break;
                    default:
                        // Valid, keep.
                        return;
                    }
                }
                else {
                    // Replace with call
                }
                langitem = method = "neg";
                break;
            }
            assert(langitem);
            assert(method);

            // Needs replacement, continue
            ::HIR::GenericPath  trait { m_crate.get_lang_item_path(node.span(), langitem), {} };

            ::std::vector< ::HIR::ExprNodeP>    args;
            args.push_back( mv$(node.m_value) );

            m_replacement = NEWNODE(mv$(node.m_res_type), CallPath, sp,
                ::HIR::Path(ty_val.clone(), mv$(trait), method),
                mv$(args)
                );

            // Populate the cache for later passes
            auto& arg_types = dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement).m_cache.m_arg_types;
            arg_types.push_back( ty_val.clone() );
            arg_types.push_back( m_replacement->m_res_type.clone() );
        }


        void visit(::HIR::ExprNode_Index& node) override
        {
            const auto& sp = node.span();
            ::HIR::ExprVisitorDef::visit(node);

            const auto& ty_idx = node.m_index->m_res_type;
            const auto& ty_val = node.m_value->m_res_type;

            TU_MATCH_DEF( ::HIR::TypeData, (ty_val.data()), (val_te),
            (
                // Unknown? fall down to the method call
                ),
            (Slice,
                if( ty_idx == ::HIR::CoreType::Usize ) {
                    // Slices can be trivially indexed using usize
                    return ;
                }
                // Any other index type goes to the function call
                ),
            (Array,
                if( ty_idx == ::HIR::CoreType::Usize ) {
                    // Arrays also can be trivially indexed using usize
                    return ;
                }
                // Any other index type goes to the function call
                )
            )

            // TODO: Which trait should be used?
            const char* langitem = nullptr;
            const char* method = nullptr;
            ::HIR::BorrowType   bt;
            switch( node.m_value->m_usage )
            {
            case ::HIR::ValueUsage::Unknown:
                BUG(sp, "Usage of value in index op is unknown");
                break;
            case ::HIR::ValueUsage::Borrow:
                bt = ::HIR::BorrowType::Shared;
                langitem = method = "index";
                break;
            case ::HIR::ValueUsage::Mutate:
                bt = ::HIR::BorrowType::Unique;
                langitem = method = "index_mut";
                break;
            case ::HIR::ValueUsage::Move:
                TODO(sp, "Support moving out of indexed values");
                break;
            }
            // Needs replacement, continue
            assert(langitem);
            assert(method);

            // - Construct trait path - Index*<IdxTy>
            ::HIR::PathParams   pp;
            pp.m_types.push_back( ty_idx.clone() );
            ::HIR::GenericPath  trait { m_crate.get_lang_item_path(node.span(), langitem), mv$(pp) };

            ::std::vector< ::HIR::ExprNodeP>    args;
            args.push_back( NEWNODE( ::HIR::TypeRef::new_borrow(bt, ty_val.clone()), Borrow, sp, bt, mv$(node.m_value) ) );
            args.push_back( mv$(node.m_index) );

            m_replacement = NEWNODE( ::HIR::TypeRef::new_borrow(bt, node.m_res_type.clone()), CallPath, sp,
                ::HIR::Path(ty_val.clone(), mv$(trait), method),
                mv$(args)
                );
            // Populate the cache for later passes
            // TODO: The check pass should probably just ignore this and DIY
            auto& call_node = dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement);
            auto& arg_types = call_node.m_cache.m_arg_types;
            arg_types.push_back( ::HIR::TypeRef::new_borrow(bt, ty_val.clone()) );
            arg_types.push_back( ty_idx.clone() );
            arg_types.push_back( m_replacement->m_res_type.clone() );

            // - Dereference the result (which is an &-ptr)
            m_replacement = NEWNODE( mv$(node.m_res_type), Deref, sp,  mv$(m_replacement) );
        }

#if 0
        void visit(::HIR::ExprNode_Deref& node) override
        {
            const auto& sp = node.span();

            ::HIR::ExprVisitorDef::visit(node);

            const auto& ty_val = node.m_value->m_res_type;

            TU_MATCH_DEF( ::HIR::TypeData, (ty_val.m_data), (e),
            (
                BUG(sp, "Deref on unexpected type - " << ty_val);
                ),
            (Generic,
                ),
            (Path,
                // Box<T> ("owned_box") is magical!
                if(TU_TEST1(e.path.m_data, Generic, .m_path == m_lang_Box())
                {
                    // Leave as is - MIR handles this
                    return ;
                }
                ),
            (Pointer,
                // Leave as is (primitive operation)
                return ;
                ),
            (Borrow,
                // Leave as is (primitive operation)
                return ;
                )
            )

            const char* langitem = nullptr;
            const char* method = nullptr;
            ::HIR::BorrowType   bt;
            // - Uses the value's usage beacuse for T: Copy node.m_value->m_usage is Borrow, but node.m_usage is Move
            switch( node.m_value->m_usage )
            {
            case ::HIR::ValueUsage::Unknown:
                BUG(sp, "Unknown usage type of deref value");
                break;
            case ::HIR::ValueUsage::Borrow:
                bt = ::HIR::BorrowType::Shared;
                langitem = method = "deref";
                break;
            case ::HIR::ValueUsage::Mutate:
                bt = ::HIR::BorrowType::Unique;
                langitem = method = "deref_mut";
                break;
            case ::HIR::ValueUsage::Move:
                TODO(sp, "ValueUsage::Move for desugared Deref of " << node.m_value->m_res_type);
                break;
            }
            // Needs replacement, continue
            assert(langitem);
            assert(method);

            // - Construct trait path - Index*<IdxTy>
            ::HIR::GenericPath  trait { m_crate.get_lang_item_path(node.span(), langitem), {} };

            ::std::vector< ::HIR::ExprNodeP>    args;
            args.push_back( NEWNODE( ::HIR::TypeRef::new_borrow(bt, ty_val.clone()), Borrow, sp, bt, mv$(node.m_value) ) );

            m_replacement = NEWNODE( ::HIR::TypeRef::new_borrow(bt, node.m_res_type.clone()), CallPath, sp,
                ::HIR::Path(ty_val.clone(), mv$(trait), method),
                mv$(args)
                );
            // Populate the cache for later passes
            // TODO: The check pass should probably just ignore this and DIY
            auto& call_node = dynamic_cast< ::HIR::ExprNode_CallPath&>(*m_replacement);
            auto& arg_types = call_node.m_cache.m_arg_types;
            arg_types.push_back( ::HIR::TypeRef::new_borrow(bt, ty_val.clone()) );
            arg_types.push_back( m_replacement->m_res_type.clone() );

            // - Dereference the result (which is an &-ptr)
            m_replacement = NEWNODE( mv$(node.m_res_type), Deref, sp,  mv$(m_replacement) );
        }
#endif



        void visit(::HIR::ExprNode_Unsize& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);

            // HACK: The autoderef code has to run before usage information is avaliable, so emits "invalid" _Unsize nodes
            // - Fix that.
            if( node.m_value->m_res_type.data().is_Array() )
            {
                const Span& sp = node.span();

                ::HIR::BorrowType   bt = ::HIR::BorrowType::Shared;
                switch( node.m_usage )
                {
                case ::HIR::ValueUsage::Unknown:
                    BUG(sp, "Unknown usage type of _Unsize value");
                    break;
                case ::HIR::ValueUsage::Borrow:
                    bt = ::HIR::BorrowType::Shared;
                    break;
                case ::HIR::ValueUsage::Mutate:
                    bt = ::HIR::BorrowType::Unique;
                    break;
                case ::HIR::ValueUsage::Move:
                    TODO(sp, "Support moving in _Unsize");
                    break;
                }

                auto ty_src = ::HIR::TypeRef::new_borrow(bt, node.m_value->m_res_type.clone());
                auto ty_dst = ::HIR::TypeRef::new_borrow(bt, node.m_res_type.clone());
                auto ty_dst2 = ty_dst.clone();
                // Borrow
                node.m_value = NEWNODE( mv$(ty_src), Borrow, sp, bt, mv$(node.m_value) );
                // Unsize borrow
                m_replacement = NEWNODE( mv$(ty_dst), Unsize, sp, mv$(node.m_value), mv$(ty_dst2) );
                // Deref
                m_replacement = NEWNODE( mv$(node.m_res_type), Deref, sp,  mv$(m_replacement) );
            }
        }
    };
    class OuterVisitor:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_crate(crate)
        {
        }

        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) override {
            BUG(Span(), "visit_expr hit in OuterVisitor");
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            if(auto* e = ty.data_mut().opt_Array())
            {
                this->visit_type( e->inner );
                DEBUG("Array size " << ty);
                if( auto* cg = e->size.opt_Unevaluated() ) {
                    ExprVisitor_Mutate  ev(m_crate);
                    if(cg->is_Unevaluated())
                        ev.visit_node_ptr( *cg->as_Unevaluated() );
                }
            }
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
                ExprVisitor_Mutate  ev(m_crate);
                ev.visit_node_ptr( item.m_code );
            }
            else
            {
                DEBUG("Function code " << p << " (none)");
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override {
            if( item.m_value )
            {
                ExprVisitor_Mutate  ev(m_crate);
                ev.visit_node_ptr(item.m_value);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value )
            {
                ExprVisitor_Mutate  ev(m_crate);
                ev.visit_node_ptr(item.m_value);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            if(auto* e = item.m_data.opt_Value())
            {
                for(auto& var : e->variants)
                {
                    DEBUG("Enum value " << p << " - " << var.name);

                    if( var.expr )
                    {
                        ExprVisitor_Mutate  ev(m_crate);
                        ev.visit_node_ptr(var.expr);
                    }
                }
            }
        }
    };
}   // namespace

void HIR_Expand_UfcsEverything_Expr(const ::HIR::Crate& crate, ::HIR::ExprPtr& exp)
{
    TRACE_FUNCTION;
    ExprVisitor_Mutate  ev { crate };
    ev.visit_node_ptr(exp);
}
void HIR_Expand_UfcsEverything(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

