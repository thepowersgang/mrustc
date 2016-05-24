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
            else {
                rv->m_nodes.push_back( ::HIR::ExprNodeP( new ::HIR::ExprNode_Tuple({}) ) );
            }
        }
        
        if( v.m_local_mod )
        {
            // TODO: Populate m_traits from the local module's import list
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
            if( v.m_value )
                m_rv.reset( new ::HIR::ExprNode_Return( LowerHIR_ExprNode_Inner(*v.m_value) ) );
            else
                m_rv.reset( new ::HIR::ExprNode_Return( ::HIR::ExprNodeP(new ::HIR::ExprNode_Tuple({})) ) );
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
        case ::AST::ExprNode_BinOp::RANGE: {
            // NOTE: Not language items
            auto path_Range     = ::HIR::GenericPath( ::HIR::SimplePath("", {"ops", "Range"}) );
            auto path_RangeFrom = ::HIR::GenericPath( ::HIR::SimplePath("", {"ops", "RangeFrom"}) );
            auto path_RangeTo   = ::HIR::GenericPath( ::HIR::SimplePath("", {"ops", "RangeTo"}) );
            auto path_RangeFull = ::HIR::GenericPath( ::HIR::SimplePath("", {"ops", "RangeFull"}) );
            
            ::HIR::ExprNode_StructLiteral::t_values values;
            if( v.m_left )
                values.push_back( ::std::make_pair( ::std::string("start"), LowerHIR_ExprNode_Inner( *v.m_left ) ) );
            if( v.m_right )
                values.push_back( ::std::make_pair( ::std::string("end")  , LowerHIR_ExprNode_Inner( *v.m_right ) ) );
            
            if( v.m_left ) {
                if( v.m_right ) {
                    m_rv.reset( new ::HIR::ExprNode_StructLiteral(mv$(path_Range), nullptr, mv$(values)) );
                }
                else {
                    m_rv.reset( new ::HIR::ExprNode_StructLiteral(mv$(path_RangeFrom), nullptr, mv$(values)) );
                }
            }
            else {
                if( v.m_right ) {
                    m_rv.reset( new ::HIR::ExprNode_StructLiteral(mv$(path_RangeTo), nullptr, mv$(values)) );
                }
                else {
                    m_rv.reset( new ::HIR::ExprNode_PathValue(mv$(path_RangeFull)) );
                }
            }
            break; }
        case ::AST::ExprNode_BinOp::RANGE_INC: {
            // NOTE: Not language items
            auto path_RangeInclusive_NonEmpty = ::HIR::GenericPath( ::HIR::SimplePath("", {"ops", "RangeInclusive", "NonEmpty"}) );
            auto path_RangeToInclusive        = ::HIR::GenericPath( ::HIR::SimplePath("", {"ops", "RangeToInclusive"}) );
            
            if( v.m_left )
            {
                ::HIR::ExprNode_StructLiteral::t_values values;
                values.push_back( ::std::make_pair( ::std::string("start"), LowerHIR_ExprNode_Inner( *v.m_left ) ) );
                values.push_back( ::std::make_pair( ::std::string("end")  , LowerHIR_ExprNode_Inner( *v.m_right ) ) );
                m_rv.reset( new ::HIR::ExprNode_StructLiteral(mv$(path_RangeInclusive_NonEmpty), nullptr, mv$(values)) );
            }
            else
            {
                ::HIR::ExprNode_StructLiteral::t_values values;
                values.push_back( ::std::make_pair( ::std::string("end")  , LowerHIR_ExprNode_Inner( *v.m_right ) ) );
                m_rv.reset( new ::HIR::ExprNode_StructLiteral(mv$(path_RangeToInclusive), nullptr, mv$(values)) );
            }
            break; }
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
            // NOTE: This operator doesn't use language items, ergo it's a basic desugar and is done in expand
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
        
        TU_IFLET(::AST::Path::Class, v.m_path.m_class, Local, e,
            m_rv.reset( new ::HIR::ExprNode_CallValue(
                ::HIR::ExprNodeP(new ::HIR::ExprNode_Variable( e.name, v.m_path.binding().as_Variable().slot )),
                mv$(args)
                ) );
        )
        else
        {
            m_rv.reset( new ::HIR::ExprNode_CallPath(
                LowerHIR_Path(Span(v.get_pos()), v.m_path),
                mv$( args )
                ) );
        }
    }
    virtual void visit(::AST::ExprNode_CallMethod& v) override {
        ::std::vector< ::HIR::ExprNodeP> args;
        for(const auto& arg : v.m_args)
            args.push_back( LowerHIR_ExprNode_Inner(*arg) );
        
        // TODO: Should this be abstracted?
        ::HIR::PathParams   params;
        for(const auto& param : v.m_method.args())
            params.m_types.push_back( LowerHIR_Type(param) );
        
        m_rv.reset( new ::HIR::ExprNode_CallMethod(
            LowerHIR_ExprNode_Inner(*v.m_val),
            v.m_method.name(),
            mv$(params),
            mv$(args)
            ) );
    }
    virtual void visit(::AST::ExprNode_CallObject& v) override {
        ::std::vector< ::HIR::ExprNodeP> args;
        for(const auto& arg : v.m_args)
            args.push_back( LowerHIR_ExprNode_Inner(*arg) );
        
        m_rv.reset( new ::HIR::ExprNode_CallValue(
            LowerHIR_ExprNode_Inner(*v.m_val),
            mv$(args)
            ) );
    }
    virtual void visit(::AST::ExprNode_Loop& v) override {
        switch( v.m_type )
        {
        case ::AST::ExprNode_Loop::LOOP:
            m_rv.reset( new ::HIR::ExprNode_Loop(
                v.m_label,
                LowerHIR_ExprNode_Inner(*v.m_code)
                ) );
            break;
        case ::AST::ExprNode_Loop::WHILE: {
            ::std::vector< ::HIR::ExprNodeP>    code;
            // - if `m_cond` { () } else { break `m_label` }
            code.push_back( ::HIR::ExprNodeP(new ::HIR::ExprNode_If(
                LowerHIR_ExprNode_Inner(*v.m_cond),
                ::HIR::ExprNodeP( new ::HIR::ExprNode_Tuple({}) ),
                ::HIR::ExprNodeP( new ::HIR::ExprNode_LoopControl(v.m_label, false) )
                )) );
            code.push_back( LowerHIR_ExprNode_Inner(*v.m_code) );
            
            m_rv.reset( new ::HIR::ExprNode_Loop(
                v.m_label,
                ::HIR::ExprNodeP(new ::HIR::ExprNode_Block(false, mv$(code)))
                ) );
            break; }
        case ::AST::ExprNode_Loop::WHILELET: {
            ::std::vector< ::HIR::ExprNode_Match::Arm>  arms;
            
            // - Matches pattern - Run inner code
            arms.push_back(::HIR::ExprNode_Match::Arm {
                ::make_vec1( LowerHIR_Pattern(v.m_pattern) ),
                ::HIR::ExprNodeP(),
                LowerHIR_ExprNode_Inner(*v.m_code)
                });
            // - Matches anything else - break
            arms.push_back(::HIR::ExprNode_Match::Arm {
                ::make_vec1( ::HIR::Pattern() ),
                ::HIR::ExprNodeP(),
                ::HIR::ExprNodeP( new ::HIR::ExprNode_LoopControl(v.m_label, false) )
                });
            
            m_rv.reset( new ::HIR::ExprNode_Loop(
                v.m_label,
                ::HIR::ExprNodeP(new ::HIR::ExprNode_Match(
                    LowerHIR_ExprNode_Inner(*v.m_cond),
                    mv$(arms)
                    ))
                ) );
            break; }
        case ::AST::ExprNode_Loop::FOR:
            // NOTE: This should already be desugared (as a pass before resolve)
            BUG(v.get_pos(), "Encountered still-sugared for loop");
            break;
        }
    }
    virtual void visit(::AST::ExprNode_Match& v) override {
        ::std::vector< ::HIR::ExprNode_Match::Arm>  arms;
        
        for(const auto& arm : v.m_arms)
        {
            ::HIR::ExprNode_Match::Arm  new_arm {
                {},
                LowerHIR_ExprNode_Inner_Opt(arm.m_cond.get()),
                LowerHIR_ExprNode_Inner(*arm.m_code)
                };
            
            for(const auto& pat : arm.m_patterns)
                new_arm.m_patterns.push_back( LowerHIR_Pattern(pat) );
        
            arms.push_back( mv$(new_arm) );
        }
        
        m_rv.reset( new ::HIR::ExprNode_Match(
            LowerHIR_ExprNode_Inner(*v.m_val),
            mv$(arms)
            ));
    }
    virtual void visit(::AST::ExprNode_If& v) override {
        m_rv.reset( new ::HIR::ExprNode_If(
            LowerHIR_ExprNode_Inner(*v.m_cond),
            LowerHIR_ExprNode_Inner(*v.m_true),
            LowerHIR_ExprNode_Inner_Opt(&*v.m_false)
            ));
    }
    virtual void visit(::AST::ExprNode_IfLet& v) override {
        ::std::vector< ::HIR::ExprNode_Match::Arm>  arms;
        
        // - Matches pattern - Take true branch
        arms.push_back(::HIR::ExprNode_Match::Arm {
            ::make_vec1( LowerHIR_Pattern(v.m_pattern) ),
            ::HIR::ExprNodeP(),
            LowerHIR_ExprNode_Inner(*v.m_true)
            });
        // - Matches anything else - take false branch
        arms.push_back(::HIR::ExprNode_Match::Arm {
            ::make_vec1( ::HIR::Pattern() ),
            ::HIR::ExprNodeP(),
            v.m_false ? LowerHIR_ExprNode_Inner(*v.m_false) : ::HIR::ExprNodeP( new ::HIR::ExprNode_Tuple({}) )
            });
        m_rv.reset( new ::HIR::ExprNode_Match(
            LowerHIR_ExprNode_Inner(*v.m_value),
            mv$(arms)
            ));
    }
    
    virtual void visit(::AST::ExprNode_Integer& v) override {
        struct H {
            static ::HIR::CoreType get_type(Span sp, ::eCoreType ct) {
                switch(ct)
                {
                case CORETYPE_ANY:  return ::HIR::CoreType::Str;

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
                
                case CORETYPE_CHAR: return ::HIR::CoreType::Char;
                
                default:
                    BUG(sp, "Unknown type for integer literal - " << ct);
                }
            }
        };
        m_rv.reset( new ::HIR::ExprNode_Literal(
            ::HIR::ExprNode_Literal::Data::make_Integer({
                H::get_type( Span(v.get_pos()), v.m_datatype ),
                v.m_value
                })
            ) );
    }
    virtual void visit(::AST::ExprNode_Float& v) override {
        ::HIR::CoreType ct;
        switch(v.m_datatype)
        {
        case CORETYPE_ANY:  ct = ::HIR::CoreType::Str;  break;
        case CORETYPE_F32:  ct = ::HIR::CoreType::F32;  break;
        case CORETYPE_F64:  ct = ::HIR::CoreType::F64;  break;
        default:
            BUG(v.get_pos(), "Unknown type for float literal");
        }
        m_rv.reset( new ::HIR::ExprNode_Literal( ::HIR::ExprNode_Literal::Data::make_Float({
            ct, v.m_value
            }) ) );
    }
    virtual void visit(::AST::ExprNode_Bool& v) override {
        m_rv.reset( new ::HIR::ExprNode_Literal( ::HIR::ExprNode_Literal::Data::make_Boolean( v.m_value ) ) );
    }
    virtual void visit(::AST::ExprNode_String& v) override {
        m_rv.reset( new ::HIR::ExprNode_Literal( ::HIR::ExprNode_Literal::Data::make_String( v.m_value ) ) );
    }
    virtual void visit(::AST::ExprNode_Closure& v) override {
        ::HIR::ExprNode_Closure::args_t args;
        for(const auto& arg : v.m_args) {
            args.push_back( ::std::make_pair(
                LowerHIR_Pattern( arg.first ),
                LowerHIR_Type( arg.second )
                ) );
        }
        m_rv.reset( new ::HIR::ExprNode_Closure(
            mv$(args),
            LowerHIR_Type(v.m_return),
            LowerHIR_ExprNode_Inner(*v.m_code)
            ) );
    }
    virtual void visit(::AST::ExprNode_StructLiteral& v) override {
        ::HIR::ExprNode_StructLiteral::t_values values;
        for(const auto& val : v.m_values)
            values.push_back( ::std::make_pair(val.first, LowerHIR_ExprNode_Inner(*val.second)) );
        m_rv.reset( new ::HIR::ExprNode_StructLiteral(
            LowerHIR_GenericPath(v.get_pos(), v.m_path),
            LowerHIR_ExprNode_Inner_Opt(v.m_base_value.get()),
            mv$(values)
            ) );
    }
    virtual void visit(::AST::ExprNode_Array& v) override {
        if( v.m_size )
        {
            m_rv.reset( new ::HIR::ExprNode_ArraySized(
                LowerHIR_ExprNode_Inner( *v.m_values.at(0) ),
                // TODO: Should this size be a full expression on its own?
                LowerHIR_ExprNode_Inner( *v.m_size )
                ) );
        }
        else
        {
            ::std::vector< ::HIR::ExprNodeP>    vals;
            for(const auto& val : v.m_values)
                vals.push_back( LowerHIR_ExprNode_Inner(*val) );
            m_rv.reset( new ::HIR::ExprNode_ArrayList( mv$(vals) ) );
        }
    }
    virtual void visit(::AST::ExprNode_Tuple& v) override {
        ::std::vector< ::HIR::ExprNodeP>    vals;
        for(const auto& val : v.m_values)
            vals.push_back( LowerHIR_ExprNode_Inner(*val ) );
        m_rv.reset( new ::HIR::ExprNode_Tuple( mv$(vals) ) );
    }
    virtual void visit(::AST::ExprNode_NamedValue& v) override {
        TU_IFLET(::AST::Path::Class, v.m_path.m_class, Local, e,
            if( !v.m_path.binding().is_Variable() ) {
                BUG(v.get_pos(), "Named value was a local, but wasn't bound - " << v.m_path);
            }
            auto slot = v.m_path.binding().as_Variable().slot;
            m_rv.reset( new ::HIR::ExprNode_Variable( e.name, slot ) );
        )
        else {
            m_rv.reset( new ::HIR::ExprNode_PathValue( LowerHIR_Path(Span(v.get_pos()), v.m_path) ) );
        }
    }
    
    virtual void visit(::AST::ExprNode_Field& v) override {
        m_rv.reset( new ::HIR::ExprNode_Field(
            LowerHIR_ExprNode_Inner(*v.m_obj),
            v.m_name
            ));
    }
    virtual void visit(::AST::ExprNode_Index& v) override {
        m_rv.reset( new ::HIR::ExprNode_Index(
            LowerHIR_ExprNode_Inner(*v.m_obj),
            LowerHIR_ExprNode_Inner(*v.m_idx)
            ));
    }
    virtual void visit(::AST::ExprNode_Deref& v) override {
        m_rv.reset( new ::HIR::ExprNode_Deref(
            LowerHIR_ExprNode_Inner(*v.m_value)
            ));
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
