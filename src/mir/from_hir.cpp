/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/from_hir.hpp
 * - Construction of MIR from the HIR expression tree
 */
#include "mir.hpp"
#include "mir_ptr.hpp"
#include <hir/expr.hpp>

namespace {
    class ExprVisitor_Conv:
        public ::HIR::ExprVisitor
    {
        ::MIR::Function&    m_output;
        
        unsigned int    m_result_tmp_idx;
        
    public:
        ExprVisitor_Conv(::MIR::Function& output):
            m_output(output)
        {}
        
        
        void visit(::HIR::ExprNode_Block& node) override
        {
            // Creates a BB, all expressions end up as part of it (with all but the final expression having their results dropped)
        }
    };
}


::MIR::FunctionPointer LowerMIR(const ::HIR::ExprPtr& ptr, const ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >& args)
{
    ::MIR::Function fcn;
    
    // 1. Apply destructuring to arguments
    // 2. Destructure code
    
    return ::MIR::FunctionPointer();
}

