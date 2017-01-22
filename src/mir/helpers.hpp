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

namespace HIR {
class Crate;
class TypeRef;
class Pattern;
class SimplePath;
}

namespace MIR {

class Function;
class LValue;
class BasicBlock;

typedef unsigned int    BasicBlockId;

struct CheckFailure:
    public ::std::exception
{
};

#define MIR_BUG(state, ...) do { (state).print_bug( [&](auto& _os){_os << __VA_ARGS__; } ); throw ""; } while(0)
#define MIR_ASSERT(state, cnd, ...) do { if( !(cnd) ) (state).print_bug( [&](auto& _os){_os << "ASSERT " #cnd " failed - " << __VA_ARGS__; } ); } while(0)
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
    const ::HIR::TypeRef&   m_ret_type;
    const args_t&    m_args;
    const ::MIR::Function&  m_fcn;
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

    void set_cur_stmt(unsigned int bb_idx, unsigned int stmt_idx) {
        this->bb_idx = bb_idx;
        this->stmt_idx = stmt_idx;
    }
    void set_cur_stmt_term(unsigned int bb_idx) {
        this->bb_idx = bb_idx;
        this->stmt_idx = STMT_TERM;
    }

    void fmt_pos(::std::ostream& os) const;
    void print_bug(::std::function<void(::std::ostream& os)> cb) const {
        print_msg("ERROR", cb);
    }
    void print_todo(::std::function<void(::std::ostream& os)> cb) const {
        print_msg("TODO", cb);
    }
    void print_msg(const char* tag, ::std::function<void(::std::ostream& os)> cb) const;

    const ::MIR::BasicBlock& get_block(::MIR::BasicBlockId id) const;

    const ::HIR::TypeRef& get_static_type(::HIR::TypeRef& tmp, const ::HIR::Path& path) const;
    const ::HIR::TypeRef& get_lvalue_type(::HIR::TypeRef& tmp, const ::MIR::LValue& val) const;

    const ::HIR::TypeRef* is_type_owned_box(const ::HIR::TypeRef& ty) const;
};

}   // namespace MIR
