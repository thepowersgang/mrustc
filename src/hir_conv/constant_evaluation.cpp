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

namespace {
    struct NewvalState {
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

        ::HIR::SimplePath new_static(::HIR::TypeRef type, ::HIR::Literal value)
        {
            auto name = FMT(name_prefix << next_item_idx);
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
    struct Evaluator
    {
        const Span& root_span;
        StaticTraitResolve  resolve;
        NewvalState nvs;

        Evaluator(const Span& sp, const ::HIR::Crate& crate, NewvalState nvs):
            root_span(sp),
            resolve(crate),
            nvs( ::std::move(nvs) )
        {
        }

        ::HIR::Literal evaluate_constant(const ::HIR::ItemPath& ip, const ::HIR::ExprPtr& expr, ::HIR::TypeRef exp);

        ::HIR::Literal evaluate_constant_mir(const ::MIR::Function& fcn, MonomorphState ms, ::HIR::TypeRef exp, ::std::vector< ::HIR::Literal> args);
    };

    ::HIR::Literal clone_literal(const ::HIR::Literal& v)
    {
        TU_MATCH(::HIR::Literal, (v), (e),
        (Invalid,
            return ::HIR::Literal();
            ),
        (List,
            ::std::vector< ::HIR::Literal>  vals;
            for(const auto& val : e) {
                vals.push_back( clone_literal(val) );
            }
            return ::HIR::Literal( mv$(vals) );
            ),
        (Variant,
            return ::HIR::Literal::make_Variant({ e.idx, box$(clone_literal(*e.val)) });
            ),
        (Integer,
            return ::HIR::Literal(e);
            ),
        (Float,
            return ::HIR::Literal(e);
            ),
        (BorrowPath,
            return ::HIR::Literal(e.clone());
            ),
        (BorrowData,
            return ::HIR::Literal(box$( clone_literal(*e) ));
            ),
        (String,
            return ::HIR::Literal(e);
            )
        )
        throw "";
    }

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

    ::HIR::Literal Evaluator::evaluate_constant_mir(const ::MIR::Function& fcn, MonomorphState ms, ::HIR::TypeRef exp, ::std::vector< ::HIR::Literal> args)
    {
        // TODO: Full-blown miri
        TRACE_FUNCTION_F("exp=" << exp << ", args=" << args);

        ::MIR::TypeResolve  state { this->root_span, this->resolve, FMT_CB(,), exp, {}, fcn };

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
                TU_MATCHA( (lv), (e),
                (Return,
                    return retval;
                    ),
                (Local,
                    if( e >= locals.size() )
                        MIR_BUG(state, "Local index out of range - " << e << " >= " << locals.size());
                    return locals[e];
                    ),
                (Argument,
                    if( e.idx >= args.size() )
                        MIR_BUG(state, "Local index out of range - " << e.idx << " >= " << args.size());
                    return args[e.idx];
                    ),
                (Static,
                    MIR_TODO(state, "LValue::Static - " << e);
                    ),
                (Field,
                    auto& val = get_lval(*e.val);
                    MIR_ASSERT(state, val.is_List(), "LValue::Field on non-list literal - " << val.tag_str() << " - " << lv);
                    auto& vals = val.as_List();
                    MIR_ASSERT(state, e.field_index < vals.size(), "LValue::Field index out of range");
                    return vals[ e.field_index ];
                    ),
                (Deref,
                    auto& val = get_lval(*e.val);
                    TU_MATCH_DEF( ::HIR::Literal, (val), (ve),
                    (
                        MIR_TODO(state, "LValue::Deref - " << lv << " { " << val << " }");
                        ),
                    (BorrowData,
                        return *ve;
                        ),
                    (String,
                        // Just clone the string (hack)
                        // - TODO: Create a list?
                        return val;
                        )
                    )
                    ),
                (Index,
                    auto& val = get_lval(*e.val);
                    MIR_ASSERT(state, val.is_List(), "LValue::Index on non-list literal - " << val.tag_str() << " - " << lv);
                    auto& idx = get_lval(*e.idx);
                    MIR_ASSERT(state, idx.is_Integer(), "LValue::Index with non-integer index literal - " << idx.tag_str() << " - " << lv);
                    auto& vals = val.as_List();
                    auto idx_v = static_cast<size_t>( idx.as_Integer() );
                    MIR_ASSERT(state, idx_v < vals.size(), "LValue::Index index out of range");
                    return vals[ idx_v ];
                    ),
                (Downcast,
                    MIR_TODO(state, "LValue::Downcast - " << lv);
                    )
                )
                throw "";
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
            TU_MATCH(::MIR::Constant, (c), (e2),
            (Int,
                return ::HIR::Literal(static_cast<uint64_t>(e2.v));
                ),
            (Uint,
                return ::HIR::Literal(e2.v);
                ),
            (Float,
                return ::HIR::Literal(e2.v);
                ),
            (Bool,
                return ::HIR::Literal(static_cast<uint64_t>(e2.v));
                ),
            (Bytes,
                return ::HIR::Literal::make_String({e2.begin(), e2.end()});
                ),
            (StaticString,
                return ::HIR::Literal(e2);
                ),
            (Const,
                auto p = ms.monomorph(state.sp, e2.p);
                MonomorphState  const_ms;
                auto ent = get_ent_fullpath(state.sp, this->resolve.m_crate, p, EntNS::Value,  const_ms);
                MIR_ASSERT(state, ent.is_Constant(), "MIR Constant::Const(" << p << ") didn't point to a Constant - " << ent.tag_str());
                const auto& c = *ent.as_Constant();
                if( c.m_value_res.is_Invalid() )
                {
                    auto& item = const_cast<::HIR::Constant&>(c);
                    // Challenge: Adding items to the module might invalidate an iterator.
                    ::HIR::ItemPath mod_ip { item.m_value.m_state->m_mod_path };
                    auto eval = Evaluator { item.m_value->span(), resolve.m_crate, NewvalState { item.m_value.m_state->m_module, mod_ip, FMT(&c << "$") } };
                    DEBUG("- Evaluate " << p);
                    DEBUG("- " << ::HIR::ItemPath(p));
                    item.m_value_res = eval.evaluate_constant(::HIR::ItemPath(p), item.m_value, item.m_type.clone());

                    //check_lit_type(item.m_value->span(), item.m_type, item.m_value_res);
                }
                return clone_literal( c.m_value_res );
                ),
            (ItemAddr,
                return ::HIR::Literal::make_BorrowPath( ms.monomorph(state.sp, e2) );
                )
            )
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
            unsigned int next_stmt_idx = 0;
            for(const auto& stmt : block.statements)
            {
                state.set_cur_stmt(cur_block, next_stmt_idx++);
                DEBUG(state << stmt);

                if( ! stmt.is_Assign() ) {
                    //MIR_BUG(state, "Non-assign statement - drop " << stmt.as_Drop().slot);
                    continue ;
                }

                ::HIR::Literal  val;
                const auto& sa = stmt.as_Assign();
                TU_MATCHA( (sa.src), (e),
                (Use,
                    val = local_state.read_lval(e);
                    ),
                (Constant,
                    val = const_to_lit(e);
                    ),
                (SizedArray,
                    ::std::vector< ::HIR::Literal>  vals;
                    if( e.count > 0 )
                    {
                        vals.reserve( e.count );
                        val = read_param(e.val);
                        for(unsigned int i = 1; i < e.count; i++)
                            vals.push_back( clone_literal(val) );
                        vals.push_back( mv$(val) );
                    }
                    val = ::HIR::Literal::make_List( mv$(vals) );
                    ),
                (Borrow,
                    if( e.type != ::HIR::BorrowType::Shared ) {
                        MIR_BUG(state, "Only shared borrows are allowed in constants");
                    }

                    if( e.type != ::HIR::BorrowType::Shared ) {
                        MIR_BUG(state, "Only shared borrows are allowed in constants");
                    }
                    if( const auto* p = e.val.opt_Deref() ) {
                        if( p->val->is_Deref() )
                            MIR_TODO(state, "Undo nested deref coercion - " << *p->val);
                        val = local_state.read_lval(*p->val);
                    }
                    else if( const auto* p = e.val.opt_Static() ) {
                        // Borrow of a static, emit BorrowPath with the same path
                        val = ::HIR::Literal::make_BorrowPath( p->clone() );
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
                    ),
                (Cast,
                    auto inval = local_state.read_lval(e.val);
                    TU_MATCH_DEF(::HIR::TypeRef::Data, (e.type.m_data), (te),
                    (
                        // NOTE: Can be an unsizing!
                        MIR_TODO(state, "RValue::Cast to " << e.type << ", val = " << inval);
                        ),
                    (Primitive,
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
                        ),
                    // Allow casting any integer value to a pointer (TODO: Ensure that the pointer is sized?)
                    (Pointer,
                        TU_IFLET( ::HIR::Literal, inval, Integer, i,
                            val = ::HIR::Literal(i);
                        )
                        else if( inval.is_BorrowPath() || inval.is_BorrowData() ) {
                            val = mv$(inval);
                        }
                        else {
                            MIR_BUG(state, "Invalid cast of " << inval.tag_str() << " to " << e.type);
                        }
                        ),
                    (Borrow,
                        if( inval.is_BorrowData() || inval.is_BorrowPath() ) {
                            val = mv$(inval);
                        }
                        else {
                            MIR_BUG(state, "Invalid cast of " << inval.tag_str() << " to " << e.type);
                        }
                        )
                    )
                    ),
                (BinOp,
                    auto inval_l = read_param(e.val_l);
                    auto inval_r = read_param(e.val_r);
                    MIR_ASSERT(state, inval_l.tag() == inval_r.tag(), "Mismatched literal types in binop - " << inval_l << " and " << inval_r);
                    TU_MATCH_DEF( ::HIR::Literal, (inval_l, inval_r), (l, r),
                    (
                        MIR_TODO(state, "RValue::BinOp - " << sa.src << ", val = " << inval_l << " , " << inval_r);
                        ),
                    (Float,
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
                        ),
                    (Integer,
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
                        )
                    )
                    ),
                (UniOp,
                    auto inval = local_state.read_lval(e.val);
                    TU_IFLET( ::HIR::Literal, inval, Integer, i,
                        switch( e.op )
                        {
                        case ::MIR::eUniOp::INV:
                            val = ::HIR::Literal( ~i );
                            break;
                        case ::MIR::eUniOp::NEG:
                            val = ::HIR::Literal( -i );
                            break;
                        }
                    )
                    else TU_IFLET( ::HIR::Literal, inval, Float, i,
                        switch( e.op )
                        {
                        case ::MIR::eUniOp::INV:
                            MIR_BUG(state, "Invalid invert of Float");
                            break;
                        case ::MIR::eUniOp::NEG:
                            val = ::HIR::Literal( -i );
                            break;
                        }
                    )
                    else {
                        MIR_BUG(state, "Invalid invert of " << inval.tag_str());
                    }
                    ),
                (DstMeta,
                    MIR_TODO(state, "RValue::DstMeta");
                    ),
                (DstPtr,
                    MIR_TODO(state, "RValue::DstPtr");
                    ),
                (MakeDst,
                    auto ptr = read_param(e.ptr_val);
                    auto meta = read_param(e.meta_val);
                    if( ! meta.is_Integer() ) {
                        MIR_TODO(state, "RValue::MakeDst - (non-integral meta) " << ptr << " , " << meta);
                    }
                    else {
                        val = mv$(ptr);
                    }
                    ),
                (Tuple,
                    ::std::vector< ::HIR::Literal>  vals;
                    vals.reserve( e.vals.size() );
                    for(const auto& v : e.vals)
                        vals.push_back( read_param(v) );
                    val = ::HIR::Literal::make_List( mv$(vals) );
                    ),
                (Array,
                    ::std::vector< ::HIR::Literal>  vals;
                    vals.reserve( e.vals.size() );
                    for(const auto& v : e.vals)
                        vals.push_back( read_param(v) );
                    val = ::HIR::Literal::make_List( mv$(vals) );
                    ),
                (Variant,
                    auto ival = read_param(e.val);
                    val = ::HIR::Literal::make_Variant({ e.index, box$(ival) });
                    ),
                (Struct,
                    ::std::vector< ::HIR::Literal>  vals;
                    vals.reserve( e.vals.size() );
                    for(const auto& v : e.vals)
                        vals.push_back( read_param(v) );
                    val = ::HIR::Literal::make_List( mv$(vals) );
                    )
                )

                auto& dst = local_state.get_lval(sa.dst);
                dst = mv$(val);
            }
            state.set_cur_stmt_term(cur_block);
            TU_MATCH_DEF( ::MIR::Terminator, (block.terminator), (e),
            (
                MIR_BUG(state, "Unexpected terminator - " << block.terminator);
                ),
            (Goto,
                cur_block = e;
                ),
            (Return,
                if( exp.m_data.is_Primitive() )
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
                ),
            (Call,
                if( !e.fcn.is_Path() )
                    MIR_BUG(state, "Unexpected terminator - " << block.terminator);
                const auto& fcnp_raw = e.fcn.as_Path();
                auto fcnp = ms.monomorph(state.sp, fcnp_raw);

                auto& dst = local_state.get_lval(e.ret_val);
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
                    const auto* mir = this->resolve.m_crate.get_or_gen_mir( ::HIR::ItemPath(fcnp.clone()), fcn );
                    MIR_ASSERT(state, mir, "No MIR for function " << fcnp);
                    dst = evaluate_constant_mir(*mir, mv$(fcn_ms), fcn.m_return.clone(), mv$(call_args));
                }

                cur_block = e.ret_block;
                )
            )
        }
    }

    ::HIR::Literal Evaluator::evaluate_constant(const ::HIR::ItemPath& ip, const ::HIR::ExprPtr& expr, ::HIR::TypeRef exp)
    {
        TRACE_FUNCTION_F(ip);
        const auto* mir = this->resolve.m_crate.get_or_gen_mir(ip, expr, exp);

        if( mir ) {
            return evaluate_constant_mir(*mir, {}, mv$(exp), {});
        }
        else {
            BUG(this->root_span, "Attempting to evaluate constant expression with no associated code");
        }
    }

    void check_lit_type(const Span& sp, const ::HIR::TypeRef& type,  ::HIR::Literal& lit)
    {
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

    public:
        Expander(const ::HIR::Crate& crate):
            m_crate(crate),
            m_mod(nullptr),
            m_mod_path(nullptr)
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

        void visit_type(::HIR::TypeRef& ty) override
        {
            ::HIR::Visitor::visit_type(ty);

            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                if( e.size_val == ~0u )
                {
                    assert(e.size);
                    assert(*e.size);
                    const auto& expr_ptr = *e.size;
                    auto ty_name = FMT("ty_" << &ty << "$");

                    auto eval = Evaluator { expr_ptr->span(), m_crate, NewvalState { *m_mod, *m_mod_path, ty_name } };
                    auto val = eval.evaluate_constant(::HIR::ItemPath(*m_mod_path, ty_name.c_str()), expr_ptr, ::HIR::CoreType::Usize);
                    if( !val.is_Integer() )
                        ERROR(expr_ptr->span(), E0000, "Array size isn't an integer");
                    e.size_val = static_cast<size_t>(val.as_Integer());
                }
                DEBUG("Array " << ty << " - size = " << e.size_val);
            )
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override
        {
            ::HIR::Visitor::visit_constant(p, item);

            // NOTE: Consteval needed here for MIR match generation to work
            if( item.m_value )
            {
                auto eval = Evaluator { item.m_value->span(), m_crate, NewvalState { *m_mod, *m_mod_path, FMT(p.get_name() << "$") } };
                item.m_value_res = eval.evaluate_constant(p, item.m_value, item.m_type.clone());

                check_lit_type(item.m_value->span(), item.m_type, item.m_value_res);

                DEBUG("constant: " << item.m_type <<  " = " << item.m_value_res);
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override
        {
            ::HIR::Visitor::visit_static(p, item);

            if( item.m_value )
            {
                auto eval = Evaluator { item.m_value->span(), m_crate, NewvalState { *m_mod, *m_mod_path, FMT(p.get_name() << "$") } };
                item.m_value_res = eval.evaluate_constant(p, item.m_value, item.m_type.clone());

                check_lit_type(item.m_value->span(), item.m_type, item.m_value_res);

                DEBUG("static: " << item.m_type <<  " = " << item.m_value_res);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            if( auto* e = item.m_data.opt_Value() )
            {
                ::HIR::TypeRef  ty = ::HIR::CoreType::Isize;
                uint64_t i = 0;
                for(auto& var : e->variants)
                {
                    if( var.expr )
                    {
                        auto eval = Evaluator { var.expr->span(), m_crate, NewvalState { *m_mod, *m_mod_path, FMT(p.get_name() << "$" << var.name << "$") } };
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
                    m_exp.visit_type(ty);
                }
                void visit_path_params(::HIR::PathParams& pp) override {
                    // Explicit call to handle const params (eventually)
                    m_exp.visit_path_params(pp);
                }

                void visit(::HIR::ExprNode_ArraySized& node) override {
                    assert( node.m_size );
                    auto name = FMT("array_" << &node << "$");
                    auto eval = Evaluator { node.span(), m_exp.m_crate, NewvalState { *m_exp.m_mod, *m_exp.m_mod_path, name } };
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
                (*expr).visit(v);
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
                    iv.reset( new ::HIR::VisEnt<::HIR::ValueItem> { false, ::HIR::ValueItem::make_Static(mv$(v.second)) } );
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
