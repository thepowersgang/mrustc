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
#include <mir/helpers.hpp>

void MIR_Validate(const StaticTraitResolve& resolve, const ::HIR::ItemPath& path, const ::MIR::Function& fcn, const ::HIR::Function::args_t& args, const ::HIR::TypeRef& ret_type)
{
    Span    sp;
    ::MIR::TypeResolve   state { sp, resolve, FMT_CB(ss, ss << path;), ret_type, args, fcn };
    // Validation rules:

    // [CFA] = Control Flow Analysis
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
            
            
            state.set_cur_stmt_term(block);
            
            #define PUSH_BB(idx, desc)  do {\
                if( !(idx < fcn.blocks.size() ) )   MIR_BUG(state,  "Invalid target block - " << desc << " bb" << idx);\
                if( visited_bbs[idx] == false ) {\
                    to_visit_blocks.push_back(idx); \
                }\
                } while(0)
            TU_MATCH(::MIR::Terminator, (fcn.blocks[block].terminator), (e),
            (Incomplete,
                MIR_BUG(state,  "Encounterd `Incomplete` block in control flow");
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
    
    // [ValState] = Value state tracking (use after move, uninit, ...)
    // - [ValState] No drops or usage of uninitalised values (Uninit, Moved, or Dropped)
    // - [ValState] Temporaries are write-once.
    //  - Requires maintaining state information for all variables/temporaries with support for loops
    //  - 

    // [Flat] = Basic checks (just iterates BBs)
    // - [Flat] Types must be valid (correct type for slot etc.)
    //  - Simple check of all assignments/calls/...
    {
        for(unsigned int bb_idx = 0; bb_idx < fcn.blocks.size(); bb_idx ++)
        {
            const auto& bb = fcn.blocks[bb_idx];
            for(unsigned int stmt_idx = 0; stmt_idx < bb.statements.size(); stmt_idx ++)
            {
                const auto& stmt = bb.statements[stmt_idx];
                state.set_cur_stmt(bb_idx, stmt_idx);
                
                switch( stmt.tag() )
                {
                case ::MIR::Statement::TAGDEAD:
                    throw "";
                case ::MIR::Statement::TAG_Assign: {
                    const auto& a = stmt.as_Assign();
                    
                    auto check_type = [&](const auto& src_ty) {
                        ::HIR::TypeRef  tmp;
                        const auto& dst_ty = state.get_lvalue_type(tmp, a.dst);
                        if( src_ty == ::HIR::TypeRef::new_diverge() ) {
                        }
                        else if( src_ty == dst_ty ) {
                        }
                        else {
                            MIR_BUG(state,  "Type mismatch, destination is " << dst_ty << ", source is " << src_ty);
                        }
                        };
                    TU_MATCH(::MIR::RValue, (a.src), (e),
                    (Use,
                        ::HIR::TypeRef  tmp;
                        check_type( state.get_lvalue_type(tmp, e) );
                        ),
                    (Constant,
                        // TODO: Check constant types.
                        ::HIR::TypeRef  tmp;
                        const auto& dst_ty = state.get_lvalue_type(tmp, a.dst);
                        TU_MATCH( ::MIR::Constant, (e), (c),
                        (Int,
                            bool good = false;
                            if( dst_ty.m_data.is_Primitive() ) {
                                switch( dst_ty.m_data.as_Primitive() ) {
                                case ::HIR::CoreType::I8:
                                case ::HIR::CoreType::I16:
                                case ::HIR::CoreType::I32:
                                case ::HIR::CoreType::I64:
                                case ::HIR::CoreType::Isize:
                                    good = true;
                                    break;
                                default:
                                    break;
                                }
                            }
                            if( !good ) {
                                MIR_BUG(state,  "Type mismatch, destination is " << dst_ty << ", source is a signed integer");
                            }
                            ),
                        (Uint,
                            bool good = false;
                            if( dst_ty.m_data.is_Primitive() ) {
                                switch( dst_ty.m_data.as_Primitive() ) {
                                case ::HIR::CoreType::U8:
                                case ::HIR::CoreType::U16:
                                case ::HIR::CoreType::U32:
                                case ::HIR::CoreType::U64:
                                case ::HIR::CoreType::Usize:
                                case ::HIR::CoreType::Char:
                                    good = true;
                                    break;
                                default:
                                    break;
                                }
                            }
                            if( !good ) {
                                MIR_BUG(state,  "Type mismatch, destination is " << dst_ty << ", source is an unsigned integer");
                            }
                            ),
                        (Float,
                            bool good = false;
                            if( dst_ty.m_data.is_Primitive() ) {
                                switch( dst_ty.m_data.as_Primitive() ) {
                                case ::HIR::CoreType::F32:
                                case ::HIR::CoreType::F64:
                                    good = true;
                                    break;
                                default:
                                    break;
                                }
                            }
                            if( !good ) {
                                MIR_BUG(state,  "Type mismatch, destination is " << dst_ty << ", source is a floating point value");
                            }
                            ),
                        (Bool,
                            check_type( ::HIR::TypeRef(::HIR::CoreType::Bool) );
                            ),
                        (Bytes,
                            // TODO: Check result (could be either &[u8; N] or &[u8])
                            ),
                        (StaticString,
                            check_type( ::HIR::TypeRef::new_borrow(::HIR::BorrowType::Shared, ::HIR::CoreType::Str) );
                            ),
                        (Const,
                            // TODO: Check result type against type of const
                            ),
                        (ItemAddr,
                            // TODO: Check result type against pointer to item type
                            )
                        )
                        ),
                    (SizedArray,
                        // TODO: Check that return type is an array
                        // TODO: Check that the input type is Copy
                        ),
                    (Borrow,
                        // TODO: Check return type
                        ),
                    (Cast,
                        // TODO: Check return type
                        // TODO: Check suitability of source type (COMPLEX)
                        ),
                    (BinOp,
                        ::HIR::TypeRef  tmp_l, tmp_r;
                        const auto& ty_l = state.get_lvalue_type(tmp_l, e.val_l);
                        const auto& ty_r = state.get_lvalue_type(tmp_r, e.val_r);
                        // TODO: Check that operation is valid on these types
                        switch( e.op )
                        {
                        case ::MIR::eBinOp::BIT_SHR:
                        case ::MIR::eBinOp::BIT_SHL:
                            break;
                        default:
                            // Check argument types are equal
                            if( ty_l != ty_r )
                                MIR_BUG(state, "Type mismatch in binop, " << ty_l << " != " << ty_r);
                        }
                        // TODO: Check return type
                        ),
                    (UniOp,
                        // TODO: Check that operation is valid on this type
                        // TODO: Check return type
                        ),
                    (DstMeta,
                        // TODO: Ensure that the input type is a: Generic, Array, or DST
                        // TODO: Check return type
                        ),
                    (DstPtr,
                        // TODO: Ensure that the input type is a DST
                        // TODO: Check return type
                        ),
                    (MakeDst,
                        ),
                    (Tuple,
                        // TODO: Check return type
                        ),
                    (Array,
                        // TODO: Check return type
                        ),
                    (Variant,
                        // TODO: Check return type
                        ),
                    (Struct,
                        // TODO: Check return type
                        )
                    )
                    } break;
                case ::MIR::Statement::TAG_Drop:
                    // TODO: Anything need checking here?
                    break;
                }
            }
            
            state.set_cur_stmt_term(bb_idx);
            TU_MATCH(::MIR::Terminator, (bb.terminator), (e),
            (Incomplete,
                ),
            (Return,
                // TODO: Check if the function can return (i.e. if its return type isn't an empty type)
                ),
            (Diverge,
                ),
            (Goto,
                ),
            (Panic,
                ),
            (If,
                // Check that condition lvalue is a bool
                ::HIR::TypeRef  tmp;
                const auto& ty = state.get_lvalue_type(tmp, e.cond);
                if( ty != ::HIR::CoreType::Bool ) {
                    MIR_BUG(state, "Type mismatch in `If` - expected bool, got " << ty);
                }
                ),
            (Switch,
                // Check that the condition is an enum
                ),
            (Call,
                // Typecheck arguments and return value
                )
            )
        }
    }
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
                    MIR_Validate(m_resolve, ::HIR::ItemPath(), *e.size->m_mir, {}, ::HIR::TypeRef(::HIR::CoreType::Usize));
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
                // TODO: Get span without needing hir/expr.hpp
                static Span sp;
                
                // Replace ErasedType instances in `ret_type`
                const auto& ret_type = item.m_return;
                auto ret_type_v = clone_ty_with(sp, ret_type, [&](const auto& tpl, auto& rv) {
                    if( tpl.m_data.is_ErasedType() )
                    {
                        const auto& e = tpl.m_data.as_ErasedType();
                        assert(e.m_index < item.m_code.m_erased_types.size());
                        rv = item.m_code.m_erased_types[e.m_index].clone();
                        return true;
                    }
                    return false;
                    });
                this->m_resolve.expand_associated_types(sp, ret_type_v);
    
                MIR_Validate(m_resolve, p, *item.m_code.m_mir, item.m_args, ret_type_v);
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override {
            if( item.m_value ) {
                DEBUG("`static` value " << p);
                MIR_Validate(m_resolve, p, *item.m_value.m_mir, {}, item.m_type);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value ) {
                DEBUG("`const` value " << p);
                MIR_Validate(m_resolve, p, *item.m_value.m_mir, {}, item.m_type);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            auto _ = this->m_resolve.set_item_generics(item.m_params);
            
            // TODO: Use a different type depding on repr()
            auto enum_type = ::HIR::TypeRef(::HIR::CoreType::Isize);
            
            for(auto& var : item.m_variants)
            {
                TU_IFLET(::HIR::Enum::Variant, var.second, Value, e,
                    // TODO: Get the repr type
                    MIR_Validate(m_resolve, p + var.first, *e.expr.m_mir, {}, enum_type);
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
