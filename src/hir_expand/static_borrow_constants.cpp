/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/static_borrow_constants.cpp
 * - Converts borrows of constant values into borrows of statics
 *
 * NOTE: This is done as a post-typeck HIR pass for the following reasons:
 * - Ensures that typecheck is performed on the as-written code
 * - Reduces load on MIR generation (no attempting to MIR lower large constant expressions)
 */
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <hir_typeck/static.hpp>
#include <hir_conv/constant_evaluation.hpp>
#include <algorithm>
#include "main_bindings.hpp"
#include <hir/expr_state.hpp>
#include <trans/target.hpp>


extern RcString    g_core_crate;    // Defined in hir/from_ast.cpp

namespace {
    inline HIR::ExprNodeP mk_exprnodep(HIR::ExprNode* en, ::HIR::TypeRef ty){ en->m_res_type = mv$(ty); return HIR::ExprNodeP(en); }
}
#define NEWNODE(TY, CLASS, ...)  mk_exprnodep(new HIR::ExprNode_##CLASS(__VA_ARGS__), TY)

namespace static_borrow_constants {

    /// <summary>
    /// Mark borrows of promotable statics
    /// </summary>
    class ExprVisitor_Mark:
        public ::HIR::ExprVisitorDef
    {
        const StaticTraitResolve& m_resolve;
        const ::HIR::TypeRef*   m_self_type;
        const ::HIR::ExprPtr& m_expr_ptr;

        HIR::SimplePath m_lang_RangeFull;

        bool    m_is_constant;
        bool    m_all_constant;
    public:
        ExprVisitor_Mark(const StaticTraitResolve& resolve, const ::HIR::TypeRef* self_type, const ::HIR::ExprPtr& expr_ptr)
            :m_resolve(resolve)
            ,m_self_type(self_type)
            ,m_expr_ptr(expr_ptr)
            ,m_is_constant(false)
            ,m_all_constant(false)
        {
            m_lang_RangeFull = m_resolve.m_crate.get_lang_item_path_opt("range_full");
            // EVIL hack: Since `range_full` wasn't a lang item until (latest) 1.54, resolve the path here
            if( m_lang_RangeFull == ::HIR::SimplePath() ) {
                auto sp = ::HIR::SimplePath(g_core_crate, {"ops", "RangeFull"});
                auto& ti = m_resolve.m_crate.get_typeitem_by_path(Span(), sp);
                if(ti.is_Import()) {
                    m_lang_RangeFull = ti.as_Import().path;
                }
                else {
                    m_lang_RangeFull = mv$(sp);
                }
            }
        }
        bool all_constant() const {
            return m_all_constant;
        }
        void visit_node_ptr(::HIR::ExprPtr& root) {
            const auto& node_ref = *root;
            const char* node_ty = typeid(node_ref).name();
            TRACE_FUNCTION_FR(&*root << " " << node_ty << " : " << root->m_res_type, node_ty);

            m_all_constant = true;
            root->visit(*this);
        }
        void visit_node_ptr(::HIR::ExprNodeP& node) override {
            assert( node );
            const auto& node_ref = *node;
            const char* node_ty = typeid(node_ref).name();
            m_is_constant = false;
            {
                TRACE_FUNCTION_FR(&*node << " " << node_ty << " : " << node->m_res_type, node_ty << " " << m_is_constant << " A=" << m_all_constant);

                // If the inner didn't set `is_constant`, clear `all_constant`
                node->visit(*this);
                if( !m_is_constant ) {
                    if(m_all_constant)
                        DEBUG("m_all_constant = false");
                    m_all_constant = false;
                }
            }
            m_is_constant = false;
        }

        void visit(::HIR::ExprNode_Borrow& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);

            // If the inner is constant (Array, Struct, Literal, const)
            if( m_all_constant )
            {
                // Handle a `_Unsize` inner
                auto* value_ptr_ptr = &node.m_value;
                for(;;)
                {
                    if(auto* inner_node = dynamic_cast<::HIR::ExprNode_Index*>(value_ptr_ptr->get()))
                    {
                        value_ptr_ptr = &inner_node->m_value;
                        continue;
                    }
                    if(auto* inner_node = dynamic_cast<::HIR::ExprNode_Unsize*>(value_ptr_ptr->get()))
                    {
                        value_ptr_ptr = &inner_node->m_value;
                        continue;
                    }
                    break;
                }
                // If the inner value is already a static or it's a constant, then treat as a valid static
                auto& value_ptr = *value_ptr_ptr;
                if(auto* inner_node = dynamic_cast<::HIR::ExprNode_Deref*>(value_ptr_ptr->get()))
                {
                    m_all_constant = saved_all_constant;
                    m_is_constant = true;
                    return ;
                }
                auto usage = value_ptr->m_usage;

                bool is_zst = ([&]()->bool{
                    // HACK: `Target_GetSizeOf` calls `Target_GetSizeAndAlignOf` which doesn't work on generic arrays (needs alignment)
                    if( const auto* te = value_ptr->m_res_type.data().opt_Array() ) {
                        if( te->size.is_Known() && te->size.as_Known() == 0 ) {
                            return true;
                        }
                    }
                    size_t v = 1;
                    Target_GetSizeOf(value_ptr->span(), m_resolve, value_ptr->m_res_type, v);
                    return v == 0;
                    })();

                // Not generic (can't check for interior mutability)
                if( !is_zst && monomorphise_type_needed(value_ptr->m_res_type) )
                {
                    DEBUG("-- " << value_ptr->m_res_type << " is generic");
                }
                // Not mutable (... or at least, not a non-shared non-zst)
                else if( !is_zst && node.m_type != HIR::BorrowType::Shared )
                {
                    DEBUG("-- Mutable borrow of non-ZST");
                }
                // NOTE: Interior mutability is handled at the lower levels (function calls and consts)
                // - This allows `EnumWithImut::NotImutVariant(...)` to be a valid promoted static.
                else
                {
                    DEBUG("-- Marking static");
                    node.m_is_valid_static_borrow_constant = true;

                    m_is_constant = true;
                }
            }
            else
            {
                // TODO: It would be nice if this could see through indexing/fields, but indexing needs a constant index
                if( dynamic_cast<::HIR::ExprNode_PathValue*>(node.m_value.get()) && node.m_value->m_usage == HIR::ValueUsage::Borrow )
                {
                    m_is_constant =  true;
                }
            }

            m_all_constant = saved_all_constant;
        }

        // - Composites (set local constant if all inner are constant)
        void visit(::HIR::ExprNode_ArraySized& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
            m_all_constant = saved_all_constant;
        }
        void visit(::HIR::ExprNode_ArrayList& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
            m_all_constant = saved_all_constant;
        }
        void visit(::HIR::ExprNode_StructLiteral& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
            m_all_constant = saved_all_constant;
        }
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
            m_all_constant = saved_all_constant;
        }
        void visit(::HIR::ExprNode_Tuple& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
            m_all_constant = saved_all_constant;
        }
        // - `let` is valid if the value is constant
        void visit(::HIR::ExprNode_Let& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
            m_all_constant = saved_all_constant;
        }
        // Function calls
        void visit(::HIR::ExprNode_CallMethod& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            if( m_all_constant ) {
                MonomorphState  ms_unused;
                auto v = m_resolve.get_value(node.span(), node.m_method_path, ms_unused, true);
                DEBUG(node.m_method_path << " is " << (v.as_Function()->m_const ? "" : "NOT ") << "constant");
                if( v.as_Function()->m_const ) {
                    m_is_constant = !is_maybe_interior_mut(node);
                }
            }
            m_all_constant = saved_all_constant;
        }
        void visit(::HIR::ExprNode_CallPath& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            if( m_all_constant ) {
                MonomorphState  ms_unused;
                auto v = m_resolve.get_value(node.span(), node.m_path, ms_unused, true);
                DEBUG(node.m_path << " is " << (v.as_Function()->m_const ? "" : "NOT") << " constant");
                if( v.as_Function()->m_const ) {
                    m_is_constant = !is_maybe_interior_mut(node);
                }
            }
            m_all_constant = saved_all_constant;
        }
        // - Accessors (constant if the inner is constant)
        void visit(::HIR::ExprNode_Deref& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            if(node.m_value->m_res_type.data().is_Borrow()) {
                m_is_constant = m_all_constant;
            }
        }
        void visit(::HIR::ExprNode_Field& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        void visit(::HIR::ExprNode_Index& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            // Ensure that it's only a ".."
            if(m_all_constant)
            {
                const auto& ty = node.m_index->m_res_type;
                DEBUG("_Index: ty = " << ty);
                if( ty.data().is_Path() && ty.data().as_Path().path.m_data.is_Generic() && ty.data().as_Path().path.m_data.as_Generic().m_path == m_lang_RangeFull ) {
                    DEBUG("_Index: RangeFull - can be constant");
                    m_is_constant = !is_maybe_interior_mut(node);
                }
                else {
                    // ... or just allow it - consteval can handle indexing out of bounds, right?
                    // ref: 1.39 librustc_data_structures/indexed_vec.rs L141 `const fn from_u32_const` uses it as a compile-time assert
                    m_is_constant = !is_maybe_interior_mut(node);
                }
            }
            m_all_constant = saved_all_constant;
        }
        // - Operations (only cast currently)
        void visit(::HIR::ExprNode_Cast& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
            m_all_constant = saved_all_constant;
        }
        void visit(::HIR::ExprNode_Unsize& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
            m_all_constant = saved_all_constant;
        }
        void visit(::HIR::ExprNode_BinOp& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            if( m_all_constant )
            {
                // Only allow operations between matching primitives
                // - Which can only be integer/float ops
                if( node.m_left->m_res_type == node.m_right->m_res_type ) {
                    if( node.m_left->m_res_type.data().is_Primitive() ) {
                        m_is_constant = true;
                    }
                }
            }
            m_all_constant = saved_all_constant;
        }
        void visit(::HIR::ExprNode_UniOp& node) override {
            auto saved_all_constant = m_all_constant;
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            if( m_all_constant )
            {
                // Only allow operations on primitives
                // - Which can only be integer/float ops
                if( node.m_value->m_res_type.data().is_Primitive() ) {
                    m_is_constant = true;
                }
            }
            m_all_constant = saved_all_constant;
        }

        // - Block (only if everything? What about just has a value?)
        void visit(::HIR::ExprNode_Block& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        // - Root values
        void visit(::HIR::ExprNode_Literal& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = true;
        }
        void visit(::HIR::ExprNode_ConstParam& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            //m_is_constant = true;
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = true;
        }
        void visit(::HIR::ExprNode_PathValue& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            MonomorphState  ms;
            // If the target is a constant, set `m_is_constant`
            auto v = m_resolve.get_value(node.span(), node.m_path, ms, /*signature_only*/true);
            switch(v.tag())
            {
            case StaticTraitResolve::ValuePtr::TAG_Constant:
                if( monomorphise_path_needed(node.m_path) ) {
                    DEBUG("Constant path is still generic, can't transform into a `static`");
                }
                else {
                    m_is_constant = !is_maybe_interior_mut(node);
                    DEBUG(node.m_path << " m_is_constant=" << m_is_constant);
                }
                break;
            case StaticTraitResolve::ValuePtr::TAG_Function:
                m_is_constant = true;
                break;
            default:
                break;
            }
        }
        // - Closures: Only constant if they don't capture anything
        void visit(::HIR::ExprNode_Closure& node) override {
            auto saved_all_constant = m_all_constant;
            ::HIR::ExprVisitorDef::visit(node);
            m_all_constant = saved_all_constant;

            if( node.m_avu_cache.captured_vars.empty() ) {
                m_is_constant = true;

                // Don't allow if any local is generic.
                for(auto idx : node.m_avu_cache.local_vars ) {
                    if( monomorphise_type_needed(m_expr_ptr.m_bindings[idx]) ) {
                        m_is_constant = false;
                        break;
                    }
                }
            }
        }

    private:
        bool is_maybe_interior_mut(const ::HIR::ExprNode& node) const {
            return m_resolve.type_is_interior_mutable(node.span(), node.m_res_type) != HIR::Compare::Unequal;
        }
    };

    /// <summary>
    /// Visit all expressions and just mark borrows that are of promotable statics (don't promote just yet)
    /// </summary>
    class OuterVisitor_Mark:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
        StaticTraitResolve  m_resolve;

        const ::HIR::TypeRef*   m_self_type = nullptr;
        const HIR::ItemPath*  m_current_module_path;
        const HIR::Module*  m_current_module;

    public:
        OuterVisitor_Mark(const ::HIR::Crate& crate)
            : m_crate(crate)
            , m_resolve(m_crate)
            , m_current_module(nullptr)
        {
        }

        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override {
            auto par = m_current_module;
            auto par_p = m_current_module_path;
            m_current_module = &mod;
            m_current_module_path = &p;

            ::HIR::Visitor::visit_module(p, mod);

            m_current_module = par;
            m_current_module_path = par_p;
        }
        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override {
            ::HIR::TypeRef  self("Self", 0xFFFF);
            m_self_type = &self;
            auto _ = m_resolve.set_impl_generics(MetadataType::TraitObject, item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
            m_self_type = &self;
        }
        void visit_type_impl(::HIR::TypeImpl& impl) override {
            DEBUG("impl " << impl.m_params.fmt_args() << " " << impl.m_type << " (from " << impl.m_src_module << ")");
            const auto& srcmod = m_crate.get_mod_by_path(Span(), impl.m_src_module);
            auto mod_ip = HIR::ItemPath(impl.m_src_module);
            m_self_type = &impl.m_type;
            m_current_module = &srcmod;
            m_current_module_path = &mod_ip;

            auto _ = m_resolve.set_impl_generics(impl.m_type, impl.m_params);
            ::HIR::Visitor::visit_type_impl(impl);

            m_current_module = nullptr;
            m_self_type = nullptr;
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override {
            DEBUG("src module " << impl.m_src_module);
            const auto& srcmod = m_crate.get_mod_by_path(Span(), impl.m_src_module);
            auto mod_ip = HIR::ItemPath(impl.m_src_module);
            m_self_type = &impl.m_type;
            m_current_module = &srcmod;
            m_current_module_path = &mod_ip;

            auto _ = m_resolve.set_impl_generics(impl.m_type, impl.m_params);
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);

            m_current_module = nullptr;
            m_self_type = nullptr;
        }

        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) override {
            BUG(Span(), "visit_expr hit in OuterVisitor");
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            if( auto* ep = ty.data_mut().opt_Array() )
            {
                this->visit_type( ep->inner );
                DEBUG("Array size " << ty);
                if( auto* cg = ep->size.opt_Unevaluated() ) {
                    if(cg->is_Unevaluated())
                    {
                        ExprVisitor_Mark    ev(m_resolve, m_self_type, *cg->as_Unevaluated());
                        ev.visit_node_ptr( *cg->as_Unevaluated() );
                    }
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
            if( item.m_code )
            {
                auto _ = m_resolve.set_item_generics(item.m_params);
                DEBUG("Function code " << p);
                ExprVisitor_Mark    ev(m_resolve, m_self_type, item.m_code);
                ev.visit_node_ptr( item.m_code );
                if( item.m_const )
                {
                    if( !ev.all_constant() ) {
                        //ERROR(item.m_code->span(), E0000, "`const fn " << p << "` is not constant");
                        // - Todo: Constant blocks?
                    }
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
                ExprVisitor_Mark    ev(m_resolve, m_self_type, item.m_value);
                ev.visit_node_ptr(item.m_value);
                if( !ev.all_constant() ) {
                    //ERROR(item.m_value->span(), E0000, "`static " << p << "` is not constant");
                    //WARNING(item.m_value->span(), W0000, "`static " << p << "` is not constant");
                }
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value )
            {
                ExprVisitor_Mark    ev(m_resolve, m_self_type, item.m_value);
                ev.visit_node_ptr(item.m_value);
                if( !ev.all_constant() ) {
                    // TODO: The set of operations valid in a `static`/`const` is different to the set that is valid to promote.
                    // - Just trust the evaluator to detect invalid operations.
                    //WARNING(item.m_value->span(), W0000, "`const " << p << "` is not constant");
                }
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            if(auto* e = item.m_data.opt_Value())
            {
                auto _ = m_resolve.set_impl_generics(MetadataType::None, item.m_params);
                for(auto& var : e->variants)
                {
                    DEBUG("Enum value " << p << " - " << var.name);

                    if( var.expr )
                    {
                        ExprVisitor_Mark    ev(m_resolve, m_self_type, var.expr);
                        ev.visit_node_ptr(var.expr);
                    }
                }
            }
        }
    };

    class ExprVisitor_Mutate:
        public ::HIR::ExprVisitorDef
    {
    public:
        typedef std::function<HIR::SimplePath(Span, HIR::TypeRef, HIR::ExprPtr, HIR::GenericParams)>    t_new_static_cb;
    private:
        const StaticTraitResolve& m_resolve;
        const ::HIR::TypeRef*   m_self_type;
        t_new_static_cb m_new_static_cb;
        const ::HIR::ExprPtr& m_expr_ptr;

        HIR::SimplePath m_lang_RangeFull;

    public:
        ExprVisitor_Mutate(const StaticTraitResolve& resolve, const ::HIR::TypeRef* self_type, t_new_static_cb new_static_cb, const ::HIR::ExprPtr& expr_ptr)
            :m_resolve(resolve)
            ,m_self_type(self_type)
            ,m_new_static_cb( mv$(new_static_cb) )
            ,m_expr_ptr(expr_ptr)
        {
            m_lang_RangeFull = m_resolve.m_crate.get_lang_item_path_opt("range_full");
            // EVIL hack: Since `range_full` wasn't a lang item until (latest) 1.54, resolve the path here
            if( m_lang_RangeFull == ::HIR::SimplePath() ) {
                auto sp = ::HIR::SimplePath(g_core_crate, {"ops", "RangeFull"});
                auto& ti = m_resolve.m_crate.get_typeitem_by_path(Span(), sp);
                if(ti.is_Import()) {
                    m_lang_RangeFull = ti.as_Import().path;
                }
                else {
                    m_lang_RangeFull = mv$(sp);
                }
            }
        }
        void visit_node_ptr(::HIR::ExprPtr& root) {
            root->visit(*this);
        }

        struct Monomorph: public Monomorphiser
        {
            const ::HIR::GenericParams& params;
            unsigned ofs_impl_l;
            unsigned ofs_item_l;
            unsigned ofs_impl_t;
            unsigned ofs_item_t;
            unsigned ofs_impl_v;
            unsigned ofs_item_v;
            Monomorph(const ::HIR::GenericParams& params,
                unsigned ofs_impl_t, unsigned ofs_item_t,
                unsigned ofs_impl_v, unsigned ofs_item_v,
                unsigned ofs_impl_l, unsigned ofs_item_l
                )
                : params(params)
                , ofs_impl_t(ofs_impl_t)
                , ofs_item_t(ofs_item_t)
                , ofs_impl_v(ofs_impl_v)
                , ofs_item_v(ofs_item_v)
                , ofs_impl_l(ofs_impl_l)
                , ofs_item_l(ofs_item_l)
            {
            }
            ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& ge) const override
            {
                unsigned i;
                if( ge.binding == 0xFFFF ) {
                    i = 0;
                }
                else if( ge.binding < 256 ) {
                    i = ofs_impl_t + ge.idx();
                }
                else if( ge.binding < 2*256 ) {
                    i = ofs_item_t + ge.idx();
                }
                else {
                    BUG(sp, "Generic type " << ge << " unknown");
                }
                ASSERT_BUG(sp, i < params.m_types.size(), "Item generic type binding OOR - " << ge << " (" << i << " !< " << params.m_types.size() << ")");
                return ::HIR::TypeRef(params.m_types[i].m_name, 256 + i);
            }
            ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& ge) const {
                unsigned i;
                if( ge.binding == 0xFFFF ) {
                    BUG(sp, "Binding 0xFFFF isn't valid for values");
                }
                else if( ge.binding < 256 ) {
                    i = ofs_impl_v + ge.idx();
                }
                else if( ge.binding < 2*256 ) {
                    i = ofs_item_v + ge.idx();
                }
                else {
                    BUG(sp, "Generic value " << ge << " unknown");
                }
                ASSERT_BUG(sp, i < params.m_values.size(), "Item generic value binding OOR - " << ge << " (" << i << " !< " << params.m_values.size() << ")");
                return ::HIR::GenericRef(params.m_values[i].m_name, 256 + i);
            }
            ::HIR::LifetimeRef get_lifetime(const Span& sp, const ::HIR::GenericRef& ge) const override {
                unsigned i;
                switch(ge.group())
                {
                case 0:
                    i = ofs_impl_l + ge.idx();
                    break;
                case 1:
                    i = ofs_item_l + ge.idx();
                    break;
                default:
                    BUG(sp, "Generic lifetime " << ge << " unknown");
                }
                ASSERT_BUG(sp, i < params.m_lifetimes.size(), "Item generic lifetime binding OOR - " << ge << " (" << i << " !< " << params.m_lifetimes.size() << ")");
                return ::HIR::LifetimeRef(256 + i);
            }
        };

        Monomorph create_params(const Span& sp, ::HIR::GenericParams& params, ::HIR::PathParams& constructor_path_params) const
        {
            DEBUG("Impl: " << m_resolve.impl_generics().fmt_args());
            DEBUG("Item: " << m_resolve.item_generics().fmt_args());
            // - 0xFFFF "Self" -> 0 "Super" (if present)
            if( m_resolve.has_self() )
            {
                assert( m_self_type );
                constructor_path_params.m_types.push_back( m_self_type->clone() );
                params.m_types.push_back( ::HIR::TypeParamDef { "Super", {}, false } );  // TODO: Determine if parent Self is Sized
            }
            // - Top-level params come first
            unsigned ofs_impl_l = params.m_lifetimes.size();
            for(const auto& lft_def : m_resolve.impl_generics().m_lifetimes) {
                unsigned i = &lft_def - &m_resolve.impl_generics().m_lifetimes.front();
                constructor_path_params.m_lifetimes.push_back( ::HIR::LifetimeRef( 0*256 + i ) );
                params.m_lifetimes.push_back( ::HIR::LifetimeDef { lft_def.m_name } );
            }
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
            unsigned ofs_item_l = params.m_lifetimes.size();
            for(const auto& lft_def : m_resolve.item_generics().m_lifetimes) {
                unsigned i = &lft_def - &m_resolve.item_generics().m_lifetimes.front();
                constructor_path_params.m_lifetimes.push_back( ::HIR::LifetimeRef( 1*256 + i ) );
                params.m_lifetimes.push_back( ::HIR::LifetimeDef { lft_def.m_name } );
            }
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
            DEBUG("impl_path_params = " << params.make_nop_params(0)
                << " ofs_*_t=" << ofs_item_t << "," << ofs_impl_t << "," << params.m_types.size()
                << " ofs_*_v=" << ofs_item_v << "," << ofs_impl_v << "," << params.m_values.size()
                << " ofs_*_l=" << ofs_item_l << "," << ofs_impl_l << "," << params.m_lifetimes.size()
                );

            Monomorph monomorph_cb(params, ofs_impl_t, ofs_item_t, ofs_impl_v, ofs_item_v, ofs_impl_l, ofs_item_l);

            // - Clone the bounds (from both levels)
            auto monomorph_bound = [&](const ::HIR::GenericBound& b)->::HIR::GenericBound {
                TU_MATCH_HDRA( (b), {)
                TU_ARMA(Lifetime, e)
                    return ::HIR::GenericBound::make_Lifetime({
                            monomorph_cb.monomorph_lifetime(sp, e.test),
                            monomorph_cb.monomorph_lifetime(sp, e.valid_for),
                            });
                TU_ARMA(TypeLifetime, e)
                    return ::HIR::GenericBound::make_TypeLifetime({ monomorph_cb.monomorph_type(sp, e.type), monomorph_cb.monomorph_lifetime(sp, e.valid_for) });
                TU_ARMA(TraitBound, e)
                    return ::HIR::GenericBound::make_TraitBound  ({ (e.hrtbs ? box$(e.hrtbs->clone()) : nullptr), monomorph_cb.monomorph_type(sp, e.type), monomorph_cb.monomorph_traitpath(sp, e.trait, false) });
                TU_ARMA(TypeEquality, e)
                    return ::HIR::GenericBound::make_TypeEquality({ monomorph_cb.monomorph_type(sp, e.type), monomorph_cb.monomorph_type(sp, e.other_type) });
                }
                throw "";
                };
            for(const auto& bound : m_resolve.impl_generics().m_bounds ) {
                DEBUG("IMPL - " << bound);
                params.m_bounds.push_back( monomorph_bound(bound) );
            }
            for(const auto& bound : m_resolve.item_generics().m_bounds ) {
                DEBUG("ITEM - " << bound);
                params.m_bounds.push_back( monomorph_bound(bound) );
            }
            return monomorph_cb;
        }

        void visit(::HIR::ExprNode_Borrow& node) override {
            ::HIR::ExprVisitorDef::visit(node);

            if( node.m_is_valid_static_borrow_constant )
            {
                // Clear the flag, so subsequent visits don't repeat
                node.m_is_valid_static_borrow_constant = false;

                // Handle a `_Unsize` inner
                auto* value_ptr_ptr = &node.m_value;
                for(;;)
                {
                    if(auto* inner_node = dynamic_cast<::HIR::ExprNode_Index*>(value_ptr_ptr->get()))
                    {
                        value_ptr_ptr = &inner_node->m_value;
                        continue;
                    }
                    if(auto* inner_node = dynamic_cast<::HIR::ExprNode_Unsize*>(value_ptr_ptr->get()))
                    {
                        value_ptr_ptr = &inner_node->m_value;
                        continue;
                    }
                    break;
                }
                // If the inner value is already a static or it's a constant, then treat as a valid static
                auto& value_ptr = *value_ptr_ptr;
                if(auto* inner_node = dynamic_cast<::HIR::ExprNode_Deref*>(value_ptr_ptr->get()))
                {
                    BUG(node.span(), "Unexpected inner being deref?");
                    return ;
                }
                auto usage = value_ptr->m_usage;

                DEBUG("-- Creating static");
                // Clone the in-scope generics (same as done in closure generation)
                // - Would be picky, but hard to get the bounds right.
                HIR::GenericParams  params_def;
                HIR::PathParams constr_params;
                auto monomorph = this->create_params(node.span(), params_def, constr_params);

                struct V: public HIR::ExprVisitorDef {
                    const Monomorph&    monomorph;
                    bool is_generic;
                    std::map<unsigned,unsigned> binding_mapping;
                    V(const Monomorph& monomorph): monomorph(monomorph), is_generic(false) {}

                    void visit_type(HIR::TypeRef& ty) override {
                        if( monomorphise_type_needed(ty) ) {
                            this->is_generic = true;
                            auto new_ty = this->monomorph.monomorph_type(Span(), ty);
                            DEBUG(ty << " -> " << new_ty);
                            ty = std::move(new_ty);
                        }
                    }
                    void visit_pattern(const Span& sp, HIR::Pattern& pat) override {
                        HIR::ExprVisitorDef::visit_pattern(sp, pat);
                        for( auto& pb : pat.m_bindings )
                        {
                            auto idx = static_cast<unsigned>(binding_mapping.size());
                            binding_mapping.insert(::std::make_pair(pb.m_slot, idx));
                            pb.m_slot = idx;
                        }

                        if(auto* e = pat.m_data.opt_SplitSlice())
                        {
                            if( e->extra_bind.is_valid() )
                            {
                                auto idx = static_cast<unsigned>(binding_mapping.size());
                                binding_mapping.insert(::std::make_pair(e->extra_bind.m_slot, idx));
                                e->extra_bind.m_slot = idx;
                            }
                        }
                    }
                    void visit(HIR::ExprNode_Variable& node) override {
                        HIR::ExprVisitorDef::visit(node);
                        ASSERT_BUG(node.span(), binding_mapping.count(node.m_slot) != 0, "");
                        node.m_slot = binding_mapping.at(node.m_slot);
                    }
                    void visit(HIR::ExprNode_Closure& node) override {
                        HIR::ExprVisitorDef::visit(node);
                        // Update bindings here too
                        if( node.m_code ) {
                            for(auto& l : node.m_avu_cache.local_vars) {
                                ASSERT_BUG(node.span(), binding_mapping.count(l) != 0, "");
                                l = binding_mapping.at(l);
                            }
                            assert(node.m_avu_cache.captured_vars.empty());
                        }
                    }
                } v(monomorph);
                value_ptr->visit(v);
                v.visit_type(value_ptr->m_res_type);
                if( !v.is_generic ) {
                    params_def = HIR::GenericParams();
                    constr_params = HIR::PathParams();
                    DEBUG("Concrete static");
                }
                else {
                    DEBUG("Generic static");
                }

                auto val_expr = HIR::ExprPtr(mv$(value_ptr));
                val_expr.m_state = m_expr_ptr.m_state.clone();
                val_expr.m_state->stage = ::HIR::ExprState::Stage::Sbc;

                val_expr.m_bindings.resize(v.binding_mapping.size());
                for(auto& e : v.binding_mapping) {
                    val_expr.m_bindings[e.second] = monomorph.monomorph_type(node.span(), m_expr_ptr.m_bindings.at(e.first));
                }

                // Create new static
                auto sp = val_expr->span();

                // Replace all unknown lifetimes with `'static`
                // - (Currently) there shouldn't be any generics, need to solve that later on?
                struct M: public Monomorphiser {
                    ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& g) const override {
                        return HIR::TypeRef(g.name, g.binding);
                    }
                    ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& g) const override {
                        return g;
                    }
                    ::HIR::LifetimeRef get_lifetime(const Span& sp, const ::HIR::GenericRef& g) const override {
                        return HIR::LifetimeRef(g.binding);
                    }
                    HIR::LifetimeRef monomorph_lifetime(const Span& sp, const HIR::LifetimeRef& lft) const override {
                        if( lft == HIR::LifetimeRef() ) {
                            return  HIR::LifetimeRef::new_static();
                        }
                        else {
                            return lft;
                        }
                    }
                } m;

                auto static_ty = m.monomorph_type(sp, val_expr->m_res_type, /*allow_infer=*/false);

                auto m2 = MonomorphStatePtr(nullptr, nullptr, &constr_params);
                auto new_res_ty = m2.monomorph_type(sp, static_ty, false);
                DEBUG("new_res_ty = " << new_res_ty);

                auto path = m_new_static_cb(sp, mv$(static_ty), mv$(val_expr), mv$(params_def));
                DEBUG("> " << path << constr_params);
                // Update the `m_value` to point to a new node
                auto new_node = NEWNODE(std::move(new_res_ty), PathValue, sp, HIR::GenericPath(mv$(path), mv$(constr_params)), HIR::ExprNode_PathValue::STATIC);
                new_node->m_usage = usage;
                value_ptr = mv$(new_node);
            }
        }
    };
    class OuterVisitor:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
        StaticTraitResolve  m_resolve;

        const ::HIR::TypeRef*   m_self_type = nullptr;
        const HIR::ItemPath*  m_current_module_path;
        const HIR::Module*  m_current_module;

        std::map<const HIR::Module*, std::vector< std::pair<HIR::SimplePath, HIR::Static> > >  m_new_statics;

    public:
        OuterVisitor(const ::HIR::Crate& crate)
            : m_crate(crate)
            , m_resolve(m_crate)
            , m_current_module(nullptr)
        {
        }

        ExprVisitor_Mutate::t_new_static_cb get_new_ty_cb()
        {
            return [this](Span sp, HIR::TypeRef ty, HIR::ExprPtr val_expr, HIR::GenericParams generics)->HIR::SimplePath {
                ASSERT_BUG(sp, m_current_module, "");
                // Assign a path (based on the current list)
                auto& list = m_new_statics[m_current_module];
                auto idx = list.size();
                auto name = RcString::new_interned( FMT("lifted#" << idx) );
                auto path = (*m_current_module_path + name).get_simple_path();
                auto new_static = HIR::Static(
                    HIR::Linkage(),
                    /*is_mut=*/false,
                    mv$(ty),
                    /*m_value=*/mv$(val_expr)
                    );
                new_static.m_params = mv$(generics);
                DEBUG(path << " = " << new_static.m_value_res);
                list.push_back(std::make_pair( path, mv$(new_static) ));
                return path;
                };
        }

        void visit_crate(::HIR::Crate& crate) override {
            ::HIR::Visitor::visit_crate(crate);

            // Once the crate is complete, add the newly created statics to the module tree
            for(auto& mod_list : m_new_statics)
            {
                auto& mod = *const_cast<HIR::Module*>(mod_list.first);
                m_current_module = &mod;

                HIR::SimplePath mod_path;
                if( !mod_list.second.empty() )
                {
                    mod_path = mod_list.second[0].first;
                    mod_path.m_components.pop_back();
                }

                struct Nvs: ::HIR::Evaluator::Newval {

                    size_t m_next_idx ;
                    ::HIR::SimplePath   m_current_module_path;
                    ::HIR::Module&  m_current_module;

                    Nvs(::HIR::SimplePath current_module_path, ::HIR::Module& current_module, size_t idx)
                        : m_current_module_path(::std::move(current_module_path))
                        , m_current_module(current_module)
                        , m_next_idx (idx)
                    {
                    }

                    ::HIR::Path new_static(::HIR::TypeRef type, EncodedLiteral value) override {
                        auto name = RcString::new_interned( FMT("lifted#" << m_next_idx) );
                        m_next_idx ++;
                        auto path = m_current_module_path + name;
                        auto new_static = HIR::Static(
                            HIR::Linkage(),
                            /*is_mut=*/false,
                            ::std::move(type),
                            /*m_value=*/HIR::ExprPtr()
                        );
                        new_static.m_value_generated = true;
                        new_static.m_value_res = ::std::move(value);
                        DEBUG(path << " = " << new_static.m_value_res);
                        m_current_module.m_value_items.insert(std::make_pair( name, box$(HIR::VisEnt<HIR::ValueItem> {
                                HIR::Publicity::new_none(), // Should really be private, but we're well after checking
                                HIR::ValueItem(::std::move(new_static))
                            })) );
                        return path;
                    }
                } nvs { mod_path, mod, mod_list.second.size() };

                for(auto& new_static_pair : mod_list.second)
                {
                    Span    sp;
                    auto& new_static = new_static_pair.second;

                    TRACE_FUNCTION_F("New static " << new_static_pair.first);

                    if( !new_static.m_params.is_generic() )
                    {
                        new_static.m_value_res = ::HIR::Evaluator(sp, m_crate, nvs).evaluate_constant( new_static_pair.first, new_static.m_value, new_static.m_type.clone());
                        new_static.m_value_generated = true;
                    }

                    mod.m_value_items.insert(std::make_pair( mv$(new_static_pair.first.m_components.back()), box$(HIR::VisEnt<HIR::ValueItem> {
                        HIR::Publicity::new_none(), // Should really be private, but we're well after checking
                        HIR::ValueItem(mv$(new_static_pair.second))
                        })) );
                }
            }
        }

        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override {
            auto par = m_current_module;
            auto par_p = m_current_module_path;
            m_current_module = &mod;
            m_current_module_path = &p;

            ::HIR::Visitor::visit_module(p, mod);

            m_current_module = par;
            m_current_module_path = par_p;
        }
        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override {
            ::HIR::TypeRef  self("Self", 0xFFFF);
            m_self_type = &self;
            auto _ = m_resolve.set_impl_generics(MetadataType::TraitObject, item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
            m_self_type = &self;
        }
        void visit_type_impl(::HIR::TypeImpl& impl) override {
            DEBUG("impl " << impl.m_params.fmt_args() << " " << impl.m_type << " (from " << impl.m_src_module << ")");
            const auto& srcmod = m_crate.get_mod_by_path(Span(), impl.m_src_module);
            auto mod_ip = HIR::ItemPath(impl.m_src_module);
            m_self_type = &impl.m_type;
            m_current_module = &srcmod;
            m_current_module_path = &mod_ip;

            auto _ = m_resolve.set_impl_generics(impl.m_type, impl.m_params);
            ::HIR::Visitor::visit_type_impl(impl);

            m_current_module = nullptr;
            m_self_type = nullptr;
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override {
            DEBUG("src module " << impl.m_src_module);
            const auto& srcmod = m_crate.get_mod_by_path(Span(), impl.m_src_module);
            auto mod_ip = HIR::ItemPath(impl.m_src_module);
            m_self_type = &impl.m_type;
            m_current_module = &srcmod;
            m_current_module_path = &mod_ip;

            auto _ = m_resolve.set_impl_generics(impl.m_type, impl.m_params);
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);

            m_current_module = nullptr;
            m_self_type = nullptr;
        }

        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) override {
            BUG(Span(), "visit_expr hit in OuterVisitor");
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            if( auto* ep = ty.data_mut().opt_Array() )
            {
                this->visit_type( ep->inner );
                DEBUG("Array size " << ty);
                if( auto* cg = ep->size.opt_Unevaluated() ) {
                    if(cg->is_Unevaluated())
                    {
                        ExprVisitor_Mutate  ev(m_resolve, m_self_type, this->get_new_ty_cb(), *cg->as_Unevaluated());
                        ev.visit_node_ptr( *cg->as_Unevaluated() );
                    }
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
            if( item.m_code )
            {
                auto _ = m_resolve.set_item_generics(item.m_params);
                DEBUG("Function code " << p);
                ExprVisitor_Mutate  ev(m_resolve, m_self_type, this->get_new_ty_cb(), item.m_code);
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
                ExprVisitor_Mutate  ev(m_resolve, m_self_type, this->get_new_ty_cb(), item.m_value);
                ev.visit_node_ptr(item.m_value);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value )
            {
                ExprVisitor_Mutate  ev(m_resolve, m_self_type, this->get_new_ty_cb(), item.m_value);
                ev.visit_node_ptr(item.m_value);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            if(auto* e = item.m_data.opt_Value())
            {
                auto _ = m_resolve.set_impl_generics(MetadataType::None, item.m_params);
                for(auto& var : e->variants)
                {
                    DEBUG("Enum value " << p << " - " << var.name);

                    if( var.expr )
                    {
                        ExprVisitor_Mutate  ev(m_resolve, m_self_type, this->get_new_ty_cb(), var.expr);
                        ev.visit_node_ptr(var.expr);
                    }
                }
            }
        }
    };
}   // namespace

void HIR_Expand_StaticBorrowConstants_Mark_Expr(const ::HIR::Crate& crate, const ::HIR::ItemPath& ip, ::HIR::ExprPtr& exp)
{
    TRACE_FUNCTION_F(ip);
    StaticTraitResolve  resolve(crate);

    // TODO: Get `Self` type
    static_borrow_constants::ExprVisitor_Mark    evm(resolve, nullptr, exp);
    evm.visit_node_ptr(exp);
    if( !evm.all_constant() ) {
        //WARNING(exp->span(), W0000, "`static`/`const` " << ip << " is not constant?");
        //ERROR(exp->span(), E0000, "`static`/`const` " << ip << " is not constant");
        // TODO: How to determine if this is a static/const instead of a function?
        // - For a function, maybe could also check?
    }
}
void HIR_Expand_StaticBorrowConstants_Expr(const ::HIR::Crate& crate, const ::HIR::ItemPath& ip, ::HIR::ExprPtr& exp)
{
    TRACE_FUNCTION_F(ip);
    StaticTraitResolve  resolve(crate);

    static int static_count = 0;

    static_borrow_constants::ExprVisitor_Mutate  ev(resolve, nullptr, [&](Span sp, HIR::TypeRef ty, HIR::ExprPtr val_expr, HIR::GenericParams generics)->HIR::SimplePath {
        auto name = RcString::new_interned(FMT("lifted#C_" << static_count++));

        auto path = ::HIR::SimplePath(crate.m_crate_name, {name});
        auto new_static = HIR::Static(
            HIR::Linkage(),
            /*is_mut=*/false,
            mv$(ty),
            /*m_value=*/mv$(val_expr)
            );
        new_static.m_params = mv$(generics);
        // - Since this was neeed in consteval, it's going to need to be saved?
        new_static.m_save_literal = true;
        DEBUG(path << " = ?");
        auto boxed = box$(( ::HIR::VisEnt< ::HIR::ValueItem> { ::HIR::Publicity::new_none(), ::HIR::ValueItem( mv$(new_static) ) } ));
        crate.m_new_values.push_back( ::std::make_pair(name, mv$(boxed)) );
        return path;
        }, exp);
    ev.visit_node_ptr( exp );
}
void HIR_Expand_StaticBorrowConstants_Mark(::HIR::Crate& crate)
{
    static_borrow_constants::OuterVisitor_Mark    ov(crate);
    ov.visit_crate( crate );
}
void HIR_Expand_StaticBorrowConstants(::HIR::Crate& crate)
{
    static_borrow_constants::OuterVisitor    ov(crate);
    ov.visit_crate( crate );

    // Consteval can run again, creating new items
    for(auto& new_ty_pair : crate.m_new_types)
    {
        crate.m_root_module.m_mod_items.insert( mv$(new_ty_pair) );
    }
    crate.m_new_types.clear();
    for(auto& new_val_pair : crate.m_new_values)
    {
        crate.m_root_module.m_value_items.insert( mv$(new_val_pair) );
    }
    crate.m_new_values.clear();
}
