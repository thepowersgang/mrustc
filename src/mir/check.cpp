/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/check.cpp
 * - MIR Correctness validation
 */
#include "main_bindings.hpp"
#include "mir.hpp"
#include <hir/visitor.hpp>
#include <hir_typeck/static.hpp>

void MIR_Validate(const StaticTraitResolve& resolve, const ::MIR::Function& fcn, const ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >& args)
{
    Span    sp;
    // Validation rules:
    // - [CFA] All code paths from bb0 must end with either a return or a diverge (or loop)
    //  - Requires checking the links between basic blocks, with a bitmap to catch loops/multipath
    {
        bool returns = false;
        ::std::vector<bool> visited_bbs( fcn.blocks.size() );
        ::std::vector<unsigned int> to_visit_blocks;
        to_visit_blocks.push_back(0);
        while( to_visit_blocks.size() > 0 )
        {
            auto block = to_visit_blocks.back();
            to_visit_blocks.pop_back();
            assert(block < fcn.blocks.size());
            if( visited_bbs[block] ) {
                continue ;
            }
            visited_bbs[block] = true;
            
            #define PUSH_BB(idx, desc)  do {\
                ASSERT_BUG(sp, idx < fcn.blocks.size(), "Invalid target block on bb" << block << " - " << desc << " bb" << idx);\
                if( visited_bbs[idx] == false ) {\
                    to_visit_blocks.push_back(idx); \
                }\
                } while(0)
            TU_MATCH(::MIR::Terminator, (fcn.blocks[block].terminator), (e),
            (Incomplete,
                BUG(sp, "Encounterd `Incomplete` block in control flow - bb" << block);
                ),
            (Return,
                returns = true;
                ),
            (Diverge,
                //can_panic = true;
                ),
            (Goto,
                PUSH_BB(e, "Goto");
                ),
            (Panic,
                PUSH_BB(e.dst, "Panic");
                ),
            (If,
                PUSH_BB(e.bb0, "If true");
                PUSH_BB(e.bb1, "If false");
                ),
            (Switch,
                for(unsigned int i = 0; i < e.targets.size(); i++ ) {
                    PUSH_BB(e.targets[i], "Switch V" << i);
                }
                ),
            (Call,
                PUSH_BB(e.ret_block, "Call ret");
                PUSH_BB(e.panic_block, "Call panic");
                )
            )
            #undef PUSH_BB
        }
        if( !returns ) {
            DEBUG("- Function doesn't return.");
        }
    }
    
    // - [ValState] No drops or usage of uninitalised values (Uninit, Moved, or Dropped)
    // - [ValState] Temporaries are write-once.
    //  - Requires maintaining state information for all variables/temporaries with support for loops
    // - [Flat] Types must be valid (correct type for slot etc.)
    //  - Simple check of all assignments/calls/...
}

namespace {
    // TODO: Create visitor that handles setting up a StaticTraitResolve?
    class OuterVisitor:
        public ::HIR::Visitor
    {
        StaticTraitResolve  m_resolve;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_resolve(crate)
        {}
        
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
                    MIR_Validate(m_resolve, *e.size.m_mir, {});
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
            if( item.m_code ) {
                DEBUG("Function code " << p);
                MIR_Validate(m_resolve, *item.m_code.m_mir, item.m_args);
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override {
            if( item.m_value ) {
                DEBUG("`static` value " << p);
                MIR_Validate(m_resolve, *item.m_value.m_mir, {});
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value ) {
                DEBUG("`const` value " << p);
                MIR_Validate(m_resolve, *item.m_value.m_mir, {});
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            auto _ = this->m_resolve.set_item_generics(item.m_params);
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    MIR_Validate(m_resolve, *e.expr.m_mir, {});
                )
            }
        }
        
        // Boilerplate
        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override {
            auto _ = this->m_resolve.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
        }
        void visit_type_impl(::HIR::TypeImpl& impl) override {
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            ::HIR::Visitor::visit_type_impl(impl);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override {
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        }
    };
}

// --------------------------------------------------------------------

void MIR_CheckCrate(/*const*/ ::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}
