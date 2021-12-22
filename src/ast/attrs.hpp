/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * ast/attrs.hpp
 * - AST Attributes (#[foo] and #![foo])
 */
#ifndef _AST_ATTRS_HPP_
#define _AST_ATTRS_HPP_

#include <tagged_union.hpp>
#include "expr_ptr.hpp"
#include "../parse/tokentree.hpp"

namespace AST {

class Crate;
class Module;

//
class Attribute;
::std::ostream& operator<<(::std::ostream& os, const Attribute& x);

/// A list of attributes on an item (searchable by the attribute name)
class AttributeList
{
public:
    ::std::vector<Attribute> m_items;

    AttributeList() {}
    AttributeList(::std::vector<Attribute> items):
        m_items( mv$(items) )
    {
    }

    // Move present
    AttributeList(AttributeList&&) = default;
    AttributeList& operator=(AttributeList&&) = default;
    // No copy assign, but explicit copy
    explicit AttributeList(const AttributeList&) = default;
    AttributeList& operator=(const AttributeList&) = delete;
    // Explicit clone
    AttributeList clone() const;

    void push_back(Attribute i);

    const Attribute* get(const char *name) const;
    Attribute* get(const char *name) {
        return const_cast<Attribute*>( const_cast<const AttributeList*>(this)->get(name));
    }
    bool has(const char *name) const {
        return get(name) != 0;
    }

    friend ::std::ostream& operator<<(::std::ostream& os, const AttributeList& x);
};

TAGGED_UNION(AttributeData, None,
    (None, struct {}),
    (ValueUnexpanded, AST::ExprNodeP),
    (String, struct { ::std::string val; }),
    (List,  struct { ::std::vector<Attribute> sub_items; })
    );

struct AttributeName
{
    ::std::vector<RcString>   elems;

    bool is_trivial() const { return elems.size() == 1; }
    const RcString& as_trivial() const { return elems.at(0); }

    bool operator==(const char* s) const { return elems.size() == 1 && elems[0] == s; }
    bool operator==(const RcString& x) const { return elems.size() == 1 && elems[0] == x; }

    template<typename T>
    bool operator!=(const T& x) const { return !(*this == x); }

    friend std::ostream& operator<<(std::ostream& os, const AttributeName& x);
};

// An attribute can has a name, and optional data:
// Data can be:
// - A parenthesised token tree
//   > In 1.19 this was actually just sub-attributes
// - an associated (string) literal

class Attribute
{
    Span    m_span;
    AttributeName   m_name;
    TokenTree   m_data;
    mutable bool    m_is_used;
    // TODO: Parse as a TT then expand?
public:
    Attribute(Span sp, AttributeName name, TokenTree data):
        m_span(::std::move(sp)),
        m_name(::std::move(name)),
        m_data(::std::move(data)),
        m_is_used(false)
    {
    }

    explicit Attribute(const Attribute& x);
    Attribute& operator=(const Attribute& ) = delete;
    Attribute(Attribute&& ) = default;
    Attribute& operator=(Attribute&& ) = default;
    Attribute clone() const;

    void fmt(std::ostream& os) const;
    void mark_used() const { m_is_used = true; }
    bool is_used() const { return m_is_used; }

    const Span& span() const { return m_span; }
    const AttributeName& name() const { return m_name; }
    const TokenTree& data() const { return m_data; }


    /// Parses the data as a `="string"` and returns the string
    std::string parse_equals_string(const AST::Crate& crate, const AST::Module& mod) const;
    /// Parses the data as a `("string")` and returns the string
    std::string parse_paren_string() const;

    void parse_paren_ident_list(std::function<void(const Span& sp, RcString ident)> item_cb) const;

    friend ::std::ostream& operator<<(::std::ostream& os, const Attribute& x) {
        x.fmt(os);
        return os;
    }
};

}   // namespace AST

#endif

