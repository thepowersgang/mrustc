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

    typedef ::std::function< ::HIR::SimplePath(::HIR::Struct )>   new_type_cb_t;
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
    template<typename T>
    ::std::vector<T> make_vec3(T v1, T v2, T v3) {
        ::std::vector<T>    rv;
        rv.reserve(3);
        rv.push_back( mv$(v1) );
        rv.push_back( mv$(v2) );
        rv.push_back( mv$(v3) );
        return rv;
    }

    /// Mutate the contents of a closure to update captures, variables, and types
    class ExprVisitor_Mutate:
        public ::HIR::ExprVisitorDef
    {
        const ::HIR::TypeRef&   m_closure_type;
        const ::std::vector<unsigned int>&  m_local_vars;
        const ::std::vector< ::std::pair<unsigned int, ::HIR::ValueUsage> >&  m_captures;

        typedef ::std::function< const ::HIR::TypeRef&(const ::HIR::TypeRef&)>  t_monomorph_cb;
        t_monomorph_cb m_monomorph_cb;

        ::HIR::ExprNodeP    m_replacement;
    public:
        ExprVisitor_Mutate(
            const ::HIR::TypeRef& closure_type,
            const ::std::vector<unsigned int>& local_vars,
            const ::std::vector< ::std::pair<unsigned int, ::HIR::ValueUsage>>& captures,
            t_monomorph_cb mcb
            )
            :
            m_closure_type(closure_type),
            m_local_vars(local_vars),
            m_captures(captures),
            m_monomorph_cb(mcb)
        {
        }

        void visit_pattern(const Span& sp, ::HIR::Pattern& pat) override {
            if( pat.m_binding.is_valid() ) {
                auto binding_it = ::std::find(m_local_vars.begin(), m_local_vars.end(), pat.m_binding.m_slot);
                if( binding_it != m_local_vars.end() ) {
                    // NOTE: Offset of 1 is for `self` (`args` is destructured)
                    pat.m_binding.m_slot = 1 + binding_it - m_local_vars.begin();
                }
                else {
                    BUG(sp, "Pattern binds to non-local");
                }
            }

            TU_IFLET(::HIR::Pattern::Data, (pat.m_data), SplitSlice, e,
                TODO(sp, "Fixup binding in split slice");
            )

            ::HIR::ExprVisitorDef::visit_pattern(sp, pat);
        }

        void visit_type(::HIR::TypeRef& ty) override {
            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Generic, e,
                auto n = m_monomorph_cb(ty).clone();
                DEBUG(ty << " -> " << n);
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
            assert( ! node.m_code );

            // Fix params in path
            visit_generic_path( ::HIR::Visitor::PathContext::VALUE, node.m_obj_path );
            // Visit captures
            for(auto& subnode : node.m_captures)
            {
                visit_node_ptr(subnode);
            }
        }
        void visit(::HIR::ExprNode_Variable& node) override
        {
            // 1. Is it a closure-local?
            {
                auto binding_it = ::std::find(m_local_vars.begin(), m_local_vars.end(), node.m_slot);
                if( binding_it != m_local_vars.end() ) {
                    // NOTE: Offset of 1 is for `self` (`args` is destructured)
                    node.m_slot = 1 + binding_it - m_local_vars.begin();
                    return ;
                }
            }

            // 2. Is it a capture?
            {
                auto binding_it = ::std::find_if(m_captures.begin(), m_captures.end(), [&](const auto& x){return x.first == node.m_slot;});
                if( binding_it != m_captures.end() )
                {
                    m_replacement = NEWNODE(node.m_res_type.clone(), Field, node.span(),
                        get_self(node.span()),
                        FMT(binding_it - m_captures.begin())
                        );
                    if( binding_it->second != ::HIR::ValueUsage::Move ) {
                        auto bt = (binding_it->second == ::HIR::ValueUsage::Mutate ? ::HIR::BorrowType::Unique : ::HIR::BorrowType::Shared);

                        visit_type(m_replacement->m_res_type);
                        m_replacement->m_res_type = ::HIR::TypeRef::new_borrow( bt, mv$(m_replacement->m_res_type) );
                        m_replacement = NEWNODE(node.m_res_type.clone(), Deref, node.span(),  mv$(m_replacement));
                    }
                    m_replacement->m_usage = node.m_usage;
                    return ;
                }
            }

            BUG(node.span(), "Encountered non-captured and unknown-origin variable - " << node.m_name << " #" << node.m_slot);
        }

        ::HIR::ExprNodeP get_self(const Span& sp) const
        {
            ::HIR::ExprNodeP    self;
            switch( m_closure_type.m_data.as_Closure().node->m_class )
            {
            case ::HIR::ExprNode_Closure::Class::Unknown:
                // Assume it's NoCapture
            case ::HIR::ExprNode_Closure::Class::NoCapture:
            case ::HIR::ExprNode_Closure::Class::Shared:
                self = NEWNODE(m_closure_type.clone(), Deref, sp,
                    NEWNODE( ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, m_closure_type.clone()), Variable, sp, "self", 0)
                    );
                break;
            case ::HIR::ExprNode_Closure::Class::Mut:
                self = NEWNODE(m_closure_type.clone(), Deref, sp,
                    NEWNODE( ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, m_closure_type.clone()), Variable, sp, "self", 0)
                    );
                break;
            case ::HIR::ExprNode_Closure::Class::Once:
                self = NEWNODE(m_closure_type.clone(), Variable, sp, "self", 0);
                break;
            }
            return self;
        }
    };

    /// Visitor to replace closure types with actual type
    class ExprVisitor_Fixup:
        public ::HIR::ExprVisitorDef
    {
        const ::HIR::Crate& m_crate;
        t_cb_generic    m_monomorph_cb;
    public:
        ExprVisitor_Fixup(const ::HIR::Crate& crate, t_cb_generic monomorph_cb):
            m_crate(crate),
            m_monomorph_cb( mv$(monomorph_cb) )
        {
        }

        static void fix_type(const ::HIR::Crate& crate, t_cb_generic monomorph_cb, ::HIR::TypeRef& ty) {
            TU_IFLET( ::HIR::TypeRef::Data, ty.m_data, Closure, e,
                DEBUG("Closure: " << e.node->m_obj_path_base);
                auto path = monomorphise_genericpath_with(Span(), e.node->m_obj_path_base, monomorph_cb, false);
                const auto& str = crate.get_struct_by_path( Span(), path.m_path );
                DEBUG(ty << " -> " << path);
                ty = ::HIR::TypeRef::new_path( mv$(path), ::HIR::TypeRef::TypePathBinding::make_Struct(&str) );
            )
        }

        void visit_root(::HIR::ExprPtr& root)
        {
            TRACE_FUNCTION;

            root->visit(*this);
            visit_type(root->m_res_type);

            DEBUG("Locals");
            for(auto& ty : root.m_bindings)
                visit_type(ty);

            for(auto& ty : root.m_erased_types)
                visit_type(ty);
        }

        void visit_node_ptr(::HIR::ExprNodeP& node) override
        {
            node->visit(*this);
            visit_type(node->m_res_type);
        }

        void visit(::HIR::ExprNode_CallValue& node) override
        {
            TU_IFLET( ::HIR::TypeRef::Data, node.m_value->m_res_type.m_data, Closure, e,
                switch(e.node->m_class)
                {
                case ::HIR::ExprNode_Closure::Class::Unknown:
                    BUG(node.span(), "References an ::Unknown closure");
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
            )

            ::HIR::ExprVisitorDef::visit(node);
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            fix_type(m_crate, m_monomorph_cb, ty);
            ::HIR::ExprVisitorDef::visit_type(ty);
        }
    };

    struct H {
        static void fix_fn_params(::HIR::ExprPtr& code, const ::HIR::TypeRef& self_ty, const ::HIR::TypeRef& args_ty)
        {
            // TODO: The self_ty here is wrong, the borrow needs to be included.
            if( code.m_bindings.size() == 0 ) {
                // No bindings - Wrapper function
                // Insert 0 = Self, 1 = Args
                code.m_bindings.push_back( self_ty.clone() );
                code.m_bindings.push_back( args_ty.clone() );
            }
            else {
                // Bindings present - Actual code (which destructures `args`)
                assert( code.m_bindings.size() >= 1 );
                assert( code.m_bindings[0] == ::HIR::TypeRef() );
                code.m_bindings[0] = self_ty.clone();
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
            auto ty_of_self = closure_type.clone();
            fix_fn_params(code, ty_of_self, args_argent.second);
            return ::HIR::TraitImpl {
                mv$(params), mv$(trait_params), mv$(closure_type),
                make_map1(
                    ::std::string("call_once"), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { false, ::HIR::Function {
                        false, ::HIR::Linkage {},
                        ::HIR::Function::Receiver::Value,
                        ABI_RUST, false, false,
                        {},
                        make_vec2(
                            ::std::make_pair(
                                ::HIR::Pattern { {false, ::HIR::PatternBinding::Type::Move, "self", 0}, {} },
                                mv$(ty_of_self)
                                ),
                            mv$( args_argent )
                            ), false,
                        ret_ty.clone(),
                        mv$(code)
                        } }
                    ),
                {},
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
            auto ty_of_self = ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Unique, closure_type.clone() );
            fix_fn_params(code, ty_of_self, args_argent.second);
            return ::HIR::TraitImpl {
                mv$(params), mv$(trait_params), mv$(closure_type),
                make_map1(
                    ::std::string("call_mut"), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { false, ::HIR::Function {
                        false, ::HIR::Linkage {},
                        ::HIR::Function::Receiver::BorrowUnique,
                        ABI_RUST, false, false,
                        {},
                        make_vec2(
                            ::std::make_pair(
                                ::HIR::Pattern { {false, ::HIR::PatternBinding::Type::Move, "self", 0}, {} },
                                mv$(ty_of_self)
                                ),
                            mv$( args_argent )
                            ), false,
                        ret_ty.clone(),
                        mv$(code)
                        } }
                    ),
                {},
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
            auto ty_of_self = ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, closure_type.clone() );
            fix_fn_params(code, ty_of_self, args_argent.second);
            return ::HIR::TraitImpl {
                mv$(params), mv$(trait_params), mv$(closure_type),
                make_map1(
                    ::std::string("call"), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { false, ::HIR::Function {
                        false, ::HIR::Linkage {},
                        ::HIR::Function::Receiver::BorrowShared,
                        ABI_RUST, false, false,
                        {},
                        make_vec2(
                            ::std::make_pair(
                                ::HIR::Pattern { {false, ::HIR::PatternBinding::Type::Move, "self", 0}, {} },
                                mv$(ty_of_self)
                                ),
                            mv$(args_argent)
                            ), false,
                        ret_ty.clone(),
                        mv$(code)
                        } }
                    ),
                {},
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
            // - Lists captured variables to be stored in autogenerated struct (and how they're used, influencing the borrow type)
            ::std::vector< ::std::pair<unsigned int, ::HIR::ValueUsage> > captured_vars;

            ClosureScope(::HIR::ExprNode_Closure& node):
                node(node)
            {
            }
        };

        const StaticTraitResolve& m_resolve;
        const ::HIR::TypeRef*   m_self_type;
        ::std::vector< ::HIR::TypeRef>& m_variable_types;

        // Outputs
        out_impls_t&    m_out_impls;
        const new_type_cb_t&    m_new_type;

        /// Stack of active closures
        ::std::vector<ClosureScope> m_closure_stack;

    public:
        ExprVisitor_Extract(const StaticTraitResolve& resolve, const ::HIR::TypeRef* self_type, ::std::vector< ::HIR::TypeRef>& var_types, out_impls_t& out_impls, const new_type_cb_t& new_type):
            m_resolve(resolve),
            m_self_type(self_type),
            m_variable_types(var_types),
            m_out_impls( out_impls ),
            m_new_type( new_type )
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

            // - If this closure is a move closure, mutate `captured_vars` such that all captures are tagged with ValueUsage::Move
            if( node.m_is_move )
            {
                for(auto& cap : ent.captured_vars)
                {
                    cap.second = ::HIR::ValueUsage::Move;
                }
            }
            // --- Apply the capture set for this closure to the parent ---
            if( m_closure_stack.size() > 0 )
            {
                DEBUG("> Apply to parent");
                for(const auto& cap : ent.captured_vars)
                {
                    mark_used_variable(node.span(), cap.first, cap.second);
                }
            }


            // --- Extract and mutate code into a trait impl on the closure type ---

            // 1. Construct closure type (saving path/index in the node)
            ::HIR::GenericParams    params;
            ::HIR::PathParams   constructor_path_params;
            ::HIR::PathParams   impl_path_params;
            // - 0xFFFF "Self" -> 0 "Super" (if present)
            if( m_resolve.has_self() )
            {
                assert( m_self_type );
                constructor_path_params.m_types.push_back( m_self_type->clone() );
                params.m_types.push_back( ::HIR::TypeParamDef { "Super", {}, false } );  // TODO: Determine if parent Self is Sized
            }
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

            auto monomorph_cb = [&](const auto& ty)->const ::HIR::TypeRef& {
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
            auto cb_replace = [&](const auto& tpl, auto& rv)->bool {
                if( tpl.m_data.is_Infer() ) {
                    BUG(sp, "");
                }
                else if( tpl.m_data.is_Generic() ) {
                    rv = monomorph_cb(tpl).clone();
                    return true;
                }
                //else if( tpl.m_data.is_ErasedType() ) {
                //    const auto& e = tpl.m_data.as_ErasedType();
                //
                //    // TODO: Share code with
                //    TODO(sp, "Repalce ErasedType with origin " << e.m_origin << " #" << e.m_index);
                //    //ASSERT_BUG(sp, e.m_index < fcn_ptr->m_code.m_erased_types.size(), "");
                //    //const auto& erased_type_replacement = fcn_ptr->m_code.m_erased_types.at(e.m_index);
                //    //rv = monomorphise_type_with(sp, erased_type_replacement,  monomorph_cb, false);
                //    //return true;
                //}
                else {
                    return false;
                }
                };

            // - Clone the bounds (from both levels)
            auto monomorph_bound = [&](const ::HIR::GenericBound& b)->::HIR::GenericBound {
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
            ExprVisitor_Mutate    ev { node.m_res_type, ent.local_vars, ent.captured_vars, monomorph_cb };
            ev.visit_node_ptr( node.m_code );
            // NOTE: `ev` is used down in `Args` to convert the argument destructuring pattern

            // - Types of local variables
            DEBUG("--- Build locals and captures");
            ::std::vector< ::HIR::TypeRef>  local_types;
            local_types.push_back( ::HIR::TypeRef() );  // self - filled by make_fn*
            for(const auto binding_idx : ent.local_vars) {
                auto ty_mono = monomorphise_type_with(sp, m_variable_types.at(binding_idx).clone(), monomorph_cb);
                local_types.push_back( mv$(ty_mono) );
            }
            // - Generate types of captures, and construct the actual capture values
            //  > Capture types (with borrows and using closure's type params)
            ::std::vector< ::HIR::VisEnt< ::HIR::TypeRef> > capture_types;
            //  > Capture value nodes
            ::std::vector< ::HIR::ExprNodeP>    capture_nodes;
            capture_types.reserve( ent.captured_vars.size() );
            capture_nodes.reserve( ent.captured_vars.size() );
            for(const auto binding : ent.captured_vars)
            {
                const auto binding_idx = binding.first;
                auto binding_type = binding.second;

                const auto& cap_ty = m_variable_types.at(binding_idx);
                auto ty_mono = monomorphise_type_with(sp, cap_ty, monomorph_cb);

                auto val_node = NEWNODE(cap_ty.clone(), Variable,  sp, "", binding_idx);
                ::HIR::BorrowType   bt;

                switch(binding_type)
                {
                case ::HIR::ValueUsage::Unknown:
                    BUG(sp, "ValueUsage::Unkown on " << binding_idx);
                case ::HIR::ValueUsage::Borrow:
                    DEBUG("Capture by & _#" << binding_idx << " : " << binding_type);
                    bt = ::HIR::BorrowType::Shared;
                    capture_nodes.push_back(NEWNODE( ::HIR::TypeRef::new_borrow(bt, cap_ty.clone()), Borrow,  sp, bt, mv$(val_node) ));
                    ty_mono = ::HIR::TypeRef::new_borrow(bt, mv$(ty_mono));
                    break;
                case ::HIR::ValueUsage::Mutate:
                    DEBUG("Capture by &mut _#" << binding_idx << " : " << binding_type);
                    bt = ::HIR::BorrowType::Unique;
                    capture_nodes.push_back(NEWNODE( ::HIR::TypeRef::new_borrow(bt, cap_ty.clone()), Borrow,  sp, bt, mv$(val_node) ));
                    ty_mono = ::HIR::TypeRef::new_borrow(bt, mv$(ty_mono));
                    break;
                case ::HIR::ValueUsage::Move:
                    DEBUG("Capture by value _#" << binding_idx << " : " << binding_type);
                    capture_nodes.push_back( mv$(val_node) );
                    break;
                }

                // - Fix type to replace closure types with known paths
                ExprVisitor_Fixup   fixup { m_resolve.m_crate, monomorph_cb };
                fixup.visit_type(ty_mono);
                capture_types.push_back( ::HIR::VisEnt< ::HIR::TypeRef> { false, mv$(ty_mono) } );
            }
            auto closure_struct_path = m_new_type(
                ::HIR::Struct {
                    params.clone(),
                    ::HIR::Struct::Repr::Rust,
                    ::HIR::Struct::Data::make_Tuple(mv$(capture_types))
                    }
                );
            const auto& closure_struct_ref = m_resolve.m_crate.get_struct_by_path(sp, closure_struct_path);

            // Mark the object pathname in the closure.
            node.m_obj_path = ::HIR::GenericPath( closure_struct_path, mv$(constructor_path_params) );
            node.m_obj_path_base = node.m_obj_path.clone();
            node.m_captures = mv$(capture_nodes);
            //node.m_res_type = ::HIR::TypeRef( node.m_obj_path.clone() );
            DEBUG("-- Object name: " << node.m_obj_path);
            ::HIR::TypeRef  closure_type = ::HIR::TypeRef( ::HIR::GenericPath(node.m_obj_path.m_path.clone(), mv$(impl_path_params)) );
            closure_type.m_data.as_Path().binding = ::HIR::TypeRef::TypePathBinding::make_Struct(&closure_struct_ref);

            // - Args
            ::std::vector< ::HIR::Pattern>  args_pat_inner;
            ::std::vector< ::HIR::TypeRef>  args_ty_inner;

            for(const auto& arg : node.m_args) {
                args_pat_inner.push_back( arg.first.clone() );
                ev.visit_pattern(sp, args_pat_inner.back() );
                args_ty_inner.push_back( clone_ty_with(sp, arg.second, cb_replace) );
            }
            ::HIR::TypeRef  args_ty { mv$(args_ty_inner) };
            ::HIR::Pattern  args_pat { {}, ::HIR::Pattern::Data::make_Tuple({ mv$(args_pat_inner) }) };
            ::HIR::TypeRef  ret_type = clone_ty_with(sp, node.m_return, cb_replace);

            DEBUG("args_ty = " << args_ty << ", ret_type = " << ret_type);

            ::HIR::ExprPtr body_code { mv$(node.m_code) };
            body_code.m_bindings = mv$(local_types);

            {
                DEBUG("-- Fixing types in body code");
                ExprVisitor_Fixup   fixup { m_resolve.m_crate, monomorph_cb };
                fixup.visit_root( body_code );

                DEBUG("-- Fixing types in signature");
                fixup.visit_type( args_ty );
                fixup.visit_type( ret_type );
                // TODO: Replace erased types too
            }

            // ---
            // 3. Create trait impls
            // ---
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

        void visit_pattern(const Span& sp, ::HIR::Pattern& pat) override
        {
            if( !m_closure_stack.empty() )
            {
                add_closure_def_from_pattern(sp, pat);
            }
        }
        void visit(::HIR::ExprNode_Variable& node) override
        {
            if( !m_closure_stack.empty() )
            {
                mark_used_variable(node.span(), node.m_slot, node.m_usage);
            }
            ::HIR::ExprVisitorDef::visit(node);
        }

        void visit(::HIR::ExprNode_CallValue& node) override
        {
            const auto& fcn_ty = node.m_value->m_res_type;
            DEBUG("_CallValue - " << fcn_ty);
            if( !m_closure_stack.empty() )
            {
                TRACE_FUNCTION_F("_CallValue");
                if( node.m_trait_used == ::HIR::ExprNode_CallValue::TraitUsed::Unknown )
                {
                    if( fcn_ty.m_data.is_Closure() )
                    {
                        const auto& cn = *fcn_ty.m_data.as_Closure().node;
                        // Use the closure's class to determine if & or &mut should be taken (and which function to use)
                        ::HIR::ValueUsage   vu = ::HIR::ValueUsage::Unknown;
                        switch(cn.m_class)
                        {
                        case ::HIR::ExprNode_Closure::Class::Unknown:
                        case ::HIR::ExprNode_Closure::Class::NoCapture:
                        case ::HIR::ExprNode_Closure::Class::Shared:
                            vu = ::HIR::ValueUsage::Borrow;
                            break;
                        case ::HIR::ExprNode_Closure::Class::Mut:
                            vu = ::HIR::ValueUsage::Mutate;
                            break;
                        case ::HIR::ExprNode_Closure::Class::Once:
                            vu = ::HIR::ValueUsage::Move;
                            break;
                        }
                        node.m_value->m_usage = vu;
                    }
                    else
                    {
                        // Must be a function pointer, leave it
                    }
                }
                else
                {
                    // If the trait is known, then the &/&mut has been added
                }
                ::HIR::ExprVisitorDef::visit(node);
            }
            else if( fcn_ty.m_data.is_Closure() )
            {
                //TODO(node.span(), "Determine how value in CallValue is used on a closure");
                ::HIR::ExprVisitorDef::visit(node);
            }
            else
            {
                ::HIR::ExprVisitorDef::visit(node);
            }
        }

    private:
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
                add_closure_def_from_pattern(sp, *e.sub);
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
        void mark_used_variable(const Span& sp, unsigned int slot, ::HIR::ValueUsage usage)
        {
            const auto& closure_defs = m_closure_stack.back().local_vars;
            if( ::std::binary_search(closure_defs.begin(), closure_defs.end(), slot) ) {
                // Ignore, this is local to the current closure
                return ;
            }


            if( usage == ::HIR::ValueUsage::Move ) {
                if( m_resolve.type_is_copy(sp, m_variable_types.at(slot)) ) {
                    usage = ::HIR::ValueUsage::Borrow;
                }
                // Wait, is this valid?
                // - Maybe it's needed becuase reborrow is after this pass?
                else if( m_variable_types.at(slot).m_data.is_Borrow() && m_variable_types.at(slot).m_data.as_Borrow().type == ::HIR::BorrowType::Unique ) {
                    usage = ::HIR::ValueUsage::Mutate;
                }
                else {
                }
            }

            assert(m_closure_stack.size() > 0 );
            auto& closure_rec = m_closure_stack.back();
            auto& closure = closure_rec.node;

            auto it = ::std::lower_bound(closure_rec.captured_vars.begin(), closure_rec.captured_vars.end(), slot, [](const auto& a, const auto& b){ return a.first < b; });
            if( it == closure_rec.captured_vars.end() || it->first != slot ) {
                closure_rec.captured_vars.insert( it, ::std::make_pair(slot, usage) );
            }
            else {
                it->second = ::std::max(it->second, usage);
            }

            const char* cap_type_name = "?";
            switch( usage )
            {
            case ::HIR::ValueUsage::Unknown:
                BUG(sp, "Unknown usage of variable " << slot);
            case ::HIR::ValueUsage::Borrow:
                cap_type_name = "Borrow";
                closure.m_class = ::std::max(closure.m_class, ::HIR::ExprNode_Closure::Class::Shared);
                break;
            case ::HIR::ValueUsage::Mutate:
                cap_type_name = "Mutate";
                closure.m_class = ::std::max(closure.m_class, ::HIR::ExprNode_Closure::Class::Mut);
                break;
            case ::HIR::ValueUsage::Move:
                //if( m_resolve.type_is_copy( sp, m_variable_types.at(slot) ) ) {
                //    closure.m_class = ::std::max(closure.m_class, ::HIR::ExprNode_Closure::Class::Shared);
                //}
                //else {
                    cap_type_name = "Move";
                    closure.m_class = ::std::max(closure.m_class, ::HIR::ExprNode_Closure::Class::Once);
                //}
                break;
            }
            DEBUG("Captured " << slot << " - " << m_variable_types.at(slot) << " :: " << cap_type_name);
        }
    };

    class OuterVisitor:
        public ::HIR::Visitor
    {
        StaticTraitResolve  m_resolve;
        out_impls_t m_new_trait_impls;
        new_type_cb_t   m_new_type;
        const ::HIR::SimplePath*  m_cur_mod_path;
        const ::HIR::TypeRef*   m_self_type = nullptr;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_resolve(crate),
            m_cur_mod_path( nullptr )
        {}

        void visit_crate(::HIR::Crate& crate) override
        {
            Span    sp;

            unsigned int closure_count = 0;
            ::HIR::SimplePath   root_mod_path(crate.m_crate_name,{});
            m_cur_mod_path = &root_mod_path;
            m_new_type = [&](auto s)->auto {
                auto name = FMT("closure_I_" << closure_count);
                closure_count += 1;
                auto boxed = box$(( ::HIR::VisEnt< ::HIR::TypeItem> { false, ::HIR::TypeItem( mv$(s) ) } ));
                crate.m_root_module.m_mod_items.insert( ::std::make_pair(name, mv$(boxed)) );
                return ::HIR::SimplePath(crate.m_crate_name, {}) + name;
                };

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

            unsigned int closure_count = 0;
            auto saved_nt = mv$(m_new_type);
            m_new_type = [&](auto s)->auto {
                auto name = FMT("closure_" << closure_count);
                closure_count += 1;
                auto boxed = box$( (::HIR::VisEnt< ::HIR::TypeItem> { false, ::HIR::TypeItem( mv$(s) ) }) );
                mod.m_mod_items.insert( ::std::make_pair(name, mv$(boxed)) );
                return (p + name).get_simple_path();
                };

            ::HIR::Visitor::visit_module(p, mod);

            m_cur_mod_path = saved;
            m_new_type = mv$(saved_nt);
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

                {
                    ExprVisitor_Extract    ev(m_resolve, m_self_type, item.m_code.m_bindings, m_new_trait_impls, m_new_type);
                    ev.visit_root( *item.m_code );
                }

                {
                    ExprVisitor_Fixup   fixup(m_resolve.m_crate, [](const auto& x)->const auto&{ return x; });
                    fixup.visit_root( item.m_code );
                }
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
                //ExprVisitor_Extract    ev(m_resolve, m_self_type, tmp, m_new_trait_impls);
                //ev.visit_root(*item.m_value);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value )
            {
                //::std::vector< ::HIR::TypeRef>  tmp;
                //ExprVisitor_Extract    ev(m_resolve, m_self_type, tmp, m_new_trait_impls);
                //ev.visit_root(*item.m_value);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            //auto _ = this->m_ms.set_item_generics(item.m_params);

            /*
            if(const auto* e = item.m_data.opt_Value())
            {
                auto enum_type = ::HIR::TypeRef(::HIR::CoreType::Isize);
                for(auto& var : e->variants)
                {
                    DEBUG("Enum value " << p << " - " << var.name);
                    ::std::vector< ::HIR::TypeRef>  tmp;
                    ExprVisitor_Extract    ev(m_resolve, tmp, m_new_trait_impls);
                    ev.visit_root(var.expr);
                }
            }
            */
        }

        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override
        {
            auto _ = this->m_resolve.set_impl_generics(item.m_params);
            ::HIR::TypeRef  self("Self", 0xFFFF);
            m_self_type = &self;
            ::HIR::Visitor::visit_trait(p, item);
            m_self_type = nullptr;
        }


        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            m_self_type = &impl.m_type;
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);

            ::HIR::Visitor::visit_type_impl(impl);

            m_self_type = nullptr;
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << trait_path << " for " << impl.m_type);
            m_self_type = &impl.m_type;
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);

            ::HIR::Visitor::visit_trait_impl(trait_path, impl);

            m_self_type = nullptr;
        }
    };
}

void HIR_Expand_Closures_Expr(const ::HIR::Crate& crate, ::HIR::ExprPtr& exp)
{
    // TODO:
}

void HIR_Expand_Closures(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

