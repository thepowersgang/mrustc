
#ifndef _AST__PATTERN_HPP_INCLUDED_
#define _AST__PATTERN_HPP_INCLUDED_

#include <vector>
#include <memory>
#include <string>

namespace AST {

using ::std::unique_ptr;
using ::std::move;

class ExprNode;

class Pattern:
    public Serialisable
{
public:
    enum BindType {
        MAYBE_BIND,
        ANY,
        VALUE,
        TUPLE,
        TUPLE_STRUCT,
    };
private:
    BindType    m_class;
    ::std::string   m_binding;
    Path    m_path;
    unique_ptr<ExprNode>    m_node;
    ::std::vector<Pattern>  m_sub_patterns;
public:
    Pattern():
        m_class(ANY)
    {}

    struct TagBind {};
    Pattern(TagBind, ::std::string name):
        m_class(ANY),
        m_binding(name)
    {}

    struct TagMaybeBind {};
    Pattern(TagMaybeBind, ::std::string name):
        m_class(MAYBE_BIND),
        m_binding(name)
    {}

    struct TagValue {};
    Pattern(TagValue, unique_ptr<ExprNode> node):
        m_class(VALUE),
        m_node( ::std::move(node) )
    {}

    struct TagTuple {};
    Pattern(TagTuple, ::std::vector<Pattern> sub_patterns):
        m_class(TUPLE),
        m_sub_patterns( ::std::move(sub_patterns) )
    {}

    struct TagEnumVariant {};
    Pattern(TagEnumVariant, Path path, ::std::vector<Pattern> sub_patterns):
        m_class(TUPLE_STRUCT),
        m_path( ::std::move(path) ),
        m_sub_patterns( ::std::move(sub_patterns) )
    {}
    
    // Mutators
    void set_bind(::std::string name) {
        m_binding = name;
    }
    
    // Accessors
    const ::std::string& binding() const { return m_binding; }
    BindType type() const { return m_class; }
    ExprNode& node() { return *m_node; }
    const ExprNode& node() const { return *m_node; }
    Path& path() { return m_path; }
    const Path& path() const { return m_path; }
    ::std::vector<Pattern>& sub_patterns() { return m_sub_patterns; }
    const ::std::vector<Pattern>& sub_patterns() const { return m_sub_patterns; }

    friend ::std::ostream& operator<<(::std::ostream& os, const Pattern& pat);

    SERIALISABLE_PROTOTYPES();
};

};

#endif
