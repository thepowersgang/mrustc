/*
 */
#include <hir/expr_ptr.hpp>
#include <hir/expr.hpp>
#include <ast/expr.hpp>
#include <ast/ast.hpp>
#include "from_ast.hpp"

::std::unique_ptr< ::HIR::ExprNode> LowerHIR_ExprNode_Inner(const ::AST::ExprNode& e);
::std::unique_ptr< ::HIR::ExprNode> LowerHIR_ExprNode_Inner_Opt(const ::AST::ExprNode* e) {
    if( e ) {
        return LowerHIR_ExprNode_Inner(*e);
    }
    else {
        return nullptr;
    }
}

struct LowerHIR_ExprNode_Visitor:
    public ::AST::NodeVisitor
{
    ::std::unique_ptr< ::HIR::ExprNode> m_rv;
    
    virtual void visit(::AST::ExprNode_Block& v) override {
        auto rv = new ::HIR::ExprNode_Block();
        for(const auto& n : v.m_nodes)
        {
            if( n ) {
                rv->m_nodes.push_back( LowerHIR_ExprNode_Inner( *n ) );
            }
        }
        m_rv.reset( static_cast< ::HIR::ExprNode*>(rv) );
    }
    virtual void visit(::AST::ExprNode_Macro& v) override {
        BUG(v.get_pos(), "Hit ExprNode_Macro");
    }
    virtual void visit(::AST::ExprNode_Flow& v) override {
        switch( v.m_type )
        {
        case ::AST::ExprNode_Flow::RETURN:
            m_rv.reset( new ::HIR::ExprNode_Return( LowerHIR_ExprNode_Inner_Opt(v.m_value.get()) ) );
            break;
        case ::AST::ExprNode_Flow::CONTINUE:
        case ::AST::ExprNode_Flow::BREAK:
            if( v.m_value )
                TODO(v.get_pos(), "Handle break/continue values in HIR");
            m_rv.reset( new ::HIR::ExprNode_LoopControl( v.m_target, (v.m_type == ::AST::ExprNode_Flow::CONTINUE) ) );
            break;
        }
    }
    virtual void visit(::AST::ExprNode_LetBinding& v) override {
        m_rv.reset( new ::HIR::ExprNode_Let(
            LowerHIR_Pattern( v.m_pat ),
            LowerHIR_Type( v.m_type ),
            LowerHIR_ExprNode_Inner_Opt( v.m_value.get() )
            ) );
    }
    virtual void visit(::AST::ExprNode_Assign& v) override {
        struct H {
            static ::HIR::ExprNode_Assign::Op get_op(::AST::ExprNode_Assign::Operation o) {
                switch(o)
                {
                case ::AST::ExprNode_Assign::NONE:  return ::HIR::ExprNode_Assign::Op::None;
                case ::AST::ExprNode_Assign::ADD:   return ::HIR::ExprNode_Assign::Op::Add;
                case ::AST::ExprNode_Assign::SUB:   return ::HIR::ExprNode_Assign::Op::Sub;
                
                case ::AST::ExprNode_Assign::DIV:   return ::HIR::ExprNode_Assign::Op::Mul;
                case ::AST::ExprNode_Assign::MUL:   return ::HIR::ExprNode_Assign::Op::Div;
                case ::AST::ExprNode_Assign::MOD:   return ::HIR::ExprNode_Assign::Op::Mod;
                
                case ::AST::ExprNode_Assign::AND:   return ::HIR::ExprNode_Assign::Op::And;
                case ::AST::ExprNode_Assign::OR :   return ::HIR::ExprNode_Assign::Op::Or ;
                case ::AST::ExprNode_Assign::XOR:   return ::HIR::ExprNode_Assign::Op::Xor;
                
                case ::AST::ExprNode_Assign::SHR:   return ::HIR::ExprNode_Assign::Op::Shr;
                case ::AST::ExprNode_Assign::SHL:   return ::HIR::ExprNode_Assign::Op::Shl;
                }
                throw "";
            }
        };
        m_rv.reset( new ::HIR::ExprNode_Assign(
            H::get_op(v.m_op),
            LowerHIR_ExprNode_Inner( *v.m_slot ),
            LowerHIR_ExprNode_Inner( *v.m_value )
            ) );
    }
    virtual void visit(::AST::ExprNode_BinOp& v) override {
        ::HIR::ExprNode_BinOp::Op   op;
        switch(v.m_type)
        {
        case ::AST::ExprNode_BinOp::RANGE:
            TODO(v.get_pos(), "Desugar range");
            break;
        case ::AST::ExprNode_BinOp::RANGE_INC:
            TODO(v.get_pos(), "Desugar range");
            break;
        case ::AST::ExprNode_BinOp::PLACE_IN:
            TODO(v.get_pos(), "Desugar placement syntax");
            break;
        
        case ::AST::ExprNode_BinOp::CMPEQU :    op = ::HIR::ExprNode_BinOp::Op::CmpEqu ; if(0)
        case ::AST::ExprNode_BinOp::CMPNEQU:    op = ::HIR::ExprNode_BinOp::Op::CmpNEqu; if(0)
        case ::AST::ExprNode_BinOp::CMPLT : op = ::HIR::ExprNode_BinOp::Op::CmpLt ; if(0)
        case ::AST::ExprNode_BinOp::CMPLTE: op = ::HIR::ExprNode_BinOp::Op::CmpLtE; if(0)
        case ::AST::ExprNode_BinOp::CMPGT : op = ::HIR::ExprNode_BinOp::Op::CmpGt ; if(0)
        case ::AST::ExprNode_BinOp::CMPGTE: op = ::HIR::ExprNode_BinOp::Op::CmpGtE; if(0)
        case ::AST::ExprNode_BinOp::BOOLAND:    op = ::HIR::ExprNode_BinOp::Op::BoolAnd; if(0)
        case ::AST::ExprNode_BinOp::BOOLOR :    op = ::HIR::ExprNode_BinOp::Op::BoolOr ; if(0)
        
        case ::AST::ExprNode_BinOp::BITAND: op = ::HIR::ExprNode_BinOp::Op::And; if(0)
        case ::AST::ExprNode_BinOp::BITOR : op = ::HIR::ExprNode_BinOp::Op::Or ; if(0)
        case ::AST::ExprNode_BinOp::BITXOR: op = ::HIR::ExprNode_BinOp::Op::Xor; if(0)
        case ::AST::ExprNode_BinOp::MULTIPLY:   op = ::HIR::ExprNode_BinOp::Op::Mul; if(0)
        case ::AST::ExprNode_BinOp::DIVIDE  :   op = ::HIR::ExprNode_BinOp::Op::Div; if(0)
        case ::AST::ExprNode_BinOp::MODULO  :   op = ::HIR::ExprNode_BinOp::Op::Mod; if(0)
        case ::AST::ExprNode_BinOp::ADD:    op = ::HIR::ExprNode_BinOp::Op::Add; if(0)
        case ::AST::ExprNode_BinOp::SUB:    op = ::HIR::ExprNode_BinOp::Op::Sub; if(0)
        case ::AST::ExprNode_BinOp::SHR:    op = ::HIR::ExprNode_BinOp::Op::Shr; if(0)
        case ::AST::ExprNode_BinOp::SHL:    op = ::HIR::ExprNode_BinOp::Op::Shl;
            
            m_rv.reset( new ::HIR::ExprNode_BinOp(
                op,
                LowerHIR_ExprNode_Inner( *v.m_left ),
                LowerHIR_ExprNode_Inner( *v.m_right )
                ) );
            break;
        }
    }
    virtual void visit(::AST::ExprNode_UniOp& v) override {
        ::HIR::ExprNode_UniOp::Op   op;
        switch(v.m_type)
        {
        case ::AST::ExprNode_UniOp::BOX:
            TODO(v.get_pos(), "Desugar box");
            break;
        case ::AST::ExprNode_UniOp::QMARK:
            TODO(v.get_pos(), "Desugar question mark operator");
            break;
        
        case ::AST::ExprNode_UniOp::REF:    op = ::HIR::ExprNode_UniOp::Op::Ref   ; if(0)
        case ::AST::ExprNode_UniOp::REFMUT: op = ::HIR::ExprNode_UniOp::Op::RefMut; if(0)
        case ::AST::ExprNode_UniOp::INVERT: op = ::HIR::ExprNode_UniOp::Op::Invert; if(0)
        case ::AST::ExprNode_UniOp::NEGATE: op = ::HIR::ExprNode_UniOp::Op::Negate;
            m_rv.reset( new ::HIR::ExprNode_UniOp(
                op,
                LowerHIR_ExprNode_Inner( *v.m_value )
                ) );
            break;
        }
    }
    virtual void visit(::AST::ExprNode_Cast & v) override {
        m_rv.reset( new ::HIR::ExprNode_Cast(
            LowerHIR_ExprNode_Inner( *v.m_value ),
            LowerHIR_Type(v.m_type)
            ) );
    }
    
    virtual void visit(::AST::ExprNode_CallPath& v) override {
        ::std::vector< ::HIR::ExprNodeP> args;
        for(const auto& arg : v.m_args)
            args.push_back( LowerHIR_ExprNode_Inner(*arg) );
        m_rv.reset( new ::HIR::ExprNode_CallPath(
            LowerHIR_Path(v.m_path),
            mv$( args )
            ) );
    }
    virtual void visit(::AST::ExprNode_CallMethod& v) override {
    }
    virtual void visit(::AST::ExprNode_CallObject& v) override {
    }
    virtual void visit(::AST::ExprNode_Loop& v) override {
    }
    virtual void visit(::AST::ExprNode_Match& v) override {
    }
    virtual void visit(::AST::ExprNode_If& v) override {
    }
    virtual void visit(::AST::ExprNode_IfLet& v) override {
    }
    
    virtual void visit(::AST::ExprNode_Integer& v) override {
        struct H {
            static ::HIR::CoreType get_type(::eCoreType ct) {
                switch(ct)
                {
                case CORETYPE_I8 :  return ::HIR::CoreType::I8;
                case CORETYPE_U8 :  return ::HIR::CoreType::U8;
                case CORETYPE_I16:  return ::HIR::CoreType::I16;
                case CORETYPE_U16:  return ::HIR::CoreType::U16;
                case CORETYPE_I32:  return ::HIR::CoreType::I32;
                case CORETYPE_U32:  return ::HIR::CoreType::U32;
                case CORETYPE_I64:  return ::HIR::CoreType::I64;
                case CORETYPE_U64:  return ::HIR::CoreType::U64;

                case CORETYPE_INT:  return ::HIR::CoreType::Isize;
                case CORETYPE_UINT: return ::HIR::CoreType::Usize;
                default:
                    return ::HIR::CoreType::Str;
                }
            }
        };
        m_rv.reset( new ::HIR::ExprNode_Literal(
            ::HIR::ExprNode_Literal::Data::make_Integer({
                H::get_type( v.m_datatype ),
                v.m_value
                })
            ) );
    }
    virtual void visit(::AST::ExprNode_Float& v) override {
    }
    virtual void visit(::AST::ExprNode_Bool& v) override {
    }
    virtual void visit(::AST::ExprNode_String& v) override {
    }
    virtual void visit(::AST::ExprNode_Closure& v) override {
    }
    virtual void visit(::AST::ExprNode_StructLiteral& v) override {
    }
    virtual void visit(::AST::ExprNode_Array& v) override {
    }
    virtual void visit(::AST::ExprNode_Tuple& v) override {
    }
    virtual void visit(::AST::ExprNode_NamedValue& v) override {
    }
    
    virtual void visit(::AST::ExprNode_Field& v) override {
    }
    virtual void visit(::AST::ExprNode_Index& v) override {
    }
    virtual void visit(::AST::ExprNode_Deref& v) override {
    }
};

::std::unique_ptr< ::HIR::ExprNode> LowerHIR_ExprNode_Inner(const ::AST::ExprNode& e)
{
    LowerHIR_ExprNode_Visitor v;
    
    const_cast<::AST::ExprNode*>(&e)->visit( v );
    
    if( ! v.m_rv ) {
        BUG(e.get_pos(), typeid(e).name() << " - Yielded a nullptr HIR node");
    }
    return mv$( v.m_rv );
}

::HIR::ExprPtr LowerHIR_ExprNode(const ::AST::ExprNode& e)
{
    return ::HIR::ExprPtr( LowerHIR_ExprNode_Inner(e) );
}
