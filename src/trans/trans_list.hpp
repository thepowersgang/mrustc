/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/trans_list.hpp
 * - A list of items that require translation
 */
#pragma once

#include <hir/type.hpp>
#include <hir/path.hpp>
#include <hir_typeck/common.hpp>

class StaticTraitResolve;
namespace HIR {
class Crate;
class Function;
class Static;
}

// TODO: This is very similar to "hir_typeck/common.hpp" MonomorphState, except it owns its data
struct Trans_Params:
    public MonomorphiserPP
{
    Span    sp;
    const ::HIR::GenericParams* gdef_impl;
    ::HIR::PathParams   pp_method;
    ::HIR::PathParams   pp_impl;
    ::HIR::TypeRef  self_type;

    Trans_Params(): gdef_impl(nullptr) {}
    Trans_Params(const Span& sp):
        sp(sp), gdef_impl(nullptr)
    {}

    static Trans_Params new_impl(Span sp, HIR::TypeRef ty, HIR::PathParams impl_params) {
        Trans_Params    tp(sp);
        tp.self_type = std::move(ty);
        tp.pp_impl = std::move(impl_params);
        return tp;
    }

    const ::HIR::TypeRef& maybe_monomorph(const ::StaticTraitResolve& resolve, ::HIR::TypeRef& tmp, const ::HIR::TypeRef& p) const {
        if(monomorphise_type_needed(p)) {
            return tmp = this->monomorph(resolve, p);
        }
        else {
            return p;
        }
    }

    ::HIR::TypeRef monomorph(const ::StaticTraitResolve& resolve, const ::HIR::TypeRef& p) const;
    ::HIR::Path monomorph(const ::StaticTraitResolve& resolve, const ::HIR::Path& p) const;
    ::HIR::GenericPath monomorph(const ::StaticTraitResolve& resolve, const ::HIR::GenericPath& p) const;
    ::HIR::PathParams monomorph(const ::StaticTraitResolve& resolve, const ::HIR::PathParams& p) const;

    bool has_types() const {
        return pp_method.has_params() || pp_impl.has_params();
    }


    const ::HIR::TypeRef* get_self_type() const override {
        return &self_type;
    }
    const ::HIR::PathParams* get_impl_params() const override {
        return &pp_impl;
    }
    const ::HIR::PathParams* get_method_params() const override {
        return &pp_method;
    }
};

struct CachedFunction {
    ::HIR::TypeRef  ret_ty;
    ::HIR::Function::args_t arg_tys;
    ::MIR::FunctionPointer  code;
};
struct TransList_Function
{
    const ::HIR::Path*  path;   // Pointer into the list (std::map pointers are stable)
    const ::HIR::Function*  ptr;
    Trans_Params    pp;
    // If `pp.has_types` is true, the below is valid
    CachedFunction  monomorphised;
    /// Forces the function to not be emited as code (just emit the signature)
    bool    force_prototype;

    TransList_Function(const ::HIR::Path& path):
        path(&path),
        ptr(nullptr),
        force_prototype(false)
    {}
};
struct TransList_Static
{
    const ::HIR::Static*    ptr;
    Trans_Params    pp;
};
struct TransList_Const
{
    const ::HIR::Constant*    ptr;
    Trans_Params    pp;
};

class TransList
{
public:
    TransList() = default;
    TransList(TransList&&) = default;
    TransList(const TransList&) = delete;
    TransList& operator=(TransList&&) = default;
    TransList& operator=(const TransList&) = delete;

    ::std::map< ::HIR::Path, ::std::unique_ptr<TransList_Function> > m_functions;
    ::std::map< ::HIR::Path, ::std::unique_ptr<TransList_Static> > m_statics;
    /// Constants that are still Defer
    ::std::map< ::HIR::Path, ::std::unique_ptr<TransList_Const> > m_constants;
    ::std::map< ::HIR::Path, Trans_Params> m_vtables;
    /// Required type_id values
    ::std::set< ::HIR::TypeRef> m_typeids;
    // Required drop glue
    ::std::set< ::HIR::TypeRef>  m_drop_glue;
    /// Required struct/enum constructor impls
    ::std::set< ::HIR::GenericPath> m_constructors;
    // Automatic Clone impls
    ::std::set< ::HIR::TypeRef>  auto_clone_impls;
    // Trait methods
    ::std::set< ::HIR::Path>    trait_object_methods;

    ::std::vector< ::std::unique_ptr< ::HIR::Static>>   m_auto_statics;
    ::std::vector< ::std::unique_ptr< ::HIR::Function>> m_auto_functions;

    // .second is `true` if this is a from a reference to the type
    ::std::vector< ::std::pair<::HIR::TypeRef, bool> >  m_types;

    TransList_Function* add_function(::HIR::Path p);
    TransList_Static* add_static(::HIR::Path p);
    TransList_Const* add_const(::HIR::Path p);
    bool add_vtable(::HIR::Path p, Trans_Params pp) {
        return m_vtables.insert( ::std::make_pair( mv$(p), mv$(pp) ) ).second;
    }
};

