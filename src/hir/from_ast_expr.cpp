/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/from_ast_expr.cpp
 * - Constructs a HIR expression tree from an AST expression tree
 */
#include <hir/expr_ptr.hpp>
#include <hir/expr.hpp>
#include <ast/expr.hpp>
#include <ast/ast.hpp>
#include "from_ast.hpp"

struct LowerHIR_ExprNode_Visitor:
    public ::AST::NodeVisitor
{
    ::std::unique_ptr< ::HIR::ExprNode> m_rv;

    // Used to track if a closure is a generator or a normal closure
    // - They have different HIR node types
    bool    m_has_yield = false;

    ::std::unique_ptr< ::HIR::ExprNode> lower(::AST::ExprNodeP& ep) {
        assert(ep);
        ep->visit(*this);
        ASSERT_BUG(ep->span(), m_rv, ep.type_name() << " - Yielded a nullptr HIR node");
        return std::move(m_rv);
    }
    ::std::unique_ptr< ::HIR::ExprNode> lower_opt(::AST::ExprNodeP& ep) {
        if( ep ) {
            return lower(ep);
        }
        else {
            return nullptr;
        }
    }


    virtual void visit(::AST::ExprNode_Block& v) override {
        auto rv = new ::HIR::ExprNode_Block(v.span());
        for(auto& n : v.m_nodes)
        {
            ASSERT_BUG(v.span(), n, "NULL node encountered in block");
            rv->m_nodes.push_back( lower( n ) );
        }
        if( v.m_yields_final_value && ! rv->m_nodes.empty() )
        {
            rv->m_value_node = mv$(rv->m_nodes.back());
            rv->m_nodes.pop_back();
        }

        if( v.m_local_mod )
        {
            // TODO: Populate m_traits from the local module's import list
            rv->m_local_mod = ::HIR::SimplePath(g_crate_name, v.m_local_mod->path().nodes);
        }

        switch(v.m_block_type)
        {
        case AST::ExprNode_Block::Type::Bare:
            break;
        case AST::ExprNode_Block::Type::Unsafe:
            rv->m_is_unsafe = true;
            break;
        case AST::ExprNode_Block::Type::Const:
            break;
        }

        if( v.m_label != "" )
        {
            if(rv->m_value_node)
            {
                // TODO: Hygine (resolve should turn loop labels into loop indexes)
                auto* break_node = new ::HIR::ExprNode_LoopControl(v.span(), v.m_label.name, /*cont=*/false, ::std::move(rv->m_value_node));
                rv->m_nodes.push_back(HIR::ExprNodeP(break_node));
                rv->m_value_node.reset();
            }
            auto* loop = new ::HIR::ExprNode_Loop(v.span(), v.m_label.name, HIR::ExprNodeP(rv));
            loop->m_require_label = true;
            m_rv.reset(loop);
        }
        else
        {
            m_rv.reset( static_cast< ::HIR::ExprNode*>(rv) );
        }

        switch(v.m_block_type)
        {
        case AST::ExprNode_Block::Type::Bare:
            break;
        case AST::ExprNode_Block::Type::Unsafe:
            break;
        case AST::ExprNode_Block::Type::Const:
            m_rv.reset( new ::HIR::ExprNode_ConstBlock(v.span(), std::move(m_rv)) );
            break;
        }
    }
    virtual void visit(::AST::ExprNode_Try& v) override {
        TODO(v.span(), "Handle _Try");
    }
    virtual void visit(::AST::ExprNode_Macro& v) override {
        BUG(v.span(), "Hit ExprNode_Macro");
    }
    virtual void visit(::AST::ExprNode_Asm& v) override {
        ::std::vector< ::HIR::ExprNode_Asm::ValRef> outputs;
        ::std::vector< ::HIR::ExprNode_Asm::ValRef> inputs;
        for(auto& vr : v.m_output)
            outputs.push_back( ::HIR::ExprNode_Asm::ValRef { vr.name, lower(vr.value) } );
        for(auto& vr : v.m_input)
            inputs.push_back( ::HIR::ExprNode_Asm::ValRef { vr.name, lower(vr.value) } );

        m_rv.reset( new ::HIR::ExprNode_Asm( v.span(), v.m_text, mv$(outputs), mv$(inputs), v.m_clobbers, v.m_flags ) );
    }
    virtual void visit(::AST::ExprNode_Asm2& v) override {
        std::vector< ::HIR::ExprNode_Asm2::Param>  params;
        for(auto& p : v.m_params)
        {
            TU_MATCH_HDRA((p), {)
            TU_ARMA(Const, e) {
                ASSERT_BUG(v.span(), e, "Missing node for ASM Const");
                params.push_back( lower(e) );
                }
            TU_ARMA(Sym, e) {
                params.push_back( LowerHIR_Path(v.span(), e, FromAST_PathClass::Value) );
                }
            TU_ARMA(RegSingle, e) {
                params.push_back(::HIR::ExprNode_Asm2::Param::make_RegSingle({
                    e.dir,
                    e.spec.clone(),
                    e.val ? lower(e.val) : nullptr  // e.g. `lateout(regname) _`
                    }));
                }
            TU_ARMA(Reg, e) {
                params.push_back(::HIR::ExprNode_Asm2::Param::make_Reg({
                    e.dir,
                    e.spec.clone(),
                    e.val_in ? lower(e.val_in) : nullptr,
                    e.val_out ? lower(e.val_out) : nullptr
                    }));
                }
            }
        }
        m_rv.reset( new ::HIR::ExprNode_Asm2( v.span(), v.m_options, v.m_lines, mv$(params) ) );
    }
    virtual void visit(::AST::ExprNode_Flow& v) override {
        switch( v.m_type )
        {
        case ::AST::ExprNode_Flow::RETURN:
            if( v.m_value )
                m_rv.reset( new ::HIR::ExprNode_Return( v.span(), lower(v.m_value) ) );
            else
                m_rv.reset( new ::HIR::ExprNode_Return( v.span(), ::HIR::ExprNodeP(new ::HIR::ExprNode_Tuple(v.span(), {})) ) );
            break;
        case ::AST::ExprNode_Flow::YIELD:
            m_has_yield = true;
            m_rv.reset( new ::HIR::ExprNode_Yield( v.span(), lower(v.m_value) ) );
            break;
        case ::AST::ExprNode_Flow::CONTINUE:
        case ::AST::ExprNode_Flow::BREAK: {
            auto val = v.m_value ? lower(v.m_value) : ::HIR::ExprNodeP();
            ASSERT_BUG(v.span(), !(v.m_type == ::AST::ExprNode_Flow::CONTINUE && val), "Continue with a value isn't allowed");
            m_rv.reset( new ::HIR::ExprNode_LoopControl( v.span(), v.m_target.name, (v.m_type == ::AST::ExprNode_Flow::CONTINUE), mv$(val) ) );
            } break;
        case ::AST::ExprNode_Flow::YEET:
            BUG(v.span(), "do yeet should have been desugared");
            break;
        }
    }
    virtual void visit(::AST::ExprNode_LetBinding& v) override {
        if( v.m_else ) {
            // Cannot be expanded in expand, as it needs `None` to have been resolved to the enum variant
            // So, it's expanded here - with the cooperation of `Resolve_Absolute` allocating some variable bindings for us
            auto pat = LowerHIR_Pattern( v.m_pat );
            auto type = LowerHIR_Type( v.m_type );
            auto node_value = lower(v.m_value);
            auto node_else = lower(v.m_else);

            auto base = v.m_letelse_slots.first;
            auto count = v.m_letelse_slots.second;
            DEBUG(pat);
            struct V: public HIR::Visitor {
                unsigned base;
                unsigned count;
                std::vector<HIR::PatternBinding>    bindings;
                std::map<unsigned, unsigned>    mapping;
                V(unsigned base, unsigned count)
                    : base(base)
                    , count(count)
                {}
                void visit_pattern(::HIR::Pattern& pat) override {
                    HIR::Visitor::visit_pattern(pat);
                    for(size_t i = 0; i < pat.m_bindings.size(); i ++) {
                        this->handle_binding(pat.m_bindings[i]);
                    }
                    // SplitSlice also defines bindings
                    if(auto* e = pat.m_data.opt_SplitSlice() ) {
                        if( e->extra_bind.is_valid() ) {
                            this->handle_binding(e->extra_bind);
                        }
                    }
                    // - SplitTuple doesn't?
                    //if(auto* e = pat.m_data.opt_SplitTuple() ) {
                    //    if( e->extra_bind.is_valid() ) {
                    //        this->handle_binding(e->extra_bind);
                    //    }
                    //}
                }
                void handle_binding(::HIR::PatternBinding& pb) {
                    auto it = mapping.find(pb.m_slot);
                    if( it == mapping.end() ) {
                        ASSERT_BUG(Span(), bindings.size() < this->count, "Miscount of variables in `let-else` - only allocated " << this->count);
                        unsigned new_idx = base + bindings.size();

                        bindings.push_back( HIR::PatternBinding(pb) );
                        bindings.back().m_type = HIR::PatternBinding::Type::Move;
                        it = mapping.insert(std::make_pair(pb.m_slot, new_idx)).first;
                    }
                    pb.m_mutable = false;
                    pb.m_slot = it->second;
                }
            } visitor(base, count);
            visitor.visit_pattern(pat);
            /* 
             * ```
             * let (a,b,c,...) = match $value: $ty {
             *     $pat => (a,b,c,...),
             *     _ => { let _: ! = $else; },
             *     };
             * ```
             */
            std::vector<HIR::Pattern>   new_pats;
            std::vector<HIR::ExprNodeP> tuple_vals;
            for(size_t i = 0; i < visitor.bindings.size(); i++) {
                auto& binding = visitor.bindings[i];
                tuple_vals.push_back(HIR::ExprNodeP( new HIR::ExprNode_Variable(v.span(), binding.m_name, base + i) ));
                new_pats.push_back(HIR::Pattern(std::move(binding), HIR::Pattern::Data {}));
            }

            std::vector<HIR::ExprNode_Match::Arm>   match_arms(2);
            // `$pat => (a,b,c,...),`
            match_arms[0].m_patterns.push_back(std::move(pat));
            match_arms[0].m_code.reset(new HIR::ExprNode_Tuple(v.span(), std::move(tuple_vals)));
            match_arms[1].m_patterns.push_back(HIR::Pattern());
            // `_ => loop { let _: ! = $else; },
            match_arms[1].m_code.reset(new HIR::ExprNode_Let(v.span(), HIR::Pattern(), HIR::TypeRef::new_diverge(), std::move(node_else)));
            match_arms[1].m_code.reset(new HIR::ExprNode_Loop(v.span(), "",  std::move(match_arms[1].m_code), /*require_label*/true));
            // HACK: Just use the code as-is.
            //match_arms[1].m_code = std::move(node_else);
            // `match $value: $ty {`
            auto match_value = type.data().is_Infer()   // Only emit the `: $ty` part if the type was specified (not a `_`)
                ? std::move(node_value)
                : HIR::ExprNodeP(new HIR::ExprNode_Unsize(v.span(), std::move(node_value), std::move(type)))
                ;
            auto match = HIR::ExprNodeP(new HIR::ExprNode_Match(v.span(), std::move(match_value), std::move(match_arms)));

            // `let (a,b,c,...) = ...`
            m_rv.reset( new ::HIR::ExprNode_Let( v.span(),
                HIR::Pattern(::std::vector<HIR::PatternBinding>(), HIR::Pattern::Data::make_Tuple({ std::move(new_pats) })),
                HIR::TypeRef(),
                std::move(match)
            ));
        }
        else {
            m_rv.reset( new ::HIR::ExprNode_Let( v.span(),
                LowerHIR_Pattern( v.m_pat ),
                LowerHIR_Type( v.m_type ),
                lower_opt( v.m_value )
                ) );
        }
    }
    virtual void visit(::AST::ExprNode_Assign& v) override {
        struct H {
            static ::HIR::ExprNode_Assign::Op get_op(::AST::ExprNode_Assign::Operation o) {
                switch(o)
                {
                case ::AST::ExprNode_Assign::NONE:  return ::HIR::ExprNode_Assign::Op::None;
                case ::AST::ExprNode_Assign::ADD:   return ::HIR::ExprNode_Assign::Op::Add;
                case ::AST::ExprNode_Assign::SUB:   return ::HIR::ExprNode_Assign::Op::Sub;

                case ::AST::ExprNode_Assign::MUL:   return ::HIR::ExprNode_Assign::Op::Mul;
                case ::AST::ExprNode_Assign::DIV:   return ::HIR::ExprNode_Assign::Op::Div;
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
        m_rv.reset( new ::HIR::ExprNode_Assign( v.span(),
            H::get_op(v.m_op),
            lower( v.m_slot ),
            lower( v.m_value )
            ) );
    }
    virtual void visit(::AST::ExprNode_BinOp& v) override {
        ::HIR::ExprNode_BinOp::Op   op;
        switch(v.m_type)
        {
        case ::AST::ExprNode_BinOp::RANGE: {
            BUG(v.span(), "Unexpected RANGE binop");
            break; }
        case ::AST::ExprNode_BinOp::RANGE_INC: {
            BUG(v.span(), "Unexpected RANGE_INC binop");
            break; }
        case ::AST::ExprNode_BinOp::PLACE_IN:
            m_rv.reset(new ::HIR::ExprNode_Emplace(v.span(),
                ::HIR::ExprNode_Emplace::Type::Placer,
                lower(v.m_left),
                lower(v.m_right)
                ));
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

            m_rv.reset( new ::HIR::ExprNode_BinOp( v.span(),
                op,
                lower( v.m_left ),
                lower( v.m_right )
                ) );
            break;
        }
    }
    virtual void visit(::AST::ExprNode_UniOp& v) override {
        ::HIR::ExprNode_UniOp::Op   op;
        switch(v.m_type)
        {
        case ::AST::ExprNode_UniOp::BOX: {
            m_rv.reset(new ::HIR::ExprNode_Emplace(v.span(),
                ::HIR::ExprNode_Emplace::Type::Boxer,
                ::HIR::ExprNodeP(new ::HIR::ExprNode_Tuple(v.span(), {})),
                lower(v.m_value)
                ));
            } break;
        case ::AST::ExprNode_UniOp::QMARK:
            BUG(v.span(), "Encounterd question mark operator (should have been expanded in AST)");
            break;

        case ::AST::ExprNode_UniOp::REF:
            m_rv.reset(new ::HIR::ExprNode_Borrow(v.span(), ::HIR::BorrowType::Shared, lower( v.m_value ) ));
            break;
        case ::AST::ExprNode_UniOp::RawBorrow:
            m_rv.reset(new ::HIR::ExprNode_RawBorrow(v.span(), ::HIR::BorrowType::Shared, lower( v.m_value ) ));
            break;
        case ::AST::ExprNode_UniOp::REFMUT:
            m_rv.reset(new ::HIR::ExprNode_Borrow(v.span(), ::HIR::BorrowType::Unique, lower( v.m_value ) ));
            break;
        case ::AST::ExprNode_UniOp::RawBorrowMut:
            m_rv.reset(new ::HIR::ExprNode_RawBorrow(v.span(), ::HIR::BorrowType::Unique, lower( v.m_value ) ));
            break;

        case ::AST::ExprNode_UniOp::AWait:
            TODO(v.span(), "Convert AWait operator");
            break;

        case ::AST::ExprNode_UniOp::INVERT: op = ::HIR::ExprNode_UniOp::Op::Invert; if(0)
        case ::AST::ExprNode_UniOp::NEGATE: op = ::HIR::ExprNode_UniOp::Op::Negate;
            m_rv.reset( new ::HIR::ExprNode_UniOp( v.span(),
                op,
                lower( v.m_value )
                ) );
            break;
        }
    }
    virtual void visit(::AST::ExprNode_Cast& v) override {
        m_rv.reset( new ::HIR::ExprNode_Cast( v.span(),
            lower( v.m_value ),
            LowerHIR_Type(v.m_type)
            ) );
    }
    virtual void visit(::AST::ExprNode_TypeAnnotation& v) override {
        // TODO: Create a proper node for this
        // - Using `Unsize` works pretty well, but isn't quite "correct"
        m_rv.reset( new ::HIR::ExprNode_Unsize( v.span(),
            lower( v.m_value ),
            LowerHIR_Type(v.m_type)
            ) );
    }

    virtual void visit(::AST::ExprNode_CallPath& v) override {
        ::std::vector< ::HIR::ExprNodeP> args;
        for(auto& arg : v.m_args)
            args.push_back( lower(arg) );

        if(const auto* e = v.m_path.m_class.opt_Local()) {
            m_rv.reset( new ::HIR::ExprNode_CallValue( v.span(),
                ::HIR::ExprNodeP(new ::HIR::ExprNode_Variable( v.span(), e->name, v.m_path.m_bindings.value.binding.as_Variable().slot )),
                mv$(args)
                ) );
        }
        else
        {
            TU_MATCH_HDRA( (v.m_path.m_bindings.value.binding), {)
            default:
                m_rv.reset( new ::HIR::ExprNode_CallPath( v.span(),
                    LowerHIR_Path(v.span(), v.m_path, FromAST_PathClass::Value),
                    mv$( args )
                    ) );
            TU_ARMA(Static, e) {
                m_rv.reset( new ::HIR::ExprNode_CallValue( v.span(),
                    ::HIR::ExprNodeP(new ::HIR::ExprNode_PathValue( v.span(), LowerHIR_Path(v.span(), v.m_path, FromAST_PathClass::Value), ::HIR::ExprNode_PathValue::STATIC )),
                    mv$(args)
                    ) );
                }
            //TU_ARMA(TypeAlias, e) {
            //    TODO(v.span(), "CallPath -> TupleVariant TypeAlias");
            //    }
            TU_ARMA(EnumVar, e) {
                m_rv.reset( new ::HIR::ExprNode_TupleVariant( v.span(),
                    LowerHIR_GenericPath(v.span(), v.m_path, FromAST_PathClass::Value), false,
                    mv$( args )
                    ) );
                }
            TU_ARMA(Struct, e) {
                m_rv.reset( new ::HIR::ExprNode_TupleVariant( v.span(),
                    LowerHIR_GenericPath(v.span(), v.m_path, FromAST_PathClass::Value), true,
                    mv$( args )
                    ) );
                }
            }
        }
    }
    virtual void visit(::AST::ExprNode_CallMethod& v) override {
        ::std::vector< ::HIR::ExprNodeP> args;
        for(auto& arg : v.m_args)
            args.push_back( lower(arg) );

        m_rv.reset( new ::HIR::ExprNode_CallMethod( v.span(),
            lower(v.m_val),
            v.m_method.name(),
            LowerHIR_PathParams(v.span(), v.m_method.args(), /*allow_assoc=*/false),
            mv$(args)
            ) );
    }
    virtual void visit(::AST::ExprNode_CallObject& v) override {
        ::std::vector< ::HIR::ExprNodeP> args;
        for(auto& arg : v.m_args)
            args.push_back( lower(arg) );

        m_rv.reset( new ::HIR::ExprNode_CallValue( v.span(),
            lower(v.m_val),
            mv$(args)
            ) );
    }
    virtual void visit(::AST::ExprNode_Loop& v) override {
        switch( v.m_type )
        {
        case ::AST::ExprNode_Loop::LOOP:
            m_rv.reset( new ::HIR::ExprNode_Loop( v.span(),
                v.m_label.name,
                lower(v.m_code)
                ) );
            break;
        case ::AST::ExprNode_Loop::WHILE: {
            ::std::vector< ::HIR::ExprNodeP>    code;
            // - if `m_cond` { () } else { break `m_label` }
            code.push_back( ::HIR::ExprNodeP(new ::HIR::ExprNode_If( v.span(),
                lower(v.m_cond),
                ::HIR::ExprNodeP( new ::HIR::ExprNode_Tuple(v.span(), {}) ),
                ::HIR::ExprNodeP( new ::HIR::ExprNode_LoopControl(v.span(), v.m_label.name, false) )
                )) );
            code.push_back( lower(v.m_code) );

            m_rv.reset( new ::HIR::ExprNode_Loop( v.span(),
                v.m_label.name,
                ::HIR::ExprNodeP(new ::HIR::ExprNode_Block( v.span(), false, mv$(code), {} ))
                ) );
            break; }
        case ::AST::ExprNode_Loop::FOR:
            // NOTE: This should already be desugared (as a pass before resolve)
            BUG(v.span(), "Encountered still-sugared for loop");
            break;
        }

        // Iterate the constructed loop and determine if there are any `break` statements pointing to it
        {
            struct LoopVisitor:
                public ::HIR::ExprVisitorDef
            {
                const RcString& top_label;
                bool    top_is_broken;
                ::std::vector< const RcString*>   name_stack;

                LoopVisitor(const RcString& top_label):
                    top_label(top_label),
                    top_is_broken(false),
                    name_stack()
                {}

                void visit(::HIR::ExprNode_Loop& node) override {
                    bool push = !node.m_require_label;  // Ignore any loops that require a targeted break
                    if( push ) {
                        this->name_stack.push_back( &node.m_label );
                    }
                    ::HIR::ExprVisitorDef::visit(node);
                    if( push ) {
                        this->name_stack.pop_back( );
                    }
                }
                void visit(::HIR::ExprNode_LoopControl& node) override {
                    ::HIR::ExprVisitorDef::visit(node);

                    if( node.m_continue ) {
                    }
                    else {
                        for( auto it = this->name_stack.rbegin(); it != this->name_stack.rend(); ++ it )
                        {
                            if( node.m_label == "" || node.m_label == **it )
                                return ;
                        }
                        if( node.m_label == "" || node.m_label == this->top_label ) {
                            this->top_is_broken = true;
                        }
                        else {
                            // break is for a higher loop
                        }
                    }
                }
            };
        }
    }
    virtual void visit(::AST::ExprNode_WhileLet& v) override {

#if 0
        {
            ::std::vector< ::HIR::ExprNode_Match::Arm>  arms;

            // - Matches pattern - Run inner code
            arms.push_back(::HIR::ExprNode_Match::Arm {
                ::make_vec1( LowerHIR_Pattern(v.m_pattern) ),
                ::HIR::ExprNodeP(),
                lower(v.m_code)
                });
            // - Matches anything else - break
            arms.push_back(::HIR::ExprNode_Match::Arm {
                ::make_vec1( ::HIR::Pattern() ),
                ::HIR::ExprNodeP(),
                ::HIR::ExprNodeP( new ::HIR::ExprNode_LoopControl( v.span(), v.m_label.name, false) )
                });

            m_rv.reset( new ::HIR::ExprNode_Loop( v.span(),
                v.m_label.name,
                ::HIR::ExprNodeP(new ::HIR::ExprNode_Match( v.span(),
                    lower(v.m_cond),
                    mv$(arms)
                ))
            ) );
        }
#else
        TODO(v.span(), "while let (chained)");
#endif

        // Iterate the constructed loop and determine if there are any `break` statements pointing to it
        {
            struct LoopVisitor:
                public ::HIR::ExprVisitorDef
            {
                const RcString& top_label;
                bool    top_is_broken;
                ::std::vector< const RcString*>   name_stack;

                LoopVisitor(const RcString& top_label):
                    top_label(top_label),
                    top_is_broken(false),
                    name_stack()
                {}

                void visit(::HIR::ExprNode_Loop& node) override {
                    bool push = !node.m_require_label;  // Ignore any loops that require a targeted break
                    if( push ) {
                        this->name_stack.push_back( &node.m_label );
                    }
                    ::HIR::ExprVisitorDef::visit(node);
                    if( push ) {
                        this->name_stack.pop_back( );
                    }
                }
                void visit(::HIR::ExprNode_LoopControl& node) override {
                    ::HIR::ExprVisitorDef::visit(node);

                    if( node.m_continue ) {
                    }
                    else {
                        for( auto it = this->name_stack.rbegin(); it != this->name_stack.rend(); ++ it )
                        {
                            if( node.m_label == "" || node.m_label == **it )
                                return ;
                        }
                        if( node.m_label == "" || node.m_label == this->top_label ) {
                            this->top_is_broken = true;
                        }
                        else {
                            // break is for a higher loop
                        }
                    }
                }
            };
        }
    }
    virtual void visit(::AST::ExprNode_Match& v) override {
        ::std::vector< ::HIR::ExprNode_Match::Arm>  arms;

        for(auto& arm : v.m_arms)
        {
            ::std::vector< ::HIR::ExprNode_Match::Guard>    guards;
            guards.reserve(arm.m_guard.size());
            for(auto& c : arm.m_guard) {
                auto cond_pat = c.opt_pat
                    ? LowerHIR_Pattern(*c.opt_pat)
                    : HIR::Pattern { HIR::PatternBinding(), HIR::Pattern::Data::make_Value({ ::HIR::Pattern::Value::make_Integer({
                        HIR::CoreType::Bool,
                        U128(1)
                        }) }) }
                    ;
                auto cond_val = lower_opt(c.value);
                guards.push_back(::HIR::ExprNode_Match::Guard { std::move(cond_pat), std::move(cond_val) });
            }
            ::HIR::ExprNode_Match::Arm  new_arm {
                {},
                mv$(guards),
                lower(arm.m_code)
                };

            for(const auto& pat : arm.m_patterns)
                new_arm.m_patterns.push_back( LowerHIR_Pattern(pat) );

            arms.push_back( mv$(new_arm) );
        }

        m_rv.reset( new ::HIR::ExprNode_Match( v.span(),
            lower(v.m_val),
            mv$(arms)
            ));
    }
    virtual void visit(::AST::ExprNode_If& v) override {
        m_rv.reset( new ::HIR::ExprNode_If( v.span(),
            lower(v.m_cond),
            lower(v.m_true),
            lower_opt(v.m_false)
            ));
    }
    virtual void visit(::AST::ExprNode_IfLet& v) override {
#if 0
        ::std::vector< ::HIR::ExprNode_Match::Arm>  arms;

        std::vector<HIR::Pattern>   patterns;
        patterns.reserve(v.m_patterns.size());
        for(const auto& pat : v.m_patterns)
            patterns.push_back( LowerHIR_Pattern(pat) );
        // - Matches pattern - Take true branch
        arms.push_back(::HIR::ExprNode_Match::Arm {
            mv$(patterns),
            ::HIR::ExprNodeP(),
            lower(v.m_true)
            });
        // - Matches anything else - take false branch
        arms.push_back(::HIR::ExprNode_Match::Arm {
            ::make_vec1( ::HIR::Pattern() ),
            ::HIR::ExprNodeP(),
            v.m_false ? lower(v.m_false) : ::HIR::ExprNodeP(new ::HIR::ExprNode_Tuple(v.span(), {}))
            });
        m_rv.reset( new ::HIR::ExprNode_Match( v.span(),
            lower(v.m_value),
            mv$(arms)
            ));
#else
TODO(v.span(), "while let (chained)");
#endif
    }

    virtual void visit(::AST::ExprNode_WildcardPattern& v) override {
        ERROR(v.span(), E0000, "`_` is only valid in expressions on the left-hand side of an assignment");
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
                case CORETYPE_I128: return ::HIR::CoreType::I128;
                case CORETYPE_U128: return ::HIR::CoreType::U128;

                case CORETYPE_INT:  return ::HIR::CoreType::Isize;
                case CORETYPE_UINT: return ::HIR::CoreType::Usize;

                case CORETYPE_CHAR: return ::HIR::CoreType::Char;

                default:
                    BUG(sp, "Unknown type for integer literal - " << coretype_name(ct));
                }
            }
        };
        if( v.m_datatype == CORETYPE_F32 || v.m_datatype == CORETYPE_F64 ) {
            DEBUG("Integer annotated as float, create float node");
            m_rv.reset( new ::HIR::ExprNode_Literal( v.span(),
                ::HIR::ExprNode_Literal::Data::make_Float({
                    (v.m_datatype == CORETYPE_F32 ? ::HIR::CoreType::F32 : ::HIR::CoreType::F64),
                    v.m_value.to_double()
                    })
                ) );
            return ;
        }
        m_rv.reset( new ::HIR::ExprNode_Literal( v.span(),
            ::HIR::ExprNode_Literal::Data::make_Integer({
                H::get_type( v.span(), v.m_datatype ),
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
            BUG(v.span(), "Unknown type for float literal - " << coretype_name(v.m_datatype));
        }
        m_rv.reset( new ::HIR::ExprNode_Literal( v.span(),
            ::HIR::ExprNode_Literal::Data::make_Float({ ct, v.m_value })
            ) );
    }
    virtual void visit(::AST::ExprNode_Bool& v) override {
        m_rv.reset( new ::HIR::ExprNode_Literal( v.span(), ::HIR::ExprNode_Literal::Data::make_Boolean( v.m_value ) ) );
    }
    virtual void visit(::AST::ExprNode_String& v) override {
        m_rv.reset( new ::HIR::ExprNode_Literal( v.span(), ::HIR::ExprNode_Literal::Data::make_String( v.m_value ) ) );
    }
    virtual void visit(::AST::ExprNode_ByteString& v) override {
        ::std::vector<char> dat { v.m_value.begin(), v.m_value.end() };
        m_rv.reset( new ::HIR::ExprNode_Literal( v.span(), ::HIR::ExprNode_Literal::Data::make_ByteString( mv$(dat) ) ) );
    }
    virtual void visit(::AST::ExprNode_Closure& v) override {
        ::HIR::ExprNode_Closure::args_t args;
        for(const auto& arg : v.m_args) {
            args.push_back( ::std::make_pair(
                LowerHIR_Pattern( arg.first ),
                LowerHIR_Type( arg.second )
                ) );
        }

        auto orig_has_yield = m_has_yield;
        m_has_yield = false;
        auto inner = lower(v.m_code);
        auto has_yield = m_has_yield;
        m_has_yield = orig_has_yield;

        if(has_yield)
        {
            // NOTE: One argument could be present with yielding arguments?
            if(!args.empty()) {
                ERROR(v.span(), E0000, "Generator closures don't take arguments.");
            }
            m_rv.reset( new ::HIR::ExprNode_Generator( v.span(),
                //mv$(args),
                LowerHIR_Type(v.m_return),
                mv$(inner),
                v.m_is_move,
                v.m_is_pinned
                ) );
        }
        else
        {
            if( v.m_is_pinned ) {
                ERROR(v.span(), E0000, "Invalid use of `static` on non-yielding closure");
            }
            m_rv.reset( new ::HIR::ExprNode_Closure( v.span(),
                mv$(args),
                LowerHIR_Type(v.m_return),
                mv$(inner),
                v.m_is_move
                ) );
        }
    }
    virtual void visit(::AST::ExprNode_StructLiteral& v) override {
        if( v.m_path.m_bindings.type.binding.is_Union() )
        {
            if( v.m_values.size() != 1 )
                ERROR(v.span(), E0000, "Union constructors can only specify a single field");
            if( v.m_base_value )
                ERROR(v.span(), E0000, "Union constructors can't take a base value");
        }

        ::HIR::ExprNode_StructLiteral::t_values values;
        for(auto& val : v.m_values)
            values.push_back( ::std::make_pair(val.name, lower(val.value)) );
        auto ty = LowerHIR_Type( ::TypeRef(v.span(), v.m_path) );
        if( v.m_path.m_bindings.type.binding.is_EnumVar() )
        {
            ASSERT_BUG(v.span(), TU_TEST1(ty.data(), Path, .path.m_data.is_Generic()), "Enum variant path not GenericPath: " << ty );
            auto& gp = ty.get_unique().as_Path().path.m_data.as_Generic();
            auto var_name = gp.m_path.pop_component();
            ty = ::HIR::TypeRef::new_path( ::HIR::Path(mv$(ty), mv$(var_name)), {} );
        }
        m_rv.reset( new ::HIR::ExprNode_StructLiteral( v.span(),
            mv$(ty),
            ! v.m_path.m_bindings.type.binding.is_EnumVar(),
            lower_opt(v.m_base_value),
            mv$(values)
            ) );
    }
    virtual void visit(::AST::ExprNode_StructLiteralPattern& v) override {
        ERROR(v.span(), E0000, "struct literal with an empty `..`");
    }
    virtual void visit(::AST::ExprNode_Array& v) override {
        if( v.m_size )
        {
            m_rv.reset( new ::HIR::ExprNode_ArraySized( v.span(),
                lower( v.m_values.at(0) ),
                // TODO: Should this size be a full expression on its own?
                lower( v.m_size )
                ) );
        }
        else
        {
            ::std::vector< ::HIR::ExprNodeP>    vals;
            for(auto& val : v.m_values)
                vals.push_back( lower(val) );
            m_rv.reset( new ::HIR::ExprNode_ArrayList( v.span(), mv$(vals) ) );
        }
    }
    virtual void visit(::AST::ExprNode_Tuple& v) override {
        ::std::vector< ::HIR::ExprNodeP>    vals;
        for(auto& val : v.m_values)
            vals.push_back( lower(val) );
        m_rv.reset( new ::HIR::ExprNode_Tuple( v.span(), mv$(vals) ) );
    }
    virtual void visit(::AST::ExprNode_NamedValue& v) override {
        if(const auto* e = v.m_path.m_class.opt_Local())
        {
            TU_MATCH_HDRA( (v.m_path.m_bindings.value.binding), {)
            default:
                BUG(v.span(), "Named value was a local, but wasn't bound to a known type - " << v.m_path);
            TU_ARMA(Generic, binding) {
                m_rv.reset( new ::HIR::ExprNode_ConstParam( v.span(), e->name, binding.index ) );
                }
            TU_ARMA(Variable, binding) {
                m_rv.reset( new ::HIR::ExprNode_Variable( v.span(), e->name, binding.slot ) );
                }
            }
        }
        else
        {
            TU_MATCH_HDRA( (v.m_path.m_bindings.value.binding), {)
            TU_ARMA(Struct, e) {
                ASSERT_BUG(v.span(), e.struct_ || e.hir, "PathValue bound to a struct but pointer not set - " << v.m_path);
                // Check the form and emit a PathValue if not a unit
                bool is_tuple_constructor = false;
                if( e.struct_ )
                {
                    if( e.struct_->m_data.is_Struct() ) {
                        ERROR(v.span(), E0000, "Named value referring to a struct that isn't tuple-like or unit-like - " << v.m_path);
                    }
                    is_tuple_constructor = e.struct_->m_data.is_Tuple();
                }
                else
                {
                    const auto& str = *e.hir;
                    if( str.m_data.is_Unit() ) {
                        is_tuple_constructor = false;
                    }
                    else if( str.m_data.is_Tuple() ) {
                        is_tuple_constructor = true;
                    }
                    else {
                        ERROR(v.span(), E0000, "Named value referring to a struct that isn't tuple-like or unit-like - " << v.m_path);
                    }
                }
                if( is_tuple_constructor ) {
                    m_rv.reset( new ::HIR::ExprNode_PathValue( v.span(), LowerHIR_Path(v.span(), v.m_path, FromAST_PathClass::Value), ::HIR::ExprNode_PathValue::STRUCT_CONSTR ) );
                }
                else {
                    m_rv.reset( new ::HIR::ExprNode_UnitVariant( v.span(), LowerHIR_GenericPath(v.span(), v.m_path, FromAST_PathClass::Value), true ) );
                }
                }
            TU_ARMA(EnumVar, e) {
                ASSERT_BUG(v.span(), e.enum_ || e.hir, "PathValue bound to an enum but pointer not set - " << v.m_path);
                const auto& var_name = v.m_path.nodes().back().name();
                bool is_tuple_constructor = false;
                unsigned int var_idx;
                if( e.enum_ )
                {
                    const auto& enm = *e.enum_;
                    auto it = ::std::find_if(enm.variants().begin(), enm.variants().end(), [&](const auto& x){ return x.m_name == var_name; });
                    assert(it != enm.variants().end());

                    var_idx = static_cast<unsigned int>(it - enm.variants().begin());
                    if( it->m_data.is_Struct() ) {
                        ERROR(v.span(), E0000, "Named value referring to an enum that isn't tuple-like or unit-like - " << v.m_path);
                    }
                    is_tuple_constructor = it->m_data.is_Tuple() && it->m_data.as_Tuple().m_items.size() > 0;
                }
                else
                {
                    const auto& enm = *e.hir;
                    auto idx = enm.find_variant(var_name);
                    assert(idx != SIZE_MAX);

                    var_idx = idx;
                    if( const auto* ee = enm.m_data.opt_Data() )
                    {
                        if( ee->at(idx).type == ::HIR::TypeRef::new_unit() ) {
                        }
                        // TODO: Assert that it's not a struct-like
                        else {
                            is_tuple_constructor = true;
                        }
                    }
                }
                (void)var_idx;  // TODO: Save time later by saving this.
                if( is_tuple_constructor ) {
                    m_rv.reset( new ::HIR::ExprNode_PathValue( v.span(), LowerHIR_Path(v.span(), v.m_path, FromAST_PathClass::Value), ::HIR::ExprNode_PathValue::ENUM_VAR_CONSTR ) );
                }
                else {
                    m_rv.reset( new ::HIR::ExprNode_UnitVariant( v.span(), LowerHIR_GenericPath(v.span(), v.m_path, FromAST_PathClass::Value), false ) );
                }
                }
            TU_ARMA(Function, e) {
                m_rv.reset( new ::HIR::ExprNode_PathValue( v.span(), LowerHIR_Path(v.span(), v.m_path, FromAST_PathClass::Value), ::HIR::ExprNode_PathValue::FUNCTION ) );
                }
            TU_ARMA(Static, e) {
                if( e.static_ )
                {
                    if( e.static_->s_class() != ::AST::Static::CONST ) {
                        m_rv.reset( new ::HIR::ExprNode_PathValue( v.span(), LowerHIR_Path(v.span(), v.m_path, FromAST_PathClass::Value), ::HIR::ExprNode_PathValue::STATIC ) );
                    }
                    else {
                        m_rv.reset( new ::HIR::ExprNode_PathValue( v.span(), LowerHIR_Path(v.span(), v.m_path, FromAST_PathClass::Value), ::HIR::ExprNode_PathValue::CONSTANT ) );
                    }
                }
                else if( e.hir )
                {
                    m_rv.reset( new ::HIR::ExprNode_PathValue( v.span(), LowerHIR_Path(v.span(), v.m_path, FromAST_PathClass::Value), ::HIR::ExprNode_PathValue::STATIC ) );
                }
                // HACK: If the HIR pointer is nullptr, then it refers to a `const
                else
                {
                    m_rv.reset( new ::HIR::ExprNode_PathValue( v.span(), LowerHIR_Path(v.span(), v.m_path, FromAST_PathClass::Value), ::HIR::ExprNode_PathValue::CONSTANT ) );
                }
                }
            break; default:
                auto p = LowerHIR_Path(v.span(), v.m_path, FromAST_PathClass::Value);
                ASSERT_BUG(v.span(), !p.m_data.is_Generic(), "Unknown binding for PathValue but path is generic - " << v.m_path);
                m_rv.reset( new ::HIR::ExprNode_PathValue( v.span(), mv$(p), ::HIR::ExprNode_PathValue::UNKNOWN ) );
            }
        }
    }

    virtual void visit(::AST::ExprNode_Field& v) override {
        m_rv.reset( new ::HIR::ExprNode_Field( v.span(),
            lower(v.m_obj),
            v.m_name
            ));
    }
    virtual void visit(::AST::ExprNode_Index& v) override {
        m_rv.reset( new ::HIR::ExprNode_Index( v.span(),
            lower(v.m_obj),
            lower(v.m_idx)
            ));
    }
    virtual void visit(::AST::ExprNode_Deref& v) override {
        m_rv.reset( new ::HIR::ExprNode_Deref( v.span(),
            lower(v.m_value)
            ));
    }
};

::HIR::ExprPtr LowerHIR_ExprNode(const ::AST::ExprNode& e)
{
    LowerHIR_ExprNode_Visitor v;

    const_cast<::AST::ExprNode*>(&e)->visit( v );

    if( ! v.m_rv ) {
        BUG(e.span(), typeid(e).name() << " - Yielded a nullptr HIR node");
    }

    return ::HIR::ExprPtr( mv$( v.m_rv ) );
}
