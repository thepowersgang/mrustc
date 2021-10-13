/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/helpers.hpp
 * - MIR Manipulation helpers
 */
#pragma once
#include <vector>
#include <functional>
#include <hir_typeck/static.hpp>
#include <mir/mir.hpp>

namespace HIR {
class Crate;
class TypeRef;
struct Pattern;
struct SimplePath;
}

namespace MIR {

class Function;
struct LValue;
class Constant;
struct BasicBlock;
class Terminator;
class Statement;
class RValue;
class Param;

typedef unsigned int    BasicBlockId;

struct CheckFailure:
    public ::std::exception
{
};

#define MIR_BUG(state, ...) do { const char* __fcn = __FUNCTION__; (state).print_bug( [&](auto& _os){_os << __fcn << ": " << __VA_ARGS__; } ); throw ""; } while(0)
#define MIR_ASSERT(state, cnd, ...) do { if( !(cnd) ) (state).print_bug( [&](auto& _os){_os << __FILE__ << ":" << __LINE__ << " ASSERT " #cnd " failed - " << __VA_ARGS__; } ); } while(0)
#define MIR_TODO(state, ...) do { (state).print_todo( [&](auto& _os){_os << __VA_ARGS__; } ); throw ""; } while(0)
#define MIR_DEBUG(state, ...) do { DEBUG(FMT_CB(_ss, (state).fmt_pos(_ss);) << __VA_ARGS__); } while(0)

class TypeResolve
{
public:
    typedef ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >   args_t;
private:
    const unsigned int STMT_TERM = ~0u;

public:
    const Span& sp;
    const ::StaticTraitResolve& m_resolve;
    const ::HIR::Crate& m_crate;
private:
    ::FmtLambda m_path;
public:
    const ::HIR::TypeRef&   m_ret_type;
    const args_t&    m_args;
    const ::MIR::Function&  m_fcn;

    // If set, these override the list in `m_fcn`
    const ::HIR::TypeRef*   m_monomorphed_rettype;
    const ::std::vector<::HIR::TypeRef>*    m_monomorphed_locals;
private:
    const ::HIR::SimplePath*    m_lang_Box = nullptr;

    unsigned int bb_idx = 0;
    unsigned int stmt_idx = 0;

public:
    TypeResolve(const Span& sp, const ::StaticTraitResolve& resolve, ::FmtLambda path, const ::HIR::TypeRef& ret_type, const args_t& args, const ::MIR::Function& fcn):
        sp(sp),
        m_resolve(resolve),
        m_crate(resolve.m_crate),
        m_path(path),
        m_ret_type(ret_type),
        m_args(args),
        m_fcn(fcn)
        , m_monomorphed_rettype(nullptr)
        , m_monomorphed_locals(nullptr)
    {
        if( m_crate.m_lang_items.count("owned_box") > 0 ) {
            m_lang_Box = &m_crate.m_lang_items.at("owned_box");
        }
    }

    void set_cur_stmt(const ::MIR::BasicBlock& bb, const ::MIR::Statement& stmt) {
        assert(&stmt >= &bb.statements.front());
        assert(&stmt <= &bb.statements.back());
        this->set_cur_stmt(bb, &stmt - bb.statements.data());
    }
    void set_cur_stmt(const ::MIR::BasicBlock& bb, unsigned int stmt_idx) {
        assert(&bb >= &m_fcn.blocks.front());
        assert(&bb <= &m_fcn.blocks.back());
        this->set_cur_stmt(&bb - m_fcn.blocks.data(), stmt_idx);
    }
    void set_cur_stmt(unsigned int bb_idx, unsigned int stmt_idx) {
        this->bb_idx = bb_idx;
        this->stmt_idx = stmt_idx;
    }
    void set_cur_stmt_term(const ::MIR::BasicBlock& bb) {
        assert(&bb >= &m_fcn.blocks.front());
        assert(&bb <= &m_fcn.blocks.back());
        this->set_cur_stmt_term(&bb - m_fcn.blocks.data());
    }
    void set_cur_stmt_term(unsigned int bb_idx) {
        this->bb_idx = bb_idx;
        this->stmt_idx = STMT_TERM;
    }
    unsigned int get_cur_block() const { return bb_idx; }
    unsigned int get_cur_stmt_ofs() const;

    void fmt_pos(::std::ostream& os, bool include_path=false) const;
    void print_bug(::std::function<void(::std::ostream& os)> cb) const {
        print_msg("ERROR", cb);
    }
    void print_todo(::std::function<void(::std::ostream& os)> cb) const {
        print_msg("TODO", cb);
    }
    void print_msg(const char* tag, ::std::function<void(::std::ostream& os)> cb) const;

    const ::MIR::BasicBlock& get_block(::MIR::BasicBlockId id) const;

    const ::HIR::TypeRef& get_static_type(::HIR::TypeRef& tmp, const ::HIR::Path& path) const;
    const ::HIR::TypeRef& get_lvalue_type(::HIR::TypeRef& tmp, const ::MIR::LValue& val, unsigned wrapper_skip_count=0) const;
    const ::HIR::TypeRef& get_lvalue_type(::HIR::TypeRef& tmp, const ::MIR::LValue::CRef& val) const {
        return get_lvalue_type(tmp, val.lv(), val.lv().m_wrappers.size() - val.wrapper_count());
    }
    const ::HIR::TypeRef& get_lvalue_type(::HIR::TypeRef& tmp, const ::MIR::LValue::MRef& val) const {
        return get_lvalue_type(tmp, val.lv(), val.lv().m_wrappers.size() - val.wrapper_count());
    }
    const ::HIR::TypeRef& get_unwrapped_type(::HIR::TypeRef& tmp, const ::MIR::LValue::Wrapper& w, const ::HIR::TypeRef& ty) const;
    const ::HIR::TypeRef& get_param_type(::HIR::TypeRef& tmp, const ::MIR::Param& val) const;

    ::HIR::TypeRef get_const_type(const ::MIR::Constant& c) const;

    bool lvalue_is_copy(const ::MIR::LValue& val) const;
    const ::HIR::TypeRef* is_type_owned_box(const ::HIR::TypeRef& ty) const;

    friend ::std::ostream& operator<<(::std::ostream& os, const TypeResolve& x) {
        x.fmt_pos(os);
        return os;
    }
};


// --------------------------------------------------------------------
// MIR_Helper_GetLifetimes
// --------------------------------------------------------------------
class ValueLifetime
{
    ::std::vector<bool> statements;

public:
    ValueLifetime(::std::vector<bool> stmts):
        statements( mv$(stmts) )
    {}

    bool valid_at(size_t ofs) const {
        return statements.at(ofs);
    }

    // true if this value is used at any point
    bool is_used() const {
        for(auto v : statements)
            if( v )
                return true;
        return false;
    }
    bool overlaps(const ValueLifetime& x) const {
        assert(statements.size() == x.statements.size());
        for(unsigned int i = 0; i < statements.size(); i ++)
        {
            if( statements[i] && x.statements[i] )
                return true;
        }
        return false;
    }
    void unify(const ValueLifetime& x) {
        assert(statements.size() == x.statements.size());
        for(unsigned int i = 0; i < statements.size(); i ++)
        {
            if( x.statements[i] )
                statements[i] = true;
        }
    }
};

struct ValueLifetimes
{
    ::std::vector<size_t>   m_block_offsets;
    ::std::vector<ValueLifetime> m_slots;

    bool slot_valid(unsigned idx,  unsigned bb_idx, unsigned stmt_idx) const {
        return m_slots.at(idx).valid_at( m_block_offsets[bb_idx] + stmt_idx );
    }
};




namespace visit {
    enum class ValUsage {
        Move,
        Read,
        Write,
        Borrow,
    };

    extern bool visit_mir_lvalue(const ::MIR::LValue& lv, ValUsage u, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb);
    extern bool visit_mir_lvalue(const ::MIR::Param& p, ValUsage u, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb);
    extern bool visit_mir_lvalues(const ::MIR::RValue& rval, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb);
    extern bool visit_mir_lvalues(const ::MIR::Statement& stmt, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb);
    extern bool visit_mir_lvalues(const ::MIR::Terminator& term, ::std::function<bool(const ::MIR::LValue& , ValUsage)> cb);

    extern void visit_terminator_target_mut(::MIR::Terminator& term, ::std::function<void(::MIR::BasicBlockId&)> cb);
    extern void visit_terminator_target(const ::MIR::Terminator& term, ::std::function<void(const ::MIR::BasicBlockId&)> cb);

    
    template<typename Inner>
    class DecMut
    {
    public:
        typedef Inner Type;
    };
    template<typename Inner>
    class DecConst
    {
    public:
        typedef const Inner Type;
    };

    template<template<typename> class Dec>
    class VisitorBase
    {
    public:
        virtual void visit_type(typename Dec<::HIR::TypeRef>::Type& t)
        {
            // NOTE: Doesn't recurse
        }
        virtual void visit_path(typename Dec<::HIR::Path>::Type& path)
        {
            TU_MATCH_HDRA((path.m_data), {)
            TU_ARMA(Generic, e) {
                visit_path_params(e.m_params);
                }
            TU_ARMA(UfcsInherent, e) {
                visit_type(e.type);
                visit_path_params(e.params);
                }
            TU_ARMA(UfcsKnown, e) {
                visit_type(e.type);
                visit_path_params(e.trait.m_params);
                visit_path_params(e.params);
                }
            TU_ARMA(UfcsUnknown, e) {
                visit_type(e.type);
                visit_path_params(e.params);
                }
            }
        }
        virtual void visit_genericpath(typename Dec<::HIR::GenericPath>::Type& p)
        {
            visit_path_params(p.m_params);
        }
        virtual void visit_path_params(typename Dec<::HIR::PathParams>::Type& p)
        {
            for(auto& e : p.m_types)
                visit_type(e);
        }

        virtual bool visit_lvalue(typename Dec<::MIR::LValue>::Type& lv, ValUsage u) = 0;
        virtual bool visit_const(typename Dec<::MIR::Constant>::Type& c)
        {
            TU_MATCH_HDRA( (c), {)
            default:
                break;
            TU_ARMA(ItemAddr, e) {
                visit_path(*e);
                }
            TU_ARMA(Const, e) {
                visit_path(*e.p);
                }
            }
            return false;
        }
        virtual bool visit_param(typename Dec<::MIR::Param>::Type& p, ValUsage u)
        {
            TU_MATCH_HDRA( (p), {)
            TU_ARMA(LValue, e) {
                return visit_lvalue(e, u);
            }
            TU_ARMA(Borrow, e) {
                return visit_lvalue(e.val, ValUsage::Borrow);
            }
            TU_ARMA(Constant, e) {
                return visit_const(e);
            }
            }
            throw "";
        }
        virtual bool visit_rvalue(typename Dec<::MIR::RValue>::Type& rval)
        {
            bool rv = false;
            TU_MATCH_HDRA( (rval), {)
            TU_ARMA(Use, se) {
                rv |= visit_lvalue(se, ValUsage::Move);
                }
            TU_ARMA(Constant, se) {
                rv |= visit_const(se);
                }
            TU_ARMA(SizedArray, se) {
                rv |= visit_param(se.val, ValUsage::Read);
                }
            TU_ARMA(Borrow, se) {
                rv |= visit_lvalue(se.val, ValUsage::Borrow);
                }
            TU_ARMA(Cast, se) {
                rv |= visit_lvalue(se.val, ValUsage::Move);
                visit_type(se.type);
                }
            TU_ARMA(BinOp, se) {
                rv |= visit_param(se.val_l, ValUsage::Read);
                rv |= visit_param(se.val_r, ValUsage::Read);
                }
            TU_ARMA(UniOp, se) {
                rv |= visit_lvalue(se.val, ValUsage::Read);
                }
            TU_ARMA(DstMeta, se) {
                rv |= visit_lvalue(se.val, ValUsage::Read);
                }
            TU_ARMA(DstPtr, se) {
                rv |= visit_lvalue(se.val, ValUsage::Read);
                }
            TU_ARMA (MakeDst, se) {
                rv |= visit_param(se.ptr_val, ValUsage::Move);
                if( TU_TEST2(se.meta_val, Constant, ,ItemAddr, .get() == nullptr) ) {
                }
                else {
                    rv |= visit_param(se.meta_val, ValUsage::Move);
                }
                }
            TU_ARMA(Tuple, se) {
                for(auto& v : se.vals)
                    rv |= visit_param(v, ValUsage::Move);
                }
            TU_ARMA(Array, se) {
                for(auto& v : se.vals)
                    rv |= visit_param(v, ValUsage::Move);
                }
            TU_ARMA(UnionVariant, se) {
                visit_genericpath(se.path);
                rv |= visit_param(se.val, ValUsage::Move);
                }
            TU_ARMA(EnumVariant, se) {
                visit_genericpath(se.path);
                for(auto& v : se.vals)
                    rv |= visit_param(v, ValUsage::Move);
                }
            TU_ARMA(Struct, se) {
                visit_genericpath(se.path);
                for(auto& v : se.vals)
                    rv |= visit_param(v, ValUsage::Move);
                }
            }
            return rv;
        }
        virtual bool visit_stmt(typename Dec<::MIR::Statement>::Type& stmt)
        {
            bool rv = false;
            TU_MATCH_HDRA( (stmt), {)
            TU_ARMA(Assign, e) {
                rv |= visit_rvalue(e.src);
                rv |= visit_lvalue(e.dst, ValUsage::Write);
                }
            TU_ARMA(Asm, e) {
                for(auto& v : e.inputs)
                    rv |= visit_lvalue(v.second, ValUsage::Read);
                for(auto& v : e.outputs)
                    rv |= visit_lvalue(v.second, ValUsage::Write);
                }
            TU_ARMA(Asm2, e) {
                for(auto& p : e.params)
                {
                    TU_MATCH_HDRA( (p), { )
                    TU_ARMA(Const, v)
                        rv |= visit_const(v);
                    TU_ARMA(Sym, v)
                        /*rv |= */visit_path(v);
                    TU_ARMA(Reg, v) {
                        if(v.input)
                            rv |= visit_param(*v.input, ValUsage::Read);
                        if(v.output)
                            rv |= visit_lvalue(*v.output, ValUsage::Write);
                        }
                    }
                }
                }
            TU_ARMA(SetDropFlag, e) {
                }
            TU_ARMA(Drop, e) {
                rv |= visit_lvalue(e.slot, ValUsage::Move);
                }
            TU_ARMA(ScopeEnd, e) {
                }
            }
            return rv;
        }

        virtual bool visit_block_id(typename Dec<::MIR::BasicBlockId>::Type& bb_id) {
            return false;
        }

        virtual bool visit_terminator(typename Dec<::MIR::Terminator>::Type& term)
        {
            bool rv = false;
            TU_MATCH_HDRA( (term), {)
            TU_ARMA(Incomplete, e) {
                }
            TU_ARMA(Return, e) {
                }
            TU_ARMA(Diverge, e) {
                }
            TU_ARMA(Goto, e) {
                visit_block_id(e);
                }
            TU_ARMA(Panic, e) {
                visit_block_id(e.dst);
                }
            TU_ARMA(If, e) {
                rv |= visit_lvalue(e.cond, ValUsage::Read);
                rv |= visit_block_id(e.bb0);
                rv |= visit_block_id(e.bb1);
                }
            TU_ARMA(Switch, e) {
                rv |= visit_lvalue(e.val, ValUsage::Read);
                for(auto& target : e.targets)
                    rv |= visit_block_id(target);
                }
            TU_ARMA(SwitchValue, e) {
                rv |= visit_lvalue(e.val, ValUsage::Read);
                for(auto& target : e.targets)
                    rv |= visit_block_id(target);
                rv |= visit_block_id(e.def_target);
                }
            TU_ARMA(Call, e) {
                TU_MATCH_HDRA( (e.fcn), {)
                TU_ARMA(Value, ce) {
                    rv |= visit_lvalue(ce, ValUsage::Read);
                    }
                TU_ARMA(Path, ce) {
                    visit_path(ce);
                    }
                TU_ARMA(Intrinsic, ce) {
                    visit_path_params(ce.params);
                    }
                }
                for(auto& v : e.args)
                    rv |= visit_param(v, ValUsage::Read);
                rv |= visit_lvalue(e.ret_val, ValUsage::Write);
                rv |= visit_block_id(e.ret_block);
                rv |= visit_block_id(e.panic_block);
                }
            }
            return rv;
        }

        virtual void visit_function(::MIR::TypeResolve& state, typename Dec<::MIR::Function>::Type& fcn)
        {
            for(auto& t : fcn.locals)
            {
                visit_type(t);
            }

            for(unsigned int block_idx = 0; block_idx < fcn.blocks.size(); block_idx ++)
            {
                auto& block = fcn.blocks[block_idx];
                for(auto& stmt : block.statements)
                {
                    state.set_cur_stmt(block_idx, (&stmt - &block.statements.front()));
                    visit_stmt(stmt);
                }
                if( block.terminator.tag() == ::MIR::Terminator::TAGDEAD )
                    continue ;
                state.set_cur_stmt_term(block_idx);
                visit_terminator(block.terminator);
            }
        }
    };

    class Visitor: public VisitorBase<DecConst>
    {
    public:
        virtual bool visit_lvalue(const ::MIR::LValue& lv, ValUsage u) override
        {
            if( lv.m_root.is_Static() ) {
                visit_path(lv.m_root.as_Static());
            }

            for(auto& w : lv.m_wrappers)
            {
                if( w.is_Index() )
                {
                    if( visit_lvalue(LValue::new_Local(w.as_Index()), ValUsage::Read) )
                        return true;
                }
            }
            return false;
        }
    };
    class VisitorMut: public VisitorBase<DecMut>
    {
    public:
        virtual bool visit_lvalue(::MIR::LValue& lv, ValUsage u) override
        {
            if( lv.m_root.is_Static() ) {
                visit_path(lv.m_root.as_Static());
            }
            for(auto& w : lv.m_wrappers)
            {
                if( w.is_Index() )
                {
                    auto lv = LValue::new_Local(w.as_Index());
                    bool rv = visit_lvalue(lv, ValUsage::Read);
                    ASSERT_BUG(Span(), lv.is_Local(), "visit_lvalue on Index mutated the index to a non-local");
                    w = ::MIR::LValue::Wrapper::new_Index(lv.as_Local());
                    if(rv) {
                        return true;
                    }
                }
            }
            return false;
        }
    };
}   // namespace visit

}   // namespace MIR

extern ::MIR::ValueLifetimes MIR_Helper_GetLifetimes(::MIR::TypeResolve& state, const ::MIR::Function& fcn, bool dump_debug, const ::std::vector<bool>* mask=nullptr);
