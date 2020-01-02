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

#define MIR_BUG(state, ...) do { (state).print_bug( [&](auto& _os){_os << __VA_ARGS__; } ); throw ""; } while(0)
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
}   // namespace visit

}   // namespace MIR

extern ::MIR::ValueLifetimes MIR_Helper_GetLifetimes(::MIR::TypeResolve& state, const ::MIR::Function& fcn, bool dump_debug, const ::std::vector<bool>* mask=nullptr);
