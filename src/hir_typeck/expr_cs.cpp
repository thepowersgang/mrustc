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
        ::HIR::ExprNodeP* right_node_ptr;
        
        friend ::std::ostream& operator<<(::std::ostream& os, const Coercion& v) {
            os << v.left_ty << " := " << &**v.right_node_ptr << " (" << (*v.right_node_ptr)->m_res_type << ")";
            return os;
        }
    };
    struct Associated
    {
        Span    span;
        ::HIR::TypeRef  left_ty;
        
        ::HIR::SimplePath   trait;
        ::HIR::PathParams   params;
        ::HIR::TypeRef  impl_ty;
        const char* name;   // if "", no type is used (and left is ignored) - Just does trait selection
        
        friend ::std::ostream& operator<<(::std::ostream& os, const Associated& v) {
            os << v.left_ty << " = " << "<" << v.impl_ty << " as " << v.trait << v.params << ">::" << v.name;
            return os;
        }
    };
    
    struct IVarPossible
    {
        // TODO: If an ivar is eliminated (i.e. has its type dropped) while its pointer is here - things will break
        //::std::vector<const ::HIR::TypeRef*>    types;
        ::std::vector<::HIR::TypeRef>    types;
    };
    
    const ::HIR::Crate& m_crate;
    
    ::std::vector<Binding>  m_bindings;
    HMTypeInferrence    m_ivars;
    TraitResolution m_resolve;
    
    ::std::vector<Coercion> link_coerce;
    ::std::vector<Associated> link_assoc;
    /// Nodes that need revisiting (e.g. method calls when the receiver isn't known)
    ::std::vector< ::HIR::ExprNode*>    to_visit;
    
    ::std::vector< IVarPossible>    possible_ivar_vals;
    
    Context(const ::HIR::Crate& crate, const ::HIR::GenericParams* impl_params, const ::HIR::GenericParams* item_params):
        m_crate(crate),
        m_resolve(m_ivars, crate, impl_params, item_params)
    {
    }
    
    void dump() const;
    
    bool take_changed() { return m_ivars.take_changed(); }
    bool has_rules() const {
        return link_coerce.size() > 0 || link_assoc.size() > 0 || to_visit.size() > 0;
    }
    
    void add_ivars(::HIR::TypeRef& ty) {
        m_ivars.add_ivars(ty);
    }
    // - Equate two types, with no possibility of coercion
    //  > Errors if the types are incompatible.
    //  > Forces types if one side is an infer
    void equate_types(const Span& sp, const ::HIR::TypeRef& l, const ::HIR::TypeRef& r);
    // - Equate two types, allowing inferrence
    void equate_types_coerce(const Span& sp, const ::HIR::TypeRef& l, ::HIR::ExprNodeP& node_ptr);
    // - Equate a type to an associated type (if name == "", no equation is done, but trait is searched)
    void equate_types_assoc(const Span& sp, const ::HIR::TypeRef& l,  const ::HIR::SimplePath& trait, ::std::vector< ::HIR::TypeRef> ty_args, const ::HIR::TypeRef& impl_ty, const char *name);

    // - List `t` as a possible type for `ivar_index`
    void possible_equate_type(unsigned int ivar_index, const ::HIR::TypeRef& t);
    
    // - Add a pattern binding (forcing the type to match)
    void add_binding(const Span& sp, ::HIR::Pattern& pat, ::HIR::TypeRef& type);
    
    void add_var(unsigned int index, const ::std::string& name, ::HIR::TypeRef type);
    const ::HIR::TypeRef& get_var(const Span& sp, unsigned int idx) const;
    
    // - Add a revisit entry
    void add_revisit(::HIR::ExprNode& node);

    const ::HIR::TypeRef& get_type(const ::HIR::TypeRef& ty) const { return m_ivars.get_type(ty); }
    
private:
    void add_ivars_params(::HIR::PathParams& params) {
        m_ivars.add_ivars_params(params);
    }
};

static void fix_param_count(const Span& sp, Context& context, const ::HIR::Path& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params);
static void fix_param_count(const Span& sp, Context& context, const ::HIR::GenericPath& path, const ::HIR::GenericParams& param_defs,  ::HIR::PathParams& params);

namespace {
    
    /// (HELPER) Populate the cache for nodes that use visit_call
    void visit_call_populate_cache(Context& context, const Span& sp, ::HIR::Path& path, ::HIR::ExprCallCache& cache)
    {
        assert(cache.m_arg_types.size() == 0);
        
        const ::HIR::Function*  fcn_ptr = nullptr;
        ::std::function<const ::HIR::TypeRef&(const ::HIR::TypeRef&)>    monomorph_cb;
        
        TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
        (Generic,
            const auto& fcn = context.m_crate.get_function_by_path(sp, e.m_path);
            fix_param_count(sp, context, path, fcn.m_params,  e.m_params);
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
                        return context.get_type(path_params.m_types[idx]);
                    }
                    else {
                        BUG(sp, "Generic bounding out of total range");
                    }
                };
            ),
        (UfcsKnown,
            const auto& trait = context.m_crate.get_trait_by_path(sp, e.trait.m_path);
            fix_param_count(sp, context, path, trait.m_params, e.trait.m_params);
            if( trait.m_values.count(e.item) == 0 ) {
                BUG(sp, "Method '" << e.item << "' of trait " << e.trait.m_path << " doesn't exist");
            }
            const auto& fcn = trait.m_values.at(e.item).as_Function();
            fix_param_count(sp, context, path, fcn.m_params,  e.params);
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
                        return context.get_type(trait_params.m_types[idx]);
                    }
                    else if( ge.binding < 512 ) {
                        auto idx = ge.binding - 256;
                        if( idx >= path_params.m_types.size() ) {
                            BUG(sp, "Generic param out of input range - " << idx << " '"<<ge.name<<"' >= " << path_params.m_types.size());
                        }
                        return context.get_type(path_params.m_types[idx]);
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
            context.m_crate.find_type_impls(*e.type, [&](const auto& ty)->const auto& {
                    if( ty.m_data.is_Infer() )
                        return context.get_type(ty);
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
            fix_param_count(sp, context, path, fcn_ptr->m_params,  e.params);
            cache.m_fcn_params = &fcn_ptr->m_params;
            
            
            // If the impl block has parameters, figure out what types they map to
            // - The function params are already mapped (from fix_param_count)
            auto& impl_params = cache.m_ty_impl_params;
            if( impl_ptr->m_params.m_types.size() > 0 ) {
                impl_params.m_types.resize( impl_ptr->m_params.m_types.size() );
                impl_ptr->m_type.match_generics(sp, *e.type, context.m_ivars.callback_resolve_infer(), [&](auto idx, const auto& ty) {
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
                        return context.get_type(*e.type);
                    }
                    else if( ge.binding < 256 ) {
                        auto idx = ge.binding;
                        if( idx >= impl_params.m_types.size() ) {
                            BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << impl_params.m_types.size());
                        }
                        return context.get_type(impl_params.m_types[idx]);
                    }
                    else if( ge.binding < 512 ) {
                        auto idx = ge.binding - 256;
                        if( idx >= fcn_params.m_types.size() ) {
                            BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << fcn_params.m_types.size());
                        }
                        return context.get_type(fcn_params.m_types[idx]);
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
        
        // TODO: Bounds (encoded as associated)
        for(const auto& bound : cache.m_fcn_params->m_bounds)
        {
            TU_MATCH(::HIR::GenericBound, (bound), (be),
            (Lifetime,
                ),
            (TypeLifetime,
                ),
            (TraitBound,
                auto real_type = monomorphise_type_with(sp, be.type, cache.m_monomorph_cb);
                auto real_trait = monomorphise_genericpath_with(sp, be.trait.m_path, cache.m_monomorph_cb, false);
                DEBUG("Bound " << be.type << ":  " << be.trait);
                DEBUG("= (" << real_type << ": " << real_trait << ")");
                const auto& trait_params = real_trait.m_params;
                context.equate_types_assoc(sp, ::HIR::TypeRef(), be.trait.m_path.m_path, mv$(trait_params.clone().m_types), real_type, "");
                ),
            (TypeEquality,
                auto real_type_left = context.m_resolve.expand_associated_types(sp, monomorphise_type_with(sp, be.type, cache.m_monomorph_cb));
                auto real_type_right = context.m_resolve.expand_associated_types(sp, monomorphise_type_with(sp, be.other_type, cache.m_monomorph_cb));
                context.equate_types(sp, real_type_left, real_type_right);
                )
            )
        }
    }
    
    // -----------------------------------------------------------------------
    // Enumeration visitor
    // 
    // Iterates the HIR expression tree and extracts type "equations"
    // -----------------------------------------------------------------------
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
            TRACE_FUNCTION_F(&node << " { ... }");
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
            TRACE_FUNCTION_F(&node << " return ...");
            this->context.add_ivars( node.m_value->m_res_type );

            this->context.equate_types_coerce(node.span(), this->ret_type, node.m_value);
            
            node.m_value->visit( *this );
        }
        
        void visit(::HIR::ExprNode_Loop& node) override
        {
            TRACE_FUNCTION_F(&node << " loop { ... }");
            
            this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
            // TODO: This is more correct, but could cause variables to be falsely marked as !
            //this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_diverge());
            
            this->context.add_ivars(node.m_code->m_res_type);
            this->context.equate_types(node.span(), node.m_code->m_res_type, ::HIR::TypeRef::new_unit());
            node.m_code->visit( *this );
        }
        void visit(::HIR::ExprNode_LoopControl& node) override
        {
            TRACE_FUNCTION_F(&node << " " << (node.m_continue ? "continue" : "break") << " '" << node.m_label);
            // Nothing
        }
        
        void visit(::HIR::ExprNode_Let& node) override
        {
            TRACE_FUNCTION_F(&node << " let " << node.m_pattern << ": " << node.m_type);
            
            this->context.add_ivars( node.m_type );
            this->context.add_binding(node.span(), node.m_pattern, node.m_type);
            
            this->context.add_ivars( node.m_value->m_res_type );
            this->context.equate_types_coerce( node.span(), node.m_type, node.m_value );
            
            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_Match& node) override
        {
            TRACE_FUNCTION_F(&node << " match ...");
            
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
            TRACE_FUNCTION_F(&node << " if ...");
            
            this->context.add_ivars( node.m_cond->m_res_type );
            this->context.equate_types_coerce(node.m_cond->span(), ::HIR::TypeRef(::HIR::CoreType::Bool), node.m_cond);
            node.m_cond->visit( *this );
            
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
            TRACE_FUNCTION_F(&node << "... = ...");
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
            TRACE_FUNCTION_F(&node << "... "<<::HIR::ExprNode_BinOp::opname(node.m_op)<<" ...");
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
            TRACE_FUNCTION_F(&node << " " << ::HIR::ExprNode_UniOp::opname(node.m_op) << "...");
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
                break;
            case ::HIR::ExprNode_UniOp::Op::Negate:
                this->context.equate_types_assoc(node.span(), node.m_res_type,  this->context.m_crate.get_lang_item_path(node.span(), "neg"), {}, node.m_value->m_res_type.clone(), "Output");
                break;
            }
            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_Cast& node) override
        {
            TRACE_FUNCTION_F(&node << " ... as " << node.m_res_type);
            this->context.add_ivars( node.m_value->m_res_type );
            
            this->context.add_revisit(node);
            
            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_Unsize& node) override
        {
            BUG(node.span(), "Hit _Unsize");
        }
        void visit(::HIR::ExprNode_Index& node) override
        {
            TRACE_FUNCTION_F(&node << " ... [ ... ]");
            this->context.add_ivars( node.m_value->m_res_type );
            this->context.add_ivars( node.m_index->m_res_type );
            
            const auto& op_trait = this->context.m_crate.get_lang_item_path(node.span(), "index");
            this->context.equate_types_assoc(node.span(), node.m_res_type,  op_trait, ::make_vec1(node.m_index->m_res_type.clone()), node.m_value->m_res_type.clone(), "Output");
            
            node.m_value->visit( *this );
            node.m_index->visit( *this );
        }
        void visit(::HIR::ExprNode_Deref& node) override
        {
            TRACE_FUNCTION_F(&node << " *...");
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
            const auto& sp = node.span();
            TRACE_FUNCTION_F(&node << " " << node.m_path << "(...) [" << (node.m_is_struct ? "struct" : "enum") << "]");
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
            
            const auto& ty_params = node.m_path.m_params.m_types;
            auto monomorph_cb = [&](const auto& gt)->const auto& {
                const auto& ge = gt.m_data.as_Generic();
                if( ge.binding == 0xFFFF ) {
                    return ty;
                }
                else if( ge.binding < 256 ) {
                    if( ge.binding >= ty_params.size() ) {
                        BUG(sp, "Type parameter index out of range (#" << ge.binding << " " << ge.name << ")");
                    }
                    return ty_params[ge.binding];
                }
                else {
                    BUG(sp, "Method-level parameter on struct (#" << ge.binding << " " << ge.name << ")");
                }
                };
            
            // Bind fields with type params (coercable)
            node.m_arg_types.resize( node.m_args.size() );
            for( unsigned int i = 0; i < node.m_args.size(); i ++ )
            {
                const auto& des_ty_r = fields[i].ent;
                if( monomorphise_type_needed(des_ty_r) ) {
                    node.m_arg_types[i] = monomorphise_type_with(sp, des_ty_r, monomorph_cb);
                    this->context.equate_types_coerce(node.span(), node.m_arg_types[i],  node.m_args[i]);
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
            TRACE_FUNCTION_F(&node << " " << node.m_path << "{...} [" << (node.m_is_struct ? "struct" : "enum") << "]");
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
            
            const auto& ty_params = node.m_path.m_params.m_types;
            auto monomorph_cb = [&](const auto& gt)->const auto& {
                const auto& ge = gt.m_data.as_Generic();
                if( ge.binding == 0xFFFF ) {
                    return ty;
                }
                else if( ge.binding < 256 ) {
                    if( ge.binding >= ty_params.size() ) {
                        BUG(node.span(), "Type parameter index out of range (#" << ge.binding << " " << ge.name << ")");
                    }
                    return ty_params[ge.binding];
                }
                else {
                    BUG(node.span(), "Method-level parameter on struct (#" << ge.binding << " " << ge.name << ")");
                }
                };
            
            node.m_value_types.resize( fields.size() );
            
            // Bind fields with type params (coercable)
            for( auto& val : node.m_values)
            {
                const auto& name = val.first;
                auto it = ::std::find_if(fields.begin(), fields.end(), [&](const auto& v)->bool{ return v.first == name; });
                assert(it != fields.end());
                const auto& des_ty_r = it->second.ent;
                auto& des_ty_cache = node.m_value_types[it - fields.begin()];
                
                if( monomorphise_type_needed(des_ty_r) ) {
                    if( des_ty_cache == ::HIR::TypeRef() ) {
                        des_ty_cache = monomorphise_type_with(node.span(), des_ty_r, monomorph_cb);
                    }
                    // TODO: Is it an error when it's already populated?
                    this->context.equate_types_coerce(node.span(), des_ty_cache,  val.second);
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
            TRACE_FUNCTION_F(&node << " " << node.m_path << " [" << (node.m_is_struct ? "struct" : "enum") << "]");
            
            // - Create ivars in path, and set result type
            const auto ty = this->get_structenum_ty(node.span(), node.m_is_struct, node.m_path);
            this->context.equate_types(node.span(), node.m_res_type, ty);
        }

        void visit(::HIR::ExprNode_CallPath& node) override
        {
            this->visit_path(node.span(), node.m_path);
            TRACE_FUNCTION_F(&node << " " << node.m_path << "(...)");
            for( auto& val : node.m_args ) {
                this->context.add_ivars( val->m_res_type );
            }
            
            // Populate cache
            {
                visit_call_populate_cache(this->context, node.span(), node.m_path, node.m_cache);
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
            TRACE_FUNCTION_F(&node << " ...(...)");
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
            TRACE_FUNCTION_F(&node << " (...)."<<node.m_method<<"(...)");
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
            TRACE_FUNCTION_F(&node << " (...)."<<node.m_field);
            this->context.add_ivars( node.m_value->m_res_type );
            
            this->context.add_revisit( node );
            
            node.m_value->visit( *this );
        }
        
        void visit(::HIR::ExprNode_Tuple& node) override
        {
            TRACE_FUNCTION_F(&node << " (...,)");
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
            TRACE_FUNCTION_F(&node << " [...,]");
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
            TRACE_FUNCTION_F(&node << " [...; "<<node.m_size_val<<"]");
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
            this->visit_path(sp, node.m_path);
            TRACE_FUNCTION_F(&node << " " << node.m_path);
            
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
            TRACE_FUNCTION_F(&node << " " << node.m_name << "{" << node.m_slot << "}");
            
            this->context.equate_types(node.span(), node.m_res_type,  this->context.get_var(node.span(), node.m_slot));
        }
        
        void visit(::HIR::ExprNode_Closure& node) override
        {
            TRACE_FUNCTION_F(&node << " |...| ...");
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
        void visit_generic_path(const Span& sp, ::HIR::GenericPath& gp) {
            for(auto& ty : gp.m_params.m_types)
                this->context.add_ivars(ty);
        }
        void visit_path(const Span& sp, ::HIR::Path& path) {
            TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
            (Generic,
                this->visit_generic_path(sp, e);
                ),
            (UfcsKnown,
                this->context.add_ivars(*e.type);
                this->visit_generic_path(sp, e.trait);
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
    };

    // -----------------------------------------------------------------------
    // Revisit Class
    //
    // Handles visiting nodes during inferrence passes
    // -----------------------------------------------------------------------
    class ExprVisitor_Revisit:
        public ::HIR::ExprVisitor
    {
        Context& context;
        bool m_completed;
    public:
        ExprVisitor_Revisit(Context& context):
            context(context),
            m_completed(false)
        {}
        
        bool node_completed() const {
            return m_completed;
        }

        void visit(::HIR::ExprNode_Block& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Return& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Let& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Loop& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_LoopControl& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Match& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_If& node) override {
            no_revisit(node);
        }
        
        void visit(::HIR::ExprNode_Assign& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_BinOp& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_UniOp& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Cast& node) override {
            const auto& sp = node.span();
            const auto& tgt_ty = this->context.get_type(node.m_res_type);
            const auto& src_ty = this->context.get_type(node.m_value->m_res_type);
            TU_MATCH( ::HIR::TypeRef::Data, (tgt_ty.m_data), (e),
            (Infer,
                // Can't know anything
                //this->m_completed = true;
                ),
            (Diverge,
                BUG(sp, "");
                ),
            (Primitive,
                // Don't have anything to contribute
                this->m_completed = true;
                ),
            (Path,
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                ),
            (Generic,
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                ),
            (TraitObject,
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                ),
            (Array,
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                ),
            (Slice,
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                ),
            (Tuple,
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                ),
            (Borrow,
                TODO(sp, "Cast to borrow");
                ),
            (Pointer,
                TU_MATCH_DEF( ::HIR::TypeRef::Data, (src_ty.m_data), (s_e),
                (
                    ERROR(sp, E0000, "Invalid cast to pointer");
                    ),
                (Infer,
                    ),
                (Borrow,
                    // Check class (must be equal) and type
                    // TODO: Check class
                    this->context.equate_types(sp, *e.inner, *s_e.inner);
                    ),
                (Pointer,
                    // Allow with no link?
                    )
                )
                ),
            (Function,
                ERROR(sp, E0000, "Non-scalar cast to " << this->context.m_ivars.fmt_type(tgt_ty));
                ),
            (Closure,
                BUG(sp, "");
                )
            )
        }
        void visit(::HIR::ExprNode_Unsize& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Index& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Deref& node) override {
            no_revisit(node);
        }
        
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_CallPath& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_CallValue& node) override {
            // TODO:
            const auto& ty = this->context.get_type(node.m_value->m_res_type);
            DEBUG("(CallValue) ty = " << ty);
            
            TU_MATCH_DEF(decltype(ty.m_data), (ty.m_data), (e),
            (
                // Search for FnOnce impl
                const auto& lang_FnOnce = this->context.m_crate.get_lang_item_path(node.span(), "fn_once");
                
                ::HIR::TypeRef  fcn_args_tup;
                ::HIR::TypeRef  fcn_ret;
                
                // Create a param set with a single tuple (of all argument types)
                ::HIR::PathParams   trait_pp;
                {
                    ::std::vector< ::HIR::TypeRef>  arg_types;
                    for(const auto& arg : node.m_args) {
                        arg_types.push_back( this->context.get_type(arg->m_res_type).clone() );
                    }
                    trait_pp.m_types.push_back( ::HIR::TypeRef( mv$(arg_types) ) );
                }
                auto was_bounded = this->context.m_resolve.find_trait_impls_bound(node.span(), lang_FnOnce, trait_pp, ty, [&](const auto& , const auto& args, const auto& assoc) {
                        const auto& tup = args.m_types[0];
                        if( !tup.m_data.is_Tuple() )
                            ERROR(node.span(), E0000, "FnOnce expects a tuple argument, got " << tup);
                        fcn_args_tup = tup.clone();
                        return true;
                        });
                if( was_bounded )
                {
                    // RV must be in a bound
                    fcn_ret = ::HIR::TypeRef( ::HIR::Path(::HIR::Path::Data::make_UfcsKnown({
                        box$( ty.clone() ),
                        ::HIR::GenericPath(lang_FnOnce),
                        "Output",
                        {}
                        })) );
                    fcn_ret.m_data.as_Path().path.m_data.as_UfcsKnown().trait.m_params.m_types.push_back( fcn_args_tup.clone() );
                }
                else if( !ty.m_data.is_Generic() )
                {
                    TODO(node.span(), "Search for other implementations of FnOnce for " << ty);
                }
                else
                {
                    // Didn't find anything. Error?
                    ERROR(node.span(), E0000, "Unable to find an implementation of Fn* for " << ty);
                }
                
                node.m_arg_types = mv$( fcn_args_tup.m_data.as_Tuple() );
                node.m_arg_types.push_back( mv$(fcn_ret) );
                ),
            (Closure,
                for( const auto& arg : e.m_arg_types )
                    node.m_arg_types.push_back( arg.clone() );
                node.m_arg_types.push_back( e.m_rettype->clone() );
                ),
            (Function,
                for( const auto& arg : e.m_arg_types )
                    node.m_arg_types.push_back( arg.clone() );
                node.m_arg_types.push_back( e.m_rettype->clone() );
                ),
            (Borrow,
                // TODO: Autoderef?
                return ;
                ),
            (Infer,
                // No idea yet
                return ;
                )
            )
            assert( node.m_arg_types.size() == node.m_args.size() + 1 );
            for(unsigned int i = 0; i < node.m_args.size(); i ++)
            {
                this->context.equate_types_coerce(node.span(), node.m_arg_types[i], node.m_args[i]);
            }
            this->context.equate_types(node.span(), node.m_res_type, node.m_arg_types.back());
            this->m_completed = true;
        }
        void visit(::HIR::ExprNode_CallMethod& node) override {
            const auto& ty = this->context.get_type(node.m_value->m_res_type);
            //const auto ty = this->context.m_resolve.expand_associated_types(node.span(), this->context.get_type(node.m_value->m_res_type).clone());
            TRACE_FUNCTION_F("(CallMethod) ty = " << this->context.m_ivars.fmt_type(ty));
            // Using autoderef, locate this method on the type
            ::HIR::Path   fcn_path { ::HIR::SimplePath() };
            unsigned int deref_count = this->context.m_resolve.autoderef_find_method(node.span(), node.m_traits, ty, node.m_method,  fcn_path);
            if( deref_count != ~0u )
            {
                DEBUG("- deref_count = " << deref_count);
                visit_call_populate_cache(this->context, node.span(), fcn_path, node.m_cache);
                
                node.m_method_path = mv$(fcn_path);
                // NOTE: Steals the params from the node
                TU_MATCH(::HIR::Path::Data, (node.m_method_path.m_data), (e),
                (Generic,
                    ),
                (UfcsUnknown,
                    ),
                (UfcsKnown,
                    e.params = mv$(node.m_params);
                    ),
                (UfcsInherent,
                    e.params = mv$(node.m_params);
                    )
                )
                
                assert( node.m_cache.m_arg_types.size() >= 1);
                
                if( node.m_args.size()+1 != node.m_cache.m_arg_types.size() - 1 ) {
                    ERROR(node.span(), E0000, "Incorrect number of arguments to " << fcn_path);
                }
                
                // Link arguments
                // 1+ because it's a method call (#0 is Self)
                DEBUG("node.m_cache.m_arg_types = " << node.m_cache.m_arg_types);
                for(unsigned int i = 0; i < node.m_args.size(); i ++)
                {
                    this->context.equate_types_coerce(node.span(), node.m_cache.m_arg_types[1+i], node.m_args[i]);
                }
                this->context.equate_types(node.span(), node.m_res_type,  node.m_cache.m_arg_types.back());
                
                node.m_method_path = mv$(fcn_path);
                this->m_completed = true;
            }
        }
        void visit(::HIR::ExprNode_Field& node) override {
            // TODO:
            TODO(node.span(), "ExprNode_Field - revisit");
        }

        void visit(::HIR::ExprNode_Literal& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_PathValue& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Variable& node) override {
            no_revisit(node);
        }
        
        void visit(::HIR::ExprNode_StructLiteral& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_Tuple& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_ArrayList& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_ArraySized& node) override {
            no_revisit(node);
        }
        
        void visit(::HIR::ExprNode_Closure& node) override {
            no_revisit(node);
        }
    private:
        void no_revisit(::HIR::ExprNode& node) {
            BUG(node.span(), "Node revisit unexpected - " << typeid(node).name());
        }
    };
    
    // -----------------------------------------------------------------------
    // Post-inferrence visitor
    //
    // Saves the inferred types into the HIR expression tree, and ensures that
    // all types were inferred.
    // -----------------------------------------------------------------------
    class ExprVisitor_Apply:
        public ::HIR::ExprVisitorDef
    {
        HMTypeInferrence& ivars;
    public:
        ExprVisitor_Apply(HMTypeInferrence& ivars):
            ivars(ivars)
        {
        }
        void visit_node_ptr(::HIR::ExprNodeP& node) override {
            const char* node_ty = typeid(*node).name();
            TRACE_FUNCTION_FR(node_ty << " : " << node->m_res_type, node_ty);
            this->check_type_resolved(node->span(), node->m_res_type, node->m_res_type);
            DEBUG(node_ty << " : = " << node->m_res_type);
            ::HIR::ExprVisitorDef::visit_node_ptr(node);
        }
        
    private:
        void check_type_resolved(const Span& sp, ::HIR::TypeRef& ty, const ::HIR::TypeRef& top_type) const {
            TU_MATCH(::HIR::TypeRef::Data, (ty.m_data), (e),
            (Infer,
                auto new_ty = this->ivars.get_type(ty).clone();
                // - Move over before checking, so that the source type mentions the correct ivar
                ty = mv$(new_ty);
                if( ty.m_data.is_Infer() ) {
                    ERROR(sp, E0000, "Failed to infer type " << ty << " in "  << top_type);
                }
                check_type_resolved(sp, ty, top_type);
                ),
            (Diverge,
                // Leaf
                ),
            (Primitive,
                // Leaf
                ),
            (Path,
                // TODO:
                ),
            (Generic,
                // Leaf
                ),
            (TraitObject,
                // TODO:
                ),
            (Array,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Slice,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Tuple,
                for(auto& st : e)
                    this->check_type_resolved(sp, st, top_type);
                ),
            (Borrow,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Pointer,
                this->check_type_resolved(sp, *e.inner, top_type);
                ),
            (Function,
                this->check_type_resolved(sp, *e.m_rettype, top_type);
                for(auto& st : e.m_arg_types)
                    this->check_type_resolved(sp, st, top_type);
                ),
            (Closure,
                this->check_type_resolved(sp, *e.m_rettype, top_type);
                for(auto& st : e.m_arg_types)
                    this->check_type_resolved(sp, st, top_type);
                )
            )
        }
    };
}


void Context::dump() const {
    m_ivars.dump();
    DEBUG("CS Context - " << link_coerce.size() << " Coercions, " << link_assoc.size() << " associated, " << to_visit.size() << " nodes");
    for(const auto& v : link_coerce) {
        DEBUG(v);
    }
    for(const auto& v : link_assoc) {
        DEBUG(v);
    }
    for(const auto& v : to_visit) {
        DEBUG(&v << " " << typeid(*v).name());
    }
}

void Context::equate_types(const Span& sp, const ::HIR::TypeRef& li, const ::HIR::TypeRef& ri) {
    // Instantly apply equality

    // TODO: Check if the type contains a replacable associated type
    ::HIR::TypeRef  l_tmp;
    ::HIR::TypeRef  r_tmp;
    const auto& l_t = this->m_resolve.expand_associated_types(sp, this->m_ivars.get_type(li), l_tmp);
    const auto& r_t = this->m_resolve.expand_associated_types(sp, this->m_ivars.get_type(ri), r_tmp);
    
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
            // Helper function for Path and TraitObject
            auto equality_typeparams = [&](const ::HIR::PathParams& l, const ::HIR::PathParams& r) {
                    if( l.m_types.size() != r.m_types.size() ) {
                        ERROR(sp, E0000, "Type mismatch in type params `" << l << "` and `" << r << "`");
                    }
                    for(unsigned int i = 0; i < l.m_types.size(); i ++)
                    {
                        this->equate_types(sp, l.m_types[i], r.m_types[i]);
                    }
                };
            
            // If either side is !, return early
            // TODO: Should ! end up in an ivar?
            TU_IFLET(::HIR::TypeRef::Data, l_t.m_data, Diverge, l_e,
                return ;
            )
            TU_IFLET(::HIR::TypeRef::Data, r_t.m_data, Diverge, r_e,
                return ;
            )
            
            if( l_t.m_data.tag() != r_t.m_data.tag() ) {
                ERROR(sp, E0000, "Type mismatch between " << this->m_ivars.fmt_type(l_t) << " and " << this->m_ivars.fmt_type(r_t));
            }
            TU_MATCH( ::HIR::TypeRef::Data, (l_t.m_data, r_t.m_data), (l_e, r_e),
            (Infer,
                throw "";
                ),
            (Diverge,
                // ignore?
                ),
            (Primitive,
                if( l_e != r_e ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                ),
            (Path,
                if( l_e.path.m_data.tag() != r_e.path.m_data.tag() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                TU_MATCH(::HIR::Path::Data, (l_e.path.m_data, r_e.path.m_data), (lpe, rpe),
                (Generic,
                    if( lpe.m_path != rpe.m_path ) {
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    }
                    equality_typeparams(lpe.m_params, rpe.m_params);
                    ),
                (UfcsInherent,
                    equality_typeparams(lpe.params, rpe.params);
                    if( lpe.item != rpe.item )
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    this->equate_types(sp, *lpe.type, *rpe.type);
                    ),
                (UfcsKnown,
                    equality_typeparams(lpe.trait.m_params, rpe.trait.m_params);
                    equality_typeparams(lpe.params, rpe.params);
                    if( lpe.item != rpe.item )
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    this->equate_types(sp, *lpe.type, *rpe.type);
                    ),
                (UfcsUnknown,
                    // TODO: If the type is fully known, locate a suitable trait item
                    equality_typeparams(lpe.params, rpe.params);
                    if( lpe.item != rpe.item )
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    this->equate_types(sp, *lpe.type, *rpe.type);
                    )
                )
                ),
            (Generic,
                if( l_e.binding != r_e.binding ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                ),
            (TraitObject,
                if( l_e.m_trait.m_path.m_path != r_e.m_trait.m_path.m_path ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                equality_typeparams(l_e.m_trait.m_path.m_params, r_e.m_trait.m_path.m_params);
                for(auto it_l = l_e.m_trait.m_type_bounds.begin(), it_r = r_e.m_trait.m_type_bounds.begin(); it_l != l_e.m_trait.m_type_bounds.end(); it_l++, it_r++ ) {
                    if( it_l->first != it_r->first ) {
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - associated bounds differ");
                    }
                    this->equate_types(sp, it_l->second, it_r->second);
                }
                if( l_e.m_markers.size() != r_e.m_markers.size() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - trait counts differ");
                }
                // TODO: Is this list sorted in any way? (if it's not sorted, this could fail when source does Send+Any instead of Any+Send)
                for(unsigned int i = 0; i < l_e.m_markers.size(); i ++ )
                {
                    auto& l_p = l_e.m_markers[i];
                    auto& r_p = r_e.m_markers[i];
                    if( l_p.m_path != r_p.m_path ) {
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    }
                    equality_typeparams(l_p.m_params, r_p.m_params);
                }
                // NOTE: Lifetime is ignored
                ),
            (Array,
                this->equate_types(sp, *l_e.inner, *r_e.inner);
                if( l_e.size_val != r_e.size_val ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - sizes differ");
                }
                ),
            (Slice,
                this->equate_types(sp, *l_e.inner, *r_e.inner);
                ),
            (Tuple,
                if( l_e.size() != r_e.size() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Tuples are of different length");
                }
                for(unsigned int i = 0; i < l_e.size(); i ++)
                {
                    this->equate_types(sp, l_e[i], r_e[i]);
                }
                ),
            (Borrow,
                if( l_e.type != r_e.type ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Borrow classes differ");
                }
                this->equate_types(sp, *l_e.inner, *r_e.inner);
                ),
            (Pointer,
                if( l_e.type != r_e.type ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t << " - Pointer mutability differs");
                }
                this->equate_types(sp, *l_e.inner, *r_e.inner);
                ),
            (Function,
                if( l_e.is_unsafe != r_e.is_unsafe
                    || l_e.m_abi != r_e.m_abi
                    || l_e.m_arg_types.size() != r_e.m_arg_types.size()
                    )
                {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                this->equate_types(sp, *l_e.m_rettype, *r_e.m_rettype);
                for(unsigned int i = 0; i < l_e.m_arg_types.size(); i ++ ) {
                    this->equate_types(sp, l_e.m_arg_types[i], r_e.m_arg_types[i]);
                }
                ),
            (Closure,
                if( l_e.m_arg_types.size() != r_e.m_arg_types.size() ) {
                    ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                }
                this->equate_types(sp, *l_e.m_rettype, *r_e.m_rettype);
                for( unsigned int i = 0; i < l_e.m_arg_types.size(); i ++ )
                {
                    this->equate_types(sp, l_e.m_arg_types[i], r_e.m_arg_types[i]);
                }
                )
            )
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
        TU_MATCH(::HIR::Pattern::Value, (e.val), (v),
        (Integer,
            if( v.type != ::HIR::CoreType::Str ) {
                this->equate_types(sp, type, ::HIR::TypeRef(v.type));
            }
            ),
        (String,
            this->equate_types(sp, type, ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::TypeRef(::HIR::CoreType::Str) ));
            ),
        (Named,
            // TODO: Get type of the value and equate it
            )
        )
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
    this->m_ivars.get_type(l);
    // - Just record the equality
    this->link_coerce.push_back(Coercion {
        l.clone(), &node_ptr
        });
}
void Context::equate_types_assoc(const Span& sp, const ::HIR::TypeRef& l,  const ::HIR::SimplePath& trait, ::std::vector< ::HIR::TypeRef> ty_args, const ::HIR::TypeRef& impl_ty, const char *name)
{
    ::HIR::PathParams   pp;
    pp.m_types = mv$(ty_args);
    this->link_assoc.push_back(Associated {
        sp,
        l.clone(),
        
        trait.clone(),
        mv$(pp),
        impl_ty.clone(),
        name
        });
}
void Context::add_revisit(::HIR::ExprNode& node) {
    this->to_visit.push_back( &node );
}

void Context::possible_equate_type(unsigned int ivar_index, const ::HIR::TypeRef& t) {
    DEBUG(ivar_index << " ?= " << t << " " << this->m_ivars.get_type(t));
    {
        ::HIR::TypeRef  ty_l;
        ty_l.m_data.as_Infer().index = ivar_index;
        assert( m_ivars.get_type(ty_l).m_data.is_Infer() );
    }
    if( ivar_index >= possible_ivar_vals.size() ) {
        possible_ivar_vals.resize( ivar_index + 1 );
    }
    //possible_ivar_vals[ivar_index].types.push_back( &t );
    possible_ivar_vals[ivar_index].types.push_back( t.clone() );
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

namespace {
    bool check_coerce_borrow(Context& context, const ::HIR::TypeRef& inner_l, const ::HIR::TypeRef& inner_r, ::HIR::ExprNodeP& node_ptr)
    {
        const auto& sp = node_ptr->span();
        
        const auto& ty_dst = context.m_ivars.get_type(inner_l);
        const auto& ty_src = context.m_ivars.get_type(inner_r);
        // TODO: Various coercion types
        // - Trait/Slice unsizing
        // - Deref
        // - Unsize?

        // If the types are already equal, no operation is required
        if( context.m_ivars.types_equal(ty_dst, ty_src) ) {
            return true;
        }
        
        // If the source is '_', we can't know yet
        TU_IFLET(::HIR::TypeRef::Data, ty_src.m_data, Infer, r_e,
            // - Except if it's known to be a primitive
            if( r_e.ty_class != ::HIR::InferClass::None ) {
                context.equate_types(sp, ty_dst, ty_src);
                return true;
            }
            context.possible_equate_type(r_e.index, ty_dst);
            return false;
        )
        
        TU_IFLET(::HIR::TypeRef::Data, ty_dst.m_data, Infer, l_e,
            if( l_e.ty_class == ::HIR::InferClass::None ) {
                context.possible_equate_type(l_e.index, ty_src);
                return false;
            }
            // - Otherwise, it could be a deref?
        )
        
        // Fast hack for slices
        #if 0
        if( left_inner_res.m_data.is_Slice() && !right_inner_res.m_data.is_Slice() )
        {
            const auto& left_slice = left_inner_res.m_data.as_Slice();
            TU_IFLET(::HIR::TypeRef::Data, right_inner_res.m_data, Array, right_array,
                this->apply_equality(sp, *left_slice.inner, cb_left, *right_array.inner, cb_right, nullptr);
                auto span = node_ptr->span();
                node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Unsize( mv$(span), mv$(node_ptr), l_t.clone() ));
                node_ptr->m_res_type = l_t.clone();
                
                this->mark_change();
                return ;
            )
            else TU_IFLET(::HIR::TypeRef::Data, right_inner_res.m_data, Generic, right_arg,
                TODO(sp, "Search for Unsize bound on generic");
            )
            else
            {
                // Apply deref coercions
            }
        }
        #endif
        
        // Deref coercions
        {
            ::HIR::TypeRef  tmp_ty;
            const ::HIR::TypeRef*   out_ty = &ty_src;
            unsigned int count = 0;
            ::std::vector< ::HIR::TypeRef>  types;
            while( (out_ty = context.m_resolve.autoderef(sp, *out_ty, tmp_ty)) )
            {
                count += 1;
                
                if( out_ty->m_data.is_Infer() && out_ty->m_data.as_Infer().ty_class == ::HIR::InferClass::None ) {
                    // Hit a _, so can't keep going
                    break;
                }
                
                types.push_back( out_ty->clone() );
                
                if( context.m_ivars.types_equal(ty_dst, *out_ty) == false ) {
                    // Check equivalence
                    if( ty_dst.m_data.tag() == out_ty->m_data.tag() ) {
                        TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty_dst.m_data, out_ty->m_data), (d_e, s_e),
                        (
                            DEBUG("");
                            continue ;
                            ),
                        (Slice,
                            // Equate!
                            context.equate_types(sp, ty_dst, *out_ty);
                            // - Fall through
                            )
                        )
                    }
                    else {
                        continue ;
                    }
                }
                
                while(count --)
                {
                    auto span = node_ptr->span();
                    node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Deref( mv$(span), mv$(node_ptr) ));
                    node_ptr->m_res_type = mv$( types.back() );
                    types.pop_back();
                }
                
                context.m_ivars.mark_change();
                return true;
            }
            // Either ran out of deref, or hit a _
        }
        
        // - If `right`: ::core::marker::Unsize<`left`>
        // - If left can be dereferenced to right
        // - If left is a slice, right can unsize/deref (Defunct?)
        return false;
    }
    bool check_coerce(Context& context, const Context::Coercion& v) {
        ::HIR::ExprNodeP& node_ptr = *v.right_node_ptr;
        const auto& sp = node_ptr->span();
        const auto& ty = context.m_ivars.get_type(v.left_ty);
        const auto& ty_r = context.m_ivars.get_type(node_ptr->m_res_type);
        TRACE_FUNCTION_F(v << " - " << ty << " := " << ty_r);
        
        if( context.m_ivars.types_equal(ty, ty_r) ) {
            return true;
        }
        
        // TODO: CoerceUnsized trait
        
        // 1. Check that the source type can coerce
        TU_MATCH( ::HIR::TypeRef::Data, (ty_r.m_data), (e),
        (Infer,
            if( e.ty_class != ::HIR::InferClass::None ) {
                context.equate_types(sp, ty,  ty_r);
                return true;
            }
            ),
        (Diverge,
            return true;
            ),
        (Primitive,
            context.equate_types(sp, ty,  node_ptr->m_res_type);
            return true;
            ),
        (Path,
            //TODO(Span(), "check_coerce - Coercion from " << ty_r);
            context.equate_types(sp, ty,  node_ptr->m_res_type);
            return true;
            ),
        (Generic,
            //TODO(Span(), "check_coerce - Coercion from " << ty_r);
            context.equate_types(sp, ty,  node_ptr->m_res_type);
            return true;
            ),
        (TraitObject,
            // TODO: Can bare trait objects coerce?
            context.equate_types(sp, ty,  node_ptr->m_res_type);
            return true;
            ),
        (Array,
            // TODO: Can raw arrays coerce to anything?
            context.equate_types(sp, ty,  node_ptr->m_res_type);
            return true;
            ),
        (Slice,
            context.equate_types(sp, ty,  node_ptr->m_res_type);
            return true;
            ),
        (Tuple,
            context.equate_types(sp, ty,  node_ptr->m_res_type);
            return true;
            ),
        (Borrow,
            // TODO: Borrows can have unsizing and deref coercions applied
            ),
        (Pointer,
            // TODO: Pointers coerce from borrows and similar pointers
            ),
        (Function,
            TODO(sp, "check_coerce - Coercion from " << ty_r);
            ),
        (Closure,
            // TODO: Can closures coerce to anything?
            // - (eventually maybe fn() if they don't capture, but that's not rustc yet)
            context.equate_types(sp, ty,  node_ptr->m_res_type);
            return true;
            //TODO(sp, "check_coerce - Coercion from " << ty_r << " to " << ty);
            )
        )
        
        // 2. Check target type is a valid coercion
        // - Otherwise - Force equality
        TU_MATCH( ::HIR::TypeRef::Data, (ty.m_data), (l_e),
        (Infer,
            if( l_e.ty_class != ::HIR::InferClass::None ) {
                context.equate_types(sp, ty,  ty_r);
                return true;
            }
            // Can't do anything yet?
            // - Later code can handle "only path" coercions

            context.possible_equate_type(l_e.index,  ty_r);
            return false;
            ),
        (Diverge,
            return true;
            ),
        (Primitive,
            // TODO: `str` is a coercion target? (but only via &str)
            context.equate_types(sp, ty,  node_ptr->m_res_type);
            return true;
            ),
        (Path,
            //TODO(Span(), "check_coerce - Coercion to " << ty);
            // TODO: CoerceUnsized
            context.equate_types(sp, ty,  node_ptr->m_res_type);
            return true;
            ),
        (Generic,
            //TODO(Span(), "check_coerce - Coercion to " << ty);
            context.equate_types(sp, ty,  node_ptr->m_res_type);
            return true;
            ),
        (TraitObject,
            // TODO: Can bare trait objects coerce?
            context.equate_types(sp, ty,  ty_r);
            return true;
            ),
        (Array,
            context.equate_types(sp, ty,  ty_r);
            return true;
            ),
        (Slice,
            context.equate_types(sp, ty,  ty_r);
            return true;
            ),
        (Tuple,
            context.equate_types(sp, ty,  ty_r);
            return true;
            ),
        (Borrow,
            TU_IFLET(::HIR::TypeRef::Data, ty_r.m_data, Borrow, r_e,
                // If using `&mut T` where `&const T` is expected - insert a reborrow (&*)
                if( l_e.type == ::HIR::BorrowType::Shared && r_e.type == ::HIR::BorrowType::Unique ) {
                    context.equate_types(sp, *l_e.inner, *r_e.inner);
                    
                    // Add cast down
                    auto span = node_ptr->span();
                    // *<inner>
                    node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Deref(mv$(span), mv$(node_ptr)));
                    node_ptr->m_res_type = l_e.inner->clone();
                    // &*<inner>
                    node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_UniOp(mv$(span), ::HIR::ExprNode_UniOp::Op::Ref, mv$(node_ptr)));
                    node_ptr->m_res_type = ty.clone();
                    
                    context.m_ivars.mark_change();
                    return true;
                }
                // TODO: &move reboorrowing rules?
                
                if( l_e.type != r_e.type ) {
                    // TODO: This could be allowed if left == Shared && right == Unique (reborrowing)
                    ERROR(sp, E0000, "Type mismatch between " << ty << " and " << ty_r << " - Borrow classes differ");
                }
                
                // - Check for coercions
                return check_coerce_borrow(context, *l_e.inner, *r_e.inner, node_ptr);
            )
            else TU_IFLET(::HIR::TypeRef::Data, ty_r.m_data, Infer, r_e,
                // Leave for now
                if( r_e.ty_class != ::HIR::InferClass::None ) {
                    // ERROR: Must be compatible
                    context.equate_types(sp, ty,  ty_r);
                    BUG(sp, "Type error expected " << ty << " == " << ty_r);
                }
                
                context.possible_equate_type(r_e.index, ty);
                return false;
            )
            else {
                // Error - Must be compatible
                // - Hand off to equate
                context.equate_types(sp, ty,  ty_r);
                BUG(sp, "Type error expected " << ty << " == " << ty_r);
            }
            ),
        (Pointer,
            // TODO: Pointers coerce from borrows and similar pointers
            TU_IFLET(::HIR::TypeRef::Data, ty_r.m_data, Borrow, r_e,
                context.equate_types(sp, *l_e.inner, *r_e.inner);
                return true;
            )
            else TU_IFLET(::HIR::TypeRef::Data, ty_r.m_data, Pointer, r_e,
                // If using `*mut T` where `*const T` is expected - add cast
                if( l_e.type == ::HIR::BorrowType::Shared && r_e.type == ::HIR::BorrowType::Unique ) {
                    context.equate_types(sp, *l_e.inner, *r_e.inner);
                    
                    // Add cast down
                    auto span = node_ptr->span();
                    node_ptr->m_res_type = ty_r.clone();
                    node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Cast( mv$(span), mv$(node_ptr), ty.clone() ));
                    node_ptr->m_res_type = ty.clone();
                    
                    context.m_ivars.mark_change();
                    return true;
                }
                
                if( l_e.type != r_e.type ) {
                    ERROR(sp, E0000, "Type mismatch between " << ty << " and " << ty_r << " - Pointer mutability differs");
                }
                context.equate_types(sp, *l_e.inner, *r_e.inner);
                return true;
            )
            else TU_IFLET(::HIR::TypeRef::Data, ty_r.m_data, Infer, r_e,
                if( r_e.ty_class != ::HIR::InferClass::None ) {
                    // ERROR: Must be compatible
                    context.equate_types(sp, ty,  ty_r);
                    BUG(sp, "Type error expected " << ty << " == " << ty_r);
                }
                // Can't do much for now
                context.possible_equate_type(r_e.index, ty);
                return false;
            )
            else {
                // Error: Must be compatible
                context.equate_types(sp, ty,  ty_r);
                BUG(sp, "Type error expected " << ty << " == " << ty_r);
            }
            ),
        (Function,
            // TODO: Could capture-less closures coerce to fn() types?
            TODO(sp, "check_coerce - Coercion to " << ty);
            ),
        (Closure,
            context.equate_types(sp, ty,  node_ptr->m_res_type);
            return true;
            )
        )
        
        TODO(sp, "Typecheck_Code_CS - Coercion " << context.m_ivars.fmt_type(ty) << " from " << context.m_ivars.fmt_type(node_ptr->m_res_type));
        return false;
    }
    
    bool check_associated(Context& context, const Context::Associated& v)
    {
        const auto& sp = v.span;
        TRACE_FUNCTION_F(v);
        
        ::HIR::TypeRef  possible_impl_ty;
        ::HIR::PathParams   possible_params;
        ::HIR::TypeRef  output_type;
        
        // Search for ops trait impl
        unsigned int count = 0;
        DEBUG("Searching for impl " << v.trait << v.params << " for " << context.m_ivars.fmt_type(v.impl_ty));
        bool found = context.m_resolve.find_trait_impls(sp, v.trait, v.params,  v.impl_ty,
            [&](const auto& impl_ty, const auto& args, const auto& assoc) {
                assert( args.m_types.size() == v.params.m_types.size() );
                ::HIR::Compare cmp = impl_ty.compare_with_placeholders(sp, v.impl_ty, context.m_ivars.callback_resolve_infer());
                for( unsigned int i = 0; i < args.m_types.size(); i ++ )
                {
                    const auto& impl_ty = args.m_types[i];
                    const auto& rule_ty = v.params.m_types[i];
                    cmp &= impl_ty.compare_with_placeholders(sp, rule_ty, context.m_ivars.callback_resolve_infer());
                }
                if( cmp == ::HIR::Compare::Unequal ) {
                    DEBUG("- (fail) bounded impl " << v.trait << v.params << " (ty_right = " << context.m_ivars.fmt_type(v.impl_ty));
                    return false;
                }
                count += 1;
                if( cmp == ::HIR::Compare::Equal ) {
                    if( v.name[0] ) {
                        output_type = assoc.at(v.name).clone();
                    }
                    return true;
                }
                else {
                    if( possible_impl_ty == ::HIR::TypeRef() ) {
                        possible_impl_ty = impl_ty.clone();
                        possible_params = args.clone();
                        if( v.name[0] ) {
                            output_type = assoc.at(v.name).clone();
                        }
                    }
                    
                    return false;
                }
            });
        if( found ) {
            // Fully-known impl
            if( v.name[0] ) {
                context.equate_types(sp, v.left_ty, output_type);
            }
            return true;
        }
        else if( count == 0 ) {
            // No applicable impl
            // - TODO: This should really only fire when there isn't an impl. But it currently fires when _
            DEBUG("No impl of " << v.trait << v.params << " for " << v.impl_ty);
            if( !context.m_ivars.type_contains_ivars(v.impl_ty) ) {
                ERROR(sp, E0000, "Failed to find an impl of " << v.trait << v.params << " for " << context.m_ivars.fmt_type(v.impl_ty));
            }
            return false;
        }
        else if( count == 1 ) {
            DEBUG("Only one impl " << v.trait << possible_params << " for " << possible_impl_ty << " - params=" << possible_params << ", ty=" << possible_impl_ty << ", out=" << output_type);
            // Only one possible impl
            if( v.name[0] ) {
                context.equate_types(sp, v.left_ty, output_type);
            }
            assert( possible_impl_ty != ::HIR::TypeRef() );
            context.equate_types(sp, v.impl_ty, possible_impl_ty);
            for( unsigned int i = 0; i < possible_params.m_types.size(); i ++ ) {
                context.equate_types(sp, v.params.m_types[i], possible_params.m_types[i]);
            }
            return true;
        }
        else {
            // Multiple possible impls, don't know yet
            DEBUG("Multiple impls");
            return false;
        }
    }
}


void Typecheck_Code_CS(const typeck::ModuleState& ms, t_args& args, const ::HIR::TypeRef& result_type, ::HIR::ExprPtr& expr)
{
    TRACE_FUNCTION;
    
    auto root_ptr = expr.into_unique();
    Context context { ms.m_crate, ms.m_impl_generics, ms.m_item_generics };
    
    for( auto& arg : args ) {
        context.add_binding( Span(), arg.first, arg.second );
    }

    // - Build up ruleset from node tree
    {
        ExprVisitor_Enum    visitor(context, ms.m_traits, result_type);
        context.add_ivars(root_ptr->m_res_type);
        root_ptr->visit(visitor);
        
        //context.equate_types(expr->span(), result_type, root_ptr->m_res_type);
        context.equate_types_coerce(expr->span(), result_type, root_ptr);
    }
    
    const unsigned int MAX_ITERATIONS = 100;
    unsigned int count = 0;
    while( context.take_changed() && context.has_rules() && count < MAX_ITERATIONS )
    {
        TRACE_FUNCTION_F("=== PASS " << count << " ===");
        context.dump();
        
        // 1. Check coercions for ones that cannot coerce due to RHS type (e.g. `str` which doesn't coerce to anything)
        // 2. (???) Locate coercions that cannot coerce (due to being the only way to know a type)
        // - Keep a list in the ivar of what types that ivar could be equated to.
        DEBUG("--- Coercion checking");
        for(auto it = context.link_coerce.begin(); it != context.link_coerce.end(); ) {
            if( check_coerce(context, *it) ) {
                DEBUG("- Consumed coercion " << it->left_ty << " := " << (**it->right_node_ptr).m_res_type);
                it = context.link_coerce.erase(it);
            }
            else {
                ++ it;
            }
        }
        // 3. Check associated type rules
        DEBUG("--- Associated types");
        for(auto it = context.link_assoc.begin(); it != context.link_assoc.end(); ) {
            if( check_associated(context, *it) ) {
                DEBUG("- Consumed associated type rule - " << *it);
                it = context.link_assoc.erase(it);
            }
            else {
                ++ it;
            }
        }
        // 4. Revisit nodes that require revisiting
        DEBUG("--- Node revisits");
        for( auto it = context.to_visit.begin(); it != context.to_visit.end(); )
        {
            ::HIR::ExprNode& node = **it;
            ExprVisitor_Revisit visitor { context };
            node.visit( visitor );
            //  - If the node is completed, remove it
            if( visitor.node_completed() ) {
                DEBUG("- Completed " << &node << " - " << typeid(node).name());
                it = context.to_visit.erase(it);
            }
            else {
                ++ it;
            }
        }
        
        // Check the possible equations
        DEBUG("--- IVar possibilities");
        unsigned int i = 0;
        for(auto& ivar_ent : context.possible_ivar_vals)
        {
            if( ivar_ent.types.size() == 0 ) {
                // No idea! (or unused)
                i ++ ;
                continue ;
            }
            else if( ivar_ent.types.size() > 1 ) {
                // De-duplicate list (taking into account other ivars)
                for( auto it = ivar_ent.types.begin(); it != ivar_ent.types.end(); )
                {
                    bool found = false;
                    for( auto it2 = ivar_ent.types.begin(); it2 != it; ++ it2 ) {
                        //if( context.m_ivars.types_equal( **it, **it2 ) ) {
                        if( context.m_ivars.types_equal( *it, *it2 ) ) {
                            found = true;
                            break;
                        }
                    }
                    if( found ) {
                        it = ivar_ent.types.erase(it);
                    }
                    else {
                        ++ it;
                    }
                }
            }
            else {
                // One possibility, no need to dedup
            }
            
            ::HIR::TypeRef  ty_l_ivar;
            ty_l_ivar.m_data.as_Infer().index = i;
            const auto& ty_l = context.m_ivars.get_type(ty_l_ivar);
            
            if( !ty_l.m_data.is_Infer() ) {
                DEBUG("- IVar " << ty_l << " had possibilities, but was known");
            }
            else if( ivar_ent.types.size() == 1 ) {
                //const ::HIR::TypeRef& ty_r = ivar_ent.types[0];
                const ::HIR::TypeRef& ty_r = ivar_ent.types[0];
                // Only one possibility
                DEBUG("- IVar " << ty_l << " = " << ty_r);
                context.equate_types(Span(), ty_l, ty_r);
            }
            else {
            }
            
            ivar_ent.types.clear();
            i ++ ;
        }
        
        count ++;
        context.m_resolve.compact_ivars(context.m_ivars);
    }
    
    // - Validate typeck
    expr = ::HIR::ExprPtr( mv$(root_ptr) );
    {
        DEBUG("==== VALIDATE ====");
        context.dump();
        
        ExprVisitor_Apply   visitor { context.m_ivars };
        expr->visit( visitor );
    }
}

