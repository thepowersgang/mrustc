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

template <typename T>
struct Named
{
    Span    span;
    AttributeList   attrs;
    bool    is_pub;
    RcString   name;
    T   data;

    Named():
        is_pub(false)
    {}
    Named(Named&&) = default;
    Named(const Named&) = default;
    Named& operator=(Named&&) = default;
    Named(Span sp, AttributeList attrs, bool is_pub, RcString name, T data):
        span(sp),
        attrs( ::std::move(attrs) ),
        is_pub( is_pub ),
        name( ::std::move(name) ),
        data( ::std::move(data) )
    {
    }
};

template <typename T>
using NamedList = ::std::vector<Named<T> >;

}   // namespace AST

