/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/annotate_value_usage.cpp
 * - Marks _Variable, _Index, _Deref, and _Field nodes with how the result is used
 */
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <hir_typeck/static.hpp>
#include <algorithm>
#include "main_bindings.hpp"
#include <hir/expr_state.hpp>

namespace {

    class ExprVisitor_Mark:
        public ::HIR::ExprVisitor//Def
    {
        const StaticTraitResolve&    m_resolve;
        ::std::vector< ::HIR::ValueUsage>   m_usage;

        struct UsageGuard
        {
            ExprVisitor_Mark& m_parent;
            bool    m_pop;
            UsageGuard(ExprVisitor_Mark& parent, bool pop):
                m_parent(parent),
                m_pop(pop)
            {
            }
            ~UsageGuard()
            {
                if(m_pop) {
                    m_parent.m_usage.pop_back();
                }
            }
        };

        ::HIR::ValueUsage get_usage() const {
            return (m_usage.empty() ? ::HIR::ValueUsage::Move : m_usage.back());
        }
        UsageGuard push_usage(::HIR::ValueUsage u) {
            if( get_usage() == u ) {
                return UsageGuard(*this, false);
            }
            else {
                m_usage.push_back( u );
                return UsageGuard(*this, true);
            }
        }

    public:
        ExprVisitor_Mark(const StaticTraitResolve& resolve):
            m_resolve(resolve)
        {}

        void visit_root(::HIR::ExprPtr& root_ptr)
        {
            assert(root_ptr);
            root_ptr->m_usage = this->get_usage();
            auto expected_size = m_usage.size();
            root_ptr->visit( *this );
            assert( m_usage.size() == expected_size );
        }
        void visit_node_ptr(::HIR::ExprNodeP& node_ptr) override
        {
            assert(node_ptr);

            const auto& node_ref = *node_ptr;
            const char* node_tyname = typeid(node_ref).name();
            TRACE_FUNCTION_FR(&*node_ptr << " " << node_tyname, node_ptr->m_usage);

            node_ptr->m_usage = this->get_usage();

            auto expected_size = m_usage.size();
            node_ptr->visit( *this );
            assert( m_usage.size() == expected_size );
        }

        void visit(::HIR::ExprNode_Block& node) override
        {
            auto _ = this->push_usage( ::HIR::ValueUsage::Move );

            for( auto& subnode : node.m_nodes ) {
                this->visit_node_ptr(subnode);
            }
            if( node.m_value_node )
                this->visit_node_ptr(node.m_value_node);
        }

        void visit(::HIR::ExprNode_Asm& node) override
        {
            auto _ = this->push_usage( ::HIR::ValueUsage::Move );
            for(auto& v : node.m_outputs)
            {
                this->visit_node_ptr(v.value);
            }
            for(auto& v : node.m_inputs)
            {
                this->visit_node_ptr(v.value);
            }
        }
        void visit(::HIR::ExprNode_Return& node) override
        {
            auto _ = this->push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr( node.m_value );
        }
        void visit(::HIR::ExprNode_Let& node) override
        {
            if( node.m_value )
            {
                auto _ = this->push_usage( this->get_usage_for_pattern(node.span(), node.m_pattern, node.m_type) );
                this->visit_node_ptr( node.m_value );
            }
        }
        void visit(::HIR::ExprNode_Loop& node) override
        {
            auto _ = this->push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr( node.m_code );
        }
        void visit(::HIR::ExprNode_LoopControl& node) override
        {
            // NOTE: Leaf
            if( node.m_value )
            {
                this->visit_node_ptr(node.m_value);
            }
        }
        void visit(::HIR::ExprNode_Match& node) override
        {
            {
                const auto& val_ty = node.m_value->m_res_type;
                ::HIR::ValueUsage   vu = ::HIR::ValueUsage::Unknown;
                for( const auto& arm : node.m_arms )
                {
                    for( const auto& pat : arm.m_patterns )
                        vu = ::std::max( vu, this->get_usage_for_pattern(node.span(), pat, val_ty) );
                }
                auto _ = this->push_usage( vu );
                this->visit_node_ptr( node.m_value );
            }

            auto _ = this->push_usage( ::HIR::ValueUsage::Move );
            for(auto& arm : node.m_arms)
            {
                if( arm.m_cond ) {
                    this->visit_node_ptr( arm.m_cond );
                }
                this->visit_node_ptr( arm.m_code );
            }
        }
        void visit(::HIR::ExprNode_If& node) override
        {
            auto _ = this->push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr( node.m_cond );
            this->visit_node_ptr( node.m_true );
            if( node.m_false ) {
                this->visit_node_ptr( node.m_false );
            }
        }

        void visit(::HIR::ExprNode_Assign& node) override
        {
            {
                auto _ = this->push_usage( ::HIR::ValueUsage::Mutate );
                this->visit_node_ptr(node.m_slot);
            }
            {
                auto _ = this->push_usage( ::HIR::ValueUsage::Move );
                this->visit_node_ptr(node.m_value);
            }
        }
        void visit(::HIR::ExprNode_UniOp& node) override
        {
            m_usage.push_back( ::HIR::ValueUsage::Move );

            this->visit_node_ptr(node.m_value);

            m_usage.pop_back();
        }
        void visit(::HIR::ExprNode_Borrow& node) override
        {
            switch(node.m_type)
            {
            case ::HIR::BorrowType::Shared:
                m_usage.push_back( ::HIR::ValueUsage::Borrow );
                break;
            case ::HIR::BorrowType::Unique:
                m_usage.push_back( ::HIR::ValueUsage::Mutate );
                break;
            case ::HIR::BorrowType::Owned:
                m_usage.push_back( ::HIR::ValueUsage::Move );
                break;
            }

            this->visit_node_ptr(node.m_value);

            m_usage.pop_back();
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
            case ::HIR::ExprNode_BinOp::Op::CmpGtE:
                m_usage.push_back( ::HIR::ValueUsage::Borrow );
                break;
            default:
                m_usage.push_back( ::HIR::ValueUsage::Move );
                break;
            }

            this->visit_node_ptr(node.m_left);
            this->visit_node_ptr(node.m_right);

            m_usage.pop_back();
        }
        void visit(::HIR::ExprNode_Cast& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr(node.m_value);
        }
        void visit(::HIR::ExprNode_Unsize& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr(node.m_value);
        }
        void visit(::HIR::ExprNode_Index& node) override
        {
            // TODO: Override to ::Borrow if Res: Copy and moving
            if( this->get_usage() == ::HIR::ValueUsage::Move && m_resolve.type_is_copy(node.span(), node.m_res_type) ) {
                auto _ = push_usage( ::HIR::ValueUsage::Borrow );
                this->visit_node_ptr(node.m_value);
            }
            else {
                this->visit_node_ptr(node.m_value);
            }

            auto _ = push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr(node.m_index);
        }
        void visit(::HIR::ExprNode_Deref& node) override
        {
            if( this->get_usage() == ::HIR::ValueUsage::Move && m_resolve.type_is_copy(node.span(), node.m_res_type) ) {
                auto _ = push_usage( ::HIR::ValueUsage::Borrow );
                this->visit_node_ptr(node.m_value);
            }
            // Pointers only need a borrow to be derefernced.
            else if( node.m_res_type.m_data.is_Pointer() ) {
                auto _ = push_usage( ::HIR::ValueUsage::Borrow );
                this->visit_node_ptr(node.m_value);
            }
            else {
                this->visit_node_ptr(node.m_value);
            }
        }

        void visit(::HIR::ExprNode_Emplace& node) override
        {
            if( node.m_type == ::HIR::ExprNode_Emplace::Type::Noop ) {
                if( node.m_place )
                    this->visit_node_ptr(node.m_place);
                this->visit_node_ptr(node.m_value);
            }
            else {
                auto _ = push_usage( ::HIR::ValueUsage::Move );
                if( node.m_place )
                    this->visit_node_ptr(node.m_place);
                this->visit_node_ptr(node.m_value);
            }
        }

        void visit(::HIR::ExprNode_Field& node) override
        {
            bool is_copy = m_resolve.type_is_copy(node.span(), node.m_res_type);
            DEBUG("ty = " << node.m_res_type << ", is_copy=" << is_copy);
            // If taking this field by value, but the type is Copy - pretend it's a borrow.
            if( this->get_usage() == ::HIR::ValueUsage::Move && is_copy ) {
                auto _ = push_usage( ::HIR::ValueUsage::Borrow );
                this->visit_node_ptr(node.m_value);
            }
            else {
                this->visit_node_ptr(node.m_value);
            }
        }

        void visit(::HIR::ExprNode_TupleVariant& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );

            for( auto& val : node.m_args )
                this->visit_node_ptr(val);
        }
        void visit(::HIR::ExprNode_CallPath& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );

            for( auto& val : node.m_args )
                this->visit_node_ptr(val);
        }
        void visit(::HIR::ExprNode_CallValue& node) override
        {
            // TODO: Different usage based on trait.
            ::HIR::ValueUsage   vu = ::HIR::ValueUsage::Borrow;
            switch( node.m_trait_used )
            {
            case ::HIR::ExprNode_CallValue::TraitUsed::Unknown:
                //TODO(node.span(), "Annotate usage when CallValue trait is unknown");
                // - Can only happen when the callee is a closure, could do detectection of closure class in this pass?
                // - Assume Move for now.
                vu = ::HIR::ValueUsage::Move;
                break;
            case ::HIR::ExprNode_CallValue::TraitUsed::Fn:
                vu = ::HIR::ValueUsage::Borrow;
                break;
            case ::HIR::ExprNode_CallValue::TraitUsed::FnMut:
                vu = ::HIR::ValueUsage::Mutate;
                break;
            case ::HIR::ExprNode_CallValue::TraitUsed::FnOnce:
                vu = ::HIR::ValueUsage::Move;
                break;
            }
            {
                auto _ = push_usage( vu );
                this->visit_node_ptr(node.m_value);
            }

            auto _ = push_usage( ::HIR::ValueUsage::Move );
            for( auto& val : node.m_args )
                this->visit_node_ptr(val);
        }
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            {
                assert(node.m_cache.m_fcn);
                ::HIR::ValueUsage   vu = ::HIR::ValueUsage::Borrow;
                switch(node.m_cache.m_fcn->m_receiver)
                {
                case ::HIR::Function::Receiver::Free:
                    BUG(node.span(), "_CallMethod resolved to free function");
                case ::HIR::Function::Receiver::Value:
                case ::HIR::Function::Receiver::Box:
                case ::HIR::Function::Receiver::Custom:
                case ::HIR::Function::Receiver::BorrowOwned:
                    vu = ::HIR::ValueUsage::Move;
                    break;
                case ::HIR::Function::Receiver::BorrowUnique:
                    vu = ::HIR::ValueUsage::Mutate;
                    break;
                case ::HIR::Function::Receiver::BorrowShared:
                    vu = ::HIR::ValueUsage::Borrow;
                    break;
                //case ::HIR::Function::Receiver::PointerMut:
                //case ::HIR::Function::Receiver::PointerConst:
                }
                auto _ = push_usage( vu );
                this->visit_node_ptr(node.m_value);
            }
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            for( auto& val : node.m_args )
                this->visit_node_ptr(val);
        }

        void visit(::HIR::ExprNode_Literal& node) override
        {
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override
        {
        }
        void visit(::HIR::ExprNode_PathValue& node) override
        {
        }
        void visit(::HIR::ExprNode_Variable& node) override
        {
        }
        void visit(::HIR::ExprNode_ConstParam& node) override
        {
        }

        void visit(::HIR::ExprNode_StructLiteral& node) override
        {
            const auto& sp = node.span();
            const auto& ty_path = node.m_real_path;
            if( node.m_base_value ) {
                bool is_moved = false;
                const auto& tpb = node.m_base_value->m_res_type.m_data.as_Path().binding;
                const ::HIR::Struct* str;
                if( tpb.is_Enum() ) {
                    const auto& enm = *tpb.as_Enum();
                    auto idx = enm.find_variant(ty_path.m_path.m_components.back());
                    ASSERT_BUG(sp, idx != SIZE_MAX, "");
                    const auto& var_ty = enm.m_data.as_Data()[idx].type;
                    str = var_ty.m_data.as_Path().binding.as_Struct();
                }
                else {
                    str = tpb.as_Struct();
                }
                ASSERT_BUG(sp, str->m_data.is_Named(), "");
                const auto& fields = str->m_data.as_Named();

                ::std::vector<bool> provided_mask( fields.size() );
                for( const auto& fld : node.m_values ) {
                    unsigned idx = ::std::find_if( fields.begin(), fields.end(), [&](const auto& x){ return x.first == fld.first; }) - fields.begin();
                    provided_mask[idx] = true;
                }

                const auto monomorph_cb = monomorphise_type_get_cb(node.span(), nullptr, &ty_path.m_params, nullptr);
                for( unsigned int i = 0; i < fields.size(); i ++ ) {
                    if( ! provided_mask[i] ) {
                        const auto& ty_o = fields[i].second.ent;
                        ::HIR::TypeRef  tmp;
                        const auto& ty_m = (monomorphise_type_needed(ty_o) ? tmp = monomorphise_type_with(node.span(), ty_o, monomorph_cb) : ty_o);
                        bool is_copy = m_resolve.type_is_copy(node.span(), ty_m);
                        if( !is_copy ) {
                            DEBUG("- Field " << i << " " << fields[i].first << ": " << ty_m << " moved");
                            is_moved = true;
                        }
                    }
                }

                // If only Copy fields will be used, set usage to Borrow
                auto _ = push_usage( is_moved ? ::HIR::ValueUsage::Move : ::HIR::ValueUsage::Borrow );
                this->visit_node_ptr(node.m_base_value);
            }

            auto _ = push_usage( ::HIR::ValueUsage::Move );
            for( auto& fld_val : node.m_values ) {
                this->visit_node_ptr(fld_val.second);
            }
        }
        void visit(::HIR::ExprNode_UnionLiteral& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr(node.m_value);
        }
        void visit(::HIR::ExprNode_Tuple& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            for( auto& val : node.m_vals ) {
                this->visit_node_ptr(val);
            }
        }
        void visit(::HIR::ExprNode_ArrayList& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            for( auto& val : node.m_vals ) {
                this->visit_node_ptr(val);
            }
        }
        void visit(::HIR::ExprNode_ArraySized& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr(node.m_val);
        }

        void visit(::HIR::ExprNode_Closure& node) override
        {
            auto _ = push_usage( ::HIR::ValueUsage::Move );
            this->visit_node_ptr(node.m_code);
        }

    private:
        ::HIR::ValueUsage get_usage_for_pattern_binding(const Span& sp, const ::HIR::PatternBinding& pb, const ::HIR::TypeRef& ty) const
        {
            switch( pb.m_type )
            {
            case ::HIR::PatternBinding::Type::Move:
                if( m_resolve.type_is_copy(sp, ty) )
                    return ::HIR::ValueUsage::Borrow;
                else
                    return ::HIR::ValueUsage::Move;
            case ::HIR::PatternBinding::Type::MutRef:
                return ::HIR::ValueUsage::Mutate;
            case ::HIR::PatternBinding::Type::Ref:
                return ::HIR::ValueUsage::Borrow;
            }
            throw "";
        }

        ::HIR::ValueUsage get_usage_for_pattern(const Span& sp, const ::HIR::Pattern& pat, const ::HIR::TypeRef& outer_ty) const
        {
            if( pat.m_binding.is_valid() ) {
                return get_usage_for_pattern_binding(sp, pat.m_binding, outer_ty);
            }

            // Implicit derefs
            const ::HIR::TypeRef* typ = &outer_ty;
            for(size_t i = 0; i < pat.m_implicit_deref_count; i ++)
            {
                typ = &*typ->m_data.as_Borrow().inner;
            }
            const ::HIR::TypeRef& ty = *typ;

            TU_MATCHA( (pat.m_data), (pe),
            (Any,
                return ::HIR::ValueUsage::Borrow;
                ),
            (Box,
                // NOTE: Specific to `owned_box`
                const auto& sty = ty.m_data.as_Path().path.m_data.as_Generic().m_params.m_types.at(0);
                return get_usage_for_pattern(sp, *pe.sub, sty);
                ),
            (Ref,
                return get_usage_for_pattern(sp, *pe.sub, *ty.m_data.as_Borrow().inner);
                ),
            (Tuple,
                ASSERT_BUG(sp, ty.m_data.is_Tuple(), "Tuple pattern with non-tuple type - " << ty);
                const auto& subtys = ty.m_data.as_Tuple();
                assert(pe.sub_patterns.size() == subtys.size());
                auto rv = ::HIR::ValueUsage::Borrow;
                for(unsigned int i = 0; i < subtys.size(); i ++)
                    rv = ::std::max(rv, get_usage_for_pattern(sp, pe.sub_patterns[i], subtys[i]));
                return rv;
                ),
            (SplitTuple,
                ASSERT_BUG(sp, ty.m_data.is_Tuple(), "SplitTuple pattern with non-tuple type - " << ty);
                const auto& subtys = ty.m_data.as_Tuple();
                assert(pe.leading.size() + pe.trailing.size() <= subtys.size());
                auto rv = ::HIR::ValueUsage::Borrow;
                for(unsigned int i = 0; i < pe.leading.size(); i ++)
                    rv = ::std::max(rv, get_usage_for_pattern(sp, pe.leading[i], subtys[i]));
                for(unsigned int i = 0; i < pe.trailing.size(); i ++)
                    rv = ::std::max(rv, get_usage_for_pattern(sp, pe.trailing[pe.trailing.size() - 1 - i], subtys[subtys.size() - 1 - i]));
                return rv;
                ),
            (StructValue,
                return ::HIR::ValueUsage::Borrow;
                ),
            (StructTuple,
                // TODO: Avoid monomorphising all the time.
                const auto& str = *pe.binding;
                ASSERT_BUG(sp, str.m_data.is_Tuple(), "StructTuple pattern with non-tuple struct - " << str.m_data.tag_str());
                const auto& flds = str.m_data.as_Tuple();
                assert(pe.sub_patterns.size() == flds.size());
                auto monomorph_cb = monomorphise_type_get_cb(sp, nullptr,  &pe.path.m_params, nullptr);

                auto rv = ::HIR::ValueUsage::Borrow;
                for(unsigned int i = 0; i < flds.size(); i ++) {
                    auto sty = monomorphise_type_with(sp, flds[i].ent, monomorph_cb);
                    rv = ::std::max(rv, get_usage_for_pattern(sp, pe.sub_patterns[i], sty));
                }
                return rv;
                ),
            (Struct,
                const auto& str = *pe.binding;
                if( pe.is_wildcard() )
                    return ::HIR::ValueUsage::Borrow;
                if( pe.sub_patterns.empty() && (TU_TEST1(str.m_data, Tuple, .empty()) || str.m_data.is_Unit()) ) {
                    return ::HIR::ValueUsage::Borrow;
                }
                ASSERT_BUG(sp, str.m_data.is_Named(), "Struct pattern on non-brace struct");
                const auto& flds = str.m_data.as_Named();
                auto monomorph_cb = monomorphise_type_get_cb(sp, nullptr,  &pe.path.m_params, nullptr);

                auto rv = ::HIR::ValueUsage::Borrow;
                for(const auto& fld_pat : pe.sub_patterns)
                {
                    auto fld_it = ::std::find_if(flds.begin(), flds.end(), [&](const auto& x){return x.first == fld_pat.first;});
                    ASSERT_BUG(sp, fld_it != flds.end(), "");

                    auto sty = monomorphise_type_with(sp, fld_it->second.ent, monomorph_cb);
                    rv = ::std::max(rv, get_usage_for_pattern(sp, fld_pat.second, sty));
                }
                return rv;
                ),
            (Value,
                return ::HIR::ValueUsage::Borrow;
                ),
            (Range,
                return ::HIR::ValueUsage::Borrow;
                ),
            (EnumValue,
                return ::HIR::ValueUsage::Borrow;
                ),
            (EnumTuple,
                const auto& enm = *pe.binding_ptr;
                ASSERT_BUG(sp, enm.m_data.is_Data(), "");
                const auto& var = enm.m_data.as_Data().at(pe.binding_idx);
                const auto& str = *var.type.m_data.as_Path().binding.as_Struct();
                ASSERT_BUG(sp, str.m_data.is_Tuple(), "");
                const auto& flds = str.m_data.as_Tuple();
                assert(pe.sub_patterns.size() == flds.size());
                auto monomorph_cb = monomorphise_type_get_cb(sp, nullptr,  &pe.path.m_params, nullptr);

                auto rv = ::HIR::ValueUsage::Borrow;
                for(unsigned int i = 0; i < flds.size(); i ++) {
                    auto sty = monomorphise_type_with(sp, flds[i].ent, monomorph_cb);
                    rv = ::std::max(rv, get_usage_for_pattern(sp, pe.sub_patterns[i], sty));
                }
                return rv;
                ),
            (EnumStruct,
                const auto& enm = *pe.binding_ptr;
                ASSERT_BUG(sp, enm.m_data.is_Data(), "EnumStruct pattern on non-data enum");
                const auto& var = enm.m_data.as_Data().at(pe.binding_idx);
                const auto& str = *var.type.m_data.as_Path().binding.as_Struct();
                ASSERT_BUG(sp, str.m_data.is_Named(), "EnumStruct pattern on non-struct variant - " << pe.path);
                const auto& flds = str.m_data.as_Named();
                auto monomorph_cb = monomorphise_type_get_cb(sp, nullptr,  &pe.path.m_params, nullptr);

                auto rv = ::HIR::ValueUsage::Borrow;
                for(const auto& fld_pat : pe.sub_patterns)
                {
                    auto fld_it = ::std::find_if(flds.begin(), flds.end(), [&](const auto& x){return x.first == fld_pat.first;});
                    ASSERT_BUG(sp, fld_it != flds.end(), "");

                    auto sty = monomorphise_type_with(sp, fld_it->second.ent, monomorph_cb);
                    rv = ::std::max(rv, get_usage_for_pattern(sp, fld_pat.second, sty));
                }
                return rv;
                ),
            (Slice,
                const auto& inner_ty = (ty.m_data.is_Array() ? *ty.m_data.as_Array().inner : *ty.m_data.as_Slice().inner);
                auto rv = ::HIR::ValueUsage::Borrow;
                for(const auto& pat : pe.sub_patterns)
                    rv = ::std::max(rv, get_usage_for_pattern(sp, pat, inner_ty));
                return rv;
                ),
            (SplitSlice,
                const auto& inner_ty = (ty.m_data.is_Array() ? *ty.m_data.as_Array().inner : *ty.m_data.as_Slice().inner);
                auto rv = ::HIR::ValueUsage::Borrow;
                for(const auto& pat : pe.leading)
                    rv = ::std::max(rv, get_usage_for_pattern(sp, pat, inner_ty));
                for(const auto& pat : pe.trailing)
                    rv = ::std::max(rv, get_usage_for_pattern(sp, pat, inner_ty));
                if( pe.extra_bind.is_valid() )
                    rv = ::std::max(rv, get_usage_for_pattern_binding(sp, pe.extra_bind, inner_ty));
                return rv;
                )
            )
            throw "";
        }
    };


    class OuterVisitor:
        public ::HIR::Visitor
    {
        StaticTraitResolve   m_resolve;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_resolve(crate)
        {}

        void visit_expr(::HIR::ExprPtr& exp) override {
            if( exp )
            {
                ExprVisitor_Mark    ev { m_resolve };
                ev.visit_root( exp );
            }
        }

        // ------
        // Code-containing items
        // ------
        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override {
            auto _ = this->m_resolve.set_item_generics(item.m_params);
            DEBUG("Function " << p);
            ::HIR::Visitor::visit_function(p, item);
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override {
            // NOTE: No generics
            ::HIR::Visitor::visit_static(p, item);
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            // NOTE: No generics
            ::HIR::Visitor::visit_constant(p, item);
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            auto _ = this->m_resolve.set_item_generics(item.m_params);
            ::HIR::Visitor::visit_enum(p, item);
        }


        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override {
            auto _ = this->m_resolve.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
        }

        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);

            ::HIR::Visitor::visit_type_impl(impl);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);

            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        }
    };
}

void HIR_Expand_AnnotateUsage_Expr(const ::HIR::Crate& crate, ::HIR::ExprPtr& exp)
{
    assert(exp);
    StaticTraitResolve   resolve { crate };
    if(exp.m_state->m_impl_generics)   resolve.set_impl_generics(*exp.m_state->m_impl_generics);
    if(exp.m_state->m_item_generics)   resolve.set_item_generics(*exp.m_state->m_item_generics);
    ExprVisitor_Mark    ev { resolve };
    ev.visit_root(exp);
}

void HIR_Expand_AnnotateUsage(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}
