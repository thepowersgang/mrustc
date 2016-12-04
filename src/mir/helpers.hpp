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

class TypeResolve
{
public:
    typedef ::std::vector< ::std::pair< ::HIR::Pattern, ::HIR::TypeRef> >   args_t;
private:
    const Span& sp;
    const ::HIR::Crate& m_crate;
    const ::HIR::TypeRef&   m_ret_type;
    const args_t&    m_args;
    const ::MIR::Function&  m_fcn;
    ::StaticTraitResolve    m_resolve;
    const ::HIR::SimplePath*    m_lang_Box = nullptr;

public:
    TypeResolve(const Span& sp, const ::HIR::Crate& crate, const ::HIR::TypeRef& ret_type, const args_t& args, const ::MIR::Function& fcn):
        sp(sp),
        m_crate(crate),
        m_ret_type(ret_type),
        m_args(args),
        m_fcn(fcn),
        m_resolve(crate)
    {
        if( m_crate.m_lang_items.count("owned_box") > 0 ) {
            m_lang_Box = &m_crate.m_lang_items.at("owned_box");
        }
    }
    
    const ::HIR::TypeRef& get_lvalue_type(::HIR::TypeRef& tmp, const ::MIR::LValue& val) const;
    
private:
    const ::HIR::TypeRef* is_type_owned_box(const ::HIR::TypeRef& ty) const;
};


}   // namespace MIR
