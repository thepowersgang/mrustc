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
#include <hir/expr_state.hpp>
#include "main_bindings.hpp"

namespace {
    inline HIR::ExprNodeP mk_exprnodep(HIR::ExprNode* en, ::HIR::TypeRef ty){ en->m_res_type = mv$(ty); return HIR::ExprNodeP(en); }
}
#define NEWNODE(TY, CLASS, ...)  mk_exprnodep(new HIR::ExprNode_##CLASS(__VA_ARGS__), TY)

namespace {

    typedef ::std::function< ::std::pair<::HIR::SimplePath, const ::HIR::TypeItem*>(const char* prefix, const char* suffix, ::HIR::TypeItem )>   new_type_cb_t;
    typedef ::std::vector< ::std::pair< ::HIR::ExprNode_Closure::Class, ::HIR::TraitImpl> > out_impls_closure_t;
    typedef ::std::vector< ::HIR::TraitImpl > out_impls_generator_t;
    struct OutState
    {
        out_impls_closure_t     impls_closure;
        out_impls_generator_t   impls_generator;
        ::std::vector< ::HIR::TraitImpl >   impls_drop;

        new_type_cb_t   new_type;

        void push_new_impls(const Span& sp, ::HIR::Crate& crate);
    };

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

    void OutState::push_new_impls(const Span& sp, ::HIR::Crate& crate)
    {
        auto push_trait_impl = [&](const ::HIR::SimplePath& p, std::unique_ptr<::HIR::TraitImpl> ptr) {
            auto& trait_impl_list_r = crate.m_all_trait_impls[p].get_list_for_type_mut(ptr->m_type);
            trait_impl_list_r.push_back(ptr.get());
            auto& trait_impl_list   = crate.m_trait_impls[p].get_list_for_type_mut(ptr->m_type);
            trait_impl_list.push_back(mv$(ptr));
            };
        for(auto& impl : this->impls_closure)
        {
            switch(impl.first)
            {
            case ::HIR::ExprNode_Closure::Class::Once:
                push_trait_impl(crate.get_lang_item_path(sp, "fn_once"), box$(impl.second));
                break;
            case ::HIR::ExprNode_Closure::Class::Mut:
                push_trait_impl(crate.get_lang_item_path(sp, "fn_mut" ), box$(impl.second));
                break;
            case ::HIR::ExprNode_Closure::Class::Shared:
                push_trait_impl(crate.get_lang_item_path(sp, "fn"     ), box$(impl.second));
                break;
            case ::HIR::ExprNode_Closure::Class::NoCapture: {
                assert(impl.second.m_methods.size() == 1);
                assert(impl.second.m_types.empty());
                assert(impl.second.m_constants.empty());
                // NOTE: This should always have a name
                const auto& path = impl.second.m_type.data().as_Path().path.m_data.as_Generic().m_path;
                DEBUG("Adding type impl " << path);
                auto ptr = box$(::HIR::TypeImpl {
                    mv$(impl.second.m_params),
                    mv$(impl.second.m_type),
                    make_map1(
                        impl.second.m_methods.begin()->first,
                        ::HIR::TypeImpl::VisImplEnt< ::HIR::Function> { ::HIR::Publicity::new_global(), false,  mv$(impl.second.m_methods.begin()->second.data) }
                        ),
                    {},
                    mv$(impl.second.m_src_module)
                    });
                crate.m_all_type_impls.named[path].push_back( ptr.get() );
                crate.m_type_impls.named[path].push_back( mv$(ptr) );
                } break;
            case ::HIR::ExprNode_Closure::Class::Unknown:
                BUG(Span(), "Encountered Unkown closure type in new impls");
                break;
            }
        }
        for(auto& impl : this->impls_generator)
        {
            push_trait_impl( crate.get_lang_item_path(sp, "generator"), box$(impl) );
        }
        for(auto& impl : this->impls_drop)
        {
            push_trait_impl( crate.get_lang_item_path(sp, "drop"), box$(impl) );
        }
        this->impls_closure.resize(0);
        this->impls_generator.resize(0);
        this->impls_drop.resize(0);
    }

    /// Mutate the contents of a closure to update captures, variables, and types
    class ExprVisitor_Mutate:
        public ::HIR::ExprVisitorDef
    {
        const ::HIR::TypeRef&   m_closure_type;
        const ::std::vector<unsigned int>&  m_local_vars;
        const ::std::vector< ::std::pair<unsigned int, ::HIR::ValueUsage> >&  m_captures;

        const Monomorphiser& m_monomorphiser;

        ::HIR::ExprNodeP    m_replacement;
    public:
        ExprVisitor_Mutate(
            const ::HIR::TypeRef& closure_type,
            const ::std::vector<unsigned int>& local_vars,
            const ::std::vector< ::std::pair<unsigned int, ::HIR::ValueUsage>>& captures,
            const Monomorphiser& mcb
            )
            :
            m_closure_type(closure_type),
            m_local_vars(local_vars),
            m_captures(captures),
            m_monomorphiser(mcb)
        {
        }

        void visit_pattern(const Span& sp, ::HIR::Pattern& pat) override {
            for(auto& pb : pat.m_bindings)
            {
                auto binding_it = ::std::find(m_local_vars.begin(), m_local_vars.end(), pb.m_slot);
                if( binding_it != m_local_vars.end() ) {
                    // NOTE: Offset of 1 is for `self` (`args` is destructured)
                    pb.m_slot = 1 + binding_it - m_local_vars.begin();
                }
                else {
                    BUG(sp, "Pattern binds to non-local - " << pb);
                }
            }

            if(auto* e = pat.m_data.opt_SplitSlice())
            {
                if( e->extra_bind.is_valid() )
                {
                    auto binding_it = ::std::find(m_local_vars.begin(), m_local_vars.end(), e->extra_bind.m_slot);
                    if( binding_it != m_local_vars.end() ) {
                        // NOTE: Offset of 1 is for `self` (`args` is destructured)
                        e->extra_bind.m_slot = 1 + binding_it - m_local_vars.begin();
                    }
                    else {
                        BUG(sp, "Pattern (split slice extra) binds to non-local - " << e->extra_bind);
                    }
                }
            }

            ::HIR::ExprVisitorDef::visit_pattern(sp, pat);
        }

        void visit_type(::HIR::TypeRef& ty) override {
            ty = m_monomorphiser.monomorph_type(Span(), ty, /*allow_infer=*/true);
#if 0
            if( ty.data().is_Generic() ) {
                auto n = m_monomorph_cb(ty).clone();
                DEBUG(ty << " -> " << n);
                ty = mv$(n);
            }
            else {
                ::HIR::ExprVisitorDef::visit_type(ty);
            }
#endif
        }

        void visit_node_ptr(::HIR::ExprNodeP& node_ptr) override {
            assert( node_ptr );
            auto& node = *node_ptr;
            const char* node_ty = typeid(node).name();
            TRACE_FUNCTION_FR("[_Mutate] " << &node << " " << node_ty << " : " << node.m_res_type, node_ty);
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
                    auto new_slot = 1 + binding_it - m_local_vars.begin();
                    DEBUG("_Variable: #" << node.m_slot << " -> #" << new_slot);
                    node.m_slot = new_slot;
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
                        RcString::new_interned(FMT(binding_it - m_captures.begin()))
                        );
                    if( binding_it->second != ::HIR::ValueUsage::Move ) {
                        auto bt = (binding_it->second == ::HIR::ValueUsage::Mutate ? ::HIR::BorrowType::Unique : ::HIR::BorrowType::Shared);

                        visit_type(m_replacement->m_res_type);
                        m_replacement->m_res_type = ::HIR::TypeRef::new_borrow( bt, mv$(m_replacement->m_res_type) );
                        m_replacement = NEWNODE(node.m_res_type.clone(), Deref, node.span(),  mv$(m_replacement));
                    }
                    m_replacement->m_usage = node.m_usage;
                    DEBUG("_Variable: #" << node.m_slot << " -> capture");
                    return ;
                }
            }

            BUG(node.span(), "Encountered non-captured and unknown-origin variable - " << node.m_name << " #" << node.m_slot);
        }

        ::HIR::ExprNodeP get_self(const Span& sp) const
        {
            ::HIR::ExprNodeP    self;
            switch( m_closure_type.data().as_Closure().node->m_class )
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
    public:
        const ::HIR::Crate& m_crate;
        StaticTraitResolve  m_resolve;
        const Monomorphiser&    m_monomorphiser;
        bool    m_run_eat;
    public:
        ExprVisitor_Fixup(const ::HIR::Crate& crate, const ::HIR::GenericParams* params, const Monomorphiser& monomorphiser):
            m_crate(crate),
            m_resolve(crate),
            m_monomorphiser( monomorphiser ),
            m_run_eat(false)
        {
            if( params ) {
                m_resolve.set_impl_generics_raw(*params);
                m_run_eat = true;
            }
        }

        static void fix_type(const ::HIR::Crate& crate, const Span& sp, const Monomorphiser& monomorphiser, ::HIR::TypeRef& ty) {
            if( const auto* e = ty.data().opt_Closure() )
            {
                DEBUG("Closure: " << e->node->m_obj_path_base); // TODO: Why does this use the `_base`
                auto path = monomorphiser.monomorph_genericpath(sp, e->node->m_obj_path_base, false);
                const auto& str = *e->node->m_obj_ptr;
                DEBUG(ty << " -> " << path);
                ty = ::HIR::TypeRef::new_path( mv$(path), ::HIR::TypePathBinding::make_Struct(&str) );
            }
            if(const auto* e = ty.data().opt_Generator() )
            {
                DEBUG("Generator: " << e->node->m_obj_path);
                auto path = monomorphiser.monomorph_genericpath(sp, e->node->m_obj_path, false);
                const auto& str = *e->node->m_obj_ptr;
                DEBUG(ty << " -> " << path);
                ty = ::HIR::TypeRef::new_path( mv$(path), ::HIR::TypePathBinding::make_Struct(&str) );
            }

            if( auto* e = ty.data_mut().opt_Path() )
            {
                if( e->binding.is_Unbound() && e->path.m_data.is_UfcsKnown() )
                {
                    e->binding = ::HIR::TypePathBinding::make_Opaque({});
                }
            }
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

        void visit(::HIR::ExprNode_Cast& node) override
        {
            const Span& sp = node.span();
            // Handle casts from closures to function pointers
            if( node.m_value->m_res_type.data().is_Closure() )
            {
                TRACE_FUNCTION_FR("_Cast: " << &node << " " << node.m_value->m_res_type, node.m_value->m_res_type);

                const auto& src_te = node.m_value->m_res_type.data().as_Closure();
                ASSERT_BUG(sp, node.m_res_type.data().is_Function(), "Cannot convert closure to non-fn type");
                //const auto& dte = node.m_res_type.m_data.as_Function();
                if( src_te.node->m_class != ::HIR::ExprNode_Closure::Class::NoCapture )
                {
                    ERROR(sp, E0000, "Cannot cast a closure with captures to a fn() type");
                }

                //auto ms = MonomorphStatePtr(nullptr, &src_te.node->m_obj_path.m_params, nullptr);
                //auto monomorph = [&](const HIR::TypeRef& ty)->HIR::TypeRef { return src_te.node->m_obj_ptr ? ms.monomorph_type(sp, ty) : ty.clone_shallow(); };
                //::HIR::FunctionType    fcn_ty_inner { /*is_unsafe=*/false, ABI_RUST, monomorph(src_te.node->m_return), {} };
                //fcn_ty_inner.m_arg_types.reserve(src_te.node->m_args.size());
                //for(const auto& arg : src_te.node->m_args) {
                //    fcn_ty_inner.m_arg_types.push_back( monomorph(arg.second) );
                //}
                ::HIR::FunctionType    fcn_ty_inner { /*is_unsafe=*/false, ABI_RUST, src_te.m_rettype.clone_shallow(), {} };
                fcn_ty_inner.m_arg_types.reserve(src_te.m_arg_types.size());
                for(const auto& arg : src_te.m_arg_types) {
                    fcn_ty_inner.m_arg_types.push_back( arg.clone_shallow() );
                }
                auto res_ty = ::HIR::TypeRef(mv$(fcn_ty_inner));

                const auto& str = *src_te.node->m_obj_ptr;
                auto closure_type = ::HIR::TypeRef::new_path( src_te.node->m_obj_path.clone(), &str );
                auto fn_path = ::HIR::Path(mv$(closure_type), "call_free");
                fn_path.m_data.as_UfcsInherent().impl_params = src_te.node->m_obj_path.m_params.clone();

                node.m_value = NEWNODE(mv$(res_ty), PathValue, sp, mv$(fn_path), ::HIR::ExprNode_PathValue::FUNCTION);
            }
            ::HIR::ExprVisitorDef::visit(node);
        }

        void visit(::HIR::ExprNode_CallValue& node) override
        {
            if( const auto* e = node.m_value->m_res_type.data().opt_Closure() )
            {
                switch(e->node->m_class)
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
            }

            ::HIR::ExprVisitorDef::visit(node);
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            bool run_eat = m_run_eat;
            m_run_eat = false;
            fix_type(m_crate, Span(), m_monomorphiser, ty);
            ::HIR::ExprVisitorDef::visit_type(ty);
            if( run_eat ) {
                // TODO: Instead of running EAT, just mark any Unbound UfcsKnown types as Opaque
                //m_resolve.expand_associated_types(Span(), ty);
                m_run_eat = true;
            }
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
        static ::HIR::TraitImpl make_fnfree(
                ::HIR::GenericParams params,
                ::HIR::TypeRef closure_type,
                ::std::vector<::std::pair< ::HIR::Pattern, ::HIR::TypeRef>> args,
                ::HIR::TypeRef ret_ty,
                ::HIR::ExprPtr code
            )
        {
            // NOTE: Fixup isn't needed, there's no self
            //fix_fn_params(code, closure_type, args_argent.second);
            assert(code.m_bindings.size() > 0);
            code.m_bindings[0] = ::HIR::TypeRef::new_unit();
            return ::HIR::TraitImpl {
                mv$(params), {}, mv$(closure_type),
                make_map1(
                    RcString::new_interned("call_free"), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { false,
                        ::HIR::Function( ::HIR::Function::Receiver::Free, ::HIR::GenericParams {}, mv$(args), ret_ty.clone(), mv$(code))
                        }
                    ),
                {},
                {},
                {},
                ::HIR::SimplePath()
                };
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
                    RcString::new_interned("call_once"), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { false, ::HIR::Function {
                        ::HIR::Function::Receiver::Value,
                        ::HIR::GenericParams {},
                        make_vec2(
                            ::std::make_pair(
                                ::HIR::Pattern { HIR::PatternBinding {false, ::HIR::PatternBinding::Type::Move, "self", 0}, {} },
                                mv$(ty_of_self)
                                ),
                            mv$( args_argent )
                            ),
                        ret_ty.clone(),
                        mv$(code)
                        } }
                    ),
                {},
                {},
                make_map1(
                    RcString::new_interned("Output"), ::HIR::TraitImpl::ImplEnt< ::HIR::TypeRef> { false, mv$(ret_ty) }
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
                    RcString::new_interned("call_mut"), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { false, ::HIR::Function {
                        ::HIR::Function::Receiver::BorrowUnique,
                        ::HIR::GenericParams {},
                        make_vec2(
                            ::std::make_pair(
                                ::HIR::Pattern { {false, ::HIR::PatternBinding::Type::Move, "self", 0}, {} },
                                mv$(ty_of_self)
                                ),
                            mv$( args_argent )
                            ),
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
                    RcString::new_interned("call"), ::HIR::TraitImpl::ImplEnt< ::HIR::Function> { false, ::HIR::Function {
                        ::HIR::Function::Receiver::BorrowShared,
                        ::HIR::GenericParams {},
                        make_vec2(
                            ::std::make_pair(
                                ::HIR::Pattern { {false, ::HIR::PatternBinding::Type::Move, "self", 0}, {} },
                                mv$(ty_of_self)
                                ),
                            mv$(args_argent)
                            ),
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

    /// Extract closures from the main tree
    class ExprVisitor_Extract:
        public ::HIR::ExprVisitorDef
    {
        const StaticTraitResolve& m_resolve;
        const ::HIR::TypeRef*   m_self_type;
        const ::std::vector< ::HIR::TypeRef>& m_variable_types;

        // Outputs
        OutState&   m_out;
        const char* m_new_type_suffix;

    public:
        ExprVisitor_Extract(const StaticTraitResolve& resolve, const ::HIR::TypeRef* self_type, const ::std::vector< ::HIR::TypeRef>& var_types, OutState& out, const char* new_type_suffix):
            m_resolve(resolve),
            m_self_type(self_type),
            m_variable_types(var_types),
            m_out(out),
            m_new_type_suffix(new_type_suffix)
        {
        }

        void visit_root(::HIR::ExprNode& root)
        {
            root.visit(*this);
        }

        
        struct Monomorph: public Monomorphiser
        {
            const ::HIR::GenericParams& params;
            unsigned ofs_impl_t;
            unsigned ofs_item_t;
            unsigned ofs_impl_v;
            unsigned ofs_item_v;
            Monomorph(const ::HIR::GenericParams& params,
                unsigned ofs_impl_t, unsigned ofs_item_t,
                unsigned ofs_impl_v, unsigned ofs_item_v
                )
                : params(params)
                , ofs_impl_t(ofs_impl_t)
                , ofs_item_t(ofs_item_t)
                , ofs_impl_v(ofs_impl_v)
                , ofs_item_v(ofs_item_v)
            {
            }
            ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& ge) const override
            {
                unsigned i;
                if( ge.binding == 0xFFFF ) {
                    i = 0;
                }
                else if( ge.binding < 256 ) {
                    i = ofs_impl_t + ge.binding;
                }
                else if( ge.binding < 2*256 ) {
                    i = ofs_item_t + (ge.binding - 256);
                }
                else {
                    BUG(sp, "Generic type " << ge << " unknown");
                }
                ASSERT_BUG(sp, i < params.m_types.size(), "Item generic binding OOR - " << ge << " (" << i << " !< " << params.m_types.size() << ")");
                return ::HIR::TypeRef(params.m_types[i].m_name, i);
            }
            ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& ge) const {
                unsigned i;
                if( ge.binding == 0xFFFF ) {
                    BUG(sp, "Binding 0xFFFF isn't valid for values");
                }
                else if( ge.binding < 256 ) {
                    i = ofs_impl_v + ge.binding;
                }
                else if( ge.binding < 2*256 ) {
                    i = ofs_item_v + (ge.binding - 256);
                }
                else {
                    BUG(sp, "Generic value " << ge << " unknown");
                }
                ASSERT_BUG(sp, i < params.m_values.size(), "Item generic value binding OOR - " << ge << " (" << i << " !< " << params.m_values.size() << ")");
                return ::HIR::GenericRef(params.m_values[i].m_name, i);
            }
        };

        Monomorph create_params(const Span& sp, ::HIR::GenericParams& params, ::HIR::PathParams& constructor_path_params, ::HIR::PathParams& impl_path_params) const
        {
            // - 0xFFFF "Self" -> 0 "Super" (if present)
            if( m_resolve.has_self() )
            {
                assert( m_self_type );
                constructor_path_params.m_types.push_back( m_self_type->clone() );
                params.m_types.push_back( ::HIR::TypeParamDef { "Super", {}, false } );  // TODO: Determine if parent Self is Sized
            }
            // - Top-level params come first
            unsigned ofs_impl_t = params.m_types.size();
            for(const auto& ty_def : m_resolve.impl_generics().m_types) {
                unsigned i = &ty_def - &m_resolve.impl_generics().m_types.front();
                constructor_path_params.m_types.push_back( ::HIR::TypeRef( ty_def.m_name, 0*256 + i ) );
                params.m_types.push_back( ::HIR::TypeParamDef { ty_def.m_name, {}, ty_def.m_is_sized } );
            }
            unsigned ofs_impl_v = params.m_values.size();
            for(const auto& v_def : m_resolve.impl_generics().m_values) {
                unsigned i = &v_def - &m_resolve.impl_generics().m_values.front();
                constructor_path_params.m_values.push_back( ::HIR::GenericRef( v_def.m_name, 0*256 + i ) );
                params.m_values.push_back( ::HIR::ValueParamDef { v_def.m_name, v_def.m_type.clone() } );
            }
            // - Item-level params come second
            unsigned ofs_item_t = params.m_types.size();
            for(const auto& ty_def : m_resolve.item_generics().m_types) {
                unsigned i = &ty_def - &m_resolve.item_generics().m_types.front();
                constructor_path_params.m_types.push_back( ::HIR::TypeRef( ty_def.m_name, 1*256 + i ) );
                params.m_types.push_back( ::HIR::TypeParamDef { ty_def.m_name, {}, ty_def.m_is_sized } );
            }
            unsigned ofs_item_v = params.m_values.size();
            for(const auto& v_def : m_resolve.item_generics().m_values) {
                unsigned i = &v_def - &m_resolve.item_generics().m_values.front();
                constructor_path_params.m_values.push_back( ::HIR::GenericRef( v_def.m_name, 1*256 + i ) );
                params.m_values.push_back( ::HIR::ValueParamDef { v_def.m_name, v_def.m_type.clone() } );
            }

            // Create the params used for the type on the impl block
            for(unsigned int i = 0; i < params.m_types.size(); i ++) {
                impl_path_params.m_types.push_back( ::HIR::TypeRef(params.m_types[i].m_name, i) );
            }
            for(unsigned int i = 0; i < params.m_values.size(); i ++) {
                impl_path_params.m_values.push_back( ::HIR::GenericRef(params.m_values[i].m_name, i) );
            }
            DEBUG("impl_path_params = " << impl_path_params << ", ofs_*_t=" << ofs_item_t << "," << ofs_impl_t << " ofs_*_v=" << ofs_item_v << "," << ofs_impl_v);

            Monomorph monomorph_cb(params, ofs_impl_t, ofs_item_t, ofs_impl_v, ofs_impl_v);
            auto monomorph = [&](const auto& ty){ return monomorph_cb.monomorph_type(sp, ty); };

            // - Clone the bounds (from both levels)
            auto monomorph_bound = [&](const ::HIR::GenericBound& b)->::HIR::GenericBound {
                TU_MATCH_HDRA( (b), {)
                TU_ARMA(Lifetime, e)
                    return ::HIR::GenericBound(e);
                TU_ARMA(TypeLifetime, e)
                    return ::HIR::GenericBound::make_TypeLifetime({ monomorph(e.type), e.valid_for });
                TU_ARMA(TraitBound, e)
                    return ::HIR::GenericBound::make_TraitBound({ monomorph(e.type), monomorph_cb.monomorph_traitpath(sp, e.trait, false) });
                TU_ARMA(TypeEquality, e)
                    return ::HIR::GenericBound::make_TypeEquality({ monomorph(e.type), monomorph(e.other_type) });
                }
                throw "";
                };
            for(const auto& bound : m_resolve.impl_generics().m_bounds ) {
                params.m_bounds.push_back( monomorph_bound(bound) );
            }
            for(const auto& bound : m_resolve.item_generics().m_bounds ) {
                params.m_bounds.push_back( monomorph_bound(bound) );
            }
            return monomorph_cb;
        }

        /// <summary>
        /// Main extraction closure visitor
        /// </summary>
        void visit(::HIR::ExprNode_Closure& node) override
        {
            if(!node.m_code)
            {
                DEBUG("Already expanded (via consteval?)");
                return ;
            }

            const auto& sp = node.span();

            TRACE_FUNCTION_F("Extract closure - " << node.m_res_type);

            ASSERT_BUG(sp, node.m_obj_path == ::HIR::GenericPath(), "Closure path already set? " << node.m_obj_path);

            ::HIR::ExprVisitorDef::visit(node);

            // --- Extract and mutate code into a trait impl on the closure type ---

            // 1. Prepare type params for rewriting the expression tree
            ::HIR::GenericParams params;
            ::HIR::PathParams constructor_path_params;
            ::HIR::PathParams impl_path_params;
            auto monomorph_cb = create_params(sp, params, constructor_path_params, impl_path_params);
            auto monomorph = [&](const auto& ty){ return monomorph_cb.monomorph_type(sp, ty); };

            DEBUG("--- Mutate inner code");
            // 2. Iterate over the nodes and rewrite variable accesses to either renumbered locals, or field accesses
            ExprVisitor_Mutate    ev { node.m_res_type, node.m_avu_cache.local_vars, node.m_avu_cache.captured_vars, monomorph_cb };
            ev.visit_node_ptr( node.m_code );
            // NOTE: `ev` is used down in `Args` to convert the argument destructuring pattern

            // - Types of local variables
            DEBUG("--- Build locals and captures");
            ::std::vector< ::HIR::TypeRef>  local_types;
            local_types.push_back( ::HIR::TypeRef() );  // self - filled by make_fn*
            for(const auto binding_idx : node.m_avu_cache.local_vars) {
                local_types.push_back( monomorph( m_variable_types.at(binding_idx).clone() ) );
            }
            // - Generate types of captures, and construct the actual capture values
            //  > Capture types (with borrows and using closure's type params)
            ::std::vector< ::HIR::VisEnt< ::HIR::TypeRef> > capture_types;
            //  > Capture value nodes
            ::std::vector< ::HIR::ExprNodeP>    capture_nodes;
            capture_types.reserve( node.m_avu_cache.captured_vars.size() );
            capture_nodes.reserve( node.m_avu_cache.captured_vars.size() );
            node.m_is_copy = true;
            for(const auto binding : node.m_avu_cache.captured_vars)
            {
                const auto binding_idx = binding.first;
                auto binding_type = binding.second;

                const auto& cap_ty = m_variable_types.at(binding_idx);
                auto ty_mono = monomorph(cap_ty);

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
                ExprVisitor_Fixup   fixup { m_resolve.m_crate, &params, monomorph_cb };
                fixup.visit_type(ty_mono);
                if( !fixup.m_resolve.type_is_copy(sp, ty_mono) )
                {
                    node.m_is_copy = false;
                }
                capture_types.push_back( ::HIR::VisEnt< ::HIR::TypeRef> { ::HIR::Publicity::new_none(), mv$(ty_mono) } );
            }

            // --- ---
            if( node.m_is_copy )
            {
                DEBUG("Copy closure");
            }

            auto str = ::HIR::Struct {
                params.clone(),
                ::HIR::Struct::Repr::Rust,
                ::HIR::Struct::Data::make_Tuple(mv$(capture_types))
                };
            str.m_markings.is_copy = node.m_is_copy;
            ::HIR::SimplePath   closure_struct_path;
            const ::HIR::TypeItem* closure_struct_ptr;
            ::std::tie(closure_struct_path, closure_struct_ptr) = m_out.new_type(CLOSURE_PATH_PREFIX, m_new_type_suffix, mv$(str));
            const auto& closure_struct_ref = closure_struct_ptr->as_Struct();

            // Mark the object pathname in the closure.
            node.m_obj_ptr = &closure_struct_ref;
            node.m_obj_path = ::HIR::GenericPath( closure_struct_path, mv$(constructor_path_params) );
            node.m_obj_path_base = node.m_obj_path.clone();
            node.m_captures = mv$(capture_nodes);
            //node.m_res_type = ::HIR::TypeRef( node.m_obj_path.clone() );
            DEBUG("-- Object name: " << node.m_obj_path);
            ::HIR::TypeRef  closure_type = ::HIR::TypeRef::new_path(
                ::HIR::GenericPath(node.m_obj_path.m_path.clone(), mv$(impl_path_params)),
                ::HIR::TypePathBinding::make_Struct(&closure_struct_ref)
                );

            // - Args
            ::std::vector< ::HIR::Pattern>  args_pat_inner;
            ::std::vector< ::HIR::TypeRef>  args_ty_inner;

            for(const auto& arg : node.m_args) {
                args_pat_inner.push_back( arg.first.clone() );
                ev.visit_pattern(sp, args_pat_inner.back() );
                args_ty_inner.push_back( monomorph_cb.monomorph_type(sp, arg.second) );
            }
            ::HIR::TypeRef  args_ty { mv$(args_ty_inner) };
            ::HIR::Pattern  args_pat { HIR::PatternBinding(), ::HIR::Pattern::Data::make_Tuple({ mv$(args_pat_inner) }) };
            ::HIR::TypeRef  ret_type = monomorph_cb.monomorph_type(sp, node.m_return);

            DEBUG("args_ty = " << args_ty << ", ret_type = " << ret_type);

            ::HIR::ExprPtr body_code { mv$(node.m_code) };
            body_code.m_bindings = mv$(local_types);

            {
                DEBUG("-- Fixing types in body code");
                ExprVisitor_Fixup   fixup { m_resolve.m_crate, &params, monomorph_cb };
                fixup.visit_root( body_code );

                DEBUG("-- Fixing types in signature");
                fixup.visit_type( args_ty );
                fixup.visit_type( ret_type );
                // TODO: Replace erased types too
            }

            if( node.m_is_copy )
            {
                auto lang_Copy = m_resolve.m_crate.get_lang_item_path(sp, "copy");
                auto& v = const_cast<::HIR::Crate&>(m_resolve.m_crate).m_trait_impls[lang_Copy].get_list_for_type_mut(closure_type);
                v.push_back(box$(::HIR::TraitImpl {
                    params.clone(), {}, closure_type.clone(),
                    {},
                    {},
                    {},
                    {},
                    /*source module*/::HIR::SimplePath(m_resolve.m_crate.m_crate_name, {})
                    }));
                const_cast<::HIR::Crate&>(m_resolve.m_crate).m_all_trait_impls[lang_Copy].get_list_for_type_mut(closure_type).push_back( v.back().get() );
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
            case ::HIR::ExprNode_Closure::Class::NoCapture: {

                struct H2 {
                    static ::std::pair<::HIR::ExprNode_Closure::Class, HIR::TraitImpl> make_dispatch(
                            const Span& sp,
                            ::HIR::ExprNode_Closure::Class c,
                            ::HIR::GenericParams params,
                            ::HIR::PathParams trait_params,
                            const ::HIR::TypeRef& closure_type,
                            const ::HIR::TypeRef& args_ty,
                            const ::HIR::TypeRef& ret_type
                            )
                    {
                        const auto& args_tup_inner = args_ty.data().as_Tuple();
                        // 1. Create a list of `arg.0, arg.1, arg.2, ...` for the dispatch methods
                        ::std::vector<HIR::ExprNodeP> dispatch_args;
                        ::std::vector<HIR::TypeRef> dispatch_node_args_cache;
                        dispatch_args.reserve( args_tup_inner.size() );
                        dispatch_node_args_cache.reserve( args_tup_inner.size()+1 );
                        for(size_t i = 0; i < args_tup_inner.size(); i ++)
                        {
                            const auto& ty = args_tup_inner[i];
                            dispatch_args.push_back( NEWNODE(ty.clone(), Field, sp,  NEWNODE(args_ty.clone(), Variable, sp, RcString::new_interned("arg"), 1), RcString::new_interned(FMT(i))) );
                            dispatch_node_args_cache.push_back( ty.clone() );
                        }
                        dispatch_node_args_cache.push_back( ret_type.clone() );
                        auto path = ::HIR::Path(closure_type.clone(), RcString::new_interned("call_free"));
                        path.m_data.as_UfcsInherent().impl_params = closure_type.data().as_Path().path.m_data.as_Generic().m_params.clone();
                        HIR::ExprNodeP  dispatch_node = NEWNODE(ret_type.clone(), CallPath, sp,
                                mv$(path),
                                mv$(dispatch_args)
                                );
                        dynamic_cast<::HIR::ExprNode_CallPath&>(*dispatch_node).m_cache.m_arg_types = mv$(dispatch_node_args_cache);

                        auto args_arg = ::std::make_pair(
                            ::HIR::Pattern { {false, ::HIR::PatternBinding::Type::Move, RcString::new_interned("args"), 1}, {} },
                            args_ty.clone()
                            );
                        HIR::TraitImpl fcn;
                        switch(c)
                        {
                        case ::HIR::ExprNode_Closure::Class::Once:
                            fcn = H::make_fnonce( mv$(params), mv$(trait_params), closure_type.clone(), mv$(args_arg), ret_type.clone(), mv$(dispatch_node) );
                            break;
                        case ::HIR::ExprNode_Closure::Class::Mut:
                            fcn = H::make_fnmut( mv$(params), mv$(trait_params), closure_type.clone(), mv$(args_arg), ret_type.clone(), mv$(dispatch_node) );
                            break;
                        case ::HIR::ExprNode_Closure::Class::Shared:
                            fcn = H::make_fn( mv$(params), mv$(trait_params), closure_type.clone(), mv$(args_arg), ret_type.clone(), mv$(dispatch_node) );
                            break;
                        default:
                            throw "";
                        }
                        return ::std::make_pair(c, mv$(fcn));
                    }
                };
                m_out.impls_closure.push_back( H2::make_dispatch(sp, ::HIR::ExprNode_Closure::Class::Once  , params.clone(), trait_params.clone(), closure_type, args_ty, ret_type) );
                m_out.impls_closure.push_back( H2::make_dispatch(sp, ::HIR::ExprNode_Closure::Class::Mut   , params.clone(), trait_params.clone(), closure_type, args_ty, ret_type) );
                m_out.impls_closure.push_back( H2::make_dispatch(sp, ::HIR::ExprNode_Closure::Class::Shared, params.clone(), mv$(trait_params)   , closure_type, args_ty, ret_type) );

                // 2. Split args_pat/args_ty into separate arguments
                ::std::vector<::std::pair< ::HIR::Pattern, ::HIR::TypeRef>> args_split;
                args_split.reserve( node.m_args.size() );
                for(size_t i = 0; i < node.m_args.size(); i ++)
                {
                    args_split.push_back(::std::make_pair(
                            mv$( args_pat.m_data.as_Tuple().sub_patterns[i] ),
                            mv$( args_ty.data().as_Tuple()[i] )
                            ));
                }
                // - Create fn_free free method
                m_out.impls_closure.push_back(::std::make_pair(
                    ::HIR::ExprNode_Closure::Class::NoCapture,
                    H::make_fnfree( mv$(params), mv$(closure_type), mv$(args_split), mv$(ret_type), mv$(body_code) )
                    ));

                } break;
            case ::HIR::ExprNode_Closure::Class::Shared: {
                const auto& lang_Fn = m_resolve.m_crate.get_lang_item_path(node.span(), "fn");
                const auto method_self_ty = ::HIR::TypeRef::new_borrow( ::HIR::BorrowType::Shared, closure_type.clone() );

                // - FnOnce
                {
                    auto dispatch_node = NEWNODE(ret_type.clone(), CallPath, sp,
                        ::HIR::Path(closure_type.clone(), ::HIR::GenericPath(lang_Fn, trait_params.clone()), RcString::new_interned("call")),
                        make_vec2(
                            NEWNODE(method_self_ty.clone(), Borrow, sp, ::HIR::BorrowType::Shared, NEWNODE(closure_type.clone(), Variable, sp, RcString::new_interned("self"), 0)),
                            NEWNODE(args_ty.clone(), Variable, sp, "arg", 1)
                            )
                        );
                    dynamic_cast<::HIR::ExprNode_CallPath&>(*dispatch_node).m_cache.m_arg_types = make_vec3( method_self_ty.clone(), args_ty.clone(), ret_type.clone() );
                    auto args_arg = ::std::make_pair(
                        ::HIR::Pattern { {false, ::HIR::PatternBinding::Type::Move, "args", 1}, {} },
                        args_ty.clone()
                        );
                    m_out.impls_closure.push_back(::std::make_pair(
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
                    m_out.impls_closure.push_back(::std::make_pair(
                        ::HIR::ExprNode_Closure::Class::Mut,
                        H::make_fnmut( params.clone(), trait_params.clone(), closure_type.clone(), mv$(args_arg), ret_type.clone(), mv$(dispatch_node) )
                        ));
                }

                // - Fn
                m_out.impls_closure.push_back(::std::make_pair(
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
                    m_out.impls_closure.push_back(::std::make_pair(
                        ::HIR::ExprNode_Closure::Class::Once,
                        H::make_fnonce( params.clone(), trait_params.clone(), closure_type.clone(), mv$(args_arg), ret_type.clone(), mv$(dispatch_node) )
                        ));
                }

                // - FnMut (code)
                m_out.impls_closure.push_back(::std::make_pair(
                    ::HIR::ExprNode_Closure::Class::Mut,
                    H::make_fnmut( mv$(params), mv$(trait_params), mv$(closure_type), ::std::make_pair(mv$(args_pat), mv$(args_ty)), mv$(ret_type), mv$(body_code) )
                    ));
                } break;
            case ::HIR::ExprNode_Closure::Class::Once:
                // - FnOnce (code)
                m_out.impls_closure.push_back(::std::make_pair(
                    ::HIR::ExprNode_Closure::Class::Once,
                    H::make_fnonce( mv$(params), mv$(trait_params), mv$(closure_type), ::std::make_pair(mv$(args_pat), mv$(args_ty)), mv$(ret_type), mv$(body_code) )
                    ));
                break;
            }
        }


        /// <summary>
        /// Main extraction generator visitor
        /// </summary>
        void visit(::HIR::ExprNode_Generator& node) override
        {
            const auto& sp = node.span();

            TRACE_FUNCTION_F("Extract generator - " << node.m_res_type);

            // 1. Recurse to obtain useful metadata
            ::HIR::ExprVisitorDef::visit(node);

            // -- Prepare type params for rewriting the expression tree
            ::HIR::GenericParams params;
            ::HIR::PathParams constructor_path_params;
            ::HIR::PathParams impl_path_params;
            auto monomorph_cb = create_params(sp, params, constructor_path_params, impl_path_params);

            // Create state index enum
            auto state_idx_type = m_out.new_type("gen_state_idx#", m_new_type_suffix, ::HIR::Enum {
                ::HIR::GenericParams(),
                false,
                ::HIR::Enum::Repr(),
                ::HIR::Enum::Class::make_Value({})
                });
            auto state_idx_ty = ::HIR::TypeRef::new_path( state_idx_type.first, &state_idx_type.second->as_Enum() );

            // Create the captures structure here, and update it afterwards with the state
            // - The final entry in captures is the state, and is pre-filled with zeroes by the creator's MIR lower
            auto state_str = ::HIR::Struct {
                params.clone(),
                ::HIR::Struct::Repr::Rust,
                ::HIR::Struct::Data::make_Tuple({}) // Will be filled in the MIR pass
            };
            state_str.m_data.as_Tuple().push_back(HIR::VisEnt<HIR::TypeRef> { HIR::Publicity::new_none(), state_idx_ty.clone() });
            ::HIR::SimplePath   state_struct_path;
            const ::HIR::TypeItem* state_struct_ptr;
            ::std::tie(state_struct_path, state_struct_ptr) = m_out.new_type("gen_state#", m_new_type_suffix, mv$(state_str));
            auto state_type = ::HIR::TypeRef::new_path( ::HIR::GenericPath(state_struct_path, impl_path_params.clone()), &state_struct_ptr->as_Struct() );
            node.m_state_data_type = state_type.clone();

            // 3. Classify varibles
            // - Captures: defined outside and need to be captured using closure capture rules (`defined_stack.empty()`)
            // - Saved: defined inside but used across a yield boundary (see GeneratorState)
            // - Local: defined and used between two yields
            size_t n_caps = node.m_avu_cache.captured_vars.size();
            size_t n_locals = node.m_avu_cache.local_vars.size();
            ::std::map<unsigned, unsigned> variable_rewrites;
            ::std::vector<HIR::ValueUsage> capture_usages; capture_usages.reserve(n_caps);
            ::std::vector<HIR::TypeRef>   new_locals; new_locals.reserve(1 + n_caps + n_locals);
            ::std::vector< ::HIR::VisEnt< ::HIR::TypeRef> > struct_ents; struct_ents.reserve(1 + n_caps);
            ::std::vector<HIR::ExprNodeP>   capture_nodes; capture_nodes.reserve(n_caps);
            // First new local is always the invocation `self`
            new_locals.push_back(HIR::TypeRef());  // `self: &mut NewStruct`
            // First ent is the runtime state (first is zeroed, the second is set to uninit)

            const auto& lang_MaybeUninit = m_resolve.m_crate.get_lang_item_path(node.span(), "maybe_uninit");
            const auto& unm_MaybeUninit = m_resolve.m_crate.get_union_by_path(node.span(), lang_MaybeUninit);
            // Wrap the state in MaybeUninit to prevent any attempt at using niche optimisations
            struct_ents.push_back(HIR::VisEnt<HIR::TypeRef> { HIR::Publicity::new_none(), ::HIR::TypeRef::new_path( ::HIR::GenericPath(lang_MaybeUninit, ::HIR::PathParams(state_type.clone())), &unm_MaybeUninit ) });

            // Add captures to the locals list first
            for(const auto& cap : node.m_avu_cache.captured_vars)
            {
                unsigned index = new_locals.size();
                variable_rewrites.insert(std::make_pair( cap.first, index ));
                new_locals.push_back( monomorph_cb.monomorph_type(sp, m_variable_types.at(cap.first)) );

                capture_usages.push_back(cap.second);
                auto cap_ty = monomorph_cb.monomorph_type(sp, m_variable_types.at(cap.first));
                struct_ents.push_back(HIR::VisEnt<HIR::TypeRef> { HIR::Publicity::new_none(), cap_ty.clone() });
                capture_nodes.push_back(HIR::ExprNodeP(new ::HIR::ExprNode_Variable(sp, "", cap.first)));
                switch(cap.second)
                {
                case ::HIR::ValueUsage::Unknown:
                    BUG(sp, "Unexpected ValueUsage::Unknown on #" << cap.first);
                case ::HIR::ValueUsage::Move: {
                    // No wrapping needed (drop handled by custom drop glue)
                    } break;
                case ::HIR::ValueUsage::Borrow:
                    capture_nodes.back()->m_res_type = cap_ty.clone();
                    cap_ty = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, mv$(cap_ty));
                    struct_ents.back().ent = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, mv$(struct_ents.back().ent));
                    capture_nodes.back() = HIR::ExprNodeP(new ::HIR::ExprNode_Borrow(sp, ::HIR::BorrowType::Shared, mv$(capture_nodes.back())));
                    break;
                case ::HIR::ValueUsage::Mutate:
                    capture_nodes.back()->m_res_type = cap_ty.clone();
                    cap_ty = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, mv$(cap_ty));
                    struct_ents.back().ent = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, mv$(struct_ents.back().ent));
                    capture_nodes.back() = HIR::ExprNodeP(new ::HIR::ExprNode_Borrow(sp, ::HIR::BorrowType::Unique, mv$(capture_nodes.back())));
                    break;
                }
                capture_nodes.back()->m_res_type = mv$(cap_ty);
            }
            for(const auto& slot : node.m_avu_cache.local_vars)
            {
                unsigned index = new_locals.size();
                variable_rewrites.insert(std::make_pair( slot, index ));
                new_locals.push_back( monomorph_cb.monomorph_type(sp, m_variable_types.at(slot)) );
            }

            // NOTE: Most of generator's lowering is done in MIR lowering
            // - This is because it needs to rewrite the flow quite severely.
            // > HOWEVER: The code is extracted here and passed over to the new impl

            // So, re-write all variable references into either a capture or a local.
            class ExprVisitor_GeneratorRewrite:
                public ::HIR::ExprVisitorDef
            {
                const Monomorph& m_monomorph;
                const ::HIR::TypeRef&   m_self_arg_type;
                const std::map<unsigned, unsigned>&   m_variable_rewrites;

                ::HIR::ExprNodeP    m_replacement;
            public:
                ExprVisitor_GeneratorRewrite(const Monomorph& monomorph, const ::HIR::TypeRef& self_arg_type, const std::map<unsigned, unsigned>& rewrites)
                    : m_monomorph(monomorph)
                    , m_self_arg_type(self_arg_type)
                    , m_variable_rewrites(rewrites)
                {
                }

                void visit_type(::HIR::TypeRef& ty) override
                {
                    ty = m_monomorph.monomorph_type(Span(), ty, /*allow_infer=*/true);
                }

                /// Support replacing nodes
                void visit_node_ptr(::HIR::ExprNodeP& node_ptr) override
                {
                    ::HIR::ExprVisitorDef::visit_node_ptr(node_ptr);
                    if( m_replacement ) {
                        node_ptr = std::move(m_replacement);
                    }
                }

                /// Rewrite variable references into either a different slot or a field access
                void visit(::HIR::ExprNode_Variable& node) override
                {
                    node.m_slot = m_variable_rewrites.at(node.m_slot);
                }

                // Custom visitor that only updates the captures and path
                // - Don't want to visit the patterns within
                void visit(::HIR::ExprNode_Closure& node) override
                {
                    assert(!node.m_code);
                    visit_generic_path(::HIR::Visitor::PathContext::TYPE, node.m_obj_path);

                    for(auto& cap : node.m_captures)
                        visit_node_ptr(cap);
                }

                /// Update variable definitions
                void visit_pattern(const Span& sp, ::HIR::Pattern& pat) override
                {
                    ::HIR::ExprVisitorDef::visit_pattern(sp, pat);
                    for(auto& pb : pat.m_bindings)
                    {
                        visit_pattern_binding(sp, pb);
                    }
                    if(auto* pe = pat.m_data.opt_SplitSlice())
                    {
                        visit_pattern_binding(sp, pe->extra_bind);
                    }
                }
                void visit_pattern_binding(const Span& sp, ::HIR::PatternBinding& binding)
                {
                    if(binding.is_valid())
                    {
                        ASSERT_BUG(sp, m_variable_rewrites.count(binding.m_slot), "Newly defined variable #" << binding.m_slot << " not in rewrite list?");
                        binding.m_slot = m_variable_rewrites.at(binding.m_slot);
                    }
                }
            };

            auto gen_str = ::HIR::Struct {
                params.clone(),
                ::HIR::Struct::Repr::Rust,
                ::HIR::Struct::Data::make_Tuple(mv$(struct_ents))
            };
            gen_str.m_markings.has_drop_impl = true;
            ::HIR::SimplePath   gen_struct_path;
            const ::HIR::TypeItem* gen_struct_ptr;
            ::std::tie(gen_struct_path, gen_struct_ptr) = m_out.new_type(GENERATOR_PATH_PREFIX, m_new_type_suffix, mv$(gen_str));
            const auto& gen_struct_ref = gen_struct_ptr->as_Struct();


            // Mark the object pathname in the closure.
            node.m_obj_ptr = &gen_struct_ref;
            node.m_obj_path = ::HIR::GenericPath( gen_struct_path, mv$(constructor_path_params) );
            node.m_captures = mv$(capture_nodes);

            ::HIR::TypeRef& self_arg_ty = new_locals[0];
            // `::path::to::struct`
            self_arg_ty = ::HIR::TypeRef::new_path( ::HIR::GenericPath(gen_struct_path, impl_path_params.clone()), &gen_struct_ref );
            // `&mut Self`
            self_arg_ty = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, std::move(self_arg_ty));
            auto lang_Pin = m_resolve.m_crate.get_lang_item_path(sp, "pin");
            auto lang_GeneratorState = m_resolve.m_crate.get_lang_item_path(sp, "generator_state");
            // `Pin<&mut Self>`
            self_arg_ty = ::HIR::TypeRef::new_path( ::HIR::GenericPath(lang_Pin, ::HIR::PathParams(std::move(self_arg_ty))), &m_resolve.m_crate.get_struct_by_path(sp, lang_Pin) );

            auto body_node = std::move(node.m_code);
            {
                ExprVisitor_GeneratorRewrite visitor_rewrite(monomorph_cb, self_arg_ty, variable_rewrites);
                visitor_rewrite.visit_node_ptr(body_node);

                DEBUG("-- Fixing types in body code");
                ExprVisitor_Fixup   fixup { m_resolve.m_crate, &params, monomorph_cb };
                fixup.visit_node_ptr( body_node );
            }

            // -- Prepare drop impl for later filling
            ::HIR::Function* fcn_drop_ptr; {
                ::HIR::Function fcn_drop;
                fcn_drop.m_receiver = HIR::Function::Receiver::BorrowUnique;
                auto drop_self_arg_ty = ::HIR::TypeRef::new_path( ::HIR::GenericPath(gen_struct_path, impl_path_params.clone()), &gen_struct_ref );
                drop_self_arg_ty = ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Unique, std::move(drop_self_arg_ty));
                fcn_drop.m_args.push_back(std::make_pair( HIR::Pattern(), mv$(drop_self_arg_ty) ));
                fcn_drop.m_return = ::HIR::TypeRef::new_unit();
                fcn_drop.m_code.reset( new ::HIR::ExprNode_Tuple(sp, {}) );
                fcn_drop.m_code->m_res_type = ::HIR::TypeRef::new_unit();
                ::HIR::TraitImpl    drop_impl;
                drop_impl.m_params = params.clone();
                drop_impl.m_type = ::HIR::TypeRef::new_path( ::HIR::GenericPath(gen_struct_path, impl_path_params.clone()), &gen_struct_ref );
                drop_impl.m_methods.insert(std::make_pair( RcString::new_interned("drop"), ::HIR::TraitImpl::ImplEnt<HIR::Function> { false, std::move(fcn_drop) } ));
                fcn_drop_ptr = &drop_impl.m_methods.at("drop").data;
                m_out.impls_drop.push_back(std::move(drop_impl));
            }

            // -- Create function
            ::HIR::Function fcn_resume;
            // - `self: Pin<&mut {Self}>`
            fcn_resume.m_args.push_back(std::make_pair( HIR::Pattern(), self_arg_ty.clone() ));
            // - `-> GeneratorState<{Yield},{Return}>`
            ::HIR::PathParams   ret_params;
            ret_params.m_types.push_back( monomorph_cb.monomorph_type(sp, node.m_yield_ty) );
            ret_params.m_types.push_back( monomorph_cb.monomorph_type(sp, node.m_return) );
            fcn_resume.m_return = ::HIR::TypeRef::new_path( ::HIR::GenericPath(lang_GeneratorState, std::move(ret_params)), &m_resolve.m_crate.get_enum_by_path(sp, lang_GeneratorState) );
            // - ` { ... }`
            // Emit as a top-level generator
            // - It has a populated body, non-zero `m_obj_ptr`, and unset `m_obj_path`
            auto v = ::std::make_unique<::HIR::ExprNode_GeneratorWrapper>(::HIR::ExprNode_GeneratorWrapper(sp, HIR::TypeRef(), mv$(body_node), false, false));
            v->m_yield_ty = monomorph_cb.monomorph_type(sp, node.m_yield_ty);
            v->m_return   = monomorph_cb.monomorph_type(sp, node.m_return);
            v->m_capture_usages = std::move(capture_usages);
            v->m_res_type = fcn_resume.m_return.clone();
            v->m_obj_ptr = node.m_obj_ptr;
            v->m_state_data_type = mv$(state_type);
            v->m_state_idx_enum = mv$(state_idx_type.first);
            v->m_drop_fcn_ptr = fcn_drop_ptr;
            body_node.reset(v.release());
            fcn_resume.m_code.reset( body_node.release() );
            fcn_resume.m_code.m_bindings = std::move(new_locals);


            // -- Create impl
            ::HIR::TraitImpl    impl;
            impl.m_params = std::move(params);
            impl.m_type = ::HIR::TypeRef::new_path( ::HIR::GenericPath(gen_struct_path, std::move(impl_path_params)), &gen_struct_ref );
            impl.m_types.insert(std::make_pair( RcString::new_interned("Yield" ), ::HIR::TraitImpl::ImplEnt<HIR::TypeRef> { false, monomorph_cb.monomorph_type(sp, node.m_yield_ty) } ));
            impl.m_types.insert(std::make_pair( RcString::new_interned("Return"), ::HIR::TraitImpl::ImplEnt<HIR::TypeRef> { false, monomorph_cb.monomorph_type(sp, node.m_return) } ));
            impl.m_methods.insert(std::make_pair( RcString::new_interned("resume"), ::HIR::TraitImpl::ImplEnt<HIR::Function> { false, std::move(fcn_resume) } ));
            m_out.impls_generator.push_back(std::move(impl));
        }

        /// Newly defined variables
        void visit_pattern(const Span& sp, ::HIR::Pattern& pat) override
        {
        }

        // Loops that contain yeild points require all referenced variables to be saved
        void visit(::HIR::ExprNode_Loop& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_Yield& node) override
        {
            ::HIR::ExprVisitorDef::visit(node);
        }

    private:
    };

    /// <summary>
    /// Top-level visitor
    /// </summary>
    class OuterVisitor:
        public ::HIR::Visitor
    {
        StaticTraitResolve  m_resolve;
        OutState    m_out;

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
            // Type construction helper used for impl blocks
            m_out.new_type = [&](const char* prefix, const char* suffix, auto s)->auto {
                auto name = RcString::new_interned(FMT(prefix << "I_" << suffix << (suffix[0] ? "_" : "") << closure_count));
                closure_count += 1;
                auto boxed = box$(( ::HIR::VisEnt< ::HIR::TypeItem> { ::HIR::Publicity::new_none(), mv$(s) } ));
                auto* ret_ptr = &boxed->ent;
                crate.m_root_module.m_mod_items.insert( ::std::make_pair(name, mv$(boxed)) );
                return ::std::make_pair( ::HIR::SimplePath(crate.m_crate_name, {}) + name, ret_ptr );
                };

            ::HIR::Visitor::visit_crate(crate);

            m_out.push_new_impls(sp, crate);
        }

        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            auto saved = m_cur_mod_path;
            auto path = p.get_simple_path();
            m_cur_mod_path = &path;

            ::std::vector< ::std::pair<RcString, std::unique_ptr< ::HIR::VisEnt< ::HIR::TypeItem> > >>  new_types;

            unsigned int closure_count = 0;
            auto saved_nt = mv$(m_out.new_type);
            m_out.new_type = [&](const char* prefix, const char* suffix, auto s)->auto {
                // TODO: Use a function on `mod` that adds a closure and makes the indexes be per suffix
                auto name = RcString( FMT(prefix << suffix << (suffix[0] ? "_" : "") << closure_count) );
                closure_count += 1;
                auto boxed = box$( (::HIR::VisEnt< ::HIR::TypeItem> { ::HIR::Publicity::new_none(),  mv$(s) }) );
                auto* ret_ptr = &boxed->ent; 
                new_types.push_back( ::std::make_pair(name, mv$(boxed)) );
                return ::std::make_pair( (p + name).get_simple_path(), ret_ptr );
                };

            ::HIR::Visitor::visit_module(p, mod);

            m_cur_mod_path = saved;
            m_out.new_type = mv$(saved_nt);

            for(auto& e : new_types)
            {
                mod.m_mod_items.insert( mv$(e) );
            }
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
                if( e->size.is_Unevaluated() ) {
                    //::std::vector< ::HIR::TypeRef>  tmp;
                    //ExprVisitor_Extract    ev(m_resolve, tmp, m_new_trait_impls);
                    //ev.visit_root( *e.size );
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
            auto _ = this->m_resolve.set_item_generics(item.m_params);
            if( item.m_code )
            {
                assert( m_cur_mod_path );
                DEBUG("Function code " << p);

                {
                    ExprVisitor_Extract    ev(m_resolve, m_self_type, item.m_code.m_bindings, m_out, p.name);
                    ev.visit_root( *item.m_code );
                }

                {
                    MonomorphiserNop    mm;
                    ExprVisitor_Fixup   fixup(m_resolve.m_crate, nullptr, mm);
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

            // TODO: Re-create m_new_type to store in the source module

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

void HIR_Expand_Closures_Expr(const ::HIR::Crate& crate_ro, ::HIR::ExprPtr& exp)
{
    Span    sp;
    auto& crate = const_cast<::HIR::Crate&>(crate_ro);
    TRACE_FUNCTION;

    StaticTraitResolve   resolve { crate };
    assert(exp);
    if(exp.m_state->m_impl_generics)   resolve.set_impl_generics(*exp.m_state->m_impl_generics);
    if(exp.m_state->m_item_generics)   resolve.set_item_generics(*exp.m_state->m_item_generics);

    const ::HIR::TypeRef*   self_type = nullptr;  // TODO: Need to be able to get this?

    static int closure_count = 0;
    OutState    out;
    out.new_type = [&](const char* prefix, const char* suffix, auto s)->auto {
        auto name = RcString::new_interned(FMT(prefix << "C_" << closure_count));
        closure_count += 1;
        auto boxed = box$(( ::HIR::VisEnt< ::HIR::TypeItem> { ::HIR::Publicity::new_none(), ::HIR::TypeItem( mv$(s) ) } ));
        auto* ret_ptr = &boxed->ent;
        crate.m_new_types.push_back( ::std::make_pair(name, mv$(boxed)) );
        return ::std::make_pair( ::HIR::SimplePath(crate.m_crate_name, {}) + name, ret_ptr );
        };

    {
        ExprVisitor_Extract    ev(resolve, self_type, exp.m_bindings, out, "");
        ev.visit_root( *exp );
    }

    {
        MonomorphiserNop    mm;
        ExprVisitor_Fixup   fixup(crate, nullptr, mm);
        fixup.visit_root( exp );
    }

    for(auto& impl : out.impls_closure)
    {
        for( auto& m : impl.second.m_methods )
        {
            m.second.data.m_code.m_state = ::HIR::ExprStatePtr(*exp.m_state);
            m.second.data.m_code.m_state->stage = ::HIR::ExprState::Stage::Typecheck;
        }
        impl.second.m_src_module = exp.m_state->m_mod_path;
    }
    for(auto& impl : out.impls_generator)
    {
        for( auto& m : impl.m_methods )
        {
            m.second.data.m_code.m_state = ::HIR::ExprStatePtr(*exp.m_state);
            m.second.data.m_code.m_state->stage = ::HIR::ExprState::Stage::Typecheck;
        }
        impl.m_src_module = exp.m_state->m_mod_path;
    }
    for(auto& impl : out.impls_drop)
    {
        for( auto& m : impl.m_methods )
        {
            m.second.data.m_code.m_state = ::HIR::ExprStatePtr(*exp.m_state);
            m.second.data.m_code.m_state->stage = ::HIR::ExprState::Stage::Typecheck;
        }
        impl.m_src_module = exp.m_state->m_mod_path;
    }
    out.push_new_impls(sp, crate);
}

void HIR_Expand_Closures(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

