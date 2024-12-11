/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * ast/item.hpp
 * - AST named item wrapper
 */
#pragma once

#include <string>
#include <vector>
#include "attrs.hpp"

namespace AST {

struct AbsolutePath;

class Visibility
{
public:
    enum class Ty {
        Private,
        Pub,    // pub
        Crate,  // crate
        PubCrate,   // pub(crate)
        PubSuper,   // pub(super)
        PubSelf,    // pub(self)
        PubIn,  // pub(in ...)
    };
private:
    ::std::shared_ptr<AST::Path>    m_in_path;  // Only valid when 
    ::std::shared_ptr<AST::AbsolutePath>    m_vis_path;   // if null, then global
    Ty  m_ty;
    Visibility(): m_ty(Ty::Pub) {}
public:
    static Visibility make_bare_private() {
        Visibility  rv;
        rv.m_ty = Ty::Private;
        return rv;
    }
    static Visibility make_global();
    static Visibility make_restricted(Ty ty, AST::AbsolutePath p);
    static Visibility make_restricted(AST::AbsolutePath p, AST::Path in_path);

    void fmt(::std::ostream& os) const;
    friend std::ostream& operator<<(::std::ostream& os, const Visibility& x);

    Ty ty() const { return m_ty; }
    bool is_global() const { return m_ty == Ty::Pub; }
    const AST::Path& in_path() const {
        assert(m_in_path);
        return *m_in_path;
    }
    const AST::AbsolutePath& vis_path() const {
        assert(m_vis_path);
        return *m_vis_path;
    }

    bool is_visible(const ::AST::AbsolutePath& from_mod) const;
    /// Returns true if this visibility is "more" than `x`
    bool contains(const Visibility& x) const;

    /// Updates this visibility such that `contains(x)` returns true
    void inplace_union(const Visibility& x);
};

template <typename T>
struct Named
{
    Span    span;
    AttributeList   attrs;
    Visibility vis;
    RcString   name;
    T   data;

    Named()
        : data()
    {}
    //Named(Named&&) = default;
    //Named(const Named&) = default;
    //Named& operator=(Named&&) = default;
    Named(Span sp, AttributeList attrs, Visibility vis, RcString name, T data):
        span(sp),
        attrs( ::std::move(attrs) ),
        vis( ::std::move(vis) ),
        name( ::std::move(name) ),
        data( ::std::move(data) )
    {
    }
};

template <typename T>
using NamedList = ::std::vector<Named<T> >;

}   // namespace AST

