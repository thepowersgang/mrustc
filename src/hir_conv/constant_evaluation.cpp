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
#include <trans/codegen.hpp>    // For encoding as part of transmute

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
            auto s = ::HIR::Static( ::HIR::Linkage(), false, mv$(type), ::HIR::ExprPtr() );
            s.m_value_res = ::std::move(value);
            s.m_save_literal = true;
            const_cast<::HIR::Module&>(mod).m_inline_statics.push_back( ::std::make_pair( mv$(name), mv$(s) ) );
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
}
namespace MIR { namespace eval {
    // TODO: Proper MIRI (using Allocations)
    class Value;
    class ValueRefInner;
    class ValueRef {
        ValueRefInner*  m_ptr;
    public:
        ~ValueRef();
        ValueRef(): m_ptr(nullptr) {}
        ValueRef(Value);

        ValueRef(const ValueRef& x) = delete;
        ValueRef& operator=(const ValueRef& x) = delete;

        ValueRef(ValueRef&& x): m_ptr(x.m_ptr) { x.m_ptr = nullptr; }
        ValueRef& operator=(ValueRef&& x) { this->~ValueRef(); m_ptr = x.m_ptr; x.m_ptr = nullptr; return *this; }

        /// Obtain a new reference to the same data
        ValueRef clone_ref() const;
        ValueRef clone_deep() const;

        operator bool() const { return m_ptr; }
        const Value& operator*() const;
              Value& operator*();
        const Value* operator->() const { return &**this; }
              Value* operator->()       { return &**this; }

        static ValueRef from_literal(const ::HIR::Literal& v);
        ::HIR::Literal to_literal() const;
    };
    TAGGED_UNION(Value, Defer,
        (Defer, struct{}),
        (Integer, uint64_t),
        (Float, double),
        (List, std::vector<ValueRef>),
        (String, std::string),
        (Variant, struct { unsigned idx; ValueRef val; }),
        (BorrowData, struct { ::HIR::TypeRef ty; ValueRef val; }),
        (BorrowPath, HIR::Path)
        );
    class ValueRefInner {
        friend class ValueRef;

        unsigned refcount;
        Value   data;

        ValueRefInner(Value data): refcount(1), data(std::move(data)) {}
    };
    ValueRef::~ValueRef()
    {
        if(m_ptr) {
            m_ptr->refcount -= 1;
            if(m_ptr->refcount == 0)
                delete m_ptr;
        }
    }
    ValueRef::ValueRef(Value v):
        m_ptr(new ValueRefInner(std::move(v)))
    {
    }
    ValueRef ValueRef::clone_ref() const {
        ValueRef    rv;
        rv.m_ptr = m_ptr;
        if(m_ptr) m_ptr->refcount += 1;
        return rv;
    }
    ValueRef ValueRef::clone_deep() const
    {
        if(!m_ptr)
            return ValueRef();
        TU_MATCH_HDRA( (**this), { )
        TU_ARMA(Defer, v)   return Value(v);
        TU_ARMA(List, v) {
            Value::Data_List   l;
            for(const auto& e : v)
                l.push_back( e.clone_deep() );
            return Value(std::move(l));
            }
        TU_ARMA(Variant, v) return Value::make_Variant({ v.idx, v.val.clone_deep() });
        TU_ARMA(Integer, v) return Value(v);
        TU_ARMA(Float, v)   return Value(v);
        TU_ARMA(String, v)  return Value(v);
        TU_ARMA(BorrowPath, v)  return Value(v.clone());
        TU_ARMA(BorrowData, v)  return Value::make_BorrowData({ v.ty.clone(), v.val.clone_ref() }); // NOTE: Shallow clone!
        }
        throw "";
    }
    const Value& ValueRef::operator*() const { assert(m_ptr); return m_ptr->data; }
          Value& ValueRef::operator*()       { assert(m_ptr); return m_ptr->data; }
    ValueRef ValueRef::from_literal(const ::HIR::Literal& lit)
    {
        TU_MATCH_HDRA( (lit), {)
        TU_ARMA(Invalid, v) return ValueRef();
        TU_ARMA(Defer, v)   return ValueRef(Value::make_Defer({}));
        TU_ARMA(Generic, v) return ValueRef(Value::make_Defer({}));
        TU_ARMA(List, v) {
            Value::Data_List    l;
            for(const auto& e : v)
                l.push_back( ValueRef::from_literal(e) );
            return ValueRef(std::move(l));
            }
        TU_ARMA(Variant, v) return ValueRef(Value::make_Variant({ v.idx, ValueRef::from_literal(*v.val) }));
        TU_ARMA(Integer, v) return ValueRef(Value::make_Integer(v));
        TU_ARMA(Float, v)   return ValueRef(Value::make_Float(v));
        TU_ARMA(String, v)  return ValueRef(Value::make_String(v));
        TU_ARMA(BorrowPath, v)  return ValueRef(Value::make_BorrowPath(v.clone()));
        //TU_ARMA(BorrowData, v)  TODO(Span(), "ValueRef::from_literal on BorrowData: " << lit);
        TU_ARMA(BorrowData, v)  return ValueRef(Value::make_BorrowData({ v.ty.clone(), ValueRef::from_literal(*v.val) }));
        }
        throw "";
    }
    ::HIR::Literal ValueRef::to_literal() const
    {
        if(!m_ptr)
            return ::HIR::Literal::make_Invalid({});
        TU_MATCH_HDRA( (**this), { )
        TU_ARMA(Defer, v)   return ::HIR::Literal::make_Defer({});
        TU_ARMA(List, v) {
            ::HIR::Literal::Data_List   l;
            for(const auto& e : v)
                l.push_back( e.to_literal() );
            return ::HIR::Literal(std::move(l));
            }
        TU_ARMA(Variant, v) return ::HIR::Literal::make_Variant({ v.idx, box$(v.val.to_literal()) });
        TU_ARMA(Integer, v) return ::HIR::Literal(v);
        TU_ARMA(Float, v)   return ::HIR::Literal(v);
        TU_ARMA(String, v)  return ::HIR::Literal(v);
        TU_ARMA(BorrowPath, v)  return ::HIR::Literal(v.clone());
        TU_ARMA(BorrowData, v)  return ::HIR::Literal::make_BorrowData({ box$(v.val.to_literal()), v.ty.clone() });
        }
        throw "";
    }
    ::std::ostream& operator<<(::std::ostream& os, const ValueRef& v);
    ::std::ostream& operator<<(::std::ostream& os, const Value& v) {
        TU_MATCH_HDRA( (v), { )
        TU_ARMA(Defer, e)   os << "Defer";
        TU_ARMA(List, e)    os << "List[" << e << "]";
        TU_ARMA(Variant, e)    os << "Variant(# " << e.idx << " " << e.val << ")";
        TU_ARMA(Integer, e) os << std::hex << "0x" << e << std::dec;
        TU_ARMA(Float, e)   os << e;
        TU_ARMA(BorrowPath, e)  os << "&" << e;
        TU_ARMA(BorrowData, e)  os << "&(" << e.val << ": " << e.ty << ")";
        TU_ARMA(String, e)  os << "\"" << FmtEscaped(e) << "\"";
        }
        return os;
    }
    ::std::ostream& operator<<(::std::ostream& os, const ValueRef& v) {
        if(!v) {
            return os << "INVALID";
        }
        else {
            return os << *v;
        }
    }
} }

namespace {
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
    EntPtr get_ent_fullpath(const Span& sp, const ::StaticTraitResolve& resolve, const ::HIR::Path& path, EntNS ns, MonomorphState& out_ms)
    {
        TU_MATCH_HDRA( (path.m_data), {)
        TU_ARMA(Generic, e) {
            out_ms = MonomorphState {};
            out_ms.pp_method = &e.m_params;
            return get_ent_simplepath(sp, resolve.m_crate, e.m_path, ns);
            }
        TU_ARMA(UfcsInherent, e) {
            // Easy (ish)
            EntPtr rv;
            resolve.m_crate.find_type_impls(e.type, [](const auto&x)->const auto& { return x; }, [&](const auto& impl) {
                switch( ns )
                {
                case EntNS::Value:
                    {
                        auto fit = impl.m_methods.find(e.item);
                        if( fit != impl.m_methods.end() )
                        {
                            DEBUG("Found method: impl" << impl.m_params.fmt_args() << " " << impl.m_type);
                            rv = EntPtr { &fit->second.data };
                            return true;
                        }
                    }
                    {
                        auto it = impl.m_constants.find(e.item);
                        if( it != impl.m_constants.end() )
                        {
                            DEBUG("Found value: impl" << impl.m_params.fmt_args() << " " << impl.m_type);
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
            }
        TU_ARMA(UfcsKnown, e) {
            EntPtr rv;
            ImplRef best_impl;
            resolve.find_impl(sp, e.trait.m_path, e.trait.m_params, e.type, [&](ImplRef impl_ref, bool is_fuzzy) {
                const auto& ie = impl_ref.m_data.as_TraitImpl();
                const HIR::TraitImpl& impl = *ie.impl;
                switch( ns )
                {
                case EntNS::Value:
                    {
                        auto it = impl.m_methods.find(e.item);
                        if( it != impl.m_methods.end() )
                        {
                            DEBUG("Found method: " << impl_ref);
                            if(!it->second.is_specialisable) {
                                best_impl = std::move(impl_ref);
                                rv = EntPtr { &it->second.data };
                                return true;
                            }
                            if(impl_ref.more_specific_than(best_impl) ) {
                                best_impl = std::move(impl_ref);
                                rv = EntPtr { &it->second.data };
                                return false;
                            }
                        }
                    }
                    {
                        auto it = impl.m_constants.find(e.item);
                        if( it != impl.m_constants.end() )
                        {
                            DEBUG("Found value: impl" << impl_ref);
                            if(!it->second.is_specialisable) {
                                best_impl = std::move(impl_ref);
                                rv = EntPtr { &it->second.data };
                                return true;
                            }
                            if(impl_ref.more_specific_than(best_impl) ) {
                                best_impl = std::move(impl_ref);
                                rv = EntPtr { &it->second.data };
                                return false;
                            }
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
            out_ms.pp_impl_data = std::move(best_impl.m_data.as_TraitImpl().impl_params);
            out_ms.pp_impl = &out_ms.pp_impl_data;
            return rv;
            }
        TU_ARMA(UfcsUnknown, e) {
            // TODO: Are these valid at this point in compilation?
            TODO(sp, "get_ent_fullpath(path = " << path << ")");
            }
        }
        throw "";
    }
    const ::HIR::Function& get_function(const Span& sp, const ::StaticTraitResolve& resolve, const ::HIR::Path& path, MonomorphState& out_ms)
    {
        auto rv = get_ent_fullpath(sp, resolve, path, EntNS::Value, out_ms);
        if(rv.is_Function()) {
            return *rv.as_Function();
        }
        else {
            TODO(sp, "Could not find function for " << path << " - " << rv.tag_str());
        }
    }
}   // namespace <anon>

namespace HIR {

    ::MIR::eval::ValueRef Evaluator::evaluate_constant_mir(
        const ::HIR::ItemPath& ip, const ::MIR::Function& fcn, MonomorphState ms,
        ::HIR::TypeRef exp, const ::HIR::Function::args_t& arg_defs,
        ::std::vector<::MIR::eval::ValueRef> args
        )
    {
        // TODO: Full-blown miri
        TRACE_FUNCTION_F("exp=" << exp << ", args=" << args);

        ::MIR::TypeResolve  state { this->root_span, this->resolve, FMT_CB(ss, ss<<ip), exp, arg_defs, fcn };

        using ::MIR::eval::ValueRef;
        using ::MIR::eval::Value;

        struct LocalState {
            ::MIR::TypeResolve& state;
            ValueRef    retval;
            ::std::vector<ValueRef>&  args;
            ::std::vector<ValueRef>  locals;

            ValueRef    temp;   // Assumes that `get_lval` isn't called twice before the result is used

            LocalState(::MIR::TypeResolve& state, ::std::vector<ValueRef>& args):
                state(state),
                retval(),
                args(args),
                locals( state.m_fcn.locals.size() )
            {
            }

            ValueRef& get_lval(const ::MIR::LValue& lv)
            {
                ValueRef* lit_ptr;
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
                    auto& val = **lit_ptr;
                    TU_MATCH_HDRA( (w), {)
                    TU_ARMA(Field, e) {
                        MIR_ASSERT(state, val.is_List(), "LValue::Field on non-list literal - " << val.tag_str() << " - " << lv);
                        auto& vals = val.as_List();
                        MIR_ASSERT(state, e < vals.size(), "LValue::Field index out of range");
                        lit_ptr = &vals[ e ];
                        }
                    TU_ARMA(Deref, e) {
                        TU_MATCH_HDRA( (val), {)
                        default:
                            MIR_TODO(state, "LValue::Deref - " << lv << " " << val.tag_str() << " { " << val << " }");
                        TU_ARMA(BorrowData, ve) {
                            lit_ptr = &ve.val;
                            }
                        TU_ARMA(BorrowPath, ve) {
                            // TODO: Get the referenced path (if possible), and return the static's value as a pointer
                            // - May need to ensure that there's no mutation...
                            if( ve.m_data.is_Generic() ) {
                                const auto& s = state.m_crate.get_static_by_path(state.sp, ve.m_data.as_Generic().m_path);
                                MIR_ASSERT(state, !s.m_value_res.is_Invalid(), "Reference to non-valid static in BorrowPath");
                                //lit_ptr = const_cast<HIR::Literal*>(&s.m_value_res);
                                lit_ptr = &(this->temp = ValueRef::from_literal(s.m_value_res));
                            }
                            else {
                                MIR_TODO(state, "LValue::Deref - BorrowPath " << val);
                            }
                            }
                        TU_ARMA(String, ve) {
                            // Just clone the string (hack)
                            // - TODO: Create a list?
                            //lit_ptr = &val;
                            }
                        }
                        }
                    TU_ARMA(Index, e) {
                        MIR_ASSERT(state, val.is_List(), "LValue::Index on non-list literal - " << val.tag_str() << " - " << lv);
                        MIR_ASSERT(state, e < locals.size(), "LValue::Index index local out of range");
                        auto& idx = *locals[e];
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
            ValueRef read_lval(const ::MIR::LValue& lv)
            {
                auto& v = get_lval(lv);
                DEBUG(lv << " = " << v);
                return v.clone_deep();
            }
        };
        LocalState  local_state( state, args );

        auto const_to_lit = [&](const ::MIR::Constant& c)->ValueRef {
            TU_MATCH_HDR( (c), {)
            TU_ARM(c, Int, e2) {
                return ValueRef(static_cast<uint64_t>(e2.v));
                }
            TU_ARM(c, Uint, e2)
                return ValueRef(e2.v);
            TU_ARM(c, Float, e2)
                return ValueRef(e2.v);
            TU_ARM(c, Bool, e2)
                return ValueRef(static_cast<uint64_t>(e2.v));
            TU_ARM(c, Bytes, e2)
                return Value::make_String({e2.begin(), e2.end()});
            TU_ARM(c, StaticString, e2)
                return Value(e2);
            TU_ARM(c, Const, e2) {
                auto p = ms.monomorph_path(state.sp, *e2.p);
                // If there's any mention of generics in this path, then return Literal::Defer
                if( visit_path_tys_with(p, [&](const auto& ty)->bool { return ty.data().is_Generic(); }) )
                {
                    DEBUG("Return Literal::Defer for constant " << *e2.p << " which references a generic parameter");
                    return Value::make_Defer({});
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
                    item.m_value_res = eval.evaluate_constant(::HIR::ItemPath(p), item.m_value, item.m_type.clone());
                    assert( !item.m_value_res.is_Invalid() );

                    //check_lit_type(item.m_value->span(), item.m_type, item.m_value_res);
                }
                if( c.m_value_res.is_Defer() )
                {
                    auto it = c.m_monomorph_cache.find(p);
                    if( it == c.m_monomorph_cache.end() )
                    {
                        auto& item = const_cast<::HIR::Constant&>(c);
                        // Challenge: Adding items to the module might invalidate an iterator.
                        ::HIR::ItemPath mod_ip { item.m_value.m_state->m_mod_path };
                        auto nvs = NewvalState { item.m_value.m_state->m_module, mod_ip, FMT("const" << &c << "#") };
                        auto eval = ::HIR::Evaluator { item.m_value.span(), resolve.m_crate, nvs };

                        DEBUG("- Evaluate monomorphed " << p);
                        DEBUG("> const_ms=" << const_ms);
                        auto ty = const_ms.monomorph_type( item.m_value.span(), item.m_type );
                        auto val = eval.evaluate_constant(::HIR::ItemPath(p), item.m_value, std::move(ty), std::move(const_ms));

                        auto insert_res = item.m_monomorph_cache.insert(std::make_pair(p.clone(), std::move(val)));
                        it = insert_res.first;
                    }

                    MIR_ASSERT(state, !it->second.is_Defer(), "Cached literal for " << p << " is Defer");
                    return ValueRef::from_literal(it->second.clone());
                }
                else
                {
                    return ValueRef::from_literal(c.m_value_res);
                }
                }
            TU_ARM(c, Generic, e2) {
                return Value::make_Defer({});
                }
            TU_ARM(c, ItemAddr, e2)
                return Value::make_BorrowPath( ms.monomorph_path(state.sp, *e2) );
            }
            throw "";
            };
        auto do_borrow = [&](::HIR::BorrowType bt, const ::MIR::LValue& val)->ValueRef {
            //if( bt != ::HIR::BorrowType::Shared ) {
            //    MIR_BUG(state, "Only shared borrows are allowed in constants");
            //}

            if( !val.m_wrappers.empty() && val.m_wrappers.back().is_Deref() ) {
                //if( p->val->is_Deref() )
                //    MIR_TODO(state, "Undo nested deref coercion - " << *p->val);
                return local_state.read_lval(val.clone_unwrapped());
            }
            else if( val.m_wrappers.empty() && val.m_root.is_Static() ){
                // Borrow of a static, emit BorrowPath with the same path
                if( bt != ::HIR::BorrowType::Shared ) {
                    MIR_BUG(state, "Only shared borrows of statics are allowed in constants");
                }
                return Value::make_BorrowPath( val.m_root.as_Static().clone() );
            }
            else {
                auto inner_val = local_state.read_lval(val);

                ::HIR::TypeRef  inner_ty;
                const auto& inner_ty_r = state.get_lvalue_type(inner_ty, val);
                if( &inner_ty_r != &inner_ty )
                    inner_ty = inner_ty_r.clone();

                return Value::make_BorrowData({ mv$(inner_ty), inner_val.clone_ref() });
            }
            };
        auto read_param = [&](const ::MIR::Param& p)->ValueRef {
            TU_MATCH_HDRA( (p), { )
            TU_ARMA(LValue, e)
                return local_state.read_lval(e);
            TU_ARMA(Borrow, e)
                return do_borrow(e.type, e.val);
            TU_ARMA(Constant, e)
                return const_to_lit(e);
            }
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

                ValueRef    val;
                const auto& sa = stmt.as_Assign();
                TU_MATCH_HDRA( (sa.src), {)
                TU_ARMA(Use, e) {
                    val = local_state.read_lval(e);
                    }
                TU_ARMA(Constant, e) {
                    val = const_to_lit(e);
                    }
                TU_ARMA(SizedArray, e) {
                    ::std::vector<ValueRef>  vals;
                    if( e.count > 0 )
                    {
                        vals.reserve( e.count );
                        val = read_param(e.val);
                        for(unsigned int i = 1; i < e.count; i++)
                            vals.push_back( val.clone_deep() );
                        vals.push_back( mv$(val) );
                    }
                    val = ValueRef( mv$(vals) );
                    }
                TU_ARMA(Borrow, e) {
                    val = do_borrow(e.type, e.val);
                    }
                TU_ARMA(Cast, e) {
                    auto inval = local_state.read_lval(e.val);
                    if( inval->is_Defer() ) {
                        val = Value::make_Defer({});
                    }
                    else
                    TU_MATCH_HDRA( (e.type.data()), {)
                    default:
                        // NOTE: Can be an unsizing!
                        MIR_TODO(state, "RValue::Cast to " << e.type << ", val = " << inval);
                    TU_ARMA(Path, te) {
                        bool done = false;
                        if(te.binding.is_Struct())
                        {
                            const HIR::Struct& str = *te.binding.as_Struct();
                            ::HIR::TypeRef  tmp;
                            const auto& src_ty = state.get_lvalue_type(tmp, e.val);
                            if( src_ty.data().is_Path() && src_ty.data().as_Path().binding.is_Struct() && src_ty.data().as_Path().binding.as_Struct() == &str )
                            {
                                if( str.m_struct_markings.coerce_unsized != HIR::StructMarkings::Coerce::None )
                                {
                                    val = std::move(inval);
                                    done = true;
                                }
                            }
                        }
                        if(!done )
                        {
                            MIR_TODO(state, "RValue::Cast to " << e.type << ", val = " << inval);
                        }
                        }
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

                            if(const auto* ve = inval->opt_Integer()) {
                                val = Value(*ve & mask);
                            }
                            else if(const auto* ve = inval->opt_Float()){
                                val = Value( static_cast<uint64_t>(*ve) & mask);
                            }
                            else if( const auto* ve = inval->opt_Variant() ) {
                                HIR::TypeRef    tmp;
                                const auto& src_ty = state.get_lvalue_type(tmp, e.val);
                                MIR_ASSERT(state, TU_TEST1(src_ty.data(), Path, .binding.is_Enum()), "Constant cast Variant to integer with invalid type - " << src_ty);
                                MIR_ASSERT(state, src_ty.data().as_Path().binding.as_Enum(), "Enum binding pointer not set! - " << src_ty);
                                const HIR::Enum& enm = *src_ty.data().as_Path().binding.as_Enum();
                                MIR_ASSERT(state, enm.is_value(), "Constant cast Variant to integer with non-value enum - " << src_ty);
                                auto enum_value = enm.get_value(ve->idx);
                                val = Value( static_cast<uint64_t>(enum_value) & mask );
                            }
                            else {
                                MIR_BUG(state, "Invalid cast of " << inval->tag_str() << " to " << e.type);
                            }
                            break;
                        case ::HIR::CoreType::F32:
                        case ::HIR::CoreType::F64:
                            if(const auto* ve = inval->opt_Integer()) {
                                val = Value( static_cast<double>(*ve) );
                            }
                            else if(const auto* ve = inval->opt_Float()) {
                                val = Value( *ve );
                            }
                            else {
                                MIR_BUG(state, "Invalid cast of " << inval->tag_str() << " to " << e.type);
                            }
                            break;
                        default:
                            MIR_TODO(state, "RValue::Cast to " << e.type << ", val = " << inval);
                        }
                        }
                    // Allow casting any integer value to a pointer (TODO: Ensure that the pointer is sized?)
                    TU_ARMA(Pointer, te) {
                        if(const auto* ve = inval->opt_Integer()) {
                            val = Value(*ve);
                        }
                        else if( inval->is_BorrowPath() || inval->is_BorrowData() ) {
                            val = mv$(inval);
                        }
                        else {
                            MIR_BUG(state, "Invalid cast of " << inval->tag_str() << " to " << e.type);
                        }
                        }
                    TU_ARMA(Borrow, te) {
                        if( inval->is_BorrowPath() || inval->is_BorrowData() ) {
                            val = mv$(inval);
                        }
                        else {
                            MIR_BUG(state, "Invalid cast of " << inval->tag_str() << " to " << e.type);
                        }
                        }
                    }
                    }
                TU_ARMA(BinOp, e) {
                    auto inval_l = read_param(e.val_l);
                    auto inval_r = read_param(e.val_r);
                    if( inval_l->is_Defer() || inval_r->is_Defer() )
                        return Value::make_Defer({});
                    MIR_ASSERT(state, inval_l->tag() == inval_r->tag(), "Mismatched literal types in binop - " << inval_l << " and " << inval_r);
                    TU_MATCH_HDRA( (*inval_l, *inval_r), {)
                    default:
                        MIR_TODO(state, "RValue::BinOp - " << sa.src << ", val = " << inval_l << " , " << inval_r);
                    TU_ARMA(Float, l, r) {
                        switch(e.op)
                        {
                        case ::MIR::eBinOp::ADD:    val = Value( l + r );  break;
                        case ::MIR::eBinOp::SUB:    val = Value( l - r );  break;
                        case ::MIR::eBinOp::MUL:    val = Value( l * r );  break;
                        case ::MIR::eBinOp::DIV:    val = Value( l / r );  break;
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
                        case ::MIR::eBinOp::EQ: val = Value( static_cast<uint64_t>(l == r) );  break;
                        case ::MIR::eBinOp::NE: val = Value( static_cast<uint64_t>(l != r) );  break;
                        case ::MIR::eBinOp::GT: val = Value( static_cast<uint64_t>(l >  r) );  break;
                        case ::MIR::eBinOp::GE: val = Value( static_cast<uint64_t>(l >= r) );  break;
                        case ::MIR::eBinOp::LT: val = Value( static_cast<uint64_t>(l <  r) );  break;
                        case ::MIR::eBinOp::LE: val = Value( static_cast<uint64_t>(l <= r) );  break;
                        }
                        }
                    TU_ARMA(Integer, l, r) {
                        switch(e.op)
                        {
                        case ::MIR::eBinOp::ADD:    val = Value( l + r );  break;
                        case ::MIR::eBinOp::SUB:    val = Value( l - r );  break;
                        case ::MIR::eBinOp::MUL:    val = Value( l * r );  break;
                        case ::MIR::eBinOp::DIV:    val = Value( l / r );  break;
                        case ::MIR::eBinOp::MOD:    val = Value( l % r );  break;
                        case ::MIR::eBinOp::ADD_OV:
                        case ::MIR::eBinOp::SUB_OV:
                        case ::MIR::eBinOp::MUL_OV:
                        case ::MIR::eBinOp::DIV_OV:
                            MIR_TODO(state, "RValue::BinOp - " << sa.src << ", val = " << inval_l << " , " << inval_r);

                        case ::MIR::eBinOp::BIT_OR : val = Value( l | r );  break;
                        case ::MIR::eBinOp::BIT_AND: val = Value( l & r );  break;
                        case ::MIR::eBinOp::BIT_XOR: val = Value( l ^ r );  break;
                        case ::MIR::eBinOp::BIT_SHL: val = Value( l << r );  break;
                        case ::MIR::eBinOp::BIT_SHR: val = Value( l >> r );  break;
                        // TODO: GT/LT are incorrect for signed integers
                        case ::MIR::eBinOp::EQ: val = Value( static_cast<uint64_t>(l == r) );  break;
                        case ::MIR::eBinOp::NE: val = Value( static_cast<uint64_t>(l != r) );  break;
                        case ::MIR::eBinOp::GT: val = Value( static_cast<uint64_t>(l >  r) );  break;
                        case ::MIR::eBinOp::GE: val = Value( static_cast<uint64_t>(l >= r) );  break;
                        case ::MIR::eBinOp::LT: val = Value( static_cast<uint64_t>(l <  r) );  break;
                        case ::MIR::eBinOp::LE: val = Value( static_cast<uint64_t>(l <= r) );  break;
                        }
                        }
                    }
                    }
                TU_ARMA(UniOp, e) {
                    auto inval = local_state.read_lval(e.val);
                    if( inval->is_Defer() )
                        return Value::make_Defer({});
                    
                    if( const auto* i = inval->opt_Integer() ) {
                        switch( e.op )
                        {
                        case ::MIR::eUniOp::INV:
                            val = Value( ~*i );
                            break;
                        case ::MIR::eUniOp::NEG:
                            val = Value( static_cast<uint64_t>(-static_cast<int64_t>(*i)) );
                            break;
                        }
                    }
                    else if( const auto* i = inval->opt_Float() ) {
                        switch( e.op )
                        {
                        case ::MIR::eUniOp::INV:
                            MIR_BUG(state, "Invalid invert of Float");
                            break;
                        case ::MIR::eUniOp::NEG:
                            val = Value( -*i );
                            break;
                        }
                    }
                    else {
                        MIR_BUG(state, "Invalid invert of " << inval->tag_str());
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
                    if(ptr->is_Defer()) return Value::make_Defer({});
                    auto meta = read_param(e.meta_val);
                    if(meta->is_Defer()) return Value::make_Defer({});
                    if( ! meta->is_Integer() ) {
                        MIR_TODO(state, "RValue::MakeDst - (non-integral meta) " << ptr << " , " << meta);
                    }
                    else {
                        val = mv$(ptr);
                    }
                    }
                TU_ARMA(Tuple, e) {
                    ::std::vector<ValueRef>  vals;
                    vals.reserve( e.vals.size() );
                    for(const auto& v : e.vals) {
                        vals.push_back( read_param(v) );
                        if( vals.back()->is_Defer() ) {
                            return Value::make_Defer({});
                        }
                    }
                    val = Value::make_List( mv$(vals) );
                    }
                TU_ARMA(Array, e) {
                    ::std::vector<ValueRef>  vals;
                    vals.reserve( e.vals.size() );
                    for(const auto& v : e.vals) {
                        vals.push_back( read_param(v) );
                        if( vals.back()->is_Defer() ) {
                            return Value::make_Defer({});
                        }
                    }
                    val = Value::make_List( mv$(vals) );
                    }
                TU_ARMA(UnionVariant, e) {
                    auto ival = read_param(e.val);
                    if(ival->is_Defer()) return Value::make_Defer({});
                    val = Value::make_Variant({ e.index, std::move(ival) });
                    }
                TU_ARMA(EnumVariant, e) {
                    ::std::vector<ValueRef>  vals;
                    vals.reserve( e.vals.size() );
                    for(const auto& v : e.vals) {
                        vals.push_back( read_param(v) );
                        if( vals.back()->is_Defer() ) {
                            return Value::make_Defer({});
                        }
                    }
                    val = Value::make_Variant({ e.index, Value::make_List( mv$(vals) ) });
                    }
                TU_ARMA(Struct, e) {
                    ::std::vector<ValueRef>  vals;
                    vals.reserve( e.vals.size() );
                    for(const auto& v : e.vals) {
                        vals.push_back( read_param(v) );
                        if( vals.back()->is_Defer() ) {
                            return Value::make_Defer({});
                        }
                    }
                    val = Value::make_List( mv$(vals) );
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
                auto& retval = local_state.retval;
                if( retval->is_Defer() )
                {
                    // Defer doesn't get manipulated
                }
                else if( exp.data().is_Primitive() )
                {
                    switch( exp.data().as_Primitive() )
                    {
                    case ::HIR::CoreType::I8:
                    case ::HIR::CoreType::I16:
                    case ::HIR::CoreType::I32:
                        MIR_ASSERT(state, retval->is_Integer(), "Int ret without a integer value - " << retval);
                        break;
                    case ::HIR::CoreType::I64:  case ::HIR::CoreType::U64:
                    case ::HIR::CoreType::I128: case ::HIR::CoreType::U128:
                    case ::HIR::CoreType::Isize: case ::HIR::CoreType::Usize:
                        MIR_ASSERT(state, retval->is_Integer(), "Int ret without a integer value - " << retval);
                        break;
                    case ::HIR::CoreType::U32:
                        MIR_ASSERT(state, retval->is_Integer(), "Int ret without a integer value - " << retval);
                        retval->as_Integer() &= 0xFFFFFFFF;
                        break;
                    case ::HIR::CoreType::U16:
                        MIR_ASSERT(state, retval->is_Integer(), "Int ret without a integer value - " << retval);
                        retval->as_Integer() &= 0xFFFF;
                        break;
                    case ::HIR::CoreType::U8:
                        MIR_ASSERT(state, retval->is_Integer(), "Int ret without a integer value - " << retval);
                        retval->as_Integer() &= 0xFF;
                        break;
                    case ::HIR::CoreType::F32:
                    case ::HIR::CoreType::F64:
                        MIR_ASSERT(state, retval->is_Float(), "Float ret without a float value");
                        break;
                    case ::HIR::CoreType::Char:
                        MIR_ASSERT(state, retval->is_Integer(), "`char` ret without an int value");
                        MIR_ASSERT(state, retval->as_Integer() <= 0x10FFFF, "`char` ret out of range - " << retval);
                        break;
                    case ::HIR::CoreType::Bool:
                        break;
                    case ::HIR::CoreType::Str:
                        MIR_BUG(state, "Bare str return type");
                    }
                }
                else
                {
                }
                return std::move(retval);
                }
            TU_ARMA(Call, e) {
                auto& dst = local_state.get_lval(e.ret_val);
                if( const auto* te = e.fcn.opt_Intrinsic() )
                {
                    if( te->name == "size_of" ) {
                        auto ty = ms.monomorph_type(state.sp, te->params.m_types.at(0));
                        size_t  size_val;
                        if( Target_GetSizeOf(state.sp, this->resolve, ty, size_val) )
                            dst = Value::make_Integer( size_val );
                        else
                            dst = Value::make_Defer({});
                    }
                    else if( te->name == "min_align_of" ) {
                        auto ty = ms.monomorph_type(state.sp, te->params.m_types.at(0));
                        size_t  align_val;
                        if( Target_GetAlignOf(state.sp, this->resolve, ty, align_val) )
                            dst = Value::make_Integer( align_val );
                        else
                            dst = Value::make_Defer({});
                    }
                    else if( te->name == "bswap" ) {
                        auto ty = ms.monomorph_type(state.sp, te->params.m_types.at(0));
                        MIR_ASSERT(state, ty.data().is_Primitive(), "bswap with non-primitive " << ty);
                        auto val_l = read_param(e.args.at(0));
                        MIR_ASSERT(state, val_l->is_Integer(), "bswap with non-integer");
                        auto val = val_l->as_Integer();
                        uint64_t rv;
                        struct H {
                            static uint16_t bswap16(uint16_t v) {
                                return (v >> 8) | (v << 8);
                            }
                            static uint32_t bswap32(uint32_t v) {
                                return bswap16(v >> 16) | (static_cast<uint32_t>(bswap16(static_cast<uint16_t>(v))) << 16);
                            }
                            static uint64_t bswap64(uint64_t v) {
                                return bswap32(v >> 32) | (static_cast<uint64_t>(bswap32(static_cast<uint32_t>(v))) << 32);
                            }
                        };
                        switch(ty.data().as_Primitive())
                        {
                        case ::HIR::CoreType::I8:
                        case ::HIR::CoreType::U8:
                            rv = val;
                            break;
                        case ::HIR::CoreType::I16:
                        case ::HIR::CoreType::U16:
                            rv = H::bswap16(val);
                            break;
                        case ::HIR::CoreType::I32:
                        case ::HIR::CoreType::U32:
                            rv = H::bswap32(val);
                            break;
                        case ::HIR::CoreType::I64:
                        case ::HIR::CoreType::U64:
                            rv = H::bswap64(val);
                            break;
                        default:
                            MIR_TODO(state, "Handle bswap with " << ty);
                        }
                        dst = Value::make_Integer( rv );
                    }
                    else if( te->name == "transmute" ) {
                        auto src_ty = ms.monomorph_type(state.sp, te->params.m_types.at(0));
                        auto dst_ty = ms.monomorph_type(state.sp, te->params.m_types.at(1));
                        auto val = read_param(e.args.at(0));
                        if(val->is_Defer()) {
                            dst = Value::make_Defer({});
                        }
                        else {
                            // Convert to bytes
                            auto encoded = Trans_EncodeLiteralAsBytes(state.sp, resolve, val.to_literal(), src_ty);
                            // Read the result back
                            struct Decoder {
                                const Span& sp;
                                const StaticTraitResolve& resolve;
                                const EncodedLiteral& lit;
                                Decoder(const Span& sp, const StaticTraitResolve& resolve, const EncodedLiteral& lit)
                                    : sp(sp)
                                    , resolve(resolve)
                                    , lit(lit)
                                {
                                }
                                HIR::Literal decode_literal(const HIR::TypeRef& ty, size_t ofs) const
                                {
                                    auto getb = [&]()->uint8_t {
                                        return lit.bytes[ofs++];
                                        };
                                    auto get_val = [&](int bsize)->uint64_t {
                                        uint64_t rv = 0;
                                        if(Target_GetCurSpec().m_arch.m_big_endian) {
                                            // Big endian
                                            for(int i = bsize; i--; ) {
                                                if( i < 8 ) {
                                                    rv |= static_cast<uint64_t>(getb()) << (8*i);
                                                }
                                            }
                                        }
                                        else {
                                            // Little endian
                                            for(int i = 0; i < bsize; i ++) {
                                                if( i < 8 ) {
                                                    rv |= static_cast<uint64_t>(getb()) << (8*i);
                                                }
                                            }
                                        }
                                        return rv;
                                        };
                                    auto ptr_size = Target_GetCurSpec().m_arch.m_pointer_bits / 8;
                                    auto get_size = [&]() { return get_val(ptr_size); };
                                    auto get_ptr = [&](uint64_t* out_v)->const Reloc* {
                                        const Reloc* rv = nullptr;
                                        for(const auto& r : lit.relocations) {
                                            if( r.ofs == ofs ) {
                                                rv = &r;
                                                break;
                                            }
                                        }
                                        *out_v = get_size();
                                        return rv;
                                        };
                                    switch(ty.data().tag())
                                    {
                                    case ::HIR::TypeData::TAGDEAD: throw "";
                                    case ::HIR::TypeData::TAG_Generic:
                                    case ::HIR::TypeData::TAG_ErasedType:
                                    case ::HIR::TypeData::TAG_Diverge:
                                    case ::HIR::TypeData::TAG_Infer:
                                    case ::HIR::TypeData::TAG_TraitObject:
                                    case ::HIR::TypeData::TAG_Slice:
                                    case ::HIR::TypeData::TAG_Closure:
                                        BUG(sp, "Unexpected " << ty << " in decoding literal");
                                    TU_ARM(ty.data(), Primitive, te) {
                                        switch(te)
                                        {
                                        case ::HIR::CoreType::U8:
                                        case ::HIR::CoreType::I8:
                                        case ::HIR::CoreType::Bool:
                                            return HIR::Literal::make_Integer(getb());
                                        case ::HIR::CoreType::U16:
                                        case ::HIR::CoreType::I16:
                                            return HIR::Literal::make_Integer(get_val(2));
                                        case ::HIR::CoreType::U32:
                                        case ::HIR::CoreType::I32:
                                        case ::HIR::CoreType::Char:
                                            return HIR::Literal::make_Integer(get_val(4));
                                        case ::HIR::CoreType::U64:
                                        case ::HIR::CoreType::I64:
                                            return HIR::Literal::make_Integer(get_val(8));
                                        case ::HIR::CoreType::U128:
                                        case ::HIR::CoreType::I128:
                                            return HIR::Literal::make_Integer(get_val(16));
                                        case ::HIR::CoreType::Usize:
                                        case ::HIR::CoreType::Isize:
                                            return HIR::Literal::make_Integer(get_size());
                                        case ::HIR::CoreType::F32: {
                                            float v;
                                            uint32_t v2 = get_val(4);
                                            memcpy(&v, &v2, 4);
                                            return HIR::Literal::make_Float(v);
                                            } break;
                                        case ::HIR::CoreType::F64: {
                                            double v;
                                            uint64_t v2 = get_val(8);
                                            memcpy(&v, &v2, 8);
                                            return HIR::Literal::make_Float(v);
                                            } break;
                                        case ::HIR::CoreType::Str:
                                            BUG(sp, "Unexpected " << ty << " in decoding literal");
                                        }
                                        } break;
                                    case ::HIR::TypeData::TAG_Path:
                                    case ::HIR::TypeData::TAG_Tuple: {
                                        const auto* repr = Target_GetTypeRepr(sp, resolve, ty);
                                        assert(repr);
                                        size_t cur_ofs = 0;
                                        unsigned var_idx = ~0u;
                                        std::vector<HIR::Literal>   values;
                                        TU_MATCH_HDRA( (repr->variants), {)
                                        TU_ARMA(None, ve) {
                                            // If the type is an enum, need to emit a Variant
                                            for(size_t i = 0; i < repr->fields.size(); i ++)
                                            {
                                                values.push_back( this->decode_literal(repr->fields[i].ty, repr->fields[i].offset) );
                                            }
                                            }
                                        TU_ARMA(NonZero, ve) {
                                            TODO(sp, "");
                                            }
                                        TU_ARMA(Linear, ve) {
                                            TODO(sp, "");
                                            }
                                        TU_ARMA(Values, ve) {
                                            TODO(sp, "");
                                            }
                                        }

                                        auto rv = HIR::Literal::make_List(std::move(values));
                                        if( var_idx != ~0u ) {
                                            return HIR::Literal::make_Variant({ var_idx, box$(rv) });
                                        }
                                        else {
                                            return rv;
                                        }
                                        } break;
                                    case ::HIR::TypeData::TAG_Borrow:
                                    case ::HIR::TypeData::TAG_Pointer: {
                                        const auto& ity = (ty.data().is_Borrow() ? ty.data().as_Borrow().inner : ty.data().as_Pointer().inner);
                                        size_t ity_size, ity_align;
                                        Target_GetSizeAndAlignOf(sp, resolve, ity, ity_size, ity_align);
                                        bool is_unsized = (ity_size == SIZE_MAX);

                                        uint64_t   v;
                                        const auto* reloc = get_ptr(&v);

                                        uint64_t    meta_v = 0;
                                        const Reloc* meta_r = nullptr;
                                        if(is_unsized)
                                        {
                                            meta_r = get_ptr(&meta_v);
                                        }

                                        if(reloc)
                                        {
                                            TODO(sp, "Pointer: w/ relocation");
                                        }
                                        else
                                        {
                                            if(meta_v != 0 || meta_r)
                                                TODO(sp, "Pointer: w/o relocation but with meta - " << meta_v);
                                            return HIR::Literal::make_Integer(v);
                                        }
                                        } break;
                                    case ::HIR::TypeData::TAG_Function:
                                        TODO(sp, "");
                                    TU_ARM(ty.data(), Array, te) {
                                        TODO(sp, "");
                                        }
                                    }
                                    TODO(sp, "");
                                }
                            };
                            dst = ValueRef::from_literal( Decoder(state.sp, resolve, encoded).decode_literal(dst_ty, 0) );
                        }
                    }
                    else {
                        MIR_TODO(state, "Call intrinsic \"" << te->name << "\" - " << block.terminator);
                    }
                }
                else if( const auto* te = e.fcn.opt_Path() )
                {
                    const auto& fcnp_raw = *te;
                    auto fcnp = ms.monomorph_path(state.sp, fcnp_raw);

                    MonomorphState  fcn_ms;
                    auto& fcn = get_function(this->root_span, this->resolve.m_crate, fcnp, fcn_ms);

                    ::std::vector<ValueRef>  call_args;
                    call_args.reserve( e.args.size() );
                    for(const auto& a : e.args)
                        call_args.push_back( read_param(a) );
                    ::HIR::Function::args_t arg_defs;
                    for(const auto& a : fcn.m_args)
                        arg_defs.push_back( ::std::make_pair(::HIR::Pattern(), fcn_ms.monomorph_type(this->root_span, a.second)) );

                    // TODO: Set m_const during parse and check here

                    // Call by invoking evaluate_constant on the function
                    {
                        TRACE_FUNCTION_F("Call const fn " << fcnp << " args={ " << call_args << " }");
                        auto fcn_ip = ::HIR::ItemPath(fcnp);
                        const auto* mir = this->resolve.m_crate.get_or_gen_mir( fcn_ip, fcn );
                        MIR_ASSERT(state, mir, "No MIR for function " << fcnp);
                        auto ret_ty = fcn_ms.monomorph_type(this->root_span, fcn.m_return);
                        dst = evaluate_constant_mir(fcn_ip, *mir, mv$(fcn_ms), mv$(ret_ty), arg_defs, mv$(call_args));
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

    void Evaluator::replace_borrow_data(const HIR::TypeRef& ty, HIR::Literal& lit)
    {
        const Span& sp = root_span;

        TRACE_FUNCTION_FR(lit, lit);
        TU_MATCH_HDRA( (lit), {)
        TU_ARMA(BorrowData, e) {
            // Create new static containing borrowed data
            this->replace_borrow_data(e.ty, *e.val);
            auto item_path = nvs.new_static( mv$(e.ty), mv$(*e.val) );
            lit = ::HIR::Literal::make_BorrowPath(mv$(item_path));
            }
        TU_ARMA(Invalid, e) { }
        TU_ARMA(Defer, e) { }
        TU_ARMA(Generic, e) { }
        TU_ARMA(BorrowPath, e) { }
        TU_ARMA(Integer, e) { }
        TU_ARMA(Float, e) { }
        TU_ARMA(String, e) { }
        TU_ARMA(List, e) {
            if(e.size() == 0)
            {
            }
            else if( const auto* te = ty.data().opt_Array() ) {
                for(auto& i : e)
                    replace_borrow_data(te->inner, i);
            }
            else if( const auto* te = ty.data().opt_Tuple() ) {
                assert(e.size() == te->size());
                for(size_t i = 0; i < e.size(); i ++)
                {
                    replace_borrow_data((*te)[i], e[i]);
                }
            }
            else if( const auto* te = ty.data().opt_Path() ) {
                ASSERT_BUG(sp, te->binding.is_Struct(), ty);
                const auto& str = *te->binding.as_Struct();
                
                HIR::TypeRef    tmp;
                auto maybe_monomorph = [&](const auto& ty)->const auto& {
                    return resolve.monomorph_expand_opt(sp, tmp, ty, MonomorphStatePtr(nullptr, &te->path.m_data.as_Generic().m_params, nullptr));
                    };
                TU_MATCH_HDRA( (str.m_data), {)
                TU_ARMA(Unit, se) {
                    BUG(sp, "Field on unit-like struct - " << ty);
                    }
                TU_ARMA(Tuple, se) {
                    ASSERT_BUG(sp, e.size() == se.size(), "Incorrect Literal::List size for tuple-struct " << te->path);
                    for(size_t i = 0; i < e.size(); i ++)
                        replace_borrow_data(maybe_monomorph(se[i].ent), e[i]);
                    }
                TU_ARMA(Named, se) {
                    ASSERT_BUG(sp, e.size() == se.size(), "Incorrect Literal::List size for tuple-struct " << te->path);
                    for(size_t i = 0; i < e.size(); i ++)
                        replace_borrow_data(maybe_monomorph(se[i].second.ent), e[i]);
                    }
                }
            }
            else {
                TODO(sp, "List " << ty << " =  " << lit);
            }
            }
        TU_ARMA(Variant, e) {
            if( !e.val->is_BorrowData() && !(e.val->is_List() && !e.val->as_List().empty()) ) {
                // Doesn't need to recurse (not a BD and not a non-empty list)
            }
            else if( const auto* te = ty.data().opt_Path() ) {
                if( te->binding.is_Union() ) {
                    const auto& unm = *te->binding.as_Union();
                    HIR::TypeRef    tmp;
                    auto maybe_monomorph = [&](const auto& ty)->const auto& {
                        return resolve.monomorph_expand_opt(sp, tmp, ty, MonomorphStatePtr(nullptr, &te->path.m_data.as_Generic().m_params, nullptr));
                        };
                    const auto& var = unm.m_variants[e.idx];
                    replace_borrow_data(maybe_monomorph(var.second.ent), *e.val);
                }
                else if( te->binding.is_Enum() ) {
                    const auto& enm = *te->binding.as_Enum();
                    HIR::TypeRef    tmp;
                    auto maybe_monomorph = [&](const auto& ty)->const auto& {
                        return resolve.monomorph_expand_opt(sp, tmp, ty, MonomorphStatePtr(nullptr, &te->path.m_data.as_Generic().m_params, nullptr));
                        };
                    ASSERT_BUG(sp, enm.m_data.is_Data(), ty << " must be data enum");
                    const auto& vars = enm.m_data.as_Data();
                    ASSERT_BUG(sp, e.idx < vars.size(), "");
                    const auto& var = vars[e.idx];
                    replace_borrow_data(maybe_monomorph(var.type), *e.val);
                }
                else {
                    // TODO: Get inner type
                    TODO(Span(), "Variant " << ty << " =  " << lit);
                }
            }
            else {
                TODO(Span(), "Variant " << ty << " =  " << lit);
            }
            }
        }
    }

    static bool has_borrow_data(const HIR::Literal& lit)
    {
        HIR::Literal    rv;
        TRACE_FUNCTION_FR(lit, rv);
        TU_MATCH_HDRA( (lit), {)
            TU_ARMA(BorrowData, e) {
            return true;
        }
        TU_ARMA(Invalid, e) {}
        TU_ARMA(Defer, e) {}
        TU_ARMA(Generic, e) {}
        TU_ARMA(BorrowPath, e) {}
        TU_ARMA(Integer, e) {}
        TU_ARMA(Float, e) {}
        TU_ARMA(String, e) {}
        TU_ARMA(List, e) {
            for(const auto& i : e)
                if( has_borrow_data(i) )
                    return true;
        }
        TU_ARMA(Variant, e) {
            return has_borrow_data(*e.val);
        }
        }
        return false;
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
                ms.self_ty = ty_self.clone();
            }
            auto rv = evaluate_constant_mir(ip, *mir, mv$(ms), exp.clone(), {}, {}).to_literal();
            // Replace all BorrowData with BorrowPath
            if(has_borrow_data(rv))
            {
                this->replace_borrow_data(exp, rv);
            }
            return rv;
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
        TU_MATCHA( (type.data()), (te),
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
                    ms.self_ty = impl.m_type.clone();
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
                        auto tp = Trans_Params::new_impl(sp, ms.self_ty.clone(), ms.pp_impl->clone());
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
                                    /*m_type=*/ms.monomorph_type(sp, template_const.m_type),
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
                                    /*m_type=*/ms.monomorph_type(sp, template_const.m_type),
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

            if(auto* e = ty.data_mut().opt_Array())
            {
                TRACE_FUNCTION_FR(ty, ty);
                if( e->size.is_Unevaluated() )
                {
                    const auto& expr_ptr = *e->size.as_Unevaluated();
                    auto ty_name = FMT("ty_" << &ty << "#");

                    auto nvs = NewvalState { *m_mod, *m_mod_path, ty_name };
                    auto eval = ::HIR::Evaluator { expr_ptr->span(), m_crate, nvs };
                    auto val = eval.evaluate_constant(::HIR::ItemPath(*m_mod_path, ty_name.c_str()), expr_ptr, ::HIR::CoreType::Usize);
                    if( val.is_Defer() ) {
                        const auto* tn = dynamic_cast<const HIR::ExprNode_ConstParam*>(&*expr_ptr);
                        if(tn) {
                            e->size = HIR::GenericRef(tn->m_name, tn->m_binding);
                        }
                        //TODO(expr_ptr->span(), "Handle defer for array sizes");
                    }
                    else if( val.is_Integer() ) {
                        e->size = val.as_Integer();
                        DEBUG("Array " << ty << " - size = " << e->size.as_Known());
                    }
                    else
                        ERROR(expr_ptr->span(), E0000, "Array size isn't an integer, got " << val.tag_str());
                }
                else
                {
                    DEBUG("Array " << ty << " - size = " << e->size);
                }
            }

            if( m_recurse_types )
            {
                m_recurse_types = false;
                if( const auto* te = ty.data().opt_Path() )
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
                auto ty = ::HIR::Enum::get_repr_type(item.m_tag_repr);
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
