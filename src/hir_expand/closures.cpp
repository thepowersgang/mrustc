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
    inline HIR::ExprNodeP mk_exprnodep(HIR::ExprNode* en, ::HIR::TypeRef ty){ en->m_res_type = mv$(ty); return HIR::ExprNodeP(en); }
}
#define NEWNODE(TY, CLASS, ...)  mk_exprnodep(new HIR::ExprNode_##CLASS(__VA_ARGS__), TY)

namespace {
    
    typedef ::std::vector< ::std::pair< ::HIR::ExprNode_Closure::Class, ::HIR::TraitImpl> > out_impls_t;
    typedef ::std::vector< ::std::pair< ::std::string, ::HIR::Struct> > out_types_t;
    
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
    template<typename T>
    ::std::vector<T> make_vec3(T v1, T v2, T v3) {
        ::std::vector<T>    rv;
        rv.reserve(3);
        rv.push_back( mv$(v1) );
        rv.push_back( mv$(v2) );
        rv.push_back( mv$(v3) );
        return rv;
    }
    
    class ExprVisitor_Mutate:
        public ::HIR::ExprVisitorDef
    {
        const ::HIR::TypeRef&   m_closure_type;
        const ::std::vector<unsigned int>&  m_local_vars;
        const ::std::vector<unsigned int>&  m_captures;
        
        typedef ::std::function< const ::HIR::TypeRef&(const ::HIR::TypeRef&)>  t_monomorph_cb;
        t_monomorph_cb m_monomorph_cb;
        
        ::HIR::ExprNodeP    m_replacement;
    public:
        ExprVisitor_Mutate(const ::HIR::TypeRef& closure_type, const ::std::vector<unsigned int>& local_vars, const ::std::vector<unsigned int>& captures, t_monomorph_cb mcb):
            m_closure_type(closure_type),
            m_local_vars(local_vars),
            m_captures(captures),
            m_monomorph_cb(mcb)
        {
        }
        
        void visit_type(::HIR::TypeRef& ty) override {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Generic, e,
                auto n = m_monomorph_cb(ty).clone();
                ty = mv$(n);
            )
            else {
                ::HIR::ExprVisitorDef::visit_type(ty);
            }
        }
        
        void visit_node_ptr(::HIR::ExprNodeP& node_ptr) override {
            assert( node_ptr );
            auto& node = *node_ptr;
            const char* node_ty = typeid(node).name();
            TRACE_FUNCTION_FR(&node << " " << node_ty << " : " << node.m_res_type, node_ty);
            node.visit(*this);
            
            if( m_replacement ) {
                node_ptr = mv$(m_replacement);
            }
            
            visit_type( node_ptr->m_res_type );
        }
        void visit(::HIR::ExprNode_Closure& node) override
        {
            // Do nothing, inner closures should just be value references now
            assert( ! node.m_code );
        }
        void visit(::HIR::ExprNode_Variable& node) override
        {
            // 1. Is it a closure-local?
            auto binding_it = ::std::find(m_local_vars.begin(), m_local_vars.end(), node.m_slot);
            if( binding_it != m_local_vars.end() ) {
                // NOTE: Offset of 2 is for `self` and `args`
                node.m_slot = 2 + binding_it - m_local_vars.begin();
                return ;
            }
            
            // 2. Is it a capture?
            binding_it = ::std::find(m_captures.begin(), m_captures.end(), node.m_slot);
            if( binding_it != m_captures.end() ) {
                m_replacement = NEWNODE(node.m_res_type.clone(), Field, node.span(),
                    NEWNODE(m_closure_type.clone(), Variable, node.span(), "self", 0),
                    FMT(binding_it - m_captures.begin())
                    );
                return ;
            }
            
            BUG(node.span(), "Encountered non-captured and unknown-origin variable - " << node.m_name << " #" << node.m_slot);
        }
    };
    
    struct H {
        static void fix_fn_params(::HIR::ExprPtr& code, const ::HIR::TypeRef& self_ty, const ::HIR::TypeRef& args_ty)
        {
            if( code.m_bindings.size() == 0 ) {
                // Insert 0 = Self, 1 = Args
                code.m_bindings.push_back( self_ty.clone() );
                code.m_bindings.push_back( args_ty.clone() );
            }
            else {
                assert( code.m_bindings.size() >= 2 );
                assert( code.m_bindings[0] == ::HIR::TypeRef() );
                assert( code.m_bindings[1] == ::HIR::TypeRef() );
                code.m_bindings[0] = self_ty.clone();
                code.m_bindings[1] = args_ty.clone();
            }
        }
        static ::HIR::TraitImpl make_fnonce(
                ::HIR::GenericParams params,
                ::HIR::PathParams trait_params,
                ::HIR::TypeRef closure_type,
                ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> args_argent,
                ::HIR::TypeRef ret_ty,
                ::HIR::ExprPtr code
            )
        {
            fix_fn_params(code, closure_type, args_argent.second);
            return ::HIR::TraitImpl {
                mv$(params), mv$(trait_params), mv$(closure_type),
                make_map1(
                    ::std::string("call_once"), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { false, ::HIR::Function {
                        "rust", false, false,
                        {},
                        make_vec2(
                            ::std::make_pair(::HIR::Pattern { {false, ::HIR::PatternBinding::Type::Move, "self", 0}, {} }, ::HIR::TypeRef("Self", 0xFFFF)),
                            mv$( args_argent )
                            ),
                        ret_ty.clone(),
                        mv$(code)
                        } }
                    ),
                {},
                make_map1(
                    ::std::string("Output"), ::HIR::TraitImpl::ImplEnt< ::HIR::TypeRef> { false, mv$(ret_ty) }
                    ),
                ::HIR::SimplePath()
                };
        }
        static ::HIR::TraitImpl make_fnmut(
                ::HIR::GenericParams params,
                ::HIR::PathParams trait_params,
                ::HIR::TypeRef closure_type,
                ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> args_argent,
                ::HIR::TypeRef ret_ty,
                ::HIR::ExprPtr code
            )
        {
            fix_fn_params(code, closure_type, args_argent.second);
            return ::HIR::TraitImpl {
                mv$(params), mv$(trait_params), mv$(closure_type),
                make_map1(
                    ::std::string("call_mut"), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { false, ::HIR::Function {
                        "rust", false, false,
                        {},
                        make_vec2(
                            ::std::make_pair(
                                ::HIR::Pattern { {false, ::HIR::PatternBinding::Type::Move, "self", 0}, {} },
                                ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Unique, ::HIR::TypeRef("Self", 0xFFFF) )
                                ),
                            mv$( args_argent )
                            ),
                        ret_ty.clone(),
                        mv$(code)
                        } }
                    ),
                {},
                {},
                ::HIR::SimplePath()
                };
        }
        static ::HIR::TraitImpl make_fn(
                ::HIR::GenericParams params,
                ::HIR::PathParams trait_params,
                ::HIR::TypeRef closure_type,
                ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> args_argent,
                ::HIR::TypeRef ret_ty,
                ::HIR::ExprPtr code
            )
        {
            fix_fn_params(code, closure_type, args_argent.second);
            return ::HIR::TraitImpl {
                mv$(params), mv$(trait_params), mv$(closure_type),
                make_map1(
                    ::std::string("call"), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { false, ::HIR::Function {
                        "rust", false, false,
                        {},
                        make_vec2(
                            ::std::make_pair(
                                ::HIR::Pattern { {false, ::HIR::PatternBinding::Type::Move, "self", 0}, {} },
                                ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, ::HIR::TypeRef("Self", 0xFFFF) )
                                ),
                            mv$(args_argent)
                            ),
                        ret_ty.clone(),
                        mv$(code)
                        } }
                    ),
                {},
                {},
                ::HIR::SimplePath()
                };
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
        const ::HIR::SimplePath&  m_module_path;
        ::std::vector< ::HIR::TypeRef>& m_variable_types;
        
        // Outputs
        out_impls_t&    m_out_impls;
        out_types_t&    m_out_types;
        
        /// Stack of active closures
        ::std::vector<ClosureScope> m_closure_stack;
    public:
        ExprVisitor_Extract(const StaticTraitResolve& resolve, const ::HIR::SimplePath& mod_path, ::std::vector< ::HIR::TypeRef>& var_types, out_impls_t& out_impls, out_types_t& out_types):
            m_resolve(resolve),
            m_module_path(mod_path),
            m_variable_types(var_types),
            m_out_impls( out_impls ),
            m_out_types( out_types )
        {
        }
        
        void visit_root(::HIR::ExprNode& root)
        {
            root.visit(*this);
        }
        
        void visit(::HIR::ExprNode_Closure& node) override
        {
            const auto& sp = node.span();
            
            TRACE_FUNCTION_F("Extract closure - " << node.m_res_type);
            
            // --- Determine borrow set ---
            m_closure_stack.push_back( ClosureScope(node) );
            
            for(const auto& arg : node.m_args) {
                add_closure_def_from_pattern(node.span(), arg.first);
            }
            
            ::HIR::ExprVisitorDef::visit(node);
            
            auto ent = mv$( m_closure_stack.back() );
            m_closure_stack.pop_back();
            
            // --- Extract and mutate code into a trait impl on the closure type ---
            
            // 1. Construct closure type (saving path/index in the node)
            ::HIR::GenericParams    params;
            ::HIR::PathParams   constructor_path_params;
            ::HIR::PathParams   impl_path_params;
            // - 0xFFFF "Self" -> 0 "Super"
            constructor_path_params.m_types.push_back( ::HIR::TypeRef("Self", 0xFFFF) );
            params.m_types.push_back( ::HIR::TypeParamDef { "Super", {}, false } );  // TODO: Maybe Self is sized?
            // - Top-level params come first
            unsigned ofs_impl = params.m_types.size();
            for(const auto& ty_def : m_resolve.impl_generics().m_types) {
                constructor_path_params.m_types.push_back( ::HIR::TypeRef( ty_def.m_name, 0*256 + params.m_types.size() - ofs_impl ) );
                params.m_types.push_back( ::HIR::TypeParamDef { ty_def.m_name, {}, ty_def.m_is_sized } );
            }
            // - Item-level params come second
            unsigned ofs_item = params.m_types.size();
            for(const auto& ty_def : m_resolve.item_generics().m_types) {
                constructor_path_params.m_types.push_back( ::HIR::TypeRef( ty_def.m_name, 1*256 + params.m_types.size() - ofs_item ) );
                params.m_types.push_back( ::HIR::TypeParamDef { ty_def.m_name, {}, ty_def.m_is_sized } );
            }
            // Create placeholders for `monomorph_cb` to use
            ::std::vector<::HIR::TypeRef>   params_placeholders;
            for(unsigned int i = 0; i < params.m_types.size(); i ++) {
                params_placeholders.push_back( ::HIR::TypeRef(params.m_types[i].m_name, i) );
                impl_path_params.m_types.push_back( ::HIR::TypeRef(params.m_types[i].m_name, i) );
            }
            
            auto monomorph_cb = [&](const auto& ty)->const auto& {
                const auto& ge = ty.m_data.as_Generic();
                if( ge.binding == 0xFFFF ) {
                    return params_placeholders.at(0);
                }
                else if( ge.binding < 256 ) {
                    auto idx = ge.binding;
                    ASSERT_BUG(sp, ofs_impl + idx < params_placeholders.size(), "Impl generic binding OOR - " << ty << " (" << ofs_impl + idx << " !< " << params_placeholders.size() << ")");
                    return params_placeholders.at(ofs_impl + idx);
                }
                else if( ge.binding < 2*256 ) {
                    auto idx = ge.binding - 256;
                    ASSERT_BUG(sp, ofs_item + idx < params_placeholders.size(), "Item generic binding OOR - " << ty << " (" << ofs_item + idx << " !< " << params_placeholders.size() << ")");
                    return params_placeholders.at(ofs_item + idx);
                }
                else {
                    BUG(sp, "Generic type " << ty << " unknown");
                }
                };
            auto monomorph = [&](const auto& ty){ return monomorphise_type_with(sp, ty, monomorph_cb); };
            
            // - Clone the bounds (from both levels)
            auto monomorph_bound = [&](const ::HIR::GenericBound& b)->auto {
                TU_MATCHA( (b), (e),
                (Lifetime,
                    return ::HIR::GenericBound(e); ),
                (TypeLifetime,
                    return ::HIR::GenericBound::make_TypeLifetime({ monomorph(e.type), e.valid_for }); ),
                (TraitBound,
                    return ::HIR::GenericBound::make_TraitBound({ monomorph(e.type), monomorphise_traitpath_with(sp, e.trait, monomorph_cb, false) }); ),
                (TypeEquality,
                    return ::HIR::GenericBound::make_TypeEquality({ monomorph(e.type), monomorph(e.other_type) }); )
                )
                throw "";
                };
            for(const auto& bound : m_resolve.impl_generics().m_bounds ) {
                params.m_bounds.push_back( monomorph_bound(bound) );
            }
            for(const auto& bound : m_resolve.item_generics().m_bounds ) {
                params.m_bounds.push_back( monomorph_bound(bound) );
            }

            
            DEBUG("--- Mutate inner code");
            // 2. Iterate over the nodes and rewrite variable accesses to either renumbered locals, or field accesses
            // - TODO: Monomorphise all referenced types within this.
            ExprVisitor_Mutate    ev { node.m_res_type, ent.local_vars, ent.node.m_var_captures, monomorph_cb };
            ev.visit_node_ptr( node.m_code );
            
            // - Types of local variables
            DEBUG("--- Build locals and captures");
            ::std::vector< ::HIR::TypeRef>  local_types;
            local_types.push_back( ::HIR::TypeRef() );  // self - filled by make_fn*
            local_types.push_back( ::HIR::TypeRef() );  // args - filled by make_fn*
            for(const auto binding_idx : ent.local_vars) {
                auto ty_mono = monomorphise_type_with(sp, m_variable_types.at(binding_idx).clone(), monomorph_cb);
                local_types.push_back( mv$(ty_mono) );
            }
            // - Types of captured variables (to be monomorphised)
            ::std::vector< ::HIR::VisEnt< ::HIR::TypeRef> > capture_types;
            for(const auto binding_idx : node.m_var_captures) {
                auto ty_mono = monomorphise_type_with(sp, m_variable_types.at(binding_idx).clone(), monomorph_cb);
                capture_types.push_back( ::HIR::VisEnt< ::HIR::TypeRef> { false, mv$(ty_mono) } );
            }
            m_out_types.push_back( ::std::make_pair(
                FMT("closure_" << &node),
                ::HIR::Struct {
                    params.clone(),
                    ::HIR::Struct::Repr::Rust,
                    ::HIR::Struct::Data::make_Tuple(mv$(capture_types))
                    }
                ));
            
            // Mark the object pathname in the closure.
            node.m_obj_path = ::HIR::GenericPath( m_module_path + m_out_types.back().first, mv$(constructor_path_params) );
            //node.m_res_type = ::HIR::TypeRef( node.m_obj_path.clone() );
            DEBUG("-- Object name: " << node.m_obj_path);
            
            // - Args
            ::std::vector< ::HIR::Pattern>  args_pat_inner;
            ::std::vector< ::HIR::TypeRef>  args_ty_inner;
            for(const auto& arg : node.m_args) {
                args_pat_inner.push_back( arg.first.clone() );
                args_ty_inner.push_back( monomorphise_type_with(sp, arg.second, monomorph_cb) );
            }
            ::HIR::TypeRef  args_ty { mv$(args_ty_inner) };
            ::HIR::Pattern  args_pat { {}, ::HIR::Pattern::Data::make_Tuple({ mv$(args_pat_inner) }) };
            ::HIR::TypeRef  ret_type = monomorphise_type_with(sp, node.m_return, monomorph_cb);
            
            DEBUG("args_ty = " << args_ty << ", ret_type = " << ret_type);
            
            ::HIR::ExprPtr body_code { mv$(node.m_code) };
            body_code.m_bindings = mv$(local_types);
            
            // 3. Create trait impls
            //::HIR::TypeRef  closure_type = node.m_res_type.clone();
            ::HIR::TypeRef  closure_type = ::HIR::TypeRef( ::HIR::GenericPath(node.m_obj_path.m_path.clone(), mv$(impl_path_params)) );
            
            ::HIR::PathParams   trait_params;
            trait_params.m_types.push_back( args_ty.clone() );
            switch(node.m_class)
            {
            case ::HIR::ExprNode_Closure::Class::Unknown:
                node.m_class = ::HIR::ExprNode_Closure::Class::NoCapture;
            case ::HIR::ExprNode_Closure::Class::NoCapture:
            case ::HIR::ExprNode_Closure::Class::Shared: {
                const auto& lang_Fn = m_resolve.m_crate.get_lang_item_path(node.span(), "fn");
                const auto method_self_ty = ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, closure_type.clone() );
                
                // - FnOnce
                {
                    auto dispatch_node = NEWNODE(ret_type.clone(), CallPath, sp,
                        ::HIR::Path(closure_type.clone(), ::HIR::GenericPath(lang_Fn, trait_params.clone()), "call"),
                        make_vec2(
                            NEWNODE(method_self_ty.clone(), Borrow, sp, ::HIR::BorrowType::Shared, NEWNODE(closure_type.clone(), Variable, sp, "self", 0)),
                            NEWNODE(args_ty.clone(), Variable, sp, "arg", 1)
                            )
                        );
                    dynamic_cast<::HIR::ExprNode_CallPath&>(*dispatch_node).m_cache.m_arg_types = make_vec3( method_self_ty.clone(), args_ty.clone(), ret_type.clone() );
                    auto args_arg = ::std::make_pair(
                        ::HIR::Pattern { {false, ::HIR::PatternBinding::Type::Move, "args", 1}, {} },
                        args_ty.clone()
                        );
                    m_out_impls.push_back(::std::make_pair(
                        ::HIR::ExprNode_Closure::Class::Once,
                        H::make_fnonce( params.clone(), trait_params.clone(), closure_type.clone(), mv$(args_arg), ret_type.clone(), mv$(dispatch_node) )
                        ));
                }
                // - FnMut
                {
                    auto self_ty = ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Unique, closure_type.clone() );
                    auto dispatch_node = NEWNODE(ret_type.clone(), CallPath, sp,
                        ::HIR::Path(closure_type.clone(), ::HIR::GenericPath(lang_Fn, trait_params.clone()), "call"),
                        make_vec2(
                            NEWNODE(method_self_ty.clone(), Borrow, sp, ::HIR::BorrowType::Shared, NEWNODE(closure_type.clone(), Deref, sp, NEWNODE(mv$(self_ty), Variable, sp, "self", 0))),
                            NEWNODE(args_ty.clone(), Variable, sp, "arg", 1)
                            )
                        );
                    dynamic_cast<::HIR::ExprNode_CallPath&>(*dispatch_node).m_cache.m_arg_types = make_vec3( method_self_ty.clone(), args_ty.clone(), ret_type.clone() );
                    auto args_arg = ::std::make_pair(
                        ::HIR::Pattern { {false, ::HIR::PatternBinding::Type::Move, "args", 1}, {} },
                        args_ty.clone()
                        );
                    m_out_impls.push_back(::std::make_pair(
                        ::HIR::ExprNode_Closure::Class::Mut,
                        H::make_fnmut( params.clone(), trait_params.clone(), closure_type.clone(), mv$(args_arg), ret_type.clone(), mv$(dispatch_node) )
                        ));
                }
                
                // - Fn
                m_out_impls.push_back(::std::make_pair(
                    ::HIR::ExprNode_Closure::Class::Shared,
                    H::make_fn( mv$(params), mv$(trait_params), mv$(closure_type), ::std::make_pair(mv$(args_pat), mv$(args_ty)), mv$(ret_type), mv$(body_code) )
                    ));
                } break;
            case ::HIR::ExprNode_Closure::Class::Mut: {
                const auto& lang_FnMut = m_resolve.m_crate.get_lang_item_path(node.span(), "fn_mut");
                const auto method_self_ty = ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Unique, closure_type.clone() );
                
                // - FnOnce
                {
                    auto dispatch_node = NEWNODE(ret_type.clone(), CallPath, sp,
                        ::HIR::Path(closure_type.clone(), ::HIR::GenericPath(lang_FnMut, trait_params.clone()), "call_mut"),
                        make_vec2(
                            NEWNODE(method_self_ty.clone(), Borrow, sp, ::HIR::BorrowType::Unique, NEWNODE(closure_type.clone(), Variable, sp, "self", 0)),
                            NEWNODE(args_ty.clone(), Variable, sp, "arg", 1)
                            )
                        );
                    dynamic_cast<::HIR::ExprNode_CallPath&>(*dispatch_node).m_cache.m_arg_types = make_vec3( method_self_ty.clone(), args_ty.clone(), ret_type.clone() );
                    auto args_arg = ::std::make_pair(
                        ::HIR::Pattern { {false, ::HIR::PatternBinding::Type::Move, "args", 1}, {} },
                        args_ty.clone()
                        );
                    m_out_impls.push_back(::std::make_pair(
                        ::HIR::ExprNode_Closure::Class::Once,
                        H::make_fnonce( params.clone(), trait_params.clone(), closure_type.clone(), mv$(args_arg), ret_type.clone(), mv$(dispatch_node) )
                        ));
                }
                
                // - FnMut (code)
                m_out_impls.push_back(::std::make_pair(
                    ::HIR::ExprNode_Closure::Class::Mut,
                    H::make_fnmut( mv$(params), mv$(trait_params), mv$(closure_type), ::std::make_pair(mv$(args_pat), mv$(args_ty)), mv$(ret_type), mv$(body_code) )
                    ));
                } break;
            case ::HIR::ExprNode_Closure::Class::Once:
                // - FnOnce (code)
                m_out_impls.push_back(::std::make_pair(
                    ::HIR::ExprNode_Closure::Class::Once,
                    H::make_fnonce( mv$(params), mv$(trait_params), mv$(closure_type), ::std::make_pair(mv$(args_pat), mv$(args_ty)), mv$(ret_type), mv$(body_code) )
                    ));
                break;
            }
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
                mark_used_variable(node.m_slot, node.m_usage);
            }
            ::HIR::ExprVisitorDef::visit(node);
        }
        
        void visit(::HIR::ExprNode_CallValue& node) override
        {
            if( !m_closure_stack.empty() )
            {
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
                
                node.m_value->visit(*this);
                for(auto& arg : node.m_args)
                    arg->visit(*this);
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
        
    private:
        bool type_is_copy(const ::HIR::TypeRef& ty) const
        {
            return m_resolve.type_is_copy(ty);
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
            (SplitTuple,
                for( const auto& subpat : e.leading )
                    add_closure_def_from_pattern(sp, subpat);
                for( const auto& subpat : e.trailing )
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
            (StructValue,
                ),
            (StructTuple,
                for(const auto& field : e.sub_patterns) {
                    add_closure_def_from_pattern(sp, field);
                }
                ),
            (Struct,
                for( auto& field_pat : e.sub_patterns ) {
                    add_closure_def_from_pattern(sp, field_pat.second);
                }
                ),
            (EnumValue,
                ),
            (EnumTuple,
                for(const auto& field : e.sub_patterns) {
                    add_closure_def_from_pattern(sp, field);
                }
                ),
            (EnumStruct,
                for( auto& field_pat : e.sub_patterns ) {
                    add_closure_def_from_pattern(sp, field_pat.second);
                }
                )
            )
        }
        void mark_used_variable(unsigned int slot, ::HIR::ValueUsage usage)
        {
            //for(const auto& closure_rec : m_closure_stack)
            //{
            //    const auto& closure_defs = closure_rec.local_vars;
                const auto& closure_defs = m_closure_stack.back().local_vars;
                if( ::std::binary_search(closure_defs.begin(), closure_defs.end(), slot) ) {
                    // Ignore, this is local to the current closure
                    return ;
                }
            //}

            assert(m_closure_stack.size() > 0 );
            auto& closure_rec = m_closure_stack.back();
            auto& closure = closure_rec.node;
            
            auto it = ::std::lower_bound(closure.m_var_captures.begin(), closure.m_var_captures.end(), slot);
            if( it == closure.m_var_captures.end() || *it != slot ) {
                closure.m_var_captures.insert( it, slot );
            }
            DEBUG("Captured " << slot << " - " << m_variable_types.at(slot));
            
            switch( usage )
            {
            case ::HIR::ValueUsage::Unknown:
                BUG(Span(), "Unknown usage of variable " << slot);
            case ::HIR::ValueUsage::Borrow:
                closure.m_class = ::std::max(closure.m_class, ::HIR::ExprNode_Closure::Class::Shared);
                break;
            case ::HIR::ValueUsage::Mutate:
                closure.m_class = ::std::max(closure.m_class, ::HIR::ExprNode_Closure::Class::Mut);
                break;
            case ::HIR::ValueUsage::Move:
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
        out_types_t m_new_types;
        const ::HIR::SimplePath*  m_cur_mod_path;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_resolve(crate),
            m_cur_mod_path( nullptr )
        {}
        
        void visit_crate(::HIR::Crate& crate) override
        {
            Span    sp;
            ::HIR::Visitor::visit_crate(crate);
            
            for(auto& impl : m_new_trait_impls)
            {
                const auto& trait =
                    impl.first == ::HIR::ExprNode_Closure::Class::Once ? crate.get_lang_item_path(sp, "fn_once")
                    : impl.first == ::HIR::ExprNode_Closure::Class::Mut ? crate.get_lang_item_path(sp, "fn_mut")
                    : /*impl.first == ::HIR::ExprNode_Closure::Class::Shared ?*/ crate.get_lang_item_path(sp, "fn")
                    ;
                crate.m_trait_impls.insert( ::std::make_pair(trait.clone(), mv$(impl.second)) );
            }
            m_new_trait_impls.resize(0);
        }
        
        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            auto saved = m_cur_mod_path;
            auto path = p.get_simple_path();
            m_cur_mod_path = &path;
            ::HIR::Visitor::visit_module(p, mod);
            m_cur_mod_path = saved;
            
            // - Insert newly created closure types
            auto new_types = mv$(m_new_types);
            for(auto& ty_def : new_types)
            {
                mod.m_mod_items.insert( ::std::make_pair(
                    mv$(ty_def.first),
                    box$(( ::HIR::VisEnt< ::HIR::TypeItem> { false, ::HIR::TypeItem(mv$(ty_def.second)) } ))
                    ));
            }
        }
        
        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) override {
            BUG(Span(), "visit_expr hit in OuterVisitor");
        }
        
        void visit_type(::HIR::TypeRef& ty) override
        {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                this->visit_type( *e.inner );
                DEBUG("Array size " << ty);
                if( e.size ) {
                    //::std::vector< ::HIR::TypeRef>  tmp;
                    //ExprVisitor_Extract    ev(m_resolve, tmp, m_new_trait_impls);
                    //ev.visit_root( *e.size );
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
            auto _ = this->m_resolve.set_item_generics(item.m_params);
            if( item.m_code )
            {
                assert( m_cur_mod_path );
                DEBUG("Function code " << p);
                ExprVisitor_Extract    ev(m_resolve, *m_cur_mod_path, item.m_code.m_bindings, m_new_trait_impls, m_new_types);
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
                //::std::vector< ::HIR::TypeRef>  tmp;
                //ExprVisitor_Extract    ev(m_resolve, *m_cur_mod_path, tmp, m_new_trait_impls);
                //ev.visit_root(*item.m_value);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value )
            {
                //::std::vector< ::HIR::TypeRef>  tmp;
                //ExprVisitor_Extract    ev(m_resolve, tmp, m_new_trait_impls);
                //ev.visit_root(*item.m_value);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            //auto _ = this->m_ms.set_item_generics(item.m_params);

            //auto enum_type = ::HIR::TypeRef(::HIR::CoreType::Isize);
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    //DEBUG("Enum value " << p << " - " << var.first);
                    //::std::vector< ::HIR::TypeRef>  tmp;
                    //ExprVisitor_Extract    ev(m_resolve, tmp, m_new_trait_impls);
                    //ev.visit_root(*e);
                )
            }
        }
        
        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override
        {
            auto _ = this->m_resolve.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
        }
        
        
        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            m_cur_mod_path = &impl.m_src_module;
            
            ::HIR::Visitor::visit_type_impl(impl);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            m_cur_mod_path = &impl.m_src_module;
            
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        }
    };
}

void HIR_Expand_Closures(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

