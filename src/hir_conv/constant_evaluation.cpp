/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_conv/constant_evaluation.cpp
 * - Minimal (integer only) constant evaluation
 */
#include "main_bindings.hpp"
#include <hir/hir.hpp>
#include <hir/expr.hpp>
#include <hir/visitor.hpp>
#include <algorithm>
#include <mir/mir.hpp>
#include <hir_typeck/common.hpp>    // Monomorph
#include <mir/helpers.hpp>
#include <trans/target.hpp>
#include <hir/expr_state.hpp>

#include "constant_evaluation.hpp"
#include <trans/monomorphise.hpp>   // For handling monomorph of MIR in provided associated constants

#define CHECK_DEFER(var) do { if( var.is_Defer() ) { m_rv = ::HIR::Literal::make_Defer({}); return ; } } while(0)

namespace {
    struct NewvalState
        : public HIR::Evaluator::Newval
    {
        const ::HIR::Module&   mod;
        const ::HIR::ItemPath&  mod_path;
        ::std::string   name_prefix;
        unsigned int next_item_idx;

        NewvalState(const ::HIR::Module& mod, const ::HIR::ItemPath& mod_path, ::std::string prefix):
            mod(mod),
            mod_path(mod_path),
            name_prefix(prefix),
            next_item_idx(0)
        {
        }

        virtual ::HIR::Path new_static(::HIR::TypeRef type, ::HIR::Literal value) override
        {
            auto name = RcString::new_interned(FMT(name_prefix << next_item_idx));
            next_item_idx ++;
            DEBUG("mod_path = " << mod_path);
            auto rv = mod_path.get_simple_path() + name.c_str();
            const_cast<::HIR::Module&>(mod).m_inline_statics.push_back( ::std::make_pair( mv$(name), ::HIR::Static {
                ::HIR::Linkage {},
                false,
                mv$(type),
                ::HIR::ExprPtr(),
                mv$(value)
                } ) );
            return rv;
        }
    };

    TAGGED_UNION(EntPtr, NotFound,
        (NotFound, struct{}),
        (Function, const ::HIR::Function*),
        (Static, const ::HIR::Static*),
        (Constant, const ::HIR::Constant*),
        (Struct, const ::HIR::Struct*)
        );
    enum class EntNS {
        Type,
        Value
    };
    EntPtr get_ent_simplepath(const Span& sp, const ::HIR::Crate& crate, const ::HIR::SimplePath& path, EntNS ns)
    {
        const ::HIR::Module& mod = crate.get_mod_by_path(sp, path, /*ignore_last_node=*/true);

        switch( ns )
        {
        case EntNS::Value: {
            auto it = mod.m_value_items.find( path.m_components.back() );
            if( it == mod.m_value_items.end() ) {
                return EntPtr {};
            }

            TU_MATCH( ::HIR::ValueItem, (it->second->ent), (e),
            (Import,
                ),
            (StructConstant,
                ),
            (StructConstructor,
                ),
            (Function,
                return EntPtr { &e };
                ),
            (Constant,
                return EntPtr { &e };
                ),
            (Static,
                return EntPtr { &e };
                )
            )
            BUG(sp, "Path " << path << " pointed to a invalid item - " << it->second->ent.tag_str());
            } break;
        case EntNS::Type: {
            auto it = mod.m_mod_items.find( path.m_components.back() );
            if( it == mod.m_mod_items.end() ) {
                return EntPtr {};
            }

            TU_MATCH( ::HIR::TypeItem, (it->second->ent), (e),
            (Import,
                ),
            (Module,
                ),
            (Trait,
                ),
            (Struct,
                return &e;
                ),
            (Union,
                ),
            (Enum,
                ),
            (ExternType,
                ),
            (TypeAlias,
                )
            )
            BUG(sp, "Path " << path << " pointed to an invalid item - " << it->second->ent.tag_str());
            } break;
        }
        throw "";
    }
    EntPtr get_ent_fullpath(const Span& sp, const ::HIR::Crate& crate, const ::HIR::Path& path, EntNS ns, MonomorphState& out_ms)
    {
        TU_MATCH(::HIR::Path::Data, (path.m_data), (e),
        (Generic,
            out_ms = MonomorphState {};
            out_ms.pp_method = &e.m_params;
            return get_ent_simplepath(sp, crate, e.m_path, ns);
            ),
        (UfcsInherent,
            // Easy (ish)
            EntPtr rv;
            crate.find_type_impls(*e.type, [](const auto&x)->const auto& { return x; }, [&](const auto& impl) {
                switch( ns )
                {
                case EntNS::Value:
                    {
                        auto fit = impl.m_methods.find(e.item);
                        if( fit != impl.m_methods.end() )
                        {
                            DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                            rv = EntPtr { &fit->second.data };
                            return true;
                        }
                    }
                    {
                        auto it = impl.m_constants.find(e.item);
                        if( it != impl.m_constants.end() )
                        {
                            rv = EntPtr { &it->second.data };
                            return true;
                        }
                    }
                    break;
                case EntNS::Type:
                    break;
                }
                return false;
                });
            out_ms = MonomorphState {};
            out_ms.pp_method = &e.params;
            out_ms.pp_impl = &e.impl_params;
            return rv;
            ),
        (UfcsKnown,
            EntPtr rv;
            crate.find_trait_impls(e.trait.m_path, *e.type, [](const auto&x)->const auto& { return x; }, [&](const auto& impl) {
                // Hacky selection of impl.
                // - TODO: Specialisation
                // - TODO: Inference? (requires full typeck)
                switch( ns )
                {
                case EntNS::Value:
                    {
                        auto fit = impl.m_methods.find(e.item);
                        if( fit != impl.m_methods.end() )
                        {
                            DEBUG("Found impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                            rv = EntPtr { &fit->second.data };
                            return true;
                        }
                    }
                    {
                        auto it = impl.m_constants.find(e.item);
                        if( it != impl.m_constants.end() )
                        {
                            rv = EntPtr { &it->second.data };
                            return true;
                        }
                    }
                    break;
                case EntNS::Type:
                    break;
                }
                return false;
                });
            out_ms = MonomorphState {};
            out_ms.pp_method = &e.params;
            // TODO: How to get pp_impl here? Needs specialisation magic.
            return rv;
            ),
        (UfcsUnknown,
            // TODO: Are these valid at this point in compilation?
            TODO(sp, "get_ent_fullpath(path = " << path << ")");
            )
        )
        throw "";
    }
    const ::HIR::Function& get_function(const Span& sp, const ::HIR::Crate& crate, const ::HIR::Path& path, MonomorphState& out_ms)
    {
        auto rv = get_ent_fullpath(sp, crate, path, EntNS::Value, out_ms);
        TU_IFLET( EntPtr, rv, Function, e,
            return *e;
        )
        else {
            TODO(sp, "Could not find function for " << path << " - " << rv.tag_str());
        }
    }
}   // namespace <anon>

namespace HIR {

    ::HIR::Literal Evaluator::evaluate_constant_mir(const ::HIR::ItemPath& ip, const ::MIR::Function& fcn, MonomorphState ms, ::HIR::TypeRef exp, ::std::vector< ::HIR::Literal> args)
    {
        // TODO: Full-blown miri
        TRACE_FUNCTION_F("exp=" << exp << ", args=" << args);

        ::MIR::TypeResolve  state { this->root_span, this->resolve, FMT_CB(ss, ss<<ip), exp, {}, fcn };

        ::HIR::Literal  retval;
        ::std::vector< ::HIR::Literal>  locals( fcn.locals.size() );


        struct LocalState {
            typedef ::std::vector< ::HIR::Literal>  t_vec_lit;
            ::MIR::TypeResolve& state;
            ::HIR::Literal&  retval;
            ::std::vector< ::HIR::Literal>&  args;
            ::std::vector< ::HIR::Literal>&  locals;

            LocalState(::MIR::TypeResolve& state, ::HIR::Literal& retval, t_vec_lit& args, t_vec_lit& locals):
                state(state),
                retval(retval),
                args(args),
                locals(locals)
            {}

            ::HIR::Literal& get_lval(const ::MIR::LValue& lv)
            {
                ::HIR::Literal* lit_ptr;
                TRACE_FUNCTION_FR(lv, *lit_ptr);
                TU_MATCHA( (lv.m_root), (e),
                (Return,
                    lit_ptr = &retval;
                    ),
                (Local,
                    MIR_ASSERT(state, e < locals.size(), "Local index out of range - " << e << " >= " << locals.size());
                    lit_ptr = &locals[e];
                    ),
                (Argument,
                    MIR_ASSERT(state, e < args.size(), "Argument index out of range - " << e << " >= " << args.size());
                    lit_ptr = &args[e];
                    ),
                (Static,
                    MIR_TODO(state, "LValue::Static - " << e);
                    )
                )

                for(const auto& w : lv.m_wrappers)
                {
                    auto& val = *lit_ptr;
                    TU_MATCH_HDRA( (w), {)
                    TU_ARMA(Field, e) {
                        MIR_ASSERT(state, val.is_List(), "LValue::Field on non-list literal - " << val.tag_str() << " - " << lv);
                        auto& vals = val.as_List();
                        MIR_ASSERT(state, e < vals.size(), "LValue::Field index out of range");
                        lit_ptr = &vals[ e ];
                        }
                    TU_ARMA(Deref, e) {
                        TU_MATCH_DEF( ::HIR::Literal, (val), (ve),
                        (
                            MIR_TODO(state, "LValue::Deref - " << lv << " { " << val << " }");
                            ),
                        (BorrowData,
                            lit_ptr = &*ve;
                            ),
                        (String,
                            // Just clone the string (hack)
                            // - TODO: Create a list?
                            lit_ptr = &val;
                            )
                        )
                        }
                    TU_ARMA(Index, e) {
                        MIR_ASSERT(state, val.is_List(), "LValue::Index on non-list literal - " << val.tag_str() << " - " << lv);
                        MIR_ASSERT(state, e < locals.size(), "LValue::Index index local out of range");
                        auto& idx = locals[e];
                        MIR_ASSERT(state, idx.is_Integer(), "LValue::Index with non-integer index literal - " << idx.tag_str() << " - " << lv);
                        auto& vals = val.as_List();
                        auto idx_v = static_cast<size_t>( idx.as_Integer() );
                        MIR_ASSERT(state, idx_v < vals.size(), "LValue::Index index out of range");
                        lit_ptr = &vals[ idx_v ];
                        }
                    TU_ARMA(Downcast, e) {
                        MIR_TODO(state, "LValue::Downcast - " << lv);
                        }
                    }
                }
                return *lit_ptr;
            }
            ::HIR::Literal read_lval(const ::MIR::LValue& lv)
            {
                auto& v = get_lval(lv);
                DEBUG(lv << " = " << v);
                TU_MATCH_DEF(::HIR::Literal, (v), (e),
                (
                    return mv$(v);
                    ),
                (Invalid,
                    MIR_BUG(state, "Read of " << lv << " yielded Invalid");
                    ),
                (BorrowPath,
                    return ::HIR::Literal(e.clone());
                    ),
                (Integer,
                    return ::HIR::Literal(e);
                    ),
                (Float,
                    return ::HIR::Literal(e);
                    )
                )
            }
        };
        LocalState  local_state( state, retval, args, locals );

        auto const_to_lit = [&](const ::MIR::Constant& c)->::HIR::Literal {
            TU_MATCH_HDR( (c), {)
            TU_ARM(c, Int, e2) {
                return ::HIR::Literal(static_cast<uint64_t>(e2.v));
                }
            TU_ARM(c, Uint, e2)
                return ::HIR::Literal(e2.v);
            TU_ARM(c, Float, e2)
                return ::HIR::Literal(e2.v);
            TU_ARM(c, Bool, e2)
                return ::HIR::Literal(static_cast<uint64_t>(e2.v));
            TU_ARM(c, Bytes, e2)
                return ::HIR::Literal::make_String({e2.begin(), e2.end()});
            TU_ARM(c, StaticString, e2)
                return ::HIR::Literal(e2);
            TU_ARM(c, Const, e2) {
                auto p = ms.monomorph(state.sp, *e2.p);
                // If there's any mention of generics in this path, then return Literal::Defer
                if( visit_path_tys_with(p, [&](const auto& ty)->bool { return ty.m_data.is_Generic(); }) )
                {
                    DEBUG("Return Literal::Defer for constant " << *e2.p << " which references a generic parameter");
                    return ::HIR::Literal::make_Defer({});
                }
                MonomorphState  const_ms;
                auto ent = get_ent_fullpath(state.sp, this->resolve.m_crate, p, EntNS::Value,  const_ms);
                MIR_ASSERT(state, ent.is_Constant(), "MIR Constant::Const(" << p << ") didn't point to a Constant - " << ent.tag_str());
                const auto& c = *ent.as_Constant();
                if( c.m_value_res.is_Invalid() )
                {
                    auto& item = const_cast<::HIR::Constant&>(c);
                    // Challenge: Adding items to the module might invalidate an iterator.
                    ::HIR::ItemPath mod_ip { item.m_value.m_state->m_mod_path };
                    auto nvs = NewvalState { item.m_value.m_state->m_module, mod_ip, FMT("const" << &c << "#") };
                    auto eval = ::HIR::Evaluator { item.m_value.span(), resolve.m_crate, nvs };
                    DEBUG("- Evaluate " << p);
                    DEBUG("- " << ::HIR::ItemPath(p));
                    item.m_value_res = eval.evaluate_constant(::HIR::ItemPath(p), item.m_value, item.m_type.clone());

                    //check_lit_type(item.m_value->span(), item.m_type, item.m_value_res);
                }
                auto it = c.m_monomorph_cache.find(*e2.p);
                if( it != c.m_monomorph_cache.end() )
                {
                    MIR_ASSERT(state, !it->second.is_Defer(), "Cached literal for " << *e2.p << " is Defer");
                    return it->second.clone();
                }
                return c.m_value_res.clone();
                }
            TU_ARM(c, Generic, e2) {
                return ::HIR::Literal::make_Defer({});
                }
            TU_ARM(c, ItemAddr, e2)
                return ::HIR::Literal::make_BorrowPath( ms.monomorph(state.sp, *e2) );
            }
            throw "";
            };
        auto read_param = [&](const ::MIR::Param& p) -> ::HIR::Literal
            {
                TU_MATCH(::MIR::Param, (p), (e),
                (LValue,
                    return local_state.read_lval(e);
                    ),
                (Constant,
                    return const_to_lit(e);
                    )
                )
                throw "";
            };

        unsigned int cur_block = 0;
        for(;;)
        {
            const auto& block = fcn.blocks[cur_block];
            for(const auto& stmt : block.statements)
            {
                state.set_cur_stmt(cur_block, &stmt - &block.statements.front());
                DEBUG(state << stmt);

                if( ! stmt.is_Assign() ) {
                    //MIR_BUG(state, "Non-assign statement - drop " << stmt.as_Drop().slot);
                    continue ;
                }

                ::HIR::Literal  val;
                const auto& sa = stmt.as_Assign();
                TU_MATCH_HDRA( (sa.src), {)
                TU_ARMA(Use, e) {
                    val = local_state.read_lval(e);
                    }
                TU_ARMA(Constant, e) {
                    val = const_to_lit(e);
                    }
                TU_ARMA(SizedArray, e) {
                    ::std::vector< ::HIR::Literal>  vals;
                    if( e.count > 0 )
                    {
                        vals.reserve( e.count );
                        val = read_param(e.val);
                        for(unsigned int i = 1; i < e.count; i++)
                            vals.push_back( val.clone() );
                        vals.push_back( mv$(val) );
                    }
                    val = ::HIR::Literal::make_List( mv$(vals) );
                    }
                TU_ARMA(Borrow, e) {
                    if( e.type != ::HIR::BorrowType::Shared ) {
                        MIR_BUG(state, "Only shared borrows are allowed in constants");
                    }

                    if( !e.val.m_wrappers.empty() && e.val.m_wrappers.back().is_Deref() ) {
                        //if( p->val->is_Deref() )
                        //    MIR_TODO(state, "Undo nested deref coercion - " << *p->val);
                        val = local_state.read_lval(e.val.clone_unwrapped());
                    }
                    else if( e.val.m_wrappers.empty() && e.val.m_root.is_Static() ){
                        // Borrow of a static, emit BorrowPath with the same path
                        val = ::HIR::Literal::make_BorrowPath( e.val.m_root.as_Static().clone() );
                    }
                    else {
                        auto inner_val = local_state.read_lval(e.val);

                        ::HIR::TypeRef  inner_ty;
                        const auto& inner_ty_r = state.get_lvalue_type(inner_ty, e.val);
                        if( &inner_ty_r != &inner_ty )
                            inner_ty = inner_ty_r.clone();

                        // Create new static containing borrowed data
                        // NOTE: Doesn't use BorrowData
                        auto item_path = this->nvs.new_static( mv$(inner_ty), mv$(inner_val) );
                        val = ::HIR::Literal::make_BorrowPath( mv$(item_path) );
                    }
                    }
                TU_ARMA(Cast, e) {
                    auto inval = local_state.read_lval(e.val);
                    if( inval.is_Defer() ) {
                        val = ::HIR::Literal::make_Defer({});
                    }
                    else
                    TU_MATCH_HDRA( (e.type.m_data), {)
                    default:
                        // NOTE: Can be an unsizing!
                        MIR_TODO(state, "RValue::Cast to " << e.type << ", val = " << inval);
                    TU_ARMA(Primitive, te) {
                        uint64_t mask;
                        switch(te)
                        {
                        // Integers mask down
                        case ::HIR::CoreType::I8:
                        case ::HIR::CoreType::U8:
                            mask = 0xFF;
                            if(0)
                        case ::HIR::CoreType::I16:
                        case ::HIR::CoreType::U16:
                            mask = 0xFFFF;
                            if(0)
                        case ::HIR::CoreType::I32:
                        case ::HIR::CoreType::U32:
                            mask = 0xFFFFFFFF;
                            if(0)
                        case ::HIR::CoreType::I64:
                        case ::HIR::CoreType::U64:
                        case ::HIR::CoreType::I128: // TODO: Proper support for 128 bit integers in consteval
                        case ::HIR::CoreType::U128:
                        case ::HIR::CoreType::Usize:
                        case ::HIR::CoreType::Isize:
                            mask = 0xFFFFFFFFFFFFFFFF;

                            TU_IFLET( ::HIR::Literal, inval, Integer, i,
                                val = ::HIR::Literal(i & mask);
                            )
                            else TU_IFLET( ::HIR::Literal, inval, Float, i,
                                val = ::HIR::Literal( static_cast<uint64_t>(i) & mask);
                            )
                            else {
                                MIR_BUG(state, "Invalid cast of " << inval.tag_str() << " to " << e.type);
                            }
                            break;
                        case ::HIR::CoreType::F32:
                        case ::HIR::CoreType::F64:
                            TU_IFLET( ::HIR::Literal, inval, Integer, i,
                                val = ::HIR::Literal( static_cast<double>(i) );
                            )
                            else TU_IFLET( ::HIR::Literal, inval, Float, i,
                                val = ::HIR::Literal( i );
                            )
                            else {
                                MIR_BUG(state, "Invalid cast of " << inval.tag_str() << " to " << e.type);
                            }
                            break;
                        default:
                            MIR_TODO(state, "RValue::Cast to " << e.type << ", val = " << inval);
                        }
                        }
                    // Allow casting any integer value to a pointer (TODO: Ensure that the pointer is sized?)
                    TU_ARMA(Pointer, te) {
                        TU_IFLET( ::HIR::Literal, inval, Integer, i,
                            val = ::HIR::Literal(i);
                        )
                        else if( inval.is_BorrowPath() || inval.is_BorrowData() ) {
                            val = mv$(inval);
                        }
                        else {
                            MIR_BUG(state, "Invalid cast of " << inval.tag_str() << " to " << e.type);
                        }
                        }
                    TU_ARMA(Borrow, te) {
                        if( inval.is_BorrowData() || inval.is_BorrowPath() ) {
                            val = mv$(inval);
                        }
                        else {
                            MIR_BUG(state, "Invalid cast of " << inval.tag_str() << " to " << e.type);
                        }
                        }
                    }
                    }
                TU_ARMA(BinOp, e) {
                    auto inval_l = read_param(e.val_l);
                    auto inval_r = read_param(e.val_r);
                    if( inval_l.is_Defer() || inval_r.is_Defer() )
                        return ::HIR::Literal::make_Defer({});
                    MIR_ASSERT(state, inval_l.tag() == inval_r.tag(), "Mismatched literal types in binop - " << inval_l << " and " << inval_r);
                    TU_MATCH_HDRA( (inval_l, inval_r), {)
                    default:
                        MIR_TODO(state, "RValue::BinOp - " << sa.src << ", val = " << inval_l << " , " << inval_r);
                    TU_ARMA(Float, l, r) {
                        switch(e.op)
                        {
                        case ::MIR::eBinOp::ADD:    val = ::HIR::Literal( l + r );  break;
                        case ::MIR::eBinOp::SUB:    val = ::HIR::Literal( l - r );  break;
                        case ::MIR::eBinOp::MUL:    val = ::HIR::Literal( l * r );  break;
                        case ::MIR::eBinOp::DIV:    val = ::HIR::Literal( l / r );  break;
                        case ::MIR::eBinOp::MOD:
                        case ::MIR::eBinOp::ADD_OV:
                        case ::MIR::eBinOp::SUB_OV:
                        case ::MIR::eBinOp::MUL_OV:
                        case ::MIR::eBinOp::DIV_OV:
                            MIR_TODO(state, "RValue::BinOp - " << sa.src << ", val = " << inval_l << " , " << inval_r);

                        case ::MIR::eBinOp::BIT_OR :
                        case ::MIR::eBinOp::BIT_AND:
                        case ::MIR::eBinOp::BIT_XOR:
                        case ::MIR::eBinOp::BIT_SHL:
                        case ::MIR::eBinOp::BIT_SHR:
                            MIR_TODO(state, "RValue::BinOp - " << sa.src << ", val = " << inval_l << " , " << inval_r);
                        // TODO: GT/LT are incorrect for signed integers
                        case ::MIR::eBinOp::EQ: val = ::HIR::Literal( static_cast<uint64_t>(l == r) );  break;
                        case ::MIR::eBinOp::NE: val = ::HIR::Literal( static_cast<uint64_t>(l != r) );  break;
                        case ::MIR::eBinOp::GT: val = ::HIR::Literal( static_cast<uint64_t>(l >  r) );  break;
                        case ::MIR::eBinOp::GE: val = ::HIR::Literal( static_cast<uint64_t>(l >= r) );  break;
                        case ::MIR::eBinOp::LT: val = ::HIR::Literal( static_cast<uint64_t>(l <  r) );  break;
                        case ::MIR::eBinOp::LE: val = ::HIR::Literal( static_cast<uint64_t>(l <= r) );  break;
                        }
                        }
                    TU_ARMA(Integer, l, r) {
                        switch(e.op)
                        {
                        case ::MIR::eBinOp::ADD:    val = ::HIR::Literal( l + r );  break;
                        case ::MIR::eBinOp::SUB:    val = ::HIR::Literal( l - r );  break;
                        case ::MIR::eBinOp::MUL:    val = ::HIR::Literal( l * r );  break;
                        case ::MIR::eBinOp::DIV:    val = ::HIR::Literal( l / r );  break;
                        case ::MIR::eBinOp::MOD:    val = ::HIR::Literal( l % r );  break;
                        case ::MIR::eBinOp::ADD_OV:
                        case ::MIR::eBinOp::SUB_OV:
                        case ::MIR::eBinOp::MUL_OV:
                        case ::MIR::eBinOp::DIV_OV:
                            MIR_TODO(state, "RValue::BinOp - " << sa.src << ", val = " << inval_l << " , " << inval_r);

                        case ::MIR::eBinOp::BIT_OR : val = ::HIR::Literal( l | r );  break;
                        case ::MIR::eBinOp::BIT_AND: val = ::HIR::Literal( l & r );  break;
                        case ::MIR::eBinOp::BIT_XOR: val = ::HIR::Literal( l ^ r );  break;
                        case ::MIR::eBinOp::BIT_SHL: val = ::HIR::Literal( l << r );  break;
                        case ::MIR::eBinOp::BIT_SHR: val = ::HIR::Literal( l >> r );  break;
                        // TODO: GT/LT are incorrect for signed integers
                        case ::MIR::eBinOp::EQ: val = ::HIR::Literal( static_cast<uint64_t>(l == r) );  break;
                        case ::MIR::eBinOp::NE: val = ::HIR::Literal( static_cast<uint64_t>(l != r) );  break;
                        case ::MIR::eBinOp::GT: val = ::HIR::Literal( static_cast<uint64_t>(l >  r) );  break;
                        case ::MIR::eBinOp::GE: val = ::HIR::Literal( static_cast<uint64_t>(l >= r) );  break;
                        case ::MIR::eBinOp::LT: val = ::HIR::Literal( static_cast<uint64_t>(l <  r) );  break;
                        case ::MIR::eBinOp::LE: val = ::HIR::Literal( static_cast<uint64_t>(l <= r) );  break;
                        }
                        }
                    }
                    }
                TU_ARMA(UniOp, e) {
                    auto inval = local_state.read_lval(e.val);
                    if( inval.is_Defer() )
                        return ::HIR::Literal::make_Defer({});
                    
                    if( const auto* i = inval.opt_Integer() ) {
                        switch( e.op )
                        {
                        case ::MIR::eUniOp::INV:
                            val = ::HIR::Literal( ~*i );
                            break;
                        case ::MIR::eUniOp::NEG:
                            val = ::HIR::Literal( static_cast<uint64_t>(-static_cast<int64_t>(*i)) );
                            break;
                        }
                    }
                    else if( const auto* i = inval.opt_Float() ) {
                        switch( e.op )
                        {
                        case ::MIR::eUniOp::INV:
                            MIR_BUG(state, "Invalid invert of Float");
                            break;
                        case ::MIR::eUniOp::NEG:
                            val = ::HIR::Literal( -*i );
                            break;
                        }
                    }
                    else {
                        MIR_BUG(state, "Invalid invert of " << inval.tag_str());
                    }
                    }
                TU_ARMA(DstMeta, e) {
                    MIR_TODO(state, "RValue::DstMeta");
                    }
                TU_ARMA(DstPtr, e) {
                    MIR_TODO(state, "RValue::DstPtr");
                    }
                TU_ARMA(MakeDst, e) {
                    auto ptr = read_param(e.ptr_val);
                    if(ptr.is_Defer()) return ::HIR::Literal::make_Defer({});
                    auto meta = read_param(e.meta_val);
                    if(meta.is_Defer()) return ::HIR::Literal::make_Defer({});
                    if( ! meta.is_Integer() ) {
                        MIR_TODO(state, "RValue::MakeDst - (non-integral meta) " << ptr << " , " << meta);
                    }
                    else {
                        val = mv$(ptr);
                    }
                    }
                TU_ARMA(Tuple, e) {
                    ::std::vector< ::HIR::Literal>  vals;
                    vals.reserve( e.vals.size() );
                    for(const auto& v : e.vals) {
                        vals.push_back( read_param(v) );
                        if( vals.back().is_Defer() ) {
                            return ::HIR::Literal::make_Defer({});
                        }
                    }
                    val = ::HIR::Literal::make_List( mv$(vals) );
                    }
                TU_ARMA(Array, e) {
                    ::std::vector< ::HIR::Literal>  vals;
                    vals.reserve( e.vals.size() );
                    for(const auto& v : e.vals) {
                        vals.push_back( read_param(v) );
                        if( vals.back().is_Defer() ) {
                            return ::HIR::Literal::make_Defer({});
                        }
                    }
                    val = ::HIR::Literal::make_List( mv$(vals) );
                    }
                TU_ARMA(Variant, e) {
                    auto ival = read_param(e.val);
                    if(ival.is_Defer()) return ::HIR::Literal::make_Defer({});
                    val = ::HIR::Literal::make_Variant({ e.index, box$(ival) });
                    }
                TU_ARMA(Struct, e) {
                    ::std::vector< ::HIR::Literal>  vals;
                    vals.reserve( e.vals.size() );
                    for(const auto& v : e.vals) {
                        vals.push_back( read_param(v) );
                        if( vals.back().is_Defer() ) {
                            return ::HIR::Literal::make_Defer({});
                        }
                    }
                    val = ::HIR::Literal::make_List( mv$(vals) );
                    }
                }

                auto& dst = local_state.get_lval(sa.dst);
                dst = mv$(val);
            }
            state.set_cur_stmt_term(cur_block);
            DEBUG(state << block.terminator);
            TU_MATCH_HDRA( (block.terminator), {)
            default:
                MIR_BUG(state, "Unexpected terminator - " << block.terminator);
            TU_ARMA(Goto, e) {
                cur_block = e;
                }
            TU_ARMA(Return, e) {
                if( retval.is_Defer() )
                {
                    // 
                }
                else if( exp.m_data.is_Primitive() )
                {
                    switch( exp.m_data.as_Primitive() )
                    {
                    case ::HIR::CoreType::I8:
                    case ::HIR::CoreType::I16:
                    case ::HIR::CoreType::I32:
                        MIR_ASSERT(state, retval.is_Integer(), "Int ret without a integer value - " << retval);
                        break;
                    case ::HIR::CoreType::I64:  case ::HIR::CoreType::U64:
                    case ::HIR::CoreType::I128: case ::HIR::CoreType::U128:
                    case ::HIR::CoreType::Isize: case ::HIR::CoreType::Usize:
                        MIR_ASSERT(state, retval.is_Integer(), "Int ret without a integer value - " << retval);
                        break;
                    case ::HIR::CoreType::U32:
                        MIR_ASSERT(state, retval.is_Integer(), "Int ret without a integer value - " << retval);
                        retval.as_Integer() &= 0xFFFFFFFF;
                        break;
                    case ::HIR::CoreType::U16:
                        MIR_ASSERT(state, retval.is_Integer(), "Int ret without a integer value - " << retval);
                        retval.as_Integer() &= 0xFFFF;
                        break;
                    case ::HIR::CoreType::U8:
                        MIR_ASSERT(state, retval.is_Integer(), "Int ret without a integer value - " << retval);
                        retval.as_Integer() &= 0xFF;
                        break;
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        MIR_ASSERT(state, retval.is_Float(), "Float ret without a float value");
                        break;
                    case ::HIR::CoreType::Char:
                        MIR_ASSERT(state, retval.is_Integer(), "`char` ret without an int value");
                        MIR_ASSERT(state, retval.as_Integer() <= 0x10FFFF, "`char` ret out of range - " << retval);
                        break;
                    case ::HIR::CoreType::Bool:
                        break;
                    case ::HIR::CoreType::Str:
                        MIR_BUG(state, "Bare str return type");
                    }
                }
                return retval;
                }
            TU_ARMA(Call, e) {
                auto& dst = local_state.get_lval(e.ret_val);
                if( const auto* te = e.fcn.opt_Intrinsic() )
                {
                    if( te->name == "size_of" ) {
                        auto ty = ms.monomorph(state.sp, te->params.m_types.at(0));
                        size_t  size_val;
                        Target_GetSizeOf(state.sp, this->resolve, ty, size_val);
                        dst = ::HIR::Literal::make_Integer( size_val );
                    }
                    else {
                        MIR_TODO(state, "Call intrinsic \"" << te->name << "\" - " << block.terminator);
                    }
                }
                else if( const auto* te = e.fcn.opt_Path() )
                {
                    const auto& fcnp_raw = *te;
                    auto fcnp = ms.monomorph(state.sp, fcnp_raw);

                    MonomorphState  fcn_ms;
                    auto& fcn = get_function(this->root_span, this->resolve.m_crate, fcnp, fcn_ms);

                    ::std::vector< ::HIR::Literal>  call_args;
                    call_args.reserve( e.args.size() );
                    for(const auto& a : e.args)
                        call_args.push_back( read_param(a) );
                    // TODO: Set m_const during parse and check here

                    // Call by invoking evaluate_constant on the function
                    {
                        TRACE_FUNCTION_F("Call const fn " << fcnp << " args={ " << call_args << " }");
                        auto fcn_ip = ::HIR::ItemPath(fcnp);
                        const auto* mir = this->resolve.m_crate.get_or_gen_mir( fcn_ip, fcn );
                        MIR_ASSERT(state, mir, "No MIR for function " << fcnp);
                        dst = evaluate_constant_mir(fcn_ip, *mir, mv$(fcn_ms), fcn.m_return.clone(), mv$(call_args));
                    }
                }
                else
                {
                    MIR_BUG(state, "Unexpected terminator - " << block.terminator);
                }
                cur_block = e.ret_block;
                }
            }
        }
    }

    ::HIR::Literal Evaluator::evaluate_constant(const ::HIR::ItemPath& ip, const ::HIR::ExprPtr& expr, ::HIR::TypeRef exp, MonomorphState ms/*={}*/)
    {
        TRACE_FUNCTION_F(ip);
        const auto* mir = this->resolve.m_crate.get_or_gen_mir(ip, expr, exp);

        if( mir ) {
            ::HIR::TypeRef  ty_self { "Self", GENERIC_Self };
            // Might want to have a fully-populated MonomorphState for expanding inside impl blocks
            // HACK: Generate a roughly-correct one
            const auto& top_ip = ip.get_top_ip();
            if( top_ip.trait && !top_ip.ty ) {
                ms.self_ty = &ty_self;
            }
            return evaluate_constant_mir(ip, *mir, mv$(ms), mv$(exp), {});
        }
        else {
            BUG(this->root_span, "Attempting to evaluate constant expression with no associated code");
        }
    }
}   // namespace HIR

namespace {

    void check_lit_type(const Span& sp, const ::HIR::TypeRef& type,  ::HIR::Literal& lit)
    {
        if( lit.is_Defer() )
            return ;
        // TODO: Mask down limited size integers
        TU_MATCHA( (type.m_data), (te),
        (Infer,
            ),
        (Diverge,
            ),
        (Generic,
            ),
        (Slice,
            ),
        (TraitObject,
            ),
        (ErasedType,
            ),
        (Closure,
            ),

        (Path,
            // List
            ),
        (Array,
            // List
            ),
        (Tuple,
            // List
            ),

        (Borrow,
            // A whole host of things
            ),
        (Pointer,
            // Integer, or itemaddr?
            ),
        (Function,
            // ItemAddr
            ),

        (Primitive,
            switch(te)
            {
            case ::HIR::CoreType::Str:
                BUG(sp, "Direct str literal not valid");
            case ::HIR::CoreType::F32:
            case ::HIR::CoreType::F64:
                ASSERT_BUG(sp, lit.is_Float(), "Bad literal type for " << type << " - " << lit);
                break;
            default:
                ASSERT_BUG(sp, lit.is_Integer(), "Bad literal type for " << type << " - " << lit);
                switch(te)
                {
                case ::HIR::CoreType::U8:   lit.as_Integer() &= (1ull<<8)-1;  break;
                case ::HIR::CoreType::U16:  lit.as_Integer() &= (1ull<<16)-1; break;
                case ::HIR::CoreType::U32:  lit.as_Integer() &= (1ull<<32)-1; break;

                case ::HIR::CoreType::I8:   lit.as_Integer() &= (1ull<<8)-1;  break;
                case ::HIR::CoreType::I16:  lit.as_Integer() &= (1ull<<16)-1; break;
                case ::HIR::CoreType::I32:  lit.as_Integer() &= (1ull<<32)-1; break;

                case ::HIR::CoreType::Usize:
                case ::HIR::CoreType::Isize:
                    if( Target_GetCurSpec().m_arch.m_pointer_bits == 32 )
                        lit.as_Integer() &= (1ull<<32)-1;
                    break;

                default:
                    break;
                }
                break;
            }
            )
        )
    }

    class Expander:
        public ::HIR::Visitor
    {
        const ::HIR::Crate& m_crate;
        const ::HIR::Module*  m_mod;
        const ::HIR::ItemPath*  m_mod_path;
        MonomorphState  m_monomorph_state;
        bool m_recurse_types;

    public:
        Expander(const ::HIR::Crate& crate):
            m_crate(crate),
            m_mod(nullptr),
            m_mod_path(nullptr)
            ,m_recurse_types(false)
        {}

        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            auto saved_mp = m_mod_path;
            auto saved_m = m_mod;
            m_mod = &mod;
            m_mod_path = &p;

            ::HIR::Visitor::visit_module(p, mod);

            m_mod = saved_m;
            m_mod_path = saved_mp;
        }
        void visit_function(::HIR::ItemPath p, ::HIR::Function& f) override
        {
            TRACE_FUNCTION_F(p);
            ::HIR::Visitor::visit_function(p, f);
        }

        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            static Span sp;
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type);
            const auto& trait = m_crate.get_trait_by_path(sp, trait_path);

            StaticTraitResolve  resolve( m_crate );
            // - TODO: Defer this call until first missing item?
            resolve.set_impl_generics(impl.m_params);

            auto mp = ::HIR::ItemPath(impl.m_src_module);
            m_mod_path = &mp;
            m_mod = &m_crate.get_mod_by_path(sp, impl.m_src_module);

            for(const auto& vi : trait.m_values)
            {
                // Search for any constants that are in the trait itself, but NOT in this impl
                // - For each of these, find the lowest parent specialisation with the constant set
                // - Ensure that the MIR has been generated for the constant (TODO: This only needs to be done for
                //   specialisations, not trait-provided)
                // - Monomorphise the MIR for this impl, and let expansion happen as usual
                if( vi.second.is_Constant() )
                {
                    if( impl.m_constants.count(vi.first) > 0 )
                        continue;
                    DEBUG("- Constant " << vi.first << " missing, looking for a source");
                    // This trait impl doesn't have this constant, need to find the provided version that applies

                    MonomorphState  ms;
                    ms.self_ty = &impl.m_type;
                    ms.pp_impl = &impl.m_trait_args;

                    resolve.find_impl(sp, trait_path, impl.m_trait_args, impl.m_type, [&](ImplRef found_impl, bool is_fuzzed)->bool {
                        ASSERT_BUG(sp, found_impl.m_data.is_TraitImpl(), "");
                        // If this found impl is the current one, keep searching
                        if( found_impl.m_data.as_TraitImpl().impl == &impl )
                            return false;
                        TODO(sp, "Found a possible parent specialisation of " << trait_path << impl.m_trait_args << " for " << impl.m_type << " - " << found_impl);
                        return false;
                        });
                    const auto& template_const = vi.second.as_Constant();
                    if( template_const.m_value_res.is_Defer() ) {
                        auto nvs = NewvalState { *m_mod, *m_mod_path, FMT("impl" << &impl << "_" << vi.first << "#") };
                        auto eval = ::HIR::Evaluator { sp, m_crate, nvs };
                        ::HIR::ExprPtr  ep;
                        Trans_Params    tp(sp);
                        tp.self_type = ms.self_ty->clone();
                        tp.pp_impl = ms.pp_impl->clone();
                        ep.m_mir = Trans_Monomorphise(resolve, mv$(tp), template_const.m_value.m_mir);
                        ep.m_state = ::HIR::ExprStatePtr( ::HIR::ExprState(*m_mod, m_mod_path->get_simple_path()) );
                        DEBUG("TMP TMP " << trait_path << " - " << ep.m_state->m_mod_path);
                        ep.m_state->stage = ::HIR::ExprState::Stage::Mir;
                        impl.m_constants.insert(::std::make_pair(
                            vi.first,
                            ::HIR::TraitImpl::ImplEnt<::HIR::Constant> {
                                /*is_specialisable=*/false,
                                ::HIR::Constant {
                                    template_const.m_params.clone(),
                                    /*m_type=*/ms.monomorph(sp, template_const.m_type),
                                    /*m_value=*/mv$(ep),
                                    ::HIR::Literal()
                                    }
                                }
                            ));
                    }
                    else {
                        //TODO(sp, "Assign associated type " << vi.first << " in impl" << impl.m_params.fmt_args() << " " << trait_path << impl.m_trait_args << " for " << impl.m_type);
                        impl.m_constants.insert(::std::make_pair(
                            vi.first,
                            ::HIR::TraitImpl::ImplEnt<::HIR::Constant> {
                                /*is_specialisable=*/false,
                                ::HIR::Constant {
                                    template_const.m_params.clone(),
                                    /*m_type=*/ms.monomorph(sp, template_const.m_type),
                                    /*m_value=*/::HIR::ExprPtr(),
                                    template_const.m_value_res.clone()
                                    }
                                }
                            ));
                    }
                }
            }

            ::HIR::PathParams   pp_impl;
            for(const auto& tp : impl.m_params.m_types)
                pp_impl.m_types.push_back( ::HIR::TypeRef(tp.m_name, pp_impl.m_types.size() | 256) );
            m_monomorph_state.pp_impl = &pp_impl;

            ::HIR::Visitor::visit_trait_impl(trait_path, impl);

            m_monomorph_state.pp_impl = nullptr;

            m_mod = nullptr;
            m_mod_path = nullptr;
        }
        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            static Span sp;
            auto mp = ::HIR::ItemPath(impl.m_src_module);
            m_mod_path = &mp;
            m_mod = &m_crate.get_mod_by_path(sp, impl.m_src_module);

            ::HIR::PathParams   pp_impl;
            for(const auto& tp : impl.m_params.m_types)
                pp_impl.m_types.push_back( ::HIR::TypeRef(tp.m_name, pp_impl.m_types.size() | 256) );
            m_monomorph_state.pp_impl = &pp_impl;

            ::HIR::Visitor::visit_type_impl(impl);

            m_mod = nullptr;
            m_mod_path = nullptr;
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            ::HIR::Visitor::visit_type(ty);

            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                TRACE_FUNCTION_FR(ty, ty);
                if( e.size_val == ~0u )
                {
                    assert(e.size);
                    assert(*e.size);
                    const auto& expr_ptr = *e.size;
                    auto ty_name = FMT("ty_" << &ty << "#");

                    auto nvs = NewvalState { *m_mod, *m_mod_path, ty_name };
                    auto eval = ::HIR::Evaluator { expr_ptr->span(), m_crate, nvs };
                    auto val = eval.evaluate_constant(::HIR::ItemPath(*m_mod_path, ty_name.c_str()), expr_ptr, ::HIR::CoreType::Usize);
                    if( val.is_Defer() )
                        TODO(expr_ptr->span(), "Handle defer for array sizes");
                    else if( val.is_Integer() )
                        e.size_val = static_cast<size_t>(val.as_Integer());
                    else
                        ERROR(expr_ptr->span(), E0000, "Array size isn't an integer, got " << val.tag_str());
                }
                DEBUG("Array " << ty << " - size = " << e.size_val);
            )

            if( m_recurse_types )
            {
                m_recurse_types = false;
                if( const auto* te = ty.m_data.opt_Path() )
                {
                    TU_MATCH_HDRA( (te->binding), {)
                    TU_ARMA(Unbound, _) {
                        }
                    TU_ARMA(Opaque, _) {
                        }
                    TU_ARMA(Struct, pbe) {
                        // If this struct hasn't been visited already, visit it
                        this->visit_struct(te->path.m_data.as_Generic().m_path, const_cast<::HIR::Struct&>(*pbe));
                        }
                    TU_ARMA(Union, pbe) {
                        }
                    TU_ARMA(Enum, pbe) {
                        }
                    TU_ARMA(ExternType, pbe) {
                        }
                    }
                }
                m_recurse_types = true;
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override
        {
            m_recurse_types = true;
            ::HIR::Visitor::visit_constant(p, item);
            m_recurse_types = false;

            // NOTE: Consteval needed here for MIR match generation to work
            if( item.m_value || item.m_value.m_mir )
            {
                auto nvs = NewvalState { *m_mod, *m_mod_path, FMT(p.get_name() << "#") };
                auto eval = ::HIR::Evaluator { item.m_value.span(), m_crate, nvs };
                item.m_value_res = eval.evaluate_constant(p, item.m_value, item.m_type.clone(), m_monomorph_state.clone());

                check_lit_type(item.m_value.span(), item.m_type, item.m_value_res);

                DEBUG("constant: " << item.m_type <<  " = " << item.m_value_res);
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override
        {
            m_recurse_types = true;
            ::HIR::Visitor::visit_static(p, item);
            m_recurse_types = false;

            if( item.m_value )
            {
                auto nvs = NewvalState { *m_mod, *m_mod_path, FMT(p.get_name() << "#") };
                auto eval = ::HIR::Evaluator { item.m_value->span(), m_crate, nvs };
                item.m_value_res = eval.evaluate_constant(p, item.m_value, item.m_type.clone());

                check_lit_type(item.m_value->span(), item.m_type, item.m_value_res);

                DEBUG("static: " << item.m_type <<  " = " << item.m_value_res);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            static Span sp;
            if( auto* e = item.m_data.opt_Value() )
            {
                auto ty = ::HIR::Enum::get_repr_type(e->repr);
                uint64_t i = 0;
                for(auto& var : e->variants)
                {
                    if( var.expr )
                    {
                        auto nvs = NewvalState { *m_mod, *m_mod_path, FMT(p.get_name() << "_" << var.name << "#") }; 
                        auto eval = ::HIR::Evaluator { var.expr->span(), m_crate, nvs };
                        auto val = eval.evaluate_constant(p, var.expr, ty.clone());
                        DEBUG("enum variant: " << p << "::" << var.name << " = " << val);
                        i = val.as_Integer();
                    }
                    var.val = i;
                    i ++;
                }
            }
            ::HIR::Visitor::visit_enum(p, item);
        }
        void visit_struct(::HIR::ItemPath p, ::HIR::Struct& item) override {
            if( item.const_eval_state != HIR::ConstEvalState::Complete )
            {
                ASSERT_BUG(Span(), item.const_eval_state == HIR::ConstEvalState::None, "Constant evaluation loop involving " << p);
                item.const_eval_state = HIR::ConstEvalState::Active;
                ::HIR::Visitor::visit_struct(p, item);
                item.const_eval_state = HIR::ConstEvalState::Complete;
            }
        }

        void visit_expr(::HIR::ExprPtr& expr) override
        {
            struct Visitor:
                public ::HIR::ExprVisitorDef
            {
                Expander& m_exp;

                Visitor(Expander& exp):
                    m_exp(exp)
                {}

                void visit_type(::HIR::TypeRef& ty) override {
                    // Need to evaluate array sizes
                    DEBUG("expr type " << ty);
                    m_exp.visit_type(ty);
                }
                void visit_path_params(::HIR::PathParams& pp) override {
                    // Explicit call to handle const params (eventually)
                    m_exp.visit_path_params(pp);
                }

                void visit(::HIR::ExprNode_ArraySized& node) override {
                    assert( node.m_size );
                    auto name = FMT("array_" << &node << "#");
                    auto nvs = NewvalState { *m_exp.m_mod, *m_exp.m_mod_path, name };
                    auto eval = ::HIR::Evaluator { node.span(), m_exp.m_crate, nvs };
                    auto val = eval.evaluate_constant( ::HIR::ItemPath(*m_exp.m_mod_path, name.c_str()), node.m_size, ::HIR::CoreType::Usize );
                    if( !val.is_Integer() )
                        ERROR(node.span(), E0000, "Array size isn't an integer");
                    node.m_size_val = static_cast<size_t>(val.as_Integer());
                    DEBUG("Array literal [?; " << node.m_size_val << "]");
                }
            };

            if( expr.get() != nullptr )
            {
                Visitor v { *this };
                //m_recurse_types = true;
                (*expr).visit(v);
                //m_recurse_types = false;
            }
        }
    };

    class ExpanderApply:
        public ::HIR::Visitor
    {

    public:
        ExpanderApply()
        {
        }

        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            if( ! mod.m_inline_statics.empty() )
            {
                for(auto& v : mod.m_inline_statics)
                {
                    // ::std::unique_ptr<VisEnt<ValueItem>>
                    ::std::unique_ptr<::HIR::VisEnt<::HIR::ValueItem>>  iv;
                    iv.reset( new ::HIR::VisEnt<::HIR::ValueItem> { ::HIR::Publicity::new_none(), ::HIR::ValueItem::make_Static(mv$(v.second)) } );
                    mod.m_value_items.insert(::std::make_pair( v.first, mv$(iv) ));
                }
                mod.m_inline_statics.clear();
            }

            ::HIR::Visitor::visit_module(p, mod);

        }
    };
}   // namespace

void ConvertHIR_ConstantEvaluate(::HIR::Crate& crate)
{
    Expander    exp { crate };
    exp.visit_crate( crate );

    ExpanderApply().visit_crate(crate);
}
void ConvertHIR_ConstantEvaluate_Expr(const ::HIR::Crate& crate, const ::HIR::ItemPath& ip, ::HIR::ExprPtr& expr_ptr)
{
    TRACE_FUNCTION_F(ip);
    // Check innards but NOT the value
    Expander    exp { crate };
    exp.visit_expr( expr_ptr );
}
