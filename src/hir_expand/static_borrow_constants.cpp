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
#include <algorithm>
#include "main_bindings.hpp"

namespace {
    inline HIR::ExprNodeP mk_exprnodep(HIR::ExprNode* en, ::HIR::TypeRef ty){ en->m_res_type = mv$(ty); return HIR::ExprNodeP(en); }
}
#define NEWNODE(TY, CLASS, ...)  mk_exprnodep(new HIR::ExprNode_##CLASS(__VA_ARGS__), TY)

namespace {

    class ExprVisitor_Extract:
        public ::HIR::ExprVisitorDef
    {
        HIR::Literal    m_out;
    public:
        ExprVisitor_Extract()
        {
        }

        HIR::Literal take_value() {
            return mv$(m_out);
        }

        void visit_node_ptr(::HIR::ExprNodeP& node) override {
            const auto& node_ref = *node;
            const char* node_ty = typeid(node_ref).name();
            // NOTE: It's the parent node that's unhandled, not this one
            BUG(node->span(), "Unhandled node type in static_borrow_constants ExprVisitor_Extract - child=" << node_ty);
        }

        void visit(::HIR::ExprNode_ArraySized& node) override {

            node.m_val->visit(*this);
            auto rv = HIR::Literal::make_List({});
            if( node.m_size_val > 0 )
            {
                for(size_t i = 0; i < node.m_size_val-1; i ++)
                {
                    rv.as_List().push_back(m_out.clone());
                }
                rv.as_List().push_back(mv$(m_out));
            }
            m_out = mv$(rv);
        }
        void visit(::HIR::ExprNode_ArrayList& node) override {
            auto rv = HIR::Literal::make_List({});
            for(auto& n : node.m_vals)
            {
                n->visit(*this);
                rv.as_List().push_back(mv$(m_out));
            }
            m_out = mv$(rv);
        }
        void visit(::HIR::ExprNode_StructLiteral& node) override {
            auto rv = HIR::Literal::make_List({});
            if( node.m_base_value )
            {
                node.m_base_value->visit(*this);
                rv = mv$(m_out);
            }
            for(auto& fld : node.m_values)
            {
                // Find field index
                // Ensure correct size
                // Set field
                TODO(node.span(), "StructLiteral");
            }
            m_out = mv$(rv);
            if( !node.m_is_struct )
            {
                unsigned var_idx = 0;
                TODO(node.span(), "StructLiteral enum/union to Literal");
                m_out = ::HIR::Literal::new_variant( var_idx, mv$(m_out) );
            }
        }
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            auto rv = HIR::Literal::make_List({});
            for(auto& n : node.m_args)
            {
                n->visit(*this);
                rv.as_List().push_back(mv$(m_out));
            }
            m_out = mv$(rv);
            if( !node.m_is_struct )
            {
                unsigned var_idx = 0;
                TODO(node.span(), "TupleVariant enum to Literal");
                m_out = ::HIR::Literal::new_variant( var_idx, mv$(m_out) );
            }
        }
        void visit(::HIR::ExprNode_Tuple& node) override {
            auto rv = HIR::Literal::make_List({});
            for(auto& n : node.m_vals)
            {
                n->visit(*this);
                rv.as_List().push_back(mv$(m_out));
            }
            m_out = mv$(rv);
        }
        // - Root values
        void visit(::HIR::ExprNode_Literal& node) override {
            TU_MATCH_HDRA( (node.m_data), {)
            TU_ARMA(Integer, e) {
                m_out = HIR::Literal::make_Integer(e.m_value);
                }
            TU_ARMA(Float, e) {
                m_out = HIR::Literal::make_Float(e.m_value);
                }
            TU_ARMA(Boolean, e) {
                m_out = HIR::Literal::make_Integer(e ? 1u : 0u);
                }
            TU_ARMA(String, e) {
                m_out = HIR::Literal::make_String(e);
                }
            TU_ARMA(ByteString, e) {
                std::string rv;
                for(auto c : e)
                    rv.push_back(c);
                m_out = HIR::Literal::make_String(mv$(rv));
                }
            }
        }
        void visit(::HIR::ExprNode_PathValue& node) override {
            // If the target is a constant, set `m_is_constant`
            TODO(node.span(), "Path constant to Literal");
        }

        // Borrow (from an inner lift)
        void visit(::HIR::ExprNode_Borrow& node) override {
            ASSERT_BUG(node.span(), node.m_type == HIR::BorrowType::Shared, "");
            ASSERT_BUG(node.span(), dynamic_cast<::HIR::ExprNode_PathValue*>(&*node.m_value), "");
            auto& val_path_node = dynamic_cast<::HIR::ExprNode_PathValue&>(*node.m_value);
            m_out = HIR::Literal::new_borrow_path( val_path_node.m_path.clone() );
        }
    };

    class ExprVisitor_Mutate:
        public ::HIR::ExprVisitorDef
    {
    public:
        typedef std::function<HIR::SimplePath(Span, HIR::TypeRef, HIR::Literal)>    t_new_static_cb;
    private:
        const StaticTraitResolve& m_resolve;
        t_new_static_cb m_new_static_cb;

        bool    m_is_constant;
        bool    m_all_constant;
    public:
        ExprVisitor_Mutate(const StaticTraitResolve& resolve, t_new_static_cb new_static_cb):
            m_resolve(resolve)
            ,m_new_static_cb( mv$(new_static_cb) )
            ,m_is_constant(false)
            ,m_all_constant(false)
        {
        }
        void visit_node_ptr(::HIR::ExprPtr& root) {
            const auto& node_ref = *root;
            const char* node_ty = typeid(node_ref).name();
            TRACE_FUNCTION_FR(&*root << " " << node_ty << " : " << root->m_res_type, node_ty);

            root->visit(*this);
        }
        void visit_node_ptr(::HIR::ExprNodeP& node) override {
            assert( node );
            const auto& node_ref = *node;
            const char* node_ty = typeid(node_ref).name();
            m_is_constant = false;
            {
                TRACE_FUNCTION_FR(&*node << " " << node_ty << " : " << node->m_res_type, node_ty << " " << m_is_constant);

                // If the inner didn't set `is_constant`, clear `all_constant`
                node->visit(*this);
                if( !m_is_constant ) {
                    m_all_constant = false;
                }
            }
            m_is_constant = false;
        }

        void visit(::HIR::ExprNode_Borrow& node) override {
            m_all_constant = true;
            ::HIR::ExprVisitorDef::visit(node);
            // If the inner is constant (Array, Struct, Literal, const)
            if( m_all_constant && node.m_type == HIR::BorrowType::Shared )
            {
                // And it's not interior mutable
                if( m_resolve.type_is_interior_mutable(node.m_value->span(), node.m_value->m_res_type) == HIR::Compare::Unequal )
                {
                    DEBUG("-- Creating static");
                    // Convert inner nodes into HIR::Literal
                    ExprVisitor_Extract ev_extract;
                    node.m_value->visit(ev_extract);
                    auto val = ev_extract.take_value();

                    // Create new static
                    auto path = m_new_static_cb(node.m_value->span(), node.m_value->m_res_type.clone(), mv$(val));
                    // Update the `m_value` to point to a new node
                    auto new_node = NEWNODE(mv$(node.m_value->m_res_type), PathValue, node.m_value->span(), mv$(path), HIR::ExprNode_PathValue::STATIC);
                    node.m_value = mv$(new_node);

                    m_is_constant = true;
                }
                else
                {
                    DEBUG("-- " << node.m_value->m_res_type << " could be interior mutable");
                }
            }
        }

        // - Composites (set local constant if all inner are constant
        void visit(::HIR::ExprNode_ArraySized& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        void visit(::HIR::ExprNode_ArrayList& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        void visit(::HIR::ExprNode_StructLiteral& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        void visit(::HIR::ExprNode_Tuple& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = m_all_constant;
        }
        // - Root values
        void visit(::HIR::ExprNode_Literal& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            m_is_constant = true;
        }
        void visit(::HIR::ExprNode_PathValue& node) override {
            ::HIR::ExprVisitorDef::visit(node);
            // TODO: If the target is a constant, set `m_is_constant`
        }
    };
    class OuterVisitor:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;

        const HIR::ItemPath*  m_current_module_path;
        const HIR::Module*  m_current_module;

        std::map<const HIR::Module*, std::vector< std::pair<RcString, HIR::Static> > >  m_new_statics;
        
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_crate(crate)
            ,m_current_module(nullptr)
        {
        }

        ExprVisitor_Mutate::t_new_static_cb get_new_ty_cb()
        {
            return [this](Span sp, HIR::TypeRef ty, HIR::Literal val)->HIR::SimplePath {
                ASSERT_BUG(sp, m_current_module, "");
                // Assign a path (based on the current list)
                auto& list = m_new_statics[m_current_module];
                auto idx = list.size();
                auto name = RcString::new_interned( FMT("lifted#" << idx) );
                auto path = (*m_current_module_path + name).get_simple_path();
                auto new_static = HIR::Static {
                    HIR::Linkage(),
                    /*is_mut=*/false,
                    mv$(ty),
                    /*m_value=*/HIR::ExprPtr(),
                    /*m_value_res=*/mv$(val)
                    };
                list.push_back(std::make_pair( name, mv$(new_static) ));
                return path;
                };
        }

        void visit_crate(::HIR::Crate& crate) override {
            ::HIR::Visitor::visit_crate(crate);

            // Once the crate is complete, add the newly created statics to the module tree
            for(auto& mod_list : m_new_statics)
            {
                auto& mod = *const_cast<HIR::Module*>(mod_list.first);
                
                for(auto& new_static_pair : mod_list.second)
                {
                    mod.m_value_items.insert(std::make_pair( mv$(new_static_pair.first), box$(HIR::VisEnt<HIR::ValueItem> {
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
        void visit_type_impl(::HIR::TypeImpl& impl) override {
            DEBUG("impl " << impl.m_params.fmt_args() << " " << impl.m_type << " (from " << impl.m_src_module << ")");
            const auto& srcmod = m_crate.get_mod_by_path(Span(), impl.m_src_module);
            auto mod_ip = HIR::ItemPath(impl.m_src_module);
            m_current_module = &srcmod;
            m_current_module_path = &mod_ip;

            ::HIR::Visitor::visit_type_impl(impl);

            m_current_module = nullptr;
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override {
            DEBUG("src module " << impl.m_src_module);
            const auto& srcmod = m_crate.get_mod_by_path(Span(), impl.m_src_module);
            auto mod_ip = HIR::ItemPath(impl.m_src_module);
            m_current_module = &srcmod;
            m_current_module_path = &mod_ip;

            ::HIR::Visitor::visit_trait_impl(trait_path, impl);

            m_current_module = nullptr;
        }

        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) override {
            BUG(Span(), "visit_expr hit in OuterVisitor");
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            if( auto* ep = ty.m_data.opt_Array() )
            {
                this->visit_type( *ep->inner );
                DEBUG("Array size " << ty);
                if( ep->size.is_Unevaluated() ) {
                    ExprVisitor_Mutate  ev(m_crate, this->get_new_ty_cb());
                    ev.visit_node_ptr( *ep->size.as_Unevaluated() );
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
                ExprVisitor_Mutate  ev(m_crate, this->get_new_ty_cb());
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
                ExprVisitor_Mutate  ev(m_crate, this->get_new_ty_cb());
                ev.visit_node_ptr(item.m_value);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value )
            {
                ExprVisitor_Mutate  ev(m_crate, this->get_new_ty_cb());
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
                        ExprVisitor_Mutate  ev(m_crate, this->get_new_ty_cb());
                        ev.visit_node_ptr(var.expr);
                    }
                }
            }
        }
    };
}   // namespace

void HIR_Expand_StaticBorrowConstants_Expr(const ::HIR::Crate& crate, ::HIR::ExprPtr& exp)
{
    ExprVisitor_Mutate  ev(crate, [&](Span sp, HIR::TypeRef ty, HIR::Literal val)->HIR::SimplePath {
        // How will this work? Can't easily mutate the crate in this context
        // - 
        TODO(exp.span(), "Create new static in per-expression context");
        });
    ev.visit_node_ptr( exp );
}
void HIR_Expand_StaticBorrowConstants(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}
