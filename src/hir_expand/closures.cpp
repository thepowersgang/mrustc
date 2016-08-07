/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/closures.cpp
 * - HIR Expansion - Closures
 */
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <hir_typeck/static.hpp>
#include <algorithm>
#include "main_bindings.hpp"


namespace {
    
    typedef ::std::vector< ::std::pair< ::HIR::ExprNode_Closure::Class, ::HIR::TraitImpl> > out_impls_t;
    
    template<typename K, typename V>
    ::std::map<K,V> make_map1(K k1, V v1) {
        ::std::map<K,V> rv;
        rv.insert( ::std::make_pair(mv$(k1), mv$(v1)) );
        return rv;
    }
    template<typename T>
    ::std::vector<T> make_vec2(T v1, T v2) {
        ::std::vector<T>    rv;
        rv.reserve(2);
        rv.push_back( mv$(v1) );
        rv.push_back( mv$(v2) );
        return rv;
    }
    
    class ExprVisitor_Mutate:
        public ::HIR::ExprVisitorDef
    {
        const ::std::vector<unsigned int>&  m_local_vars;
        const ::std::vector<unsigned int>&  m_captures;
        
        ::HIR::ExprNodeP    m_replacement;
    public:
        ExprVisitor_Mutate(const ::std::vector<unsigned int>& local_vars, const ::std::vector<unsigned int>& captures):
            m_local_vars(local_vars),
            m_captures(captures)
        {
        }
        void visit_node_ptr(::HIR::ExprNodeP& node) override {
            node->visit(*this);
            if( m_replacement ) {
                node = mv$(m_replacement);
            }
        }
        void visit(::HIR::ExprNode_Variable& node) override
        {
            // 1. Is it a closure-local?
            auto binding_it = ::std::find(m_local_vars.begin(), m_local_vars.end(), node.m_slot);
            if( binding_it != m_local_vars.end() ) {
                node.m_slot = binding_it - m_local_vars.begin();
                return ;
            }
            
            // 2. Is it a capture?
            binding_it = ::std::find(m_captures.begin(), m_captures.end(), node.m_slot);
            if( binding_it != m_captures.end() ) {
                m_replacement = ::HIR::ExprNodeP( new ::HIR::ExprNode_Field(node.span(),
                    ::HIR::ExprNodeP( new ::HIR::ExprNode_Variable(node.span(), "self", 0) ),
                    FMT(binding_it - m_captures.begin())
                    ));
                return ;
            }
            
            BUG(node.span(), "");
        }
    };
    
    class ExprVisitor_Extract:
        public ::HIR::ExprVisitorDef
    {
        enum struct Usage {
            Borrow,
            Mutate,
            Move,
        };
        
        struct ClosureScope {
            ::HIR::ExprNode_Closure&    node;
            ::std::vector<unsigned int> local_vars;
            // NOTE: Capture list is in the node
            
            ClosureScope(::HIR::ExprNode_Closure& node):
                node(node)
            {
            }
        };

        const StaticTraitResolve& m_resolve;
        ::std::vector< ::HIR::TypeRef>& m_variable_types;
        
        // Outputs
        out_impls_t&    m_out_impls;
        
        /// Stack showing how a variable is being used
        ::std::vector<Usage>    m_usage;
        /// Stack of active closures
        ::std::vector<ClosureScope> m_closure_stack;
    public:
        ExprVisitor_Extract(const StaticTraitResolve& resolve, ::std::vector< ::HIR::TypeRef>& var_types, out_impls_t& out_impls):
            m_resolve(resolve),
            m_variable_types(var_types),
            m_out_impls( out_impls )
        {
        }
        
        void visit_root(::HIR::ExprNode& root)
        {
            root.visit(*this);
        }
        
        void visit(::HIR::ExprNode_Closure& node) override
        {
            m_closure_stack.push_back( ClosureScope(node) );
            
            ::std::vector< ::HIR::Pattern>  args_pat_inner;
            ::std::vector< ::HIR::TypeRef>  args_ty_inner;
            for(const auto& arg : node.m_args) {
                args_pat_inner.push_back( arg.first.clone() );
                args_ty_inner.push_back( arg.second.clone() );
                add_closure_def_from_pattern(node.span(), arg.first);
            }
            ::HIR::TypeRef  args_ty { mv$(args_ty_inner) };
            ::HIR::Pattern  args_pat { {}, ::HIR::Pattern::Data::make_Tuple({ mv$(args_pat_inner) }) };
            
            ::HIR::ExprVisitorDef::visit(node);
            
            auto ent = mv$( m_closure_stack.back() );
            m_closure_stack.pop_back();
            
            // Extract and mutate code into a trait impl on the closure type

            // 1. Iterate over the nodes and rewrite variable accesses to either renumbered locals, or field accesses
            ExprVisitor_Mutate    ev { ent.local_vars, ent.node.m_var_captures };
            ev.visit_node_ptr( node.m_code );
            // 2. Construct closure type (saving path/index in the node)
            // Includes:
            // - Generics based on the current scope (compacted)
            ::HIR::GenericParams    params;
            // - Types of captured variables
            ::std::vector< ::HIR::TypeRef>  capture_types;
            for(const auto binding_idx : node.m_var_captures) {
                capture_types.push_back( mv$(m_variable_types.at(binding_idx)) );
            }
            // - Code
            //m_type_output_list.push_back( ::HIR::ClosureType { mv$(params), mv$(capture_types) } );
            // 3. Create trait impls
            ::HIR::TypeRef  closure_type = node.m_res_type.clone();
            ::HIR::PathParams   trait_params;
            switch(node.m_class)
            {
            case ::HIR::ExprNode_Closure::Class::Unknown:
                node.m_class = ::HIR::ExprNode_Closure::Class::NoCapture;
            case ::HIR::ExprNode_Closure::Class::NoCapture:
            case ::HIR::ExprNode_Closure::Class::Shared:
                TODO(node.span(), "Generate Fn, FnMut, and FnOnce impls");
                break;
            case ::HIR::ExprNode_Closure::Class::Mut:
                TODO(node.span(), "Generate FnMut and FnOnce impls");
                break;
            case ::HIR::ExprNode_Closure::Class::Once:
                ::HIR::TraitImpl trait_impl {
                    mv$(params), mv$(trait_params), mv$(closure_type),
                    make_map1(
                        ::std::string("call_once"), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { false, ::HIR::Function {
                            "rust", false, false,
                            {},
                            make_vec2(
                                ::std::make_pair(::HIR::Pattern { {false, ::HIR::PatternBinding::Type::Move, "self", 0}, {} }, ::HIR::TypeRef("Self", 0xFFFF)),
                                ::std::make_pair(mv$(args_pat), mv$(args_ty))
                                ),
                            node.m_return.clone(),
                            mv$(node.m_code)
                            } }
                        ),
                    {},
                    make_map1(
                        ::std::string("Output"), ::HIR::TraitImpl::ImplEnt< ::HIR::TypeRef> { false, node.m_return.clone() }
                        ),
                    ::HIR::SimplePath()
                    };
                m_out_impls.push_back(::std::make_pair( ::HIR::ExprNode_Closure::Class::Once, mv$(trait_impl) ));
                break;
            }
            //m_impls.push_back(
            TODO(node.span(), "Transform closure code into closure type - " << node.m_res_type);
        }
        
        void visit(::HIR::ExprNode_Let& node) override
        {
            if( !m_closure_stack.empty() )
            {
                add_closure_def_from_pattern(node.span(), node.m_pattern);
            }
            
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_Variable& node) override
        {
            if( !m_closure_stack.empty() )
            {
                mark_used_variable(node.m_slot);
            }
            ::HIR::ExprVisitorDef::visit(node);
        }
        
        void visit(::HIR::ExprNode_Assign& node) override
        {
            // If closure is set, set a flag on the LHS saying it's being mutated, and one on the RHS saying it's being moved.
            if( !m_closure_stack.empty() )
            {
                m_usage.push_back(Usage::Mutate);
                node.m_slot->visit(*this);
                m_usage.pop_back();
                m_usage.push_back(Usage::Move);
                node.m_value->visit(*this);
                m_usage.pop_back();
            }
            else
            {
                ::HIR::ExprVisitorDef::visit(node);
            }
        }
        void visit(::HIR::ExprNode_UniOp& node) override
        {
            if( !m_closure_stack.empty() )
            {
                switch(node.m_op)
                {
                case ::HIR::ExprNode_UniOp::Op::Ref:    m_usage.push_back( Usage::Borrow ); break;
                case ::HIR::ExprNode_UniOp::Op::RefMut: m_usage.push_back( Usage::Mutate ); break;
                default:
                    m_usage.push_back( Usage::Move );
                    break;
                }
                
                node.m_value->visit(*this);
                
                m_usage.pop_back();
            }
            else
            {
                ::HIR::ExprVisitorDef::visit(node);
            }
        }
        void visit(::HIR::ExprNode_BinOp& node) override
        {
            if( !m_closure_stack.empty() )
            {
                switch(node.m_op)
                {
                case ::HIR::ExprNode_BinOp::Op::CmpEqu:
                case ::HIR::ExprNode_BinOp::Op::CmpNEqu:
                case ::HIR::ExprNode_BinOp::Op::CmpLt:
                case ::HIR::ExprNode_BinOp::Op::CmpLtE:
                case ::HIR::ExprNode_BinOp::Op::CmpGt:
                case ::HIR::ExprNode_BinOp::Op::CmpGtE:
                    m_usage.push_back( Usage::Borrow );
                    break;
                default:
                    m_usage.push_back( Usage::Move );
                    break;
                }
                
                node.m_left ->visit(*this);
                node.m_right->visit(*this);
                
                m_usage.pop_back();
            }
            else
            {
                ::HIR::ExprVisitorDef::visit(node);
            }
        }
        void visit(::HIR::ExprNode_Field& node) override
        {
            if( !m_closure_stack.empty() )
            {
                // If attempting to use a Copy type by value, it can just be a Borrow of the inner type
                if( m_usage.back() == Usage::Move && type_is_copy(node.m_res_type) ) {
                    m_usage.push_back(Usage::Borrow);
                    node.m_value->visit( *this );
                    m_usage.pop_back();
                }
                else {
                    node.m_value->visit( *this );
                }
            }
            else
            {
                ::HIR::ExprVisitorDef::visit(node);
            }
        }
        
        void visit(::HIR::ExprNode_CallValue& node) override
        {
            if( !m_closure_stack.empty() )
            {
                TODO(node.span(), "Determine how value in CallValue is used");
                
                if( node.m_trait_used == ::HIR::ExprNode_CallValue::TraitUsed::Unknown )
                {
                    if( node.m_res_type.m_data.is_Closure() )
                    {
                        TODO(node.span(), "Determine how value in CallValue is used on a closure");
                    }
                    else
                    {
                    }
                }
                else
                {
                    // If the trait is known, then the &/&mut has been added
                }
                
                m_usage.push_back(Usage::Move);
                node.m_value->visit(*this);
                for(auto& arg : node.m_args)
                    arg->visit(*this);
                m_usage.pop_back();
            }
            else if( node.m_res_type.m_data.is_Closure() )
            {
                TODO(node.span(), "Determine how value in CallValue is used on a closure");
            }
            else
            {
                ::HIR::ExprVisitorDef::visit(node);
            }
        }
        void visit(::HIR::ExprNode_CallMethod& node) override
        {
            if( !m_closure_stack.empty() )
            {
                m_usage.push_back(Usage::Move);
                node.m_value->visit(*this);
                for(auto& arg : node.m_args)
                    arg->visit(*this);
                m_usage.pop_back();
            }
            else
            {
                ::HIR::ExprVisitorDef::visit(node);
            }
        }
        void visit(::HIR::ExprNode_CallPath& node) override
        {
            if( !m_closure_stack.empty() )
            {
                m_usage.push_back(Usage::Move);
                for(auto& arg : node.m_args)
                    arg->visit(*this);
                m_usage.pop_back();
            }
            else
            {
                ::HIR::ExprVisitorDef::visit(node);
            }
        }
    private:
        bool type_is_copy(const ::HIR::TypeRef& ty) const
        {
            TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (e),
            (
                return false;
                ),
            (Primitive,
                return e != ::HIR::CoreType::Str;
                ),
            (Array,
                return type_is_copy(*e.inner);
                )
            )
        }
        
        void add_closure_def(unsigned int slot)
        {
            assert(m_closure_stack.size() > 0);
            auto& closure_defs = m_closure_stack.back().local_vars;
            
            auto it = ::std::lower_bound(closure_defs.begin(), closure_defs.end(), slot);
            if( it == closure_defs.end() || *it != slot ) {
                closure_defs.insert(it, slot);
            }
        }
        void add_closure_def_from_pattern(const Span& sp, const ::HIR::Pattern& pat)
        {
            // Add binding indexes to m_closure_defs
            if( pat.m_binding.is_valid() ) {
                const auto& pb = pat.m_binding;
                add_closure_def(pb.m_slot);
            }
            
            // Recurse
            TU_MATCH(::HIR::Pattern::Data, (pat.m_data), (e),
            (Any,
                ),
            (Value,
                ),
            (Range,
                ),
            (Box,
                TODO(sp, "Box pattern");
                ),
            (Ref,
                add_closure_def_from_pattern(sp, *e.sub);
                ),
            (Tuple,
                for( const auto& subpat : e.sub_patterns )
                    add_closure_def_from_pattern(sp, subpat);
                ),
            (Slice,
                for(const auto& sub : e.sub_patterns)
                    add_closure_def_from_pattern(sp, sub);
                ),
            (SplitSlice,
                for(const auto& sub : e.leading)
                    add_closure_def_from_pattern( sp, sub );
                for(const auto& sub : e.trailing)
                    add_closure_def_from_pattern( sp, sub );
                if( e.extra_bind.is_valid() ) {
                    add_closure_def(e.extra_bind.m_slot);
                }
                ),
            
            // - Enums/Structs
            (StructTuple,
                for(const auto& field : e.sub_patterns) {
                    add_closure_def_from_pattern(sp, field);
                }
                ),
            (StructTupleWildcard,
                ),
            (Struct,
                for( auto& field_pat : e.sub_patterns ) {
                    add_closure_def_from_pattern(sp, field_pat.second);
                }
                ),
            (EnumTuple,
                for(const auto& field : e.sub_patterns) {
                    add_closure_def_from_pattern(sp, field);
                }
                ),
            (EnumTupleWildcard,
                ),
            (EnumStruct,
                for( auto& field_pat : e.sub_patterns ) {
                    add_closure_def_from_pattern(sp, field_pat.second);
                }
                )
            )
        }
        void mark_used_variable(unsigned int slot)
        {
            for(const auto& closure_rec : m_closure_stack)
            {
                const auto& closure_defs = closure_rec.local_vars;
                if( ::std::binary_search(closure_defs.begin(), closure_defs.end(), slot) ) {
                    // Ignore, this is local to the current closure
                    return ;
                }
            }

            assert(m_closure_stack.size() > 0 );
            auto& closure_rec = m_closure_stack.back();
            auto& closure = closure_rec.node;
            
            auto it = ::std::lower_bound(closure.m_var_captures.begin(), closure.m_var_captures.end(), slot);
            if( it == closure.m_var_captures.end() || *it != slot ) {
                closure.m_var_captures.insert( it, slot );
            }
            
            // Use the m_usage variable
            switch( m_usage.back() )
            {
            case Usage::Borrow:
                closure.m_class = ::std::max(closure.m_class, ::HIR::ExprNode_Closure::Class::Shared);
                break;
            case Usage::Mutate:
                closure.m_class = ::std::max(closure.m_class, ::HIR::ExprNode_Closure::Class::Mut);
                break;
            case Usage::Move:
                if( type_is_copy( m_variable_types.at(slot) ) ) {
                    closure.m_class = ::std::max(closure.m_class, ::HIR::ExprNode_Closure::Class::Shared);
                }
                else {
                    closure.m_class = ::std::max(closure.m_class, ::HIR::ExprNode_Closure::Class::Once);
                }
                break;
            }
        }
    };
    
    class OuterVisitor:
        public ::HIR::Visitor
    {
        StaticTraitResolve  m_resolve;
        out_impls_t m_new_trait_impls;
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
            BUG(Span(), "visit_expr hit in OuterVisitor");
        }
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                this->visit_type( *e.inner );
                DEBUG("Array size " << ty);
                if( e.size ) {
                    ::std::vector< ::HIR::TypeRef>  tmp;
                    ExprVisitor_Extract    ev(m_resolve, tmp, m_new_trait_impls);
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
                ::std::vector< ::HIR::TypeRef>  tmp;
                //ExprVisitor_Extract    ev(item.m_code.binding_types);
                ExprVisitor_Extract    ev(m_resolve, tmp, m_new_trait_impls);
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
                ::std::vector< ::HIR::TypeRef>  tmp;
                ExprVisitor_Extract    ev(m_resolve, tmp, m_new_trait_impls);
                ev.visit_root(*item.m_value);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value )
            {
                ::std::vector< ::HIR::TypeRef>  tmp;
                ExprVisitor_Extract    ev(m_resolve, tmp, m_new_trait_impls);
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
                    
                    ::std::vector< ::HIR::TypeRef>  tmp;
                    ExprVisitor_Extract    ev(m_resolve, tmp, m_new_trait_impls);
                    ev.visit_root(*e);
                )
            }
        }
    };
}

void HIR_Expand_Closures(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

