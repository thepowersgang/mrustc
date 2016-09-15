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

namespace {
    inline HIR::ExprNodeP mk_exprnodep(HIR::ExprNode* en, ::HIR::TypeRef ty){ en->m_res_type = mv$(ty); return HIR::ExprNodeP(en); }
}
#define NEWNODE(TY, SP, CLASS, ...)  mk_exprnodep(new HIR::ExprNode##CLASS(SP ,## __VA_ARGS__), TY)

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
            os << v.left_ty << " := " << v.right_node_ptr << " " << &**v.right_node_ptr << " (" << (*v.right_node_ptr)->m_res_type << ")";
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
        ::std::string   name;   // if "", no type is used (and left is ignored) - Just does trait selection
        
        // HACK: operators are special - the result when both types are primitives is ALWAYS the lefthand side
        bool    is_operator;
        
        friend ::std::ostream& operator<<(::std::ostream& os, const Associated& v) {
            if( v.name == "" ) {
                os << "req ty " << v.impl_ty << " impl " << v.trait << v.params;
            }
            else {
                os << v.left_ty << " = " << "< `" << v.impl_ty << "` as `" << v.trait << v.params << "` >::" << v.name;
            }
            return os;
        }
    };
    
    struct IVarPossible
    {
        bool force_no = false;
        // TODO: If an ivar is eliminated (i.e. has its type dropped) while its pointer is here - things will break
        //::std::vector<const ::HIR::TypeRef*>    types;
        ::std::vector<::HIR::TypeRef>    types_to;
        ::std::vector<::HIR::TypeRef>    types_from;
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
    
    inline void add_ivars(::HIR::TypeRef& ty) {
        m_ivars.add_ivars(ty);
    }
    // - Equate two types, with no possibility of coercion
    //  > Errors if the types are incompatible.
    //  > Forces types if one side is an infer
    void equate_types(const Span& sp, const ::HIR::TypeRef& l, const ::HIR::TypeRef& r);
    // - Equate two types, allowing inferrence
    void equate_types_coerce(const Span& sp, const ::HIR::TypeRef& l, ::HIR::ExprNodeP& node_ptr);
    // - Mark a type as having an unknown coercion (for this round)
    void equate_types_shadow(const Span& sp, const ::HIR::TypeRef& l);
    // - Equate a type to an associated type (if name == "", no equation is done, but trait is searched)
    void equate_types_assoc(const Span& sp, const ::HIR::TypeRef& l,  const ::HIR::SimplePath& trait, ::std::vector< ::HIR::TypeRef> ty_args, const ::HIR::TypeRef& impl_ty, const char *name, bool is_op=false);

    // - List `t` as a possible type for `ivar_index`
    void possible_equate_type_to(unsigned int ivar_index, const ::HIR::TypeRef& t);
    void possible_equate_type_from(unsigned int ivar_index, const ::HIR::TypeRef& t);
    // Mark an ivar as having an unknown possibility
    void possible_equate_type_disable(unsigned int ivar_index);
    void possible_equate_type(unsigned int ivar_index, const ::HIR::TypeRef& t, bool is_to);
    
    // - Add a pattern binding (forcing the type to match)
    void add_binding(const Span& sp, ::HIR::Pattern& pat, const ::HIR::TypeRef& type);
    
    void add_var(unsigned int index, const ::std::string& name, ::HIR::TypeRef type);
    const ::HIR::TypeRef& get_var(const Span& sp, unsigned int idx) const;
    
    // - Add a revisit entry
    void add_revisit(::HIR::ExprNode& node);

    const ::HIR::TypeRef& get_type(const ::HIR::TypeRef& ty) const { return m_ivars.get_type(ty); }
        
    /// Create an autoderef operation from val_node->m_res_type to ty_dst (handling implicit unsizing)
    ::HIR::ExprNodeP create_autoderef(::HIR::ExprNodeP val_node, ::HIR::TypeRef ty_dst) const;
    
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
        TRACE_FUNCTION_FR(path, path);
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
            
            // Add a bound requiring the Self type impl the trait
            context.equate_types_assoc(sp, ::HIR::TypeRef(), e.trait.m_path, mv$(e.trait.m_params.clone().m_types), *e.type, "");
            
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
            // TODO: Eventually, the HIR `Resolve UFCS` pass will be removed, leaving this code responsible for locating the item.
            TODO(sp, "Hit a UfcsUnknown (" << path << ") - Is this an error?");
            ),
        (UfcsInherent,
            // TODO: What if this types has ivars?
            // - Locate function (and impl block)
            const ::HIR::TypeImpl* impl_ptr = nullptr;
            context.m_crate.find_type_impls(*e.type, context.m_ivars.callback_resolve_infer(),
                [&](const auto& impl) {
                    DEBUG("- impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                    auto it = impl.m_methods.find(e.item);
                    if( it == impl.m_methods.end() )
                        return false;
                    fcn_ptr = &it->second.data;
                    impl_ptr = &impl;
                    return true;
                });
            if( !fcn_ptr ) {
                ERROR(sp, E0000, "Failed to locate function " << path);
            }
            assert(impl_ptr);
            DEBUG("Found impl" << impl_ptr->m_params.fmt_args() << " " << impl_ptr->m_type);
            fix_param_count(sp, context, path, fcn_ptr->m_params,  e.params);
            cache.m_fcn_params = &fcn_ptr->m_params;
            
            
            // If the impl block has parameters, figure out what types they map to
            // - The function params are already mapped (from fix_param_count)
            auto& impl_params = cache.m_ty_impl_params;
            if( impl_ptr->m_params.m_types.size() > 0 )
            {
                // Default-construct entires in the `impl_params` array
                impl_params.m_types.resize( impl_ptr->m_params.m_types.size() );
                
                auto cmp = impl_ptr->m_type.match_test_generics_fuzz(sp, *e.type, context.m_ivars.callback_resolve_infer(), [&](auto idx, const auto& ty) {
                    assert( idx < impl_params.m_types.size() );
                    impl_params.m_types[idx] = ty.clone();
                    return ::HIR::Compare::Equal;
                    });
                if( cmp == ::HIR::Compare::Fuzzy )
                {
                    // If the match was fuzzy, it could be due to a compound being matched against an ivar
                    DEBUG("- Fuzzy match, adding ivars and equating");
                    bool ivar_replaced = false;
                    for(auto& ty : impl_params.m_types) {
                        if( ty == ::HIR::TypeRef() ) {
                            // Allocate a new ivar for the param
                            ivar_replaced = true;
                            ty = context.m_ivars.new_ivar_tr();
                        }
                    }
                    
                    // If there were any params that weren't bound by `match_test_generics_fuzz`
                    if( ivar_replaced )
                    {
                        // Then monomorphise the impl type with the new ivars, and equate to *e.type
                        auto impl_monomorph_cb = [&](const auto& gt)->const auto& {
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
                            else {
                                BUG(sp, "Generic bounding out of total range - " << ge.binding);
                            }
                            };
                        auto impl_ty_mono = monomorphise_type_with(sp, impl_ptr->m_type, impl_monomorph_cb, false);
                        DEBUG("- impl_ty_mono = " << impl_ty_mono);
                        
                        context.equate_types(sp, impl_ty_mono, *e.type);
                    }
                }
                // - Check that all entries were populated by the above function
                for(const auto& ty : impl_params.m_types) {
                    assert( ty != ::HIR::TypeRef() );
                }
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
                    else if( ge.binding < 256*3 ) {
                        auto idx = ge.binding - 256*2;
                        TODO(sp, "Placeholder generics - " << idx);
                    }
                    else {
                        BUG(sp, "Generic bounding out of total range - " << ge.binding);
                    }
                };
            )
        )

        assert( fcn_ptr );
        cache.m_fcn = fcn_ptr;
        const auto& fcn = *fcn_ptr;
        
        // --- Monomorphise the argument/return types (into current context)
        for(const auto& arg : fcn.m_args) {
            DEBUG("Arg " << arg.first << ": " << arg.second);
            if( monomorphise_type_needed(arg.second) ) {
                cache.m_arg_types.push_back( /*this->context.expand_associated_types(sp, */monomorphise_type_with(sp, arg.second,  monomorph_cb, false)/*)*/ );
            }
            else {
                cache.m_arg_types.push_back( arg.second.clone() );
            }
        }
        DEBUG("Ret " << fcn.m_return);
        if( monomorphise_type_needed(fcn.m_return) ) {
            cache.m_arg_types.push_back( /*this->context.expand_associated_types(sp, */monomorphise_type_with(sp, fcn.m_return,  monomorph_cb, false)/*)*/ );
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
                
                const auto& trait_path = be.trait.m_path.m_path;
                context.equate_types_assoc(sp, ::HIR::TypeRef(), trait_path, mv$(trait_params.clone().m_types), real_type, "");
                
                // TODO: Either - Don't include the above impl bound, or change the below trait to the one that has that type
                for( const auto& assoc : be.trait.m_type_bounds ) {
                    ::HIR::GenericPath  type_trait_path;
                    ASSERT_BUG(sp, be.trait.m_trait_ptr, "Trait pointer not set in " << be.trait.m_path);
                    context.m_resolve.trait_contains_type(sp, real_trait, *be.trait.m_trait_ptr, assoc.first,  type_trait_path);
                    
                    auto other_ty = monomorphise_type_with(sp, assoc.second, cache.m_monomorph_cb, true);
                    
                    context.equate_types_assoc(sp, other_ty,  type_trait_path.m_path, mv$(type_trait_path.m_params.m_types), real_type, assoc.first.c_str());
                }
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
        ::std::vector< const ::HIR::TypeRef*>   closure_ret_types;
        
        ::std::vector<bool> inner_coerce_enabled_stack;
        
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
            
            const auto is_diverge = [&](const ::HIR::TypeRef& rty)->bool {
                const auto& ty = this->context.get_type(rty);
                //DEBUG("[visit(_Block) is_diverge] " << rty << " = " << ty);
                return ty.m_data.is_Diverge() || (ty.m_data.is_Infer() && ty.m_data.as_Infer().ty_class == ::HIR::InferClass::Diverge);
                };
            
            if( node.m_nodes.size() > 0 )
            {
                bool diverges = false;
                this->push_traits( node.m_traits );
                
                this->push_inner_coerce(false);
                for( unsigned int i = 0; i < node.m_nodes.size()-1; i ++ )
                {
                    auto& snp = node.m_nodes[i];
                    this->context.add_ivars( snp->m_res_type );
                    // TODO: Ignore? or force to ()? - Depends on inner
                    // - Blocks (and block-likes) are forced to ()
                    //  - What if they were '({});'? Then they're left dangling
                    snp->visit(*this);
                    
                    // If this statement yields !, then mark the block as diverging
                    // - TODO: Search the entire type for `!`? (What about pointers to it? or Option/Result?)
                    if( is_diverge(snp->m_res_type) ) {
                        diverges = true;
                    }
                }
                this->pop_inner_coerce();
                
                if( node.m_yields_final )
                {
                    auto& snp = node.m_nodes.back();
                    DEBUG("Block yields final value");
                    this->context.add_ivars( snp->m_res_type );
                    this->context.equate_types(snp->span(), node.m_res_type, snp->m_res_type);
                    snp->visit(*this);
                }
                else
                {
                    auto& snp = node.m_nodes.back();
                    this->context.add_ivars( snp->m_res_type );
                    // - Not yielded - so don't equate the return
                    snp->visit(*this);
                    
                    if( is_diverge(snp->m_res_type) ) {
                        diverges = true;
                    }
                    
                    // If a statement in this block diverges
                    if( diverges ) {
                        DEBUG("Block diverges, yield !");
                        this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_diverge());
                    }
                    else {
                        DEBUG("Block doesn't diverge but doesn't yield a value, yield ()");
                        this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
                    }
                }
                
                this->pop_traits( node.m_traits );
            }
            else
            {
                // Result should be `()`
                DEBUG("Block is empty, yield ()");
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
            }
        }
        void visit(::HIR::ExprNode_Return& node) override
        {
            TRACE_FUNCTION_F(&node << " return ...");
            this->context.add_ivars( node.m_value->m_res_type );

            const auto& ret_ty = ( this->closure_ret_types.size() > 0 ? *this->closure_ret_types.back() : this->ret_type );
            this->context.equate_types_coerce(node.span(), ret_ty, node.m_value);
            
            this->push_inner_coerce( true );
            node.m_value->visit( *this );
            this->pop_inner_coerce();
        }
        
        void visit(::HIR::ExprNode_Loop& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);
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
            
            if( node.m_value )
            {
                this->context.add_ivars( node.m_value->m_res_type );
                // If the type was omitted or was just `_`, equate
                if( node.m_type.m_data.is_Infer() ) {
                    this->context.equate_types( node.span(), node.m_type, node.m_value->m_res_type );
                    #if 0
                    this->push_inner_coerce(false);
                    #else
                    this->push_inner_coerce(true);
                    #endif
                }
                // otherwise coercions apply
                else {
                    this->context.equate_types_coerce( node.span(), node.m_type, node.m_value );
                    this->push_inner_coerce(true);
                }
                
                node.m_value->visit( *this );
                this->pop_inner_coerce();
            }
        }
        void visit(::HIR::ExprNode_Match& node) override
        {
            TRACE_FUNCTION_F(&node << " match ...");
            
            const auto& val_type = node.m_value->m_res_type;

            {
                auto _ = this->push_inner_coerce_scoped(false);
                this->context.add_ivars(node.m_value->m_res_type);
                // TODO: If a coercion point is placed here, it will allow `match &string { "..." ... }`
                node.m_value->visit( *this );
            }
            
            for(auto& arm : node.m_arms)
            {
                DEBUG("ARM " << arm.m_patterns);
                for(auto& pat : arm.m_patterns)
                {
                    this->context.add_binding(node.span(), pat, val_type);
                }
                
                if( arm.m_cond )
                {
                    auto _ = this->push_inner_coerce_scoped(false);
                    this->context.add_ivars( arm.m_cond->m_res_type );
                    this->context.equate_types(arm.m_cond->span(), ::HIR::TypeRef(::HIR::CoreType::Bool), arm.m_cond->m_res_type);
                    arm.m_cond->visit( *this );
                }
                
                this->context.add_ivars( arm.m_code->m_res_type );
                this->equate_types_inner_coerce(node.span(), node.m_res_type, arm.m_code);
                arm.m_code->visit( *this );
            }
        }
        
        void visit(::HIR::ExprNode_If& node) override
        {
            TRACE_FUNCTION_F(&node << " if ...");
            
            this->context.add_ivars( node.m_cond->m_res_type );
            
            {
                auto _ = this->push_inner_coerce_scoped(false);
                this->context.equate_types(node.m_cond->span(), ::HIR::TypeRef(::HIR::CoreType::Bool), node.m_cond->m_res_type);
                node.m_cond->visit( *this );
            }
            
            this->context.add_ivars( node.m_true->m_res_type );
            this->equate_types_inner_coerce(node.span(), node.m_res_type, node.m_true);
            node.m_true->visit( *this );
            
            if( node.m_false ) {
                this->context.add_ivars( node.m_false->m_res_type );
                this->equate_types_inner_coerce(node.span(), node.m_res_type, node.m_false);
                node.m_false->visit( *this );
            }
            else {
                this->context.equate_types(node.span(), node.m_res_type, ::HIR::TypeRef::new_unit());
            }
        }
        
        
        void visit(::HIR::ExprNode_Assign& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);
            
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
            
            auto _2 = this->push_inner_coerce_scoped( node.m_op == ::HIR::ExprNode_Assign::Op::None );
            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_BinOp& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);
            
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
                
                // NOTE: `true` marks the association as coming from a binary operation, which changes integer handling
                this->context.equate_types_assoc(node.span(), node.m_res_type,  op_trait, ::make_vec1(node.m_right->m_res_type.clone()), node.m_left->m_res_type.clone(), "Output", true);
                break; }
            }
            node.m_left ->visit( *this );
            node.m_right->visit( *this );
        }
        void visit(::HIR::ExprNode_UniOp& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);
            
            TRACE_FUNCTION_F(&node << " " << ::HIR::ExprNode_UniOp::opname(node.m_op) << "...");
            this->context.add_ivars( node.m_value->m_res_type );
            switch(node.m_op)
            {
            case ::HIR::ExprNode_UniOp::Op::Invert:
                this->context.equate_types_assoc(node.span(), node.m_res_type,  this->context.m_crate.get_lang_item_path(node.span(), "not"), {}, node.m_value->m_res_type.clone(), "Output", true);
                break;
            case ::HIR::ExprNode_UniOp::Op::Negate:
                this->context.equate_types_assoc(node.span(), node.m_res_type,  this->context.m_crate.get_lang_item_path(node.span(), "neg"), {}, node.m_value->m_res_type.clone(), "Output", true);
                break;
            }
            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_Borrow& node) override
        {
            TRACE_FUNCTION_F(&node << " &_ ...");
            this->context.add_ivars( node.m_value->m_res_type );
            
            // TODO: Can Ref/RefMut trigger coercions?
            this->context.equate_types( node.span(), node.m_res_type,  ::HIR::TypeRef::new_borrow(node.m_type, node.m_value->m_res_type.clone()) );
            
            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_Cast& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);
            
            TRACE_FUNCTION_F(&node << " ... as " << node.m_res_type);
            this->context.add_ivars( node.m_value->m_res_type );
            
            // TODO: Only revisit if the cast type requires inferring.
            this->context.add_revisit(node);
            
            node.m_value->visit( *this );
        }
        void visit(::HIR::ExprNode_Unsize& node) override
        {
            BUG(node.span(), "Hit _Unsize");
        }
        void visit(::HIR::ExprNode_Index& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);
            
            TRACE_FUNCTION_F(&node << " ... [ ... ]");
            this->context.add_ivars( node.m_value->m_res_type );
            this->context.add_ivars( node.m_index->m_res_type );
            
            this->context.add_revisit(node);
            
            node.m_value->visit( *this );
            node.m_index->visit( *this );
        }
        void visit(::HIR::ExprNode_Deref& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);
            
            TRACE_FUNCTION_F(&node << " *...");
            this->context.add_ivars( node.m_value->m_res_type );
            
            this->context.add_revisit(node);

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
                const auto* des_ty = &des_ty_r;
                if( monomorphise_type_needed(des_ty_r) ) {
                    node.m_arg_types[i] = monomorphise_type_with(sp, des_ty_r, monomorph_cb);
                    des_ty = &node.m_arg_types[i];
                }
                
                this->context.equate_types_coerce(node.span(), *des_ty,  node.m_args[i]);
            }
            
            auto _ = this->push_inner_coerce_scoped(true);
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
            if( node.m_base_value ) {
                this->context.add_ivars( node.m_base_value->m_res_type );
            }
            
            // - Create ivars in path, and set result type
            const auto ty = this->get_structenum_ty(node.span(), node.m_is_struct, node.m_path);
            this->context.equate_types(node.span(), node.m_res_type, ty);
            if( node.m_base_value ) {
                this->context.equate_types(node.span(), node.m_base_value->m_res_type, ty);
            }
            
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
                const auto* des_ty = &des_ty_r;
                
                DEBUG(name << " : " << des_ty_r);
                if( monomorphise_type_needed(des_ty_r) ) {
                    if( des_ty_cache == ::HIR::TypeRef() ) {
                        des_ty_cache = monomorphise_type_with(node.span(), des_ty_r, monomorph_cb);
                    }
                    else {
                        // TODO: Is it an error when it's already populated?
                    }
                    des_ty = &des_ty_cache;
                }
                this->equate_types_inner_coerce(node.span(), *des_ty,  val.second);
            }
            
            auto _ = this->push_inner_coerce_scoped(true);
            for( auto& val : node.m_values ) {
                val.second->visit( *this );
            }
            if( node.m_base_value ) {
                auto _ = this->push_inner_coerce_scoped(false);
                node.m_base_value->visit( *this );
            }
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override
        {
            TRACE_FUNCTION_F(&node << " " << node.m_path << " [" << (node.m_is_struct ? "struct" : "enum") << "]");
            
            // TODO: Check?
            
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

            auto _ = this->push_inner_coerce_scoped(true);
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

            {
                auto _ = this->push_inner_coerce_scoped(false);
                node.m_value->visit( *this );
            }
            auto _ = this->push_inner_coerce_scoped(true);
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
            
            {
                auto _ = this->push_inner_coerce_scoped(false);
                node.m_value->visit( *this );
            }
            auto _ = this->push_inner_coerce_scoped(true);
            for( auto& val : node.m_args ) {
                val->visit( *this );
            }
        }
        void visit(::HIR::ExprNode_Field& node) override
        {
            auto _ = this->push_inner_coerce_scoped(false);
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
            
            if( can_coerce_inner_result() ) {
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
                this->equate_types_inner_coerce(node.span(), inner_ty,  val);
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
            this->equate_types_inner_coerce(node.span(), inner_ty, node.m_val);
            this->context.equate_types(node.span(), ::HIR::TypeRef(::HIR::CoreType::Usize), node.m_size->m_res_type);
            
            node.m_val->visit( *this );
            node.m_size->visit( *this );
        }
        
        void visit(::HIR::ExprNode_Literal& node) override
        {
            TU_MATCH(::HIR::ExprNode_Literal::Data, (node.m_data), (e),
            (Integer,
                DEBUG(" (: " << e.m_type << " = " << e.m_value << ")");
                assert(node.m_res_type.m_data.is_Primitive() || node.m_res_type.m_data.as_Infer().ty_class == ::HIR::InferClass::Integer);
                ),
            (Float,
                DEBUG(" (: " << node.m_res_type << " = " << e.m_value << ")");
                assert(node.m_res_type.m_data.is_Primitive() || node.m_res_type.m_data.as_Infer().ty_class == ::HIR::InferClass::Float);
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
                    BUG(sp, "_PathValue with target=UNKNOWN and a Generic path - " << e.m_path);
                case ::HIR::ExprNode_PathValue::FUNCTION: {
                    const auto& f = this->context.m_crate.get_function_by_path(sp, e.m_path);
                    fix_param_count(sp, this->context, e, f.m_params, e.m_params);
                    
                    ::HIR::FunctionType ft {
                        f.m_unsafe,
                        f.m_abi,
                        box$( monomorphise_type(sp, f.m_params, e.m_params, f.m_return) ),
                        {}
                        };
                    for( const auto& arg : f.m_args )
                    {
                        ft.m_arg_types.push_back( monomorphise_type(sp, f.m_params, e.m_params, arg.second) );
                    }
                    
                    auto ty = ::HIR::TypeRef( ::HIR::TypeRef::Data::make_Function(mv$(ft)) );
                    this->context.equate_types(sp, node.m_res_type, ty);
                    } break;
                case ::HIR::ExprNode_PathValue::STRUCT_CONSTR: {
                    const auto& s = this->context.m_crate.get_struct_by_path(sp, e.m_path);
                    const auto& se = s.m_data.as_Tuple();
                    fix_param_count(sp, this->context, e, s.m_params, e.m_params);
                    
                    ::HIR::FunctionType ft {
                        false,
                        "rust",
                        box$( ::HIR::TypeRef( node.m_path.clone(), ::HIR::TypeRef::TypePathBinding::make_Struct(&s) ) ),
                        {}
                        };
                    for( const auto& arg : se )
                    {
                        ft.m_arg_types.push_back( monomorphise_type(sp, s.m_params, e.m_params, arg.ent) );
                    }
                    
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
                // 1. Add trait bound to be checked.
                this->context.equate_types_assoc(sp, ::HIR::TypeRef(),  e.trait.m_path, mv$(e.trait.m_params.clone().m_types), e.type->clone(), "");
                // 2. Locate this item in the trait
                // - If it's an associated `const`, will have to revisit
                const auto& trait = this->context.m_crate.get_trait_by_path(sp, e.trait.m_path);
                auto it = trait.m_values.find( e.item );
                if( it == trait.m_values.end() ) {
                    ERROR(sp, E0000, "`" << e.item << "` is not a value member of trait " << e.trait.m_path);
                }
                TU_MATCH( ::HIR::TraitValueItem, (it->second), (ie),
                (Constant,
                    TODO(sp, "Monomorpise associated constant type - " << ie.m_type);
                    ),
                (Static,
                    TODO(sp, "Monomorpise associated static type - " << ie.m_type);
                    ),
                (Function,
                    fix_param_count(sp, this->context, node.m_path, ie.m_params,  e.params);
                    
                    const auto& fcn_params = e.params;
                    const auto& trait_params = e.trait.m_params;
                    auto monomorph_cb = [&](const auto& gt)->const auto& {
                            const auto& ge = gt.m_data.as_Generic();
                            if( ge.binding == 0xFFFF ) {
                                return this->context.get_type(*e.type);
                            }
                            else if( ge.binding < 256 ) {
                                auto idx = ge.binding;
                                if( idx >= trait_params.m_types.size() ) {
                                    BUG(sp, "Generic param out of input range - " << idx << " '" << ge.name << "' >= " << trait_params.m_types.size());
                                }
                                return this->context.get_type(trait_params.m_types[idx]);
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
                        ie.m_unsafe, ie.m_abi,
                        box$( monomorphise_type_with(sp, ie.m_return,  monomorph_cb) ),
                        {}
                        };
                    for(const auto& arg : ie.m_args)
                        ft.m_arg_types.push_back( monomorphise_type_with(sp, arg.second,  monomorph_cb) );
                    auto ty = ::HIR::TypeRef(mv$(ft));
                    
                    this->context.equate_types(node.span(), node.m_res_type, ty);
                    )
                )
                ),
            (UfcsInherent,
                // TODO: If ivars are valid within the type of this UFCS, then resolution has to be deferred until iteration
                // - If they're not valid, then resolution can be done here.
                ASSERT_BUG(sp, !this->context.m_ivars.type_contains_ivars(*e.type), "Found ivar in UfcsInherent");
                
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
                        fcn_ptr = &it->second.data;
                        impl_ptr = &impl;
                        return true;
                    });
                if( !fcn_ptr ) {
                    ERROR(sp, E0000, "Failed to locate function " << node.m_path);
                }
                assert(impl_ptr);
                fix_param_count(sp, this->context, node.m_path, fcn_ptr->m_params,  e.params);
                
                // If the impl block has parameters, figure out what types they map to
                // - The function params are already mapped (from fix_param_count)
                ::HIR::PathParams   impl_params;
                if( impl_ptr->m_params.m_types.size() > 0 ) {
                    impl_params.m_types.resize( impl_ptr->m_params.m_types.size() );
                    impl_ptr->m_type.match_generics(sp, *e.type, this->context.m_ivars.callback_resolve_infer(), [&](auto idx, const auto& ty) {
                        assert( idx < impl_params.m_types.size() );
                        impl_params.m_types[idx] = ty.clone();
                        return ::HIR::Compare::Equal;
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
            ::std::vector< ::HIR::TypeRef>  arg_types;
            for(auto& arg : node.m_args) {
                arg_types.push_back( arg.second.clone() );
            }
            this->context.equate_types( node.span(), node.m_res_type, ::HIR::TypeRef::new_closure(&node, mv$(arg_types), node.m_return.clone()) );

            this->context.equate_types_coerce( node.span(), node.m_return, node.m_code );
            
            auto _ = this->push_inner_coerce_scoped(true);
            this->closure_ret_types.push_back( &node.m_return );
            node.m_code->visit( *this );
            this->closure_ret_types.pop_back( );
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
        
        class InnerCoerceGuard {
            ExprVisitor_Enum& t;
        public:
            InnerCoerceGuard(ExprVisitor_Enum& t): t(t) {}
            ~InnerCoerceGuard() {
                t.inner_coerce_enabled_stack.pop_back();
                DEBUG("inner_coerce POP (S) " << t.can_coerce_inner_result());
            }
        };
        InnerCoerceGuard push_inner_coerce_scoped(bool val) {
            DEBUG("inner_coerce PUSH (S) " << val);
            this->inner_coerce_enabled_stack.push_back(val);
            return InnerCoerceGuard(*this);
        }
        void push_inner_coerce(bool val) {
            DEBUG("inner_coerce PUSH " << val);
            this->inner_coerce_enabled_stack.push_back(val);
        }
        void pop_inner_coerce() {
            assert( this->inner_coerce_enabled_stack.size() );
            this->inner_coerce_enabled_stack.pop_back();
            DEBUG("inner_coerce POP " << can_coerce_inner_result());
        }
        bool can_coerce_inner_result() const {
            if( this->inner_coerce_enabled_stack.size() == 0 ) {
                return true;
            }
            else {
                return this->inner_coerce_enabled_stack.back();
            }
        }
        void equate_types_inner_coerce(const Span& sp, const ::HIR::TypeRef& target, ::HIR::ExprNodeP& node) {
            DEBUG("can_coerce_inner_result() = " << can_coerce_inner_result());
            if( can_coerce_inner_result() ) {
                this->context.equate_types_coerce(sp, target,  node);
            }
            else {
                this->context.equate_types(sp, target,  node->m_res_type);
            }
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
        void visit(::HIR::ExprNode_Borrow& node) override {
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
                DEBUG("- Target type is still _");
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
                    ERROR(sp, E0000, "Invalid cast to pointer from " << src_ty);
                    ),
                (Function,
                    if( *e.inner == ::HIR::TypeRef::new_unit() ) {
                        this->m_completed = true;
                    }
                    else {
                        ERROR(sp, E0000, "Invalid cast to pointer from " << src_ty);
                    }
                    ),
                (Primitive,
                    if( s_e != ::HIR::CoreType::Usize ) {
                        ERROR(sp, E0000, "Invalid cast to pointer from " << src_ty);
                    }
                    // TODO: Can't be to a fat pointer though.
                    this->m_completed = true;
                    ),
                (Infer,
                    switch( s_e.ty_class )
                    {
                    case ::HIR::InferClass::Float:
                        ERROR(sp, E0000, "Invalid cast to pointer from floating point literal");
                    case ::HIR::InferClass::Integer:
                        this->context.equate_types(sp, src_ty, ::HIR::TypeRef(::HIR::CoreType::Usize));
                        this->m_completed = true;
                        break;
                    case ::HIR::InferClass::None:
                    case ::HIR::InferClass::Diverge:
                        break;
                    }
                    ),
                (Borrow,
                    // Check class (must be equal) and type
                    // TODO: Check class
                    this->context.equate_types(sp, *e.inner, *s_e.inner);
                    this->m_completed = true;
                    ),
                (Pointer,
                    // Allow with no link?
                    this->m_completed = true;
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
            const auto& lang_Index = this->context.m_crate.get_lang_item_path(node.span(), "index");
            const auto& val_ty = this->context.get_type(node.m_value->m_res_type);
            const auto& idx_ty = this->context.get_type(node.m_index->m_res_type);
            TRACE_FUNCTION_F("Index: val=" << val_ty << ", idx=" << idx_ty << "");
            
            this->context.equate_types_shadow(node.span(), node.m_res_type);
            
            // NOTE: Indexing triggers autoderef
            unsigned int deref_count = 0;
            ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
            const auto* current_ty = &node.m_value->m_res_type;
            ::std::vector< ::HIR::TypeRef>  deref_res_types;
            
            // TODO: (CHECK) rustc doesn't use the index value type when finding the indexable item, mrustc does.
            ::HIR::PathParams   trait_pp;
            trait_pp.m_types.push_back( idx_ty.clone() );
            do {
                const auto& ty = this->context.get_type(*current_ty);
                DEBUG("(Index): (: " << ty << ")[: " << trait_pp.m_types[0] << "]");
                
                ::HIR::TypeRef  possible_index_type;
                ::HIR::TypeRef  possible_res_type;
                unsigned int count = 0;
                bool rv = this->context.m_resolve.find_trait_impls(node.span(), lang_Index, trait_pp, ty, [&](auto impl, auto cmp) {
                    possible_res_type = impl.get_type("Output");
                    count += 1;
                    if( cmp == ::HIR::Compare::Equal ) {
                        return true;
                    }
                    possible_index_type = impl.get_trait_ty_param(0);
                    return false;
                    });
                if( rv ) {
                    // TODO: Node's result type could be an &-ptr?
                    this->context.equate_types(node.span(), node.m_res_type,  possible_res_type);
                    break;
                }
                else if( count == 1 ) {
                    assert( possible_index_type != ::HIR::TypeRef() );
                    this->context.equate_types(node.span(), node.m_index->m_res_type, possible_index_type);
                    this->context.equate_types(node.span(), node.m_res_type,  possible_res_type);
                    break;
                }
                else if( count > 1 ) {
                    // Multiple fuzzy matches, don't keep dereferencing until we know.
                    current_ty = nullptr;
                    break;
                }
                else {
                    // Either no matches, or multiple fuzzy matches
                }
                
                deref_count += 1;
                current_ty = this->context.m_resolve.autoderef(node.span(), ty,  tmp_type);
                if( current_ty )
                    deref_res_types.push_back( current_ty->clone() );
            } while( current_ty );
            
            if( current_ty )
            {
                DEBUG("Found impl on type " << *current_ty << " with " << deref_count << " derefs");
                assert( deref_count == deref_res_types.size() );
                for(auto& ty_r : deref_res_types)
                {
                    auto ty = mv$(ty_r);
                    
                    node.m_value = this->context.create_autoderef( mv$(node.m_value), mv$(ty) );
                    context.m_ivars.get_type(node.m_value->m_res_type);
                }
                
                m_completed = true;
            }
        }
        void visit(::HIR::ExprNode_Deref& node) override {
            const auto& ty = this->context.get_type(node.m_value->m_res_type);
            TRACE_FUNCTION_F("Deref: ty=" << ty);
            
            TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (e),
            (
                const auto& op_trait = this->context.m_crate.get_lang_item_path(node.span(), "deref");
                this->context.equate_types_assoc(node.span(), node.m_res_type,  op_trait, {}, node.m_value->m_res_type.clone(), "Target");
                ),
            (Infer,
                // Keep trying
                return ;
                ),
            (Borrow,
                // - Not really needed, but this is cheaper.
                this->context.equate_types(node.span(), node.m_res_type, *e.inner);
                ),
            (Pointer,
                // TODO: Figure out if this node is in an unsafe block.
                this->context.equate_types(node.span(), node.m_res_type, *e.inner);
                )
            )
            this->m_completed = true;
        }
        
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_CallPath& node) override {
            no_revisit(node);
        }
        void visit(::HIR::ExprNode_CallValue& node) override {
            const auto& sp = node.span();
            const auto& ty = this->context.get_type(node.m_value->m_res_type);
            TRACE_FUNCTION_F("CallValue: ty=" << ty);
            
            TU_MATCH_DEF(decltype(ty.m_data), (ty.m_data), (e),
            (
                // Search for FnOnce impl
                const auto& lang_FnOnce = this->context.m_crate.get_lang_item_path(node.span(), "fn_once");
                const auto& lang_FnMut  = this->context.m_crate.get_lang_item_path(node.span(), "fn_mut");
                const auto& lang_Fn     = this->context.m_crate.get_lang_item_path(node.span(), "fn");
                
                ::HIR::TypeRef  fcn_args_tup;
                ::HIR::TypeRef  fcn_ret;
                
                // 1. Create a param set with a single tuple (of all argument types)
                ::HIR::PathParams   trait_pp;
                {
                    ::std::vector< ::HIR::TypeRef>  arg_types;
                    for(const auto& arg : node.m_args) {
                        arg_types.push_back( this->context.get_type(arg->m_res_type).clone() );
                    }
                    trait_pp.m_types.push_back( ::HIR::TypeRef( mv$(arg_types) ) );
                }
                
                // 2. Locate an impl of FnOnce (exists for all other Fn* traits)
                auto was_bounded = this->context.m_resolve.find_trait_impls_bound(node.span(), lang_FnOnce, trait_pp, ty, [&](auto impl, auto cmp) {
                        auto tup = impl.get_trait_ty_param(0);
                        if( !tup.m_data.is_Tuple() )
                            ERROR(node.span(), E0000, "FnOnce expects a tuple argument, got " << tup);
                        fcn_args_tup = mv$(tup);
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
                    
                    // 3. Locate the most permissive implemented Fn* trait (Fn first, then FnMut, then assume just FnOnce)
                    // NOTE: Borrowing is added by the expansion to CallPath
                    if( this->context.m_resolve.find_trait_impls_bound(node.span(), lang_Fn, trait_pp, ty, [&](auto , auto cmp) {
                        return true;
                        //return cmp == ::HIR::Compare::Equal;
                        })
                        )
                    {
                        DEBUG("-- Using Fn");
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Fn;
                    }
                    else if( this->context.m_resolve.find_trait_impls_bound(node.span(), lang_FnMut, trait_pp, ty, [&](auto , auto cmp) {
                        return true;
                        //return cmp == ::HIR::Compare::Equal;
                        })
                        )
                    {
                        DEBUG("-- Using FnMut");
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::FnMut;
                    }
                    else
                    {
                        DEBUG("-- Using FnOnce (default)");
                        node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::FnOnce;
                    }
                }
                else if( !ty.m_data.is_Generic() )
                {
                    bool found = this->context.m_resolve.find_trait_impls_crate(node.span(), lang_FnOnce, trait_pp, ty, [&](auto impl, auto cmp) {
                        TODO(node.span(), "Use impl of FnOnce - " << impl);
                        return false;
                        });
                    if( found ) {
                    }
                    TODO(node.span(), "Search crate for implementations of FnOnce for " << ty);
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
                node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Unknown;
                ),
            (Function,
                for( const auto& arg : e.m_arg_types )
                    node.m_arg_types.push_back( arg.clone() );
                node.m_arg_types.push_back( e.m_rettype->clone() );
                node.m_trait_used = ::HIR::ExprNode_CallValue::TraitUsed::Fn;
                ),
            (Borrow,
                // TODO: Locate trait impl via borrow
                TODO(sp, "CallValue on an &-ptr");
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
            const auto& sp = node.span();
            
            const auto& ty = this->context.get_type(node.m_value->m_res_type);
            TRACE_FUNCTION_F("(CallMethod) {" << this->context.m_ivars.fmt_type(ty) << "}." << node.m_method << node.m_params);
            
            // Make sure that no mentioned types are inferred until this method is known
            this->context.equate_types_shadow(node.span(), node.m_res_type);
            for( const auto& arg_node : node.m_args ) {
                this->context.equate_types_shadow(node.span(), arg_node->m_res_type);
            }
            
            // Using autoderef, locate this method on the type
            ::HIR::Path   fcn_path { ::HIR::SimplePath() };
            unsigned int deref_count = this->context.m_resolve.autoderef_find_method(node.span(), node.m_traits, ty, node.m_method,  fcn_path);
            if( deref_count != ~0u )
            {
                DEBUG("- deref_count = " << deref_count << ", fcn_path = " << fcn_path);
                
                node.m_method_path = mv$(fcn_path);
                // NOTE: Steals the params from the node
                TU_MATCH(::HIR::Path::Data, (node.m_method_path.m_data), (e),
                (Generic,
                    ),
                (UfcsUnknown,
                    ),
                (UfcsKnown,
                    e.params = mv$(node.m_params);
                    //fix_param_count(sp, this->context, node.m_method_path, fcn.m_params, e.params);
                    ),
                (UfcsInherent,
                    e.params = mv$(node.m_params);
                    //fix_param_count(sp, this->context, node.m_method_path, fcn.m_params, e.params);
                    )
                )
                visit_call_populate_cache(this->context, node.span(), node.m_method_path, node.m_cache);
                DEBUG("> m_method_path = " << node.m_method_path);
                
                assert( node.m_cache.m_arg_types.size() >= 1);
                
                if( node.m_args.size()+1 != node.m_cache.m_arg_types.size() - 1 ) {
                    ERROR(node.span(), E0000, "Incorrect number of arguments to " << fcn_path);
                }
                DEBUG("- fcn_path=" << node.m_method_path);
                
                // Link arguments
                // 1+ because it's a method call (#0 is Self)
                DEBUG("node.m_cache.m_arg_types = " << node.m_cache.m_arg_types);
                for(unsigned int i = 0; i < node.m_args.size(); i ++)
                {
                    this->context.equate_types_coerce(node.span(), node.m_cache.m_arg_types[1+i], node.m_args[i]);
                }
                this->context.equate_types(node.span(), node.m_res_type,  node.m_cache.m_arg_types.back());
                
                // Add derefs
                if( deref_count > 0 )
                {
                    DEBUG("- Inserting " << deref_count << " dereferences");
                    // Get dereferencing!
                    auto& node_ptr = node.m_value;
                    ::HIR::TypeRef  tmp_ty;
                    const ::HIR::TypeRef*   cur_ty = &node_ptr->m_res_type;
                    while( deref_count-- )
                    {
                        auto span = node_ptr->span();
                        cur_ty = this->context.m_resolve.autoderef(span, *cur_ty, tmp_ty);
                        assert(cur_ty);
                        auto ty = cur_ty->clone();
                        
                        node.m_value = this->context.create_autoderef( mv$(node.m_value), mv$(ty) );
                    }
                }
                
                // Autoref
                {
                    auto receiver_class = node.m_cache.m_fcn->m_receiver;
                    ::HIR::BorrowType   bt;
                    
                    auto& node_ptr = node.m_value;
                    auto span = node_ptr->span();
                    switch(receiver_class)
                    {
                    case ::HIR::Function::Receiver::Free:
                        BUG(sp, "Method call resolved to a free function - " << node.m_method_path);
                    case ::HIR::Function::Receiver::Value:
                        // by value - nothing needs to be added
                        break;
                    case ::HIR::Function::Receiver::BorrowShared: bt = ::HIR::BorrowType::Shared; if(0)
                    case ::HIR::Function::Receiver::BorrowUnique: bt = ::HIR::BorrowType::Unique; if(0)
                    case ::HIR::Function::Receiver::BorrowOwned:  bt = ::HIR::BorrowType::Owned; {
                        // - Add correct borrow operation
                        auto ty = ::HIR::TypeRef::new_borrow(bt, node_ptr->m_res_type.clone());
                        DEBUG("- Ref " << &*node_ptr << " -> " << ty);
                        node_ptr = NEWNODE(mv$(ty), span, _Borrow,  bt, mv$(node_ptr) );
                        } break;
                    case ::HIR::Function::Receiver::Box: {
                        // - Undo a deref (there must have been one?) and ensure that it leads to a Box<Self>
                        auto* deref_ptr = dynamic_cast< ::HIR::ExprNode_Deref*>(&*node_ptr);
                        ASSERT_BUG(sp, deref_ptr != nullptr, "Calling Box receiver method but no deref happened");
                        node_ptr = mv$(deref_ptr->m_value);
                        DEBUG("- Undo deref " << deref_ptr << " -> " << node_ptr->m_res_type);
                        // TODO: Triple-check that the input to the above Deref was a Box (lang="owned_box")
                        //TU_IFLET(::HIR::TypeRef::Data, node_ptr->m_res_type.m_data, Path, e,
                        //)
                        } break;
                    }
                }
                
                this->m_completed = true;
            }
        }
        void visit(::HIR::ExprNode_Field& node) override {
            const auto& field_name = node.m_field;
            TRACE_FUNCTION_F("(Field) name=" << field_name << ", ty = " << this->context.m_ivars.fmt_type(node.m_value->m_res_type));

            this->context.equate_types_shadow(node.span(), node.m_res_type);

            ::HIR::TypeRef  out_type;

            // Using autoderef, locate this field
            unsigned int deref_count = 0;
            ::HIR::TypeRef  tmp_type;   // Temporary type used for handling Deref
            const auto* current_ty = &node.m_value->m_res_type;
            ::std::vector< ::HIR::TypeRef>  deref_res_types;
            
            do {
                const auto& ty = this->context.m_ivars.get_type(*current_ty);
                if( ty.m_data.is_Infer() ) {
                    DEBUG("Hit ivar, returning early");
                    return ;
                }
                if( this->context.m_resolve.find_field(node.span(), ty, field_name, out_type) ) {
                    this->context.equate_types(node.span(), node.m_res_type, out_type);
                    break;
                }
                
                deref_count += 1;
                current_ty = this->context.m_resolve.autoderef(node.span(), ty,  tmp_type);
                if( current_ty )
                    deref_res_types.push_back( current_ty->clone() );
            } while(current_ty);
            
            if( !current_ty )
            {
                ERROR(node.span(), E0000, "Couldn't find the field " << field_name << " in " << this->context.m_ivars.fmt_type(node.m_value->m_res_type));
            }
            
            assert( deref_count == deref_res_types.size() );
            for(unsigned int i = 0; i < deref_res_types.size(); i ++ )
            {
                auto ty = mv$(deref_res_types[i]);
                DEBUG("- Deref " << &*node.m_value << " -> " << ty);
                if( node.m_value->m_res_type.m_data.is_Array() ) {
                    BUG(node.span(), "Field access from array/slice?");
                }
                node.m_value = NEWNODE(mv$(ty), node.span(), _Deref,  mv$(node.m_value));
                context.m_ivars.get_type(node.m_value->m_res_type);
            }
            
            m_completed = true;
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
        const Context& context;
        const HMTypeInferrence& ivars;
    public:
        ExprVisitor_Apply(const Context& context):
            context(context),
            ivars(context.m_ivars)
        {
        }
        void visit_node_ptr(::HIR::ExprPtr& node_ptr)
        {
            auto& node = *node_ptr;
            const char* node_ty = typeid(node).name();
            
            TRACE_FUNCTION_FR(&node << " " << &node << " " << node_ty << " : " << node.m_res_type, node_ty);
            this->check_type_resolved_top(node.span(), node.m_res_type);
            DEBUG(node_ty << " : = " << node.m_res_type);
            
            node_ptr->visit(*this);
            
            for( auto& ty : node_ptr.m_bindings )
                this->check_type_resolved_top(node.span(), ty);
        }
        void visit_node_ptr(::HIR::ExprNodeP& node_ptr) override {
            auto& node = *node_ptr;
            const char* node_ty = typeid(node).name();
            TRACE_FUNCTION_FR(&node << " " << &node << " " << node_ty << " : " << node.m_res_type, node_ty);
            this->check_type_resolved_top(node.span(), node.m_res_type);
            DEBUG(node_ty << " : = " << node.m_res_type);
            ::HIR::ExprVisitorDef::visit_node_ptr(node_ptr);
        }
        
        void visit_pattern(::HIR::Pattern& pat) override {
            TU_MATCH_DEF( ::HIR::Pattern::Data, (pat.m_data), (e),
            (
                ),
            (StructValue,
                // TODO: Clean
                )
            )
            ::HIR::ExprVisitorDef::visit_pattern(pat);
        }
        
        void visit(::HIR::ExprNode_Let& node) override {
            this->check_type_resolved_top(node.span(), node.m_type);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_Closure& node) override {
            for(auto& arg : node.m_args)
                this->check_type_resolved_top(node.span(), arg.second);
            this->check_type_resolved_top(node.span(), node.m_return);
            ::HIR::ExprVisitorDef::visit(node);
        }
        
        void visit(::HIR::ExprNode_CallPath& node) override {
            for(auto& ty : node.m_cache.m_arg_types)
                this->check_type_resolved_top(node.span(), ty);
            this->check_type_resolved_path(node.span(), node.m_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_CallMethod& node) override {
            for(auto& ty : node.m_cache.m_arg_types)
                this->check_type_resolved_top(node.span(), ty);
            this->check_type_resolved_path(node.span(), node.m_method_path);
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_CallValue& node) override {
            for(auto& ty : node.m_arg_types)
                this->check_type_resolved_top(node.span(), ty);
            ::HIR::ExprVisitorDef::visit(node);
        }
        
        void visit(::HIR::ExprNode_PathValue& node) override {
            this->check_type_resolved_path(node.span(), node.m_path);
        }
        void visit(::HIR::ExprNode_StructLiteral& node) override {
            this->check_type_resolved_pp(node.span(), node.m_path.m_params, ::HIR::TypeRef());
            for(auto& ty : node.m_value_types) {
                if( ty != ::HIR::TypeRef() ) {
                    this->check_type_resolved_top(node.span(), ty);
                }
            }
            
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            this->check_type_resolved_pp(node.span(), node.m_path.m_params, ::HIR::TypeRef());
            for(auto& ty : node.m_arg_types) {
                if( ty != ::HIR::TypeRef() ) {
                    this->check_type_resolved_top(node.span(), ty);
                }
            }
            
            ::HIR::ExprVisitorDef::visit(node);
        }
    private:
        void check_type_resolved_top(const Span& sp, ::HIR::TypeRef& ty) const {
            check_type_resolved(sp, ty, ty);
            ty = this->context.m_resolve.expand_associated_types(sp, mv$(ty));
        }
        void check_type_resolved_pp(const Span& sp, ::HIR::PathParams& pp, const ::HIR::TypeRef& top_type) const {
            for(auto& ty : pp.m_types)
                check_type_resolved(sp, ty, top_type);
        }
        void check_type_resolved_path(const Span& sp, ::HIR::Path& path) const {
            auto tmp = ::HIR::TypeRef(path.clone());
            //auto tmp = ::HIR::TypeRef();
            check_type_resolved_path(sp, path, tmp);
        }
        void check_type_resolved_path(const Span& sp, ::HIR::Path& path, const ::HIR::TypeRef& top_type) const {
            TU_MATCH(::HIR::Path::Data, (path.m_data), (pe),
            (Generic,
                check_type_resolved_pp(sp, pe.m_params, top_type);
                ),
            (UfcsInherent,
                check_type_resolved(sp, *pe.type, top_type);
                check_type_resolved_pp(sp, pe.params, top_type);
                ),
            (UfcsKnown,
                check_type_resolved(sp, *pe.type, top_type);
                check_type_resolved_pp(sp, pe.trait.m_params, top_type);
                check_type_resolved_pp(sp, pe.params, top_type);
                ),
            (UfcsUnknown,
                ERROR(sp, E0000, "UfcsUnknown " << path << " left in " << top_type);
                )
            )
        }
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
                check_type_resolved_path(sp, e.path, top_type);
                ),
            (Generic,
                // Leaf - no ivars
                ),
            (TraitObject,
                check_type_resolved_pp(sp, e.m_trait.m_path.m_params, top_type);
                for(auto& marker : e.m_markers) {
                    check_type_resolved_pp(sp, marker.m_params, top_type);
                }
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
    DEBUG("--- Variables");
    for(unsigned int i = 0; i < m_bindings.size(); i ++)
    {
        DEBUG(i << " " << m_bindings[i].name << ": " << this->m_ivars.fmt_type(m_bindings[i].ty));
    }
    DEBUG("--- Ivars");
    m_ivars.dump();
    DEBUG("--- CS Context - " << link_coerce.size() << " Coercions, " << link_assoc.size() << " associated, " << to_visit.size() << " nodes");
    for(const auto& v : link_coerce) {
        DEBUG(v);
    }
    for(const auto& v : link_assoc) {
        DEBUG(v);
    }
    for(const auto& v : to_visit) {
        DEBUG(&*v << " " << typeid(*v).name());
    }
    DEBUG("---");
}

void Context::equate_types(const Span& sp, const ::HIR::TypeRef& li, const ::HIR::TypeRef& ri) {
    // Instantly apply equality
    TRACE_FUNCTION_F(li << " == " << ri);

    // Check if the type contains a replacable associated type
    ::HIR::TypeRef  l_tmp;
    ::HIR::TypeRef  r_tmp;
    const auto& l_t = this->m_resolve.expand_associated_types(sp, this->m_ivars.get_type(li), l_tmp);
    const auto& r_t = this->m_resolve.expand_associated_types(sp, this->m_ivars.get_type(ri), r_tmp);
    
    #if 0
    if( l_t.m_data.is_Diverge() || r_t.m_data.is_Diverge() ) {
        return ;
    }
    #endif
    
    // If either side is still a UfcsUnkonw after `expand_associated_types`, then emit an assoc bound instead of damaging ivars
    TU_IFLET(::HIR::TypeRef::Data, r_t.m_data, Path, r_e,
        TU_IFLET(::HIR::Path::Data, r_e.path.m_data, UfcsKnown, rpe,
            if( r_e.binding.is_Unbound() ) {
                this->equate_types_assoc(sp, l_t,  rpe.trait.m_path, rpe.trait.m_params.clone().m_types, *rpe.type,  rpe.item.c_str());
                return ;
            }
        )
    )
    TU_IFLET(::HIR::TypeRef::Data, l_t.m_data, Path, l_e,
        TU_IFLET(::HIR::Path::Data, l_e.path.m_data, UfcsKnown, lpe,
            if( l_e.binding.is_Unbound() ) {
                this->equate_types_assoc(sp, r_t,  lpe.trait.m_path, lpe.trait.m_params.clone().m_types, *lpe.type,  lpe.item.c_str());
                return ;
            }
        )
    )
    
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
            #if 1
            if( l_t.m_data.is_Diverge() && r_t.m_data.is_Diverge() ) {
                return ;
            }
            else if( l_t.m_data.is_Diverge() ) {
                TU_IFLET(::HIR::TypeRef::Data, li.m_data, Infer, l_e,
                    this->m_ivars.set_ivar_to(l_e.index, r_t.clone());
                )
                return ;
            }
            else if( r_t.m_data.is_Diverge() ) {
                TU_IFLET(::HIR::TypeRef::Data, ri.m_data, Infer, r_e,
                    this->m_ivars.set_ivar_to(r_e.index, l_t.clone());
                )
                return ;
            }
            else {
            }
            #else
            if( l_t.m_data.is_Diverge() || r_t.m_data.is_Diverge() ) {
                return ;
            }
            #endif
            
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
                    if( lpe.trait.m_path != rpe.trait.m_path || lpe.item != rpe.item )
                        ERROR(sp, E0000, "Type mismatch between " << l_t << " and " << r_t);
                    equality_typeparams(lpe.trait.m_params, rpe.trait.m_params);
                    equality_typeparams(lpe.params, rpe.params);
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
// NOTE: Mutates the pattern to add ivars to contained paths
void Context::add_binding(const Span& sp, ::HIR::Pattern& pat, const ::HIR::TypeRef& type)
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
    
    
    struct H {
        static void handle_value(Context& context, const Span& sp, const ::HIR::TypeRef& type, const ::HIR::Pattern::Value& val) {
            TU_MATCH(::HIR::Pattern::Value, (val), (v),
            (Integer,
                if( v.type != ::HIR::CoreType::Str ) {
                    context.equate_types(sp, type, ::HIR::TypeRef(v.type));
                }
                ),
            (String,
                context.equate_types(sp, type, ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::TypeRef(::HIR::CoreType::Str) ));
                ),
            (Named,
                // TODO: Get type of the value and equate it
                )
            )
        }
    };
    
    // 
    TU_MATCH(::HIR::Pattern::Data, (pat.m_data), (e),
    (Any,
        // Just leave it, the pattern says nothing
        ),
    (Value,
        H::handle_value(*this, sp, type, e.val);
        ),
    (Range,
        H::handle_value(*this, sp, type, e.start);
        H::handle_value(*this, sp, type, e.end);
        ),
    (Box,
        TODO(sp, "Box pattern");
        ),
    (Ref,
        const auto& ty = this->get_type(type);
        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Borrow, te,
            if( te.type != e.type ) {
                ERROR(sp, E0000, "Pattern-type mismatch, &-ptr mutability mismatch");
            }
            this->add_binding(sp, *e.sub, *te.inner);
        )
        else {
            auto inner = this->m_ivars.new_ivar_tr();
            this->add_binding(sp, *e.sub, inner);
            this->equate_types(sp, type, ::HIR::TypeRef::new_borrow( e.type, mv$(inner) ));
        }
        ),
    (Tuple,
        const auto& ty = this->get_type(type);
        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Tuple, te,
            
            if( e.sub_patterns.size() != te.size() ) { 
                ERROR(sp, E0000, "Tuple pattern with an incorrect number of fields, expected " << e.sub_patterns.size() << "-tuple, got " << ty);
            }
            
            for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
                this->add_binding(sp, e.sub_patterns[i], te[i] );
        )
        else {
            
            ::std::vector< ::HIR::TypeRef>  sub_types;
            for(unsigned int i = 0; i < e.sub_patterns.size(); i ++ ) {
                sub_types.push_back( this->m_ivars.new_ivar_tr() );
                this->add_binding(sp, e.sub_patterns[i], sub_types[i] );
            }
            this->equate_types(sp, ty, ::HIR::TypeRef( mv$(sub_types) ));
        }
        ),
    (SplitTuple,
        const auto& ty = this->get_type(type);
        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Tuple, te,
            // - Should have been checked in AST resolve
            ASSERT_BUG(sp, e.leading.size() + e.trailing.size() <= te.size(), "Invalid field count for split tuple pattern");
            
            unsigned int tup_idx = 0;
            for(auto& subpat : e.leading) {
                this->add_binding(sp, subpat, te[tup_idx++]);
            }
            tup_idx = te.size() - e.trailing.size();
            for(auto& subpat : e.leading) {
                this->add_binding(sp, subpat, te[tup_idx++]);
            }
        )
        else {
            if( !ty.m_data.is_Infer() ) {
                ERROR(sp, E0000, "Tuple pattern on non-tuple");
            }
            
            TODO(sp, "Handle split tuple patterns when type isn't known in starting pass");
        }
        ),
    (Slice,
        const auto& ty = this->get_type(type);
        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Slice, te,
            for(auto& sub : e.sub_patterns)
                this->add_binding(sp, sub, *te.inner );
        )
        else {
            auto inner = this->m_ivars.new_ivar_tr();
            for(auto& sub : e.sub_patterns)
                this->add_binding(sp, sub, inner);
            this->equate_types(sp, type, ::HIR::TypeRef::new_slice(mv$(inner)));
        }
        ),
    (SplitSlice,
        ::HIR::TypeRef  inner;
        const auto& ty = this->get_type(type);
        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Slice, te,
            inner = te.inner->clone();
        )
        else {
            inner = this->m_ivars.new_ivar_tr();
            this->equate_types(sp, type, ::HIR::TypeRef::new_slice(inner.clone()));
        }

        for(auto& sub : e.leading)
            this->add_binding( sp, sub, inner );
        for(auto& sub : e.trailing)
            this->add_binding( sp, sub, inner );
        if( e.extra_bind.is_valid() ) {
            this->add_var( e.extra_bind.m_slot, e.extra_bind.m_name, ty.clone() );
        }
        ),
    
    // - Enums/Structs
    (StructValue,
        this->add_ivars_params( e.path.m_params );
        const auto& str = *e.binding;
        assert( str.m_data.is_Unit() );
        this->equate_types( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)) );
        ),
    (StructTuple,
        this->add_ivars_params( e.path.m_params );
        const auto& str = *e.binding;
        // - assert check from earlier pass
        assert( str.m_data.is_Tuple() );
        const auto& sd = str.m_data.as_Tuple();
        
        const auto& ty = this->get_type(type);
        const auto* params_ptr = &e.path.m_params;
        TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Path, te,
            if( ! te.binding.is_Struct() || te.binding.as_Struct() != &str ) {
                ERROR(sp, E0000, "Type mismatch in struct pattern - " << ty << " is not " << e.path);
            }
            // NOTE: Must be Generic for the above to have passed
            params_ptr = &te.path.m_data.as_Generic().m_params;
        )
        else {
            this->equate_types( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)) );
        }
        
        assert(e.binding);
        const auto& params = *params_ptr;
        
        for( unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
        {
            /*const*/ auto& sub_pat = e.sub_patterns[i];
            const auto& field_type = sd[i].ent;
            if( monomorphise_type_needed(field_type) ) {
                auto var_ty = monomorphise_type(sp, str.m_params, params,  field_type);
                this->add_binding(sp, sub_pat, var_ty);
            }
            else {
                this->add_binding(sp, sub_pat, field_type);
            }
        }
        ),
    (Struct,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            this->equate_types( sp, type, ::HIR::TypeRef::new_path(e.path.clone(), ::HIR::TypeRef::TypePathBinding(e.binding)) );
        }
        const auto& ty = this->get_type(type).clone();
        assert(e.binding);
        const auto& str = *e.binding;
        // - assert check from earlier pass
        assert( str.m_data.is_Named() );
        const auto& sd = str.m_data.as_Named();
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (ty.m_data), (te),
        (
            ERROR(sp, E0000, "Type mismatch in struct pattern - " << ty << " is not " << e.path);
            ),
        (Infer, throw ""; ),
        (Path,
            if( ! te.binding.is_Struct() || te.binding.as_Struct() != &str ) {
                ERROR(sp, E0000, "Type mismatch in struct pattern - " << ty << " is not " << e.path);
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
                    this->add_binding(sp, field_pat.second, field_type);
                }
            }
            )
        )
        ),
    (EnumValue,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            auto path = e.path.clone();
            path.m_path.m_components.pop_back();
            //::std::cout << "HHHH ExprCS: path=" << path << ", pat=" << pat << ::std::endl;

            this->equate_types( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)) );
        }
        const auto& ty = this->get_type(type);
        const auto& type = ty;

        assert(e.binding_ptr);
        const auto& enm = *e.binding_ptr;
        const auto& var = enm.m_variants[e.binding_idx].second;
        assert(var.is_Value() || var.is_Unit());
        
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
    (EnumTuple,
        this->add_ivars_params( e.path.m_params );
        if( type.m_data.is_Infer() ) {
            auto path = e.path.clone();
            path.m_path.m_components.pop_back();

            this->equate_types( sp, type, ::HIR::TypeRef::new_path(mv$(path), ::HIR::TypeRef::TypePathBinding(e.binding_ptr)) );
        }
        const auto& ty = this->get_type(type);
        const auto& type = ty;
        
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
            
            ASSERT_BUG(sp, e.sub_patterns.size() == tup_var.size(),
                "Enum pattern with an incorrect number of fields - " << e.path << " - expected " << tup_var.size() << ", got " << e.sub_patterns.size()
                );
            
            for( unsigned int i = 0; i < e.sub_patterns.size(); i ++ )
            {
                if( monomorphise_type_needed(tup_var[i].ent) ) {
                    auto var_ty = monomorphise_type(sp, enm.m_params, gp.m_params,  tup_var[i].ent);
                    this->add_binding(sp, e.sub_patterns[i], var_ty);
                }
                else {
                    this->add_binding(sp, e.sub_patterns[i], tup_var[i].ent);
                }
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
        }
        const auto& ty = this->get_type(type);
        const auto& type = ty;

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
                    this->add_binding(sp, field_pat.second, field_type);
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
    DEBUG("equate_types_coerce(" << this->link_coerce.back() << ")");
    this->m_ivars.mark_change();
}
void Context::equate_types_shadow(const Span& sp, const ::HIR::TypeRef& l)
{
    TU_MATCH_DEF(::HIR::TypeRef::Data, (this->get_type(l).m_data), (e),
    (
        ),
    (Borrow,
        TU_MATCH_DEF(::HIR::TypeRef::Data, (this->get_type(*e.inner).m_data), (e2),
        (
            ),
        (Infer,
            this->possible_equate_type_disable(e2.index);
            )
        )
        ),
    (Infer,
        this->possible_equate_type_disable(e.index);
        )
    )
}
void Context::equate_types_assoc(const Span& sp, const ::HIR::TypeRef& l,  const ::HIR::SimplePath& trait, ::std::vector< ::HIR::TypeRef> ty_args, const ::HIR::TypeRef& impl_ty, const char *name, bool is_op)
{
    ::HIR::PathParams   pp;
    pp.m_types = mv$(ty_args);
    this->link_assoc.push_back(Associated {
        sp,
        l.clone(),
        
        trait.clone(),
        mv$(pp),
        impl_ty.clone(),
        name,
        is_op
        });
    DEBUG("(" << this->link_assoc.back() << ")");
    this->m_ivars.mark_change();
}
void Context::add_revisit(::HIR::ExprNode& node) {
    this->to_visit.push_back( &node );
}

void Context::possible_equate_type_to(unsigned int ivar_index, const ::HIR::TypeRef& t) {
    // TODO: If the other side is an ivar, add to the other list too
    possible_equate_type(ivar_index, t, true);
}
void Context::possible_equate_type_from(unsigned int ivar_index, const ::HIR::TypeRef& t) {
    possible_equate_type(ivar_index, t, false);
}
void Context::possible_equate_type(unsigned int ivar_index, const ::HIR::TypeRef& t, bool is_to) {
    DEBUG(ivar_index << " ?= " << t << " " << this->m_ivars.get_type(t));
    {
        ::HIR::TypeRef  ty_l;
        ty_l.m_data.as_Infer().index = ivar_index;
        assert( m_ivars.get_type(ty_l).m_data.is_Infer() );
    }
    
    if( ivar_index >= possible_ivar_vals.size() ) {
        possible_ivar_vals.resize( ivar_index + 1 );
    }
    auto& ent = possible_ivar_vals[ivar_index];
    auto& list = (is_to ? ent.types_to : ent.types_from);
    //list.push_back( &t );
    list.push_back( t.clone() );
}
void Context::possible_equate_type_disable(unsigned int ivar_index) {
    DEBUG(ivar_index << " ?= ??");
    {
        ::HIR::TypeRef  ty_l;
        ty_l.m_data.as_Infer().index = ivar_index;
        assert( m_ivars.get_type(ty_l).m_data.is_Infer() );
    }
    
    if( ivar_index >= possible_ivar_vals.size() ) {
        possible_ivar_vals.resize( ivar_index + 1 );
    }
    auto& ent = possible_ivar_vals[ivar_index];
    ent.force_no = true;
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

::HIR::ExprNodeP Context::create_autoderef(::HIR::ExprNodeP val_node, ::HIR::TypeRef ty_dst) const
{
    const auto& span = val_node->span();
    const auto& ty_src = val_node->m_res_type;
    // Special case for going Array->Slice, insert _Unsize instead of _Deref
    if( get_type(ty_src).m_data.is_Array() )
    {
        ASSERT_BUG(span, ty_dst.m_data.is_Slice(), "Array should only ever autoderef to Slice");
        
        // HACK: Emit an invalid _Unsize op that is fixed once usage type is known.
        auto ty_dst_c = ty_dst.clone();
        val_node = NEWNODE( mv$(ty_dst), span, _Unsize,  mv$(val_node), mv$(ty_dst_c) );
        DEBUG("- Unsize " << &*val_node << " -> " << val_node->m_res_type);
    }
    else {
        val_node = NEWNODE( mv$(ty_dst), span, _Deref,  mv$(val_node) );
        DEBUG("- Deref " << &*val_node << " -> " << val_node->m_res_type);
    }
    
    return val_node;
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
    void add_coerce_borrow(Context& context, ::HIR::ExprNodeP& node_ptr, const ::HIR::TypeRef& des_borrow_inner, ::std::function<void(::HIR::ExprNodeP& n)> cb)
    {
        const auto& src_type = context.m_ivars.get_type(node_ptr->m_res_type);
        
        // Since this function operates on destructured &-ptrs, the dereferences have to be added behind a borrow
        ::HIR::ExprNodeP*   node_ptr_ptr = nullptr;
        // - If the pointed node is a borrow operation, add the dereferences within its value
        if( auto* p = dynamic_cast< ::HIR::ExprNode_Borrow*>(&*node_ptr) ) {
            node_ptr_ptr = &p->m_value;
        }
        // - Otherwise, create a new borrow operation behind which the dereferences ahppen
        if( !node_ptr_ptr ) {
            DEBUG("- Coercion node isn't a borrow, adding one");
            auto span = node_ptr->span();
            const auto& src_inner_ty = *src_type.m_data.as_Borrow().inner;
            auto borrow_type = src_type.m_data.as_Borrow().type;
            
            auto inner_ty_ref = ::HIR::TypeRef::new_borrow(borrow_type, des_borrow_inner.clone());
            
            // 1. Dereference (resulting in the dereferenced input type)
            node_ptr = NEWNODE(src_inner_ty.clone(), span, _Deref,  mv$(node_ptr));
            // 2. Borrow (resulting in the referenced output type)
            node_ptr = NEWNODE(mv$(inner_ty_ref), span, _Borrow,  borrow_type, mv$(node_ptr));
            
            // - Set node pointer reference to point into the new borrow op
            node_ptr_ptr = &dynamic_cast< ::HIR::ExprNode_Borrow&>(*node_ptr).m_value;
        }
        else {
            auto borrow_type = context.m_ivars.get_type(node_ptr->m_res_type).m_data.as_Borrow().type;
            // Set the result of the borrow operation to the output type
            node_ptr->m_res_type = ::HIR::TypeRef::new_borrow(borrow_type, des_borrow_inner.clone());
        }
        
        cb(*node_ptr_ptr);
        
        context.m_ivars.mark_change();
    }
    
    bool check_coerce_borrow(Context& context, ::HIR::BorrowType bt, const ::HIR::TypeRef& inner_l, const ::HIR::TypeRef& inner_r, ::HIR::ExprNodeP& node_ptr)
    {
        const auto& sp = node_ptr->span();
        
        const auto& ty_dst = context.m_ivars.get_type(inner_l);
        const auto& ty_src = context.m_ivars.get_type(inner_r);

        // If the types are already equal, no operation is required
        if( context.m_ivars.types_equal(ty_dst, ty_src) ) {
            return true;
        }
        

        // If either side (or both) are ivars, then coercion can't be known yet - but they could be equal
        // TODO: Fix and deduplicate the following code for InferClass::Diverge
        if( ty_src.m_data.is_Infer() && ty_dst.m_data.is_Infer() ) {
            const auto& r_e = ty_src.m_data.as_Infer();
            const auto& l_e = ty_dst.m_data.as_Infer();
            // TODO: Commented out - &-ptrs can infer to trait objects, and &-ptrs can infer from deref coercion
            //if( r_e.ty_class != ::HIR::InferClass::None ) {
            //    context.equate_types(sp, ty_dst, ty_src);
            //    return true;
            //}
            //if( l_e.ty_class != ::HIR::InferClass::None ) {
            //    context.equate_types(sp, ty_dst, ty_src);
            //    return true;
            //}
            context.possible_equate_type_to(r_e.index, ty_dst);
            context.possible_equate_type_from(l_e.index, ty_src);
            DEBUG("- Infer, add possibility");
            return false;
        }
        
        // If the source is '_', we can't know yet
        TU_IFLET(::HIR::TypeRef::Data, ty_src.m_data, Infer, r_e,
            // - Except if it's known to be a primitive
            //if( r_e.ty_class != ::HIR::InferClass::None ) {
            //    context.equate_types(sp, ty_dst, ty_src);
            //    return true;
            //}
            context.possible_equate_type_to(r_e.index, ty_dst);
            DEBUG("- Infer, add possibility");
            return false;
        )
        
        TU_IFLET(::HIR::TypeRef::Data, ty_dst.m_data, Infer, l_e,
            //if( l_e.ty_class == ::HIR::InferClass::None ) {
                context.possible_equate_type_from(l_e.index, ty_src);
                DEBUG("- Infer, add possibility");
                return false;
            //}
            // - Otherwise, it could be a deref to the same ivar? (TODO)
        )
        
        // Fast hack for slices (avoids going via the Deref impl search)
        if( ty_dst.m_data.is_Slice() && !ty_src.m_data.is_Slice() )
        {
            const auto& dst_slice = ty_dst.m_data.as_Slice();
            TU_IFLET(::HIR::TypeRef::Data, ty_src.m_data, Array, src_array,
                context.equate_types(sp, *dst_slice.inner, *src_array.inner);
                
                auto ty_dst_b = ::HIR::TypeRef::new_borrow(bt, ty_dst.clone());
                auto ty_dst_b2 = ty_dst_b.clone();
                auto span = node_ptr->span();
                node_ptr = NEWNODE( mv$(ty_dst_b), span, _Unsize,  mv$(node_ptr), mv$(ty_dst_b2) );
                
                context.m_ivars.mark_change();
                return true;
            )
            else
            {
                // Apply deref coercions
            }
        }
        
        // Deref coercions
        // - If right can be dereferenced to left
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
                            if( ty_dst .compare_with_placeholders(sp, *out_ty, context.m_ivars.callback_resolve_infer()) == ::HIR::Compare::Unequal ) {
                                DEBUG("Same tag, but not fuzzy match");
                                continue ;
                            }
                            DEBUG("Same tag and fuzzy match - assuming " << ty_dst << " == " << *out_ty);
                            context.equate_types(sp, ty_dst,  *out_ty);
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
                
                add_coerce_borrow(context, node_ptr, types.back(), [&](auto& node_ptr) {
                    // node_ptr = node that yeilds ty_src
                    assert( count == types.size() );
                    for(unsigned int i = 0; i < types.size(); i ++ )
                    {
                        auto span = node_ptr->span();
                        // TODO: Replace with a call to context.create_autoderef to handle cases where the below assertion would fire.
                        ASSERT_BUG(span, !node_ptr->m_res_type.m_data.is_Array(), "Array->Slice shouldn't be in deref coercions");
                        auto ty = mv$(types[i]);
                        node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Deref( mv$(span), mv$(node_ptr) ));
                        DEBUG("- Deref " << &*node_ptr << " -> " << ty);
                        node_ptr->m_res_type = mv$(ty);
                        context.m_ivars.get_type(node_ptr->m_res_type);
                    }
                    });
                
                return true;
            }
            // Either ran out of deref, or hit a _
        }
        
        // Desination coercions (Trait objects)
        TU_MATCH_DEF(::HIR::TypeRef::Data, (ty_dst.m_data), (e),
        (
            ),
        (TraitObject,
            const auto& trait = e.m_trait.m_path;
            // Check for trait impl
            bool found = context.m_resolve.find_trait_impls(sp, trait.m_path, trait.m_params, ty_src, [&](auto impl, auto cmp) {
                DEBUG("TraitObject coerce from - cmp="<<cmp<<", " << impl);
                return cmp == ::HIR::Compare::Equal;
                });
            if( !found ) {
                if( !context.m_ivars.type_contains_ivars(ty_src) ) {
                    // TODO: Error
                    ERROR(sp, E0000, "The trait " << e.m_trait << " is not implemented for " << ty_src);
                }
                return false;
            }
            
            for(const auto& marker : e.m_markers)
            {
                bool found = context.m_resolve.find_trait_impls(sp, marker.m_path, marker.m_params, ty_src, [&](auto impl, auto cmp) {
                    DEBUG("TraitObject coerce from - cmp="<<cmp<<", " << impl);
                    return cmp == ::HIR::Compare::Equal;
                    });
                if( !found ) {
                    if( !context.m_ivars.type_contains_ivars(ty_src) ) {
                        // TODO: Error
                        ERROR(sp, E0000, "The trait " << marker << " is not implemented for " << ty_src);
                    }
                    return false;
                }
            }
            
            // Add _Unsize operator
            auto ty_dst_b = ::HIR::TypeRef::new_borrow(bt, ty_dst.clone());
            auto ty_dst_b2 = ty_dst_b.clone();
            auto span = node_ptr->span();
            node_ptr = NEWNODE( mv$(ty_dst_b), span, _Unsize,  mv$(node_ptr), mv$(ty_dst_b2) );
            
            return true;
            )
        )
        
        TU_MATCH_DEF(::HIR::TypeRef::Data, (ty_src.m_data), (e),
        (
            ),
        (Slice,
            // NOTE: These can't even coerce to a TraitObject because of pointer size problems
            context.equate_types(sp, ty_dst, ty_src);
            return true;
        //    ),
        //(TraitObject,
        //    // TODO: Could a trait object coerce and lose a trait?
        //    context.equate_types(sp, ty_dst, ty_src);
        //    return true;
            )
        )
        
        // Search for Unsize
        // - If `right`: ::core::marker::Unsize<`left`>
        {
            const auto& lang_Unsize = context.m_crate.get_lang_item_path(sp, "unsize");
            ::HIR::PathParams   pp;
            pp.m_types.push_back( ty_dst.clone() );
            bool found = context.m_resolve.find_trait_impls(sp, lang_Unsize, pp, ty_src, [&](auto impl, auto cmp) {
                // TODO: Allow fuzzy match if only match
                return cmp == ::HIR::Compare::Equal;
                });
            if( found ) {
                DEBUG("- Unsize " << &*node_ptr << " -> " << ty_dst);
                auto ty_dst_b = ::HIR::TypeRef::new_borrow(bt, ty_dst.clone());
                auto ty_dst_b2 = ty_dst_b.clone();
                auto span = node_ptr->span();
                node_ptr = NEWNODE( mv$(ty_dst_b), span, _Unsize,  mv$(node_ptr), mv$(ty_dst_b2) );
                
                return true;
            }
        }
        
        DEBUG("TODO - Borrow Coercion " << context.m_ivars.fmt_type(ty_dst) << " from " << context.m_ivars.fmt_type(ty_src));
        if( ty_dst.compare_with_placeholders(sp, ty_src, context.m_ivars.callback_resolve_infer()) != ::HIR::Compare::Unequal )
        {
            context.equate_types(sp, ty_dst,  ty_src);
            return true;
        }
        
        // Keep trying
        return false;
    }
    bool check_coerce(Context& context, const Context::Coercion& v) {
        ::HIR::ExprNodeP& node_ptr = *v.right_node_ptr;
        const auto& sp = node_ptr->span();
        const auto& ty_dst = context.m_ivars.get_type(v.left_ty);
        const auto& ty_src = context.m_ivars.get_type(node_ptr->m_res_type);
        TRACE_FUNCTION_F(v << " - " << ty_dst << " := " << ty_src);
        
        if( context.m_ivars.types_equal(ty_dst, ty_src) ) {
            return true;
        }
        
        // CoerceUnsized trait
        // - Only valid for generic or path destination types
        if( ty_dst.m_data.is_Generic() || ty_dst.m_data.is_Path() ) {
            
            const auto& lang_CoerceUnsized = context.m_crate.get_lang_item_path(sp, "coerce_unsized");
            ::HIR::PathParams   pp;
            pp.m_types.push_back( ty_dst.clone() );
            bool found = context.m_resolve.find_trait_impls(sp, lang_CoerceUnsized, pp, ty_src, [&](auto impl, auto cmp) {
                // TODO: Allow fuzzy match if only match
                return cmp == ::HIR::Compare::Equal;
                });
            if( found )
            {
                DEBUG("- CoerceUnsize " << &*node_ptr << " -> " << ty_dst);
                
                auto span = node_ptr->span();
                node_ptr = NEWNODE( ty_dst.clone(), span, _Unsize,  mv$(node_ptr), ty_dst.clone() );
                return true;
            }
        }
        
        // 1. Check that the source type can coerce
        TU_MATCH( ::HIR::TypeRef::Data, (ty_src.m_data), (e),
        (Infer,
            // If this ivar is of a primitive, equate (as primitives never coerce)
            if( e.ty_class != ::HIR::InferClass::None ) {
                context.equate_types(sp, ty_dst,  ty_src);
                return true;
            }
            else {
                TU_IFLET(::HIR::TypeRef::Data, ty_dst.m_data, Infer, e2,
                    context.possible_equate_type_to(e.index, ty_dst);
                    context.possible_equate_type_from(e2.index, ty_src);
                )
                else {
                    context.possible_equate_type_to(e.index, ty_dst);
                    return false;
                }
            }
            ),
        (Diverge,
            return true;
            ),
        (Primitive,
            context.equate_types(sp, ty_dst,  node_ptr->m_res_type);
            return true;
            ),
        (Path,
            if( ! e.binding.is_Unbound() ) {
                // TODO: Use the CoerceUnsized trait here
                context.equate_types(sp, ty_dst,  node_ptr->m_res_type);
                return true;
            }
            ),
        (Generic,
            // TODO: CoerceUnsized bound?
            context.equate_types(sp, ty_dst,  node_ptr->m_res_type);
            return true;
            ),
        (TraitObject,
            // Raw trait objects shouldn't even be encountered here?...
            context.equate_types(sp, ty_dst,  node_ptr->m_res_type);
            return true;
            ),
        (Array,
            // Raw [T; n] doesn't coerce, only borrows do
            context.equate_types(sp, ty_dst,  node_ptr->m_res_type);
            return true;
            ),
        (Slice,
            context.equate_types(sp, ty_dst,  node_ptr->m_res_type);
            return true;
            ),
        (Tuple,
            context.equate_types(sp, ty_dst,  node_ptr->m_res_type);
            return true;
            ),
        (Borrow,
            // Borrows can have unsizing and deref coercions applied
            ),
        (Pointer,
            // Pointers coerce to similar pointers of higher restriction
            if( e.type == ::HIR::BorrowType::Shared ) {
                // *const is the bottom of the tree, it doesn't coerce to anything
                context.equate_types(sp, ty_dst,  node_ptr->m_res_type);
                return true;
            }
            ),
        (Function,
            // NOTE: Functions don't coerce (TODO: They could lose the origin marker?)
            context.equate_types(sp, ty_dst,  node_ptr->m_res_type);
            return true;
            ),
        (Closure,
            // TODO: Can closures coerce to anything?
            // - (eventually maybe fn() if they don't capture, but that's not rustc yet)
            context.equate_types(sp, ty_dst,  node_ptr->m_res_type);
            return true;
            )
        )
        
        // 2. Check target type is a valid coercion
        // - Otherwise - Force equality
        TU_MATCH( ::HIR::TypeRef::Data, (ty_dst.m_data), (l_e),
        (Infer,
            // If this ivar is of a primitive, equate (as primitives never coerce)
            // TODO: Update for InferClass::Diverge ?
            if( l_e.ty_class != ::HIR::InferClass::None ) {
                context.equate_types(sp, ty_dst,  ty_src);
                return true;
            }
            // Can't do anything yet?
            // - Later code can handle "only path" coercions

            context.possible_equate_type_from(l_e.index,  ty_src);
            DEBUG("- Infer, add possibility");
            return false;
            ),
        (Diverge,
            return true;
            ),
        (Primitive,
            context.equate_types(sp, ty_dst,  node_ptr->m_res_type);
            return true;
            ),
        (Path,
            if( ! l_e.binding.is_Unbound() ) {
                // TODO: CoerceUnsized
                context.equate_types(sp, ty_dst, ty_src);
                return true;
            }
            ),
        (Generic,
            //TODO(Span(), "check_coerce - Coercion to " << ty);
            context.equate_types(sp, ty_dst,  node_ptr->m_res_type);
            return true;
            ),
        (TraitObject,
            // TODO: Can bare trait objects coerce?
            context.equate_types(sp, ty_dst,  ty_src);
            return true;
            ),
        (Array,
            context.equate_types(sp, ty_dst,  ty_src);
            return true;
            ),
        (Slice,
            context.equate_types(sp, ty_dst,  ty_src);
            return true;
            ),
        (Tuple,
            context.equate_types(sp, ty_dst,  ty_src);
            return true;
            ),
        (Borrow,
            TU_IFLET(::HIR::TypeRef::Data, ty_src.m_data, Borrow, r_e,
                // If using `&mut T` where `&const T` is expected - insert a reborrow (&*)
                if( l_e.type == ::HIR::BorrowType::Shared && r_e.type == ::HIR::BorrowType::Unique ) {
                    context.equate_types(sp, *l_e.inner, *r_e.inner);
                    
                    // Add cast down
                    auto span = node_ptr->span();
                    // *<inner>
                    DEBUG("- Deref -> " << *l_e.inner);
                    node_ptr = NEWNODE( l_e.inner->clone(), span, _Deref,  mv$(node_ptr) );
                    context.m_ivars.get_type(node_ptr->m_res_type);
                    // &*<inner>
                    node_ptr = NEWNODE( ty_dst.clone(), span, _Borrow,  ::HIR::BorrowType::Shared, mv$(node_ptr) );
                    context.m_ivars.get_type(node_ptr->m_res_type);
                    
                    context.m_ivars.mark_change();
                    return true;
                }
                // TODO: &move reboorrowing rules?
                
                if( l_e.type != r_e.type ) {
                    // TODO: This could be allowed if left == Shared && right == Unique (reborrowing)
                    ERROR(sp, E0000, "Type mismatch between " << ty_dst << " and " << ty_src << " - Borrow classes differ");
                }
                
                // - Check for coercions
                return check_coerce_borrow(context, l_e.type, *l_e.inner, *r_e.inner, node_ptr);
            )
            else TU_IFLET(::HIR::TypeRef::Data, ty_src.m_data, Infer, r_e,
                // Leave for now
                if( r_e.ty_class != ::HIR::InferClass::None ) {
                    // ERROR: Must be compatible
                    context.equate_types(sp, ty_dst,  ty_src);
                    BUG(sp, "Type error expected " << ty_dst << " == " << ty_src);
                }
                
                context.possible_equate_type_to(r_e.index, ty_dst);
                DEBUG("- Infer, add possibility");
                return false;
            )
            // TODO: If the type is a UfcsKnown but contains ivars (i.e. would be destructured into an associated type rule)
            //   don't equate, and instead return false.
            else {
                // Error: Must be compatible, hand over to the equate code.
                // - If this returns early, it's because of a UFCS destructure
                context.equate_types(sp, ty_dst,  ty_src);
                //BUG(sp, "Type error expected " << ty << " == " << ty_src);
            }
            ),
        (Pointer,
            // Pointers coerce from borrows and similar pointers
            TU_IFLET(::HIR::TypeRef::Data, ty_src.m_data, Borrow, r_e,
                if( r_e.type != l_e.type ) {
                    ERROR(sp, E0000, "Type mismatch between " << ty_dst << " and " << ty_src << " - Mutability differs");
                }
                context.equate_types(sp, *l_e.inner, *r_e.inner);
                // Add downcast
                auto span = node_ptr->span();
                node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Cast( mv$(span), mv$(node_ptr), ty_dst.clone() ));
                node_ptr->m_res_type = ty_dst.clone();
                
                context.m_ivars.mark_change();
                return true;
            )
            else TU_IFLET(::HIR::TypeRef::Data, ty_src.m_data, Pointer, r_e,
                // If using `*mut T` where `*const T` is expected - add cast
                if( l_e.type == ::HIR::BorrowType::Shared && r_e.type == ::HIR::BorrowType::Unique ) {
                    context.equate_types(sp, *l_e.inner, *r_e.inner);
                    
                    // Add cast down
                    auto span = node_ptr->span();
                    node_ptr->m_res_type = ty_src.clone();
                    node_ptr = ::HIR::ExprNodeP(new ::HIR::ExprNode_Cast( mv$(span), mv$(node_ptr), ty_dst.clone() ));
                    node_ptr->m_res_type = ty_dst.clone();
                    
                    context.m_ivars.mark_change();
                    return true;
                }
                
                if( l_e.type != r_e.type ) {
                    ERROR(sp, E0000, "Type mismatch between " << ty_dst << " and " << ty_src << " - Pointer mutability differs");
                }
                context.equate_types(sp, *l_e.inner, *r_e.inner);
                return true;
            )
            else TU_IFLET(::HIR::TypeRef::Data, ty_src.m_data, Infer, r_e,
                if( r_e.ty_class != ::HIR::InferClass::None ) {
                    // ERROR: Must be compatible
                    context.equate_types(sp, ty_dst,  ty_src);
                    BUG(sp, "Type error expected " << ty_dst << " == " << ty_src);
                }
                // Can't do much for now
                context.possible_equate_type_to(r_e.index, ty_dst);
                DEBUG("- Infer, add possibility");
                return false;
            )
            else {
                // Error: Must be compatible, hand over to the equate code.
                // - If this returns early, it's because of a UFCS destructure
                context.equate_types(sp, ty_dst,  ty_src);
                //BUG(sp, "Type error expected " << ty << " == " << ty_src);
            }
            ),
        (Function,
            // TODO: Could capture-less closures coerce to fn() types?
            context.equate_types(sp, ty_dst, ty_src);
            return true;
            ),
        (Closure,
            context.equate_types(sp, ty_dst,  node_ptr->m_res_type);
            return true;
            )
        )
        
        //TODO(sp, "Typecheck_Code_CS - Coercion " << context.m_ivars.fmt_type(ty) << " from " << context.m_ivars.fmt_type(node_ptr->m_res_type));
        DEBUG("TODO - Coercion " << context.m_ivars.fmt_type(ty_dst) << " from " << context.m_ivars.fmt_type(node_ptr->m_res_type));
        return false;
    }
    
    bool check_associated(Context& context, const Context::Associated& v)
    {
        const auto& sp = v.span;
        TRACE_FUNCTION_F(v);
        
        ::HIR::TypeRef  possible_impl_ty;
        ::HIR::PathParams   possible_params;
        ::HIR::TypeRef  output_type;
        
        struct H {
            static bool type_is_num(const ::HIR::TypeRef& t) {
                TU_MATCH_DEF(::HIR::TypeRef::Data, (t.m_data), (e),
                ( return false; ),
                (Primitive,
                    switch(e)
                    {
                    case ::HIR::CoreType::Str:
                    case ::HIR::CoreType::Char:
                        return false;
                    default:
                        return true;
                    }
                    ),
                (Infer,
                    return e.ty_class != ::HIR::InferClass::None;
                    )
                )
            }
        };
        
        // MAGIC! Have special handling for operator overloads
        if( v.is_operator ) {
            if( v.params.m_types.size() == 0 )
            {
                // Uni ops = If the value is a primitive, the output is the same type
                const auto& ty = context.get_type(v.impl_ty);
                const auto& res = context.get_type(v.left_ty);
                if( H::type_is_num(ty) ) {
                    DEBUG("- Magic inferrence link for uniops on numerics");
                    context.equate_types(sp, res, ty);
                }
            }
            else if( v.params.m_types.size() == 1 )
            {
                // Binary operations - If both types are primitives, the output is the lefthand side
                const auto& left = context.get_type(v.impl_ty); // yes, impl = LHS of binop
                const auto& right = context.get_type(v.params.m_types.at(0));
                const auto& res = context.get_type(v.left_ty);
                if( H::type_is_num(left) && H::type_is_num(right) ) {
                    DEBUG("- Magic inferrence link for binops on numerics");
                    context.equate_types(sp, res, left);
                }
            }
            else
            {
                BUG(sp, "");
            }
        }
        
        // HACK: If the LHS is an opqaue UfcsKnown for the same trait and item, equate the inner types
        #if 0
        TU_IFLET(::HIR::TypeRef::Data, v.left_ty.m_data, Path, e,
            if( e.binding.is_Opaque() )
            {
                TU_IFLET(::HIR::Path::Data, e.path.m_data, UfcsKnown, pe,
                    if( pe.trait.m_path == v.trait && pe.item == v.name )
                    {
                        #if 0
                        context.equate_types(sp, *pe.type, v.impl_ty);
                        #else
                        TU_IFLET(::HIR::TypeRef::Data, context.get_type(*pe.type).m_data, Infer, e2,
                            //context.possible_equate_type_from(e2.index, v.impl_ty);
                            //context.possible_equate_type_to(e2.index, v.impl_ty);
                            return false;
                        )
                        else TU_IFLET(::HIR::TypeRef::Data, context.get_type(v.impl_ty).m_data, Infer, e2,
                            //context.possible_equate_type_from(e2.index, *pe.type);
                            //context.possible_equate_type_to(e2.index, *pe.type);
                            return false;
                        )
                        else {
                            context.equate_types(sp, *pe.type, v.impl_ty);
                            return true;
                        }
                        #endif
                    }
                )
            }
        )
        #endif
        
        // Locate applicable trait impl
        unsigned int count = 0;
        DEBUG("Searching for impl " << v.trait << v.params << " for " << context.m_ivars.fmt_type(v.impl_ty));
        bool found = context.m_resolve.find_trait_impls(sp, v.trait, v.params,  v.impl_ty,
            [&](auto impl, auto cmp) {
                DEBUG("[check_associated] Found cmp=" << cmp << " " << impl);
                if( v.name != "" ) {
                    auto out_ty_o = impl.get_type(v.name.c_str());
                    if( out_ty_o == ::HIR::TypeRef() )
                    {
                        out_ty_o = ::HIR::TypeRef( ::HIR::Path(::HIR::Path( v.impl_ty.clone(), ::HIR::GenericPath(v.trait, v.params.clone()), v.name, ::HIR::PathParams() )) );
                    }
                    out_ty_o = context.m_resolve.expand_associated_types(sp, mv$(out_ty_o));
                    //BUG(sp, "Getting associated type '" << v.name << "' which isn't in " << v.trait << " (" << ty << ")");
                    
                    const auto& out_ty = out_ty_o;
                
                    // - If we're looking for an associated type, allow it to eliminate impossible impls
                    //  > This makes `let v: usize = !0;` work without special cases
                    auto cmp2 = v.left_ty.compare_with_placeholders(sp, out_ty, context.m_ivars.callback_resolve_infer());
                    if( cmp2 == ::HIR::Compare::Unequal ) {
                        DEBUG("- (fail) known result can't match (" << context.m_ivars.fmt_type(v.left_ty) << " and " << context.m_ivars.fmt_type(out_ty) << ")");
                        return false;
                    }
                    // if solid or fuzzy, leave as-is
                    output_type = out_ty.clone();
                }
                count += 1;
                if( cmp == ::HIR::Compare::Equal ) {
                    return true;
                }
                else {
                    if( possible_impl_ty == ::HIR::TypeRef() ) {
                        possible_impl_ty = impl.get_impl_type();
                        possible_params = impl.get_trait_params();
                    }
                    
                    DEBUG("- (possible) " << impl);
                    
                    return false;
                }
            });
        if( found ) {
            // Fully-known impl
            if( v.name != "" ) {
                context.equate_types(sp, v.left_ty, output_type);
            }
            return true;
        }
        else if( count == 0 ) {
            // No applicable impl
            // - TODO: This should really only fire when there isn't an impl. But it currently fires when _
            DEBUG("No impl of " << v.trait << context.m_ivars.fmt(v.params) << " for " << context.m_ivars.fmt_type(v.impl_ty));
            bool is_known = !context.m_ivars.type_contains_ivars(v.impl_ty);
            for(const auto& t : v.params.m_types)
                is_known &= !context.m_ivars.type_contains_ivars(t);
            if( is_known ) {
                ERROR(sp, E0000, "Failed to find an impl of " << v.trait << context.m_ivars.fmt(v.params) << " for " << context.m_ivars.fmt_type(v.impl_ty));
            }
            return false;
        }
        else if( count == 1 ) {
            DEBUG("Only one impl " << v.trait << context.m_ivars.fmt(possible_params) << " for " << context.m_ivars.fmt_type(possible_impl_ty)
                << " - params=" << possible_params << ", ty=" << possible_impl_ty << ", out=" << output_type);
            // Only one possible impl
            if( v.name != "" ) {
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
    
    void check_ivar_poss(Context& context, unsigned int i, Context::IVarPossible& ivar_ent)
    {
        static Span _span;
        const auto& sp = _span;
        
        if( ivar_ent.types_to.size() == 0 && ivar_ent.types_from.size() == 0 ) {
            // No idea! (or unused)
            return ;
        }
        
        ::HIR::TypeRef  ty_l_ivar;
        ty_l_ivar.m_data.as_Infer().index = i;
        const auto& ty_l = context.m_ivars.get_type(ty_l_ivar);
        
        if( !ty_l.m_data.is_Infer() ) {
            DEBUG("- IVar " << i << " had possibilities, but was known to be " << ty_l);
            ivar_ent = Context::IVarPossible();
            return ;
        }
        
        if( ivar_ent.force_no == true ) {
            DEBUG("- IVar " << ty_l << " is forced unknown");
            ivar_ent = Context::IVarPossible();
            return ;
        }
        
        struct H {
            // De-duplicate list (taking into account other ivars)
            // - TODO: Use the direction and do a fuzzy equality based on coercion possibility
            static void dedup_type_list(const Context& context, ::std::vector< ::HIR::TypeRef>& list) {
                for( auto it = list.begin(); it != list.end(); )
                {
                    bool found = false;
                    for( auto it2 = list.begin(); it2 != it; ++ it2 ) {
                        //if( context.m_ivars.types_equal( **it, **it2 ) ) {
                        if( H::equal_to(context, *it, *it2) ) {
                            found = true;
                            break;
                        }
                    }
                    if( found ) {
                        it = list.erase(it);
                    }
                    else {
                        ++ it;
                    }
                }
            }
            
            // Types are equal from the view of being coercion targets
            // - Inequality here means that the targets could coexist (e.g. &[u8; N] and &[u8])
            // - Equality means that they HAVE to be equal (even if they're not currently due to ivars)
            static bool equal_to(const Context& context, const ::HIR::TypeRef& ia, const ::HIR::TypeRef& ib) {
                const auto& a = context.m_ivars.get_type(ia);
                const auto& b = context.m_ivars.get_type(ib);
                if( a.m_data.tag() != b.m_data.tag() )
                    return false;
                TU_MATCH_DEF(::HIR::TypeRef::Data, (a.m_data, b.m_data), (e_a, e_b),
                (
                    return context.m_ivars.types_equal(a, b);
                    ),
                (Borrow,
                    if( e_a.type != e_b.type )
                        return false;
                    const auto& ia = context.m_ivars.get_type(*e_a.inner);
                    const auto& ib = context.m_ivars.get_type(*e_b.inner);
                    if( ia.m_data.tag() != ib.m_data.tag() )
                        return false;
                    TU_MATCH_DEF(::HIR::TypeRef::Data, (ia.m_data, ib.m_data), (e_ia, e_ib),
                    (
                        return context.m_ivars.types_equal(ia, ib);
                        ),
                    (Infer,
                        return false;
                        ),
                    (Slice,
                        const auto& ia2 = context.m_ivars.get_type(*e_ia.inner);
                        const auto& ib2 = context.m_ivars.get_type(*e_ib.inner);
                        if(ia2.m_data.is_Infer() || ib2.m_data.is_Infer())
                            return true;
                        return context.m_ivars.types_equal(ia2, ib2);
                        )
                    )
                    ),
                (Pointer,
                    if( e_a.type != e_b.type )
                        return false;
                    // TODO: Rules are subtly different when coercing from a pointer?
                    const auto& ia2 = context.m_ivars.get_type(*e_a.inner);
                    const auto& ib2 = context.m_ivars.get_type(*e_b.inner);
                    if(ia2.m_data.is_Infer() || ib2.m_data.is_Infer())
                        return true;
                    return context.m_ivars.types_equal(ia2, ib2);
                    )
                )
                // 
                return context.m_ivars.types_equal(a, b);
            }
            // Types are equal from the view of being coercion sources
            static bool equal_from(const Context& context, const ::HIR::TypeRef& a, const ::HIR::TypeRef& b) {
                return context.m_ivars.types_equal(a, b);
            }
            
            static bool can_coerce_to(const Context& context, const ::HIR::TypeRef& dst, const ::HIR::TypeRef& src) {
                if( dst.m_data.is_Infer() )
                    return false;
                if( src.m_data.is_Infer() )
                    return false;
                
                if( dst.m_data.is_Borrow() && src.m_data.is_Borrow() ) {
                    const auto& d_e = dst.m_data.as_Borrow();
                    const auto& s_e = src.m_data.as_Borrow();
                    
                    // Higher = more specific (e.g. Unique > Shared)
                    if( s_e.type < d_e.type ) {
                        return false;
                    }
                    else if( s_e.type == d_e.type ) {
                        // Check relationship
                        // - 1. Deref chain.
                        // - 2. Trait object?
                    }
                    else {
                        return context.m_ivars.types_equal(*s_e.inner, *d_e.inner);
                    }
                }
                return false;
            }
            
            static const ::HIR::TypeRef* find_lowest_type(const Context& context, const ::std::vector< ::HIR::TypeRef>& list)
            {
                // 1. Locate types that cannot coerce to anything
                // - &TraitObject and &[T] are the main pair
                for(const auto& ty : list) {
                    TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Borrow, e,
                        TU_MATCH_DEF(::HIR::TypeRef::Data, (e.inner->m_data), (e2),
                        (
                            ),
                        (Slice,
                            return &ty;
                            ),
                        (TraitObject,
                            return &ty;
                            )
                        )
                    )
                }
                
                // 2. Search the list for a type that is a valid coercion target for all other types in the list
                // - NOTE: Ivars return `false` nomatter what order
                const auto* cur_type = &list[0];
                for(const auto& ty : list) {
                    // If ty can be coerced to the current type
                    if( H::can_coerce_to(context, *cur_type, ty) ) {
                        // - Keep current type
                    }
                    else if( H::can_coerce_to(context, ty, *cur_type) ) {
                        cur_type = &ty;
                    }
                    else {
                        // Error? Give up.
                        cur_type = nullptr;
                        break;
                    }
                }
                if( cur_type ) {
                    // TODO: Replace
                    //return cur_type;
                }
                
                return nullptr;
            }
        };
        
        TRACE_FUNCTION_F(i);
        
        // TODO: Some cases lead to two possibilities that compare different (due to inferrence) but are actually the same.
        // - The above dedup should probably be aware of the way the types are used (for coercions).
        
        if( ivar_ent.types_to.size() > 1 ) {
            H::dedup_type_list(context, ivar_ent.types_to);
        }
        if( ivar_ent.types_from.size() > 1 ) {
            H::dedup_type_list(context, ivar_ent.types_from);
        }
        
        // Prefer cases where this type is being created from a known type
        if( ivar_ent.types_from.size() == 1 ) {
            //const ::HIR::TypeRef& ty_r = *ivar_ent.types_from[0];
            const ::HIR::TypeRef& ty_r = ivar_ent.types_from[0];
            // Only one possibility
            DEBUG("- IVar " << ty_l << " = " << ty_r << " (from)");
            context.equate_types(sp, ty_l, ty_r);
        }
        else if( ivar_ent.types_to.size() == 1 ) {
            //const ::HIR::TypeRef& ty_r = *ivar_ent.types_to[0];
            const ::HIR::TypeRef& ty_r = ivar_ent.types_to[0];
            // Only one possibility
            DEBUG("- IVar " << ty_l << " = " << ty_r << " (to)");
            context.equate_types(sp, ty_l, ty_r);
        }
        else {
            DEBUG("- IVar " << ty_l << " not concretely known {" << ivar_ent.types_from << "} and {" << ivar_ent.types_to << "}" );
            
            // If one side is completely unknown, pick the most liberal of the other side
            if( ivar_ent.types_to.size() == 0 && ivar_ent.types_from.size() > 0 )
            {
                // Search for the lowest-level source type (e.g. &[T])
                const auto* lowest_type = H::find_lowest_type(context, ivar_ent.types_from);
                if( lowest_type )
                {
                    const ::HIR::TypeRef& ty_r = *lowest_type;
                    DEBUG("- IVar " << ty_l << " = " << ty_r << " (from, lowest)");
                    context.equate_types(sp, ty_l, ty_r);
                }
            }
            else if( ivar_ent.types_to.size() > 0 && ivar_ent.types_from.size() == 0 )
            {
                // TODO: Get highest-level target type
            }
            else
            {
            }
        }
        
        ivar_ent.force_no = false;
        ivar_ent.types_to.clear();
        ivar_ent.types_from.clear();
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
        DEBUG("Return type = " << result_type);
        context.equate_types_coerce(expr->span(), result_type, root_ptr);
    }
    
    const unsigned int MAX_ITERATIONS = 100;
    unsigned int count = 0;
    while( context.take_changed() /*&& context.has_rules()*/ && count < MAX_ITERATIONS )
    {
        TRACE_FUNCTION_F("=== PASS " << count << " ===");
        context.dump();
        
        // 1. Check coercions for ones that cannot coerce due to RHS type (e.g. `str` which doesn't coerce to anything)
        // 2. (???) Locate coercions that cannot coerce (due to being the only way to know a type)
        // - Keep a list in the ivar of what types that ivar could be equated to.
        DEBUG("--- Coercion checking");
        for(auto it = context.link_coerce.begin(); it != context.link_coerce.end(); ) {
            const auto& src_ty = (**it->right_node_ptr).m_res_type;
            it->left_ty = context.m_resolve.expand_associated_types( (*it->right_node_ptr)->span(), mv$(it->left_ty) );
            if( check_coerce(context, *it) ) {
                DEBUG("- Consumed coercion " << it->left_ty << " := " << src_ty);
                it = context.link_coerce.erase(it);
            }
            else {
                ++ it;
            }
        }
        // 3. Check associated type rules
        DEBUG("--- Associated types");
        //for(auto it = context.link_assoc.begin(); it != context.link_assoc.end(); ) {
        //    const auto& rule = *it;
        for(unsigned int i = 0; i < context.link_assoc.size();  ) {
            auto& rule = context.link_assoc[i];
        
            DEBUG("- " << rule);
            for( auto& ty : rule.params.m_types ) {
                ty = context.m_resolve.expand_associated_types(rule.span, mv$(ty));
            }
            if( rule.name != "" ) {
                rule.left_ty = context.m_resolve.expand_associated_types(rule.span, mv$(rule.left_ty));
            }
            rule.impl_ty = context.m_resolve.expand_associated_types(rule.span, mv$(rule.impl_ty));
        
            if( check_associated(context, rule) ) {
                DEBUG("- Consumed associated type rule - " << rule);
                context.link_assoc.erase( context.link_assoc.begin() + i );
                //it = context.link_assoc.erase(it);
            }
            else {
                //++ it;
                i ++;
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
            check_ivar_poss(context, i, ivar_ent);
            i ++ ;
        }
        

        // Finally. If nothing changed, apply ivar defaults
        if( !context.m_ivars.peek_changed() )
        {
            DEBUG("- Applying defaults");
            if( context.m_ivars.apply_defaults() ) {
                context.m_ivars.mark_change();
            }
        }
        
        count ++;
        context.m_resolve.compact_ivars(context.m_ivars);
    }
    
    if( context.has_rules() )
    {
        context.dump();
        BUG(root_ptr->span(), "Spare rules left after typecheck stabilised");
    }
    
    // - Recreate the pointer
    expr = ::HIR::ExprPtr( mv$(root_ptr) );
    //  > Steal the binding types
    expr.m_bindings.reserve( context.m_bindings.size() );
    for(auto& binding : context.m_bindings) {
        expr.m_bindings.push_back( binding.ty.clone() );
    }
    
    // - Validate typeck
    {
        DEBUG("==== VALIDATE ==== (" << count << " rounds)");
        context.dump();
        
        ExprVisitor_Apply   visitor { context };
        visitor.visit_node_ptr( expr );
    }
}

