/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/const_eval_full.cpp
 * - More-complete constant evaluation
 *
 * NOTE: This is run _after_ MIR lowering (not before, as the rest of this
 * folder is)
 */
#include "main_bindings.hpp"
#include <hir/hir.hpp>
#include <hir/expr.hpp>
#include <hir/visitor.hpp>
#include <algorithm>
#include <mir/mir.hpp>
#include <hir_typeck/common.hpp>    // Monomorph
#include <mir/helpers.hpp>

namespace {
    typedef ::std::vector< ::std::pair< ::std::string, ::HIR::Static> > t_new_values;

    struct NewvalState {
        t_new_values&   newval_output;
        const ::HIR::ItemPath&  mod_path;
        ::std::string   name_prefix;
        unsigned int next_item_idx;

        NewvalState(t_new_values& newval_output, const ::HIR::ItemPath& mod_path, ::std::string prefix):
            newval_output(newval_output),
            mod_path(mod_path),
            name_prefix(prefix),
            next_item_idx(0)
        {
        }
        NewvalState(const NewvalState&) = delete;
        NewvalState(NewvalState&&) = default;

        ::HIR::SimplePath new_static(::HIR::TypeRef type, ::HIR::Literal value)
        {
            auto name = FMT(name_prefix << next_item_idx);
            next_item_idx ++;
            auto rv = (mod_path + name.c_str()).get_simple_path();
            newval_output.push_back( ::std::make_pair( mv$(name), ::HIR::Static {
                ::HIR::Linkage(),
                false,
                mv$(type),
                ::HIR::ExprPtr(),
                mv$(value)
                } ) );
            return rv;
        }
    };

    ::HIR::Literal evaluate_constant(const Span& sp, const ::StaticTraitResolve& resolve, NewvalState& newval_state, FmtLambda name, const ::HIR::ExprPtr& expr, MonomorphState ms, ::std::vector< ::HIR::Literal> args);

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
            return ::HIR::Literal(box$(clone_literal(*e)));
            ),
        (String,
            return ::HIR::Literal(e);
            )
        )
        throw "";
    }

    void monomorph_literal_inplace(const Span& sp, ::HIR::Literal& lit, const MonomorphState& ms)
    {
        TU_MATCH(::HIR::Literal, (lit), (e),
        (Invalid,
            ),
        (List,
            for(auto& val : e) {
                monomorph_literal_inplace(sp, val, ms);
            }
            ),
        (Variant,
            monomorph_literal_inplace(sp, *e.val, ms);
            ),
        (Integer,
            ),
        (Float,
            ),
        (BorrowPath,
            DEBUG(e);
            e = ms.monomorph(sp, e);
            // TODO: expand associated types
            ),
        (BorrowData,
            monomorph_literal_inplace(sp, *e, ms);
            ),
        (String,
            )
        )
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

    ::HIR::Literal evaluate_constant_mir(const Span& sp, const StaticTraitResolve& resolve, NewvalState& newval_state, FmtLambda name, const ::MIR::Function& fcn, MonomorphState ms, ::std::vector< ::HIR::Literal> args)
    {
        TRACE_FUNCTION;

        ::MIR::TypeResolve  state { sp, resolve, name, ::HIR::TypeRef(), {}, fcn };

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
        };
        LocalState  local_state( state, retval, args, locals );

        auto get_lval = [&](const ::MIR::LValue& lv) -> ::HIR::Literal& { return local_state.get_lval(lv); };
        auto read_lval = [&](const ::MIR::LValue& lv) -> ::HIR::Literal {
            auto& v = get_lval(lv);
            TU_MATCH_DEF(::HIR::Literal, (v), (e),
            (
                return mv$(v);
                ),
            (Invalid,
                BUG(sp, "Read of lvalue with Literal::Invalid - " << lv);
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
            };
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
                MonomorphState  const_ms;
                // TODO: Monomorph the path? (Not needed... yet)
                auto ent = get_ent_fullpath(sp, resolve.m_crate, e2.p, EntNS::Value,  const_ms);
                ASSERT_BUG(sp, ent.is_Constant(), "MIR Constant::Const("<<e2.p<<") didn't point to a Constant - " << ent.tag_str());
                const auto& c = *ent.as_Constant();
                // Prefer re-evaluating the MIR.
                // - Constants insert themselves directly, so this is
                //   effectively the same thing.
                // Avoids _BorrowData leftovers.
                if( c.m_value ) {
                    return evaluate_constant(sp, resolve, newval_state, FMT_CB(ss, ss << e2.p;), ent.as_Constant()->m_value, {}, {});
                }
                else {
                    auto val = clone_literal( ent.as_Constant()->m_value_res );
                    ASSERT_BUG(sp, !val.is_Invalid(), "MIR Constant::Const("<<e2.p<<") pointed to invalid Constant - (no mir, no literal)");
                    // Monomorphise the value according to `const_ms`
                    monomorph_literal_inplace(sp, val, const_ms);
                    return val;
                }
                ),
            (ItemAddr,
                return ::HIR::Literal::make_BorrowPath( ms.monomorph(sp, e2) );
                )
            )
            throw "";
            };
        auto read_param = [&](const ::MIR::Param& p) ->::HIR::Literal {
            TU_MATCH(::MIR::Param, (p), (e),
            (LValue,
                return read_lval(e);
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

                if( ! stmt.is_Assign() ) {
                    //BUG(sp, "Non-assign statement - drop " << stmt.as_Drop().slot);
                    continue ;
                }
                const auto& sa = stmt.as_Assign();

                DEBUG(sa.dst << " = " << sa.src);
                ::HIR::Literal  val;
                TU_MATCHA( (sa.src), (e),
                (Use,
                    val = read_lval(e);
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
                    if( const auto* p = e.val.opt_Deref() ) {
                        if( p->val->is_Deref() )
                            MIR_TODO(state, "Undo nested deref coercion - " << *p->val);
                        val = read_lval(*p->val);
                    }
                    else if( const auto* p = e.val.opt_Static() ) {
                        // Borrow of a static, emit BorrowPath with the same path
                        val = ::HIR::Literal::make_BorrowPath( p->clone() );
                    }
                    else {
                        auto inner_val = read_lval(e.val);

                        ::HIR::TypeRef  inner_ty;
                        const auto& inner_ty_r = state.get_lvalue_type(inner_ty, e.val);
                        if( &inner_ty_r != &inner_ty )
                            inner_ty = inner_ty_r.clone();

                        // Create new static containing borrowed data
                        // NOTE: Doesn't use BorrowData
                        auto item_path = newval_state.new_static( mv$(inner_ty), mv$(inner_val) );
                        val = ::HIR::Literal::make_BorrowPath( mv$(item_path) );
                    }
                    ),
                (Cast,
                    auto inval = read_lval(e.val);
                    TU_MATCH_DEF(::HIR::TypeRef::Data, (e.type.m_data), (te),
                    (
                        // NOTE: Can be an unsizing!
                        TODO(sp, "RValue::Cast to " << e.type << ", val = " << inval);
                        ),
                    (Primitive,
                        auto cast_to_int = [&inval,&e,&sp](bool is_signed, unsigned bits) {
                            uint64_t mask = (bits >= 64 ? ~0ull : (1ull << bits) - 1);
                            uint64_t outval;
                            TU_IFLET( ::HIR::Literal, inval, Integer, i,
                                outval = i & mask;
                                if( bits < 64 && is_signed && (outval >> (bits-1)) )
                                    outval |= ~mask;
                            )
                            else TU_IFLET( ::HIR::Literal, inval, Float, i,
                                outval = static_cast<uint64_t>(i) & mask;
                                if( is_signed && i < 0 )
                                    outval |= ~mask;
                            )
                            else {
                                BUG(sp, "Invalid cast of " << inval.tag_str() << " to " << e.type);
                            }
                            return ::HIR::Literal(outval);
                            };
                        switch(te)
                        {
                        // Integers mask down
                        case ::HIR::CoreType::I8:   val = cast_to_int(true , 8);    break;
                        case ::HIR::CoreType::U8:   val = cast_to_int(false, 8);    break;
                        case ::HIR::CoreType::I16:  val = cast_to_int(true , 16);   break;
                        case ::HIR::CoreType::U16:  val = cast_to_int(false, 16);   break;
                        case ::HIR::CoreType::I32:  val = cast_to_int(true , 32);   break;
                        case ::HIR::CoreType::U32:  val = cast_to_int(false, 32);   break;

                        case ::HIR::CoreType::I64:  val = cast_to_int(true , 64);   break;
                        case ::HIR::CoreType::U64:  val = cast_to_int(false, 64);   break;
                        case ::HIR::CoreType::I128: val = cast_to_int(true , 64);   break;
                        case ::HIR::CoreType::U128: val = cast_to_int(false, 64);   break;
                        case ::HIR::CoreType::Isize: val = cast_to_int(true , 64);   break;
                        case ::HIR::CoreType::Usize: val = cast_to_int(false, 64);   break;

                        case ::HIR::CoreType::F32:
                        case ::HIR::CoreType::F64:
                            TU_IFLET( ::HIR::Literal, inval, Integer, i,
                                val = ::HIR::Literal( static_cast<double>(i) );
                            )
                            else TU_IFLET( ::HIR::Literal, inval, Float, i,
                                val = ::HIR::Literal( i );
                            )
                            else {
                                BUG(sp, "Invalid cast of " << inval.tag_str() << " to " << e.type);
                            }
                            break;
                        default:
                            TODO(sp, "RValue::Cast to " << e.type << ", val = " << inval);
                        }
                        ),
                    // Allow casting any integer value to a pointer (TODO: Ensure that the pointer is sized?)
                    (Pointer,
                        TU_IFLET( ::HIR::Literal, inval, Integer, i,
                            val = ::HIR::Literal(i);
                        )
                        else if( inval.is_BorrowData() || inval.is_BorrowPath() ) {
                            val = mv$(inval);
                        }
                        else {
                            BUG(sp, "Invalid cast of " << inval.tag_str() << " to " << e.type);
                        }
                        ),
                    (Borrow,
                        if( inval.is_BorrowData() || inval.is_BorrowPath() ) {
                            val = mv$(inval);
                        }
                        else {
                            BUG(sp, "Invalid cast of " << inval.tag_str() << " to " << e.type);
                        }
                        )
                    )
                    ),
                (BinOp,
                    auto inval_l = read_param(e.val_l);
                    auto inval_r = read_param(e.val_r);
                    ASSERT_BUG(sp, inval_l.tag() == inval_r.tag(), "Mismatched literal types in binop - " << inval_l << " and " << inval_r);
                    TU_MATCH_DEF( ::HIR::Literal, (inval_l, inval_r), (l, r),
                    (
                        TODO(sp, "RValue::BinOp - " << sa.src << ", val = " << inval_l << " , " << inval_r);
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
                            TODO(sp, "RValue::BinOp - " << sa.src << ", val = " << inval_l << " , " << inval_r);

                        case ::MIR::eBinOp::BIT_OR :
                        case ::MIR::eBinOp::BIT_AND:
                        case ::MIR::eBinOp::BIT_XOR:
                        case ::MIR::eBinOp::BIT_SHL:
                        case ::MIR::eBinOp::BIT_SHR:
                            TODO(sp, "RValue::BinOp - " << sa.src << ", val = " << inval_l << " , " << inval_r);
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
                            TODO(sp, "RValue::BinOp - " << sa.src << ", val = " << inval_l << " , " << inval_r);

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
                    auto inval = read_lval(e.val);
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
                            BUG(sp, "Invalid invert of Float");
                            break;
                        case ::MIR::eUniOp::NEG:
                            val = ::HIR::Literal( -i );
                            break;
                        }
                    )
                    else {
                        BUG(sp, "Invalid invert of " << inval.tag_str());
                    }
                    ),
                (DstMeta,
                    TODO(sp, "RValue::DstMeta");
                    ),
                (DstPtr,
                    TODO(sp, "RValue::DstPtr");
                    ),
                (MakeDst,
                    auto ptr = read_param(e.ptr_val);
                    auto meta = read_param(e.meta_val);
                    if( ! meta.is_Integer() ) {
                        TODO(sp, "RValue::MakeDst - (non-integral meta) " << ptr << " , " << meta);
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

                auto& dst = get_lval(sa.dst);
                DEBUG("= " << val);
                dst = mv$(val);
            }
            state.set_cur_stmt_term(cur_block);
            DEBUG("> " << block.terminator);
            TU_MATCH_DEF( ::MIR::Terminator, (block.terminator), (e),
            (
                BUG(sp, "Unexpected terminator - " << block.terminator);
                ),
            (Goto,
                cur_block = e;
                ),
            (Return,
                return retval;
                ),
            (Call,
                if( !e.fcn.is_Path() )
                    BUG(sp, "Unexpected terminator - " << block.terminator);
                const auto& fcnp_raw = e.fcn.as_Path();
                auto fcnp = ms.monomorph(sp, fcnp_raw);

                auto& dst = get_lval(e.ret_val);
                MonomorphState  fcn_ms;
                auto& fcn = get_function(sp, resolve.m_crate, fcnp, fcn_ms);

                ::std::vector< ::HIR::Literal>  call_args;
                call_args.reserve( e.args.size() );
                for(const auto& a : e.args)
                    call_args.push_back( read_param(a) );
                // TODO: Set m_const during parse and check here

                // Call by invoking evaluate_constant on the function
                {
                    TRACE_FUNCTION_F("Call const fn " << fcnp << " args={ " << call_args << " }");
                    dst = evaluate_constant(sp, resolve, newval_state, FMT_CB(ss, ss << fcnp;),  fcn.m_code, mv$(fcn_ms), mv$(call_args));
                }

                DEBUG("= " << dst);
                cur_block = e.ret_block;
                )
            )
        }
    }

    ::HIR::Literal evaluate_constant(const Span& sp, const StaticTraitResolve& resolve, NewvalState& newval_state, FmtLambda name, const ::HIR::ExprPtr& expr, MonomorphState ms, ::std::vector< ::HIR::Literal> args)
    {
        if( expr.m_mir ) {
            return evaluate_constant_mir(sp, resolve, newval_state, name, *expr.m_mir, mv$(ms), mv$(args));
        }
        else {
            BUG(sp, "Attempting to evaluate constant expression with no associated code");
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

                //case ::HIR::CoreType::I8:   lit.as_Integer() &= (1ull<<8)-1;  break;
                //case ::HIR::CoreType::I16:  lit.as_Integer() &= (1ull<<16)-1; break;
                //case ::HIR::CoreType::I32:  lit.as_Integer() &= (1ull<<32)-1; break;
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
        StaticTraitResolve  m_resolve;

        const ::HIR::ItemPath*  m_mod_path;
        t_new_values    m_new_values;

    public:
        Expander(const ::HIR::Crate& crate):
            m_crate(crate),
            m_resolve(crate)
        {}

        void visit_module(::HIR::ItemPath p, ::HIR::Module& mod) override
        {
            auto saved_mp = m_mod_path;
            m_mod_path = &p;
            auto saved = mv$( m_new_values );

            ::HIR::Visitor::visit_module(p, mod);

            for( auto& item : m_new_values )
            {
                auto boxed_ent = box$(::HIR::VisEnt<::HIR::ValueItem> { false, ::HIR::ValueItem(mv$(item.second)) });
                auto it = mod.m_value_items.find( item.first );
                if( it != mod.m_value_items.end() )
                {
                    it->second = mv$(boxed_ent);
                }
                else
                {
                    mod.m_value_items.insert( ::std::make_pair( mv$(item.first), mv$(boxed_ent) ) );
                }
            }
            m_new_values = mv$(saved);
            m_mod_path = saved_mp;
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            ::HIR::Visitor::visit_type(ty);

            TU_IFLET(::HIR::TypeRef::Data, ty.m_data, Array, e,
                assert( e.size_val != ~0u );
            )
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override
        {
            visit_type(item.m_type);
            if( item.m_value )
            {
                auto nvs = NewvalState { m_new_values, *m_mod_path, FMT(p.get_name() << "$") };
                item.m_value_res = evaluate_constant(item.m_value->span(), m_resolve, nvs, FMT_CB(ss, ss << p;), item.m_value, {}, {});

                check_lit_type(item.m_value->span(), item.m_type, item.m_value_res);
                DEBUG("constant: " << item.m_type <<  " = " << item.m_value_res);
                visit_expr(item.m_value);
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override
        {
            visit_type(item.m_type);
            if( item.m_value )
            {
                auto nvs = NewvalState { m_new_values, *m_mod_path, FMT(p.get_name() << "$") };
                item.m_value_res = evaluate_constant(item.m_value->span(), m_resolve, nvs, FMT_CB(ss, ss << p;), item.m_value, {}, {});
                DEBUG("static: " << item.m_type <<  " = " << item.m_value_res);
                visit_expr(item.m_value);
            }
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            if(auto* e = item.m_data.opt_Value())
            {
                for(auto& var : e->variants)
                {
                    if( var.expr )
                    {
                        auto nvs = NewvalState { m_new_values, *m_mod_path, FMT(p.get_name() << "$" << var.name << "$") };
                        auto val = evaluate_constant(var.expr->span(), m_resolve, nvs, FMT_CB(ss, ss << p;), var.expr, {}, {});
                        DEBUG("Enum value " << p << " - " << var.name << " = " << val);
                        // TODO: Save this value? Or just do the above to
                        // validate?
                    }
                }
            }
            ::HIR::Visitor::visit_enum(p, item);
        }
    };
}   // namespace

void ConvertHIR_ConstantEvaluateFull(::HIR::Crate& crate)
{
    Expander    exp { crate };
    exp.visit_crate( crate );
}
