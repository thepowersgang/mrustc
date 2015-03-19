
#ifndef _AST__PATTERN_HPP_INCLUDED_
#define _AST__PATTERN_HPP_INCLUDED_

#include <vector>
#include <memory>
#include <string>
#include <tagged_enum.hpp>

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
        REF,
        VALUE,
        TUPLE,
        TUPLE_STRUCT,
    };
private:
    BindType    m_class;
    ::std::string   m_binding;
    Path    m_path;
    ::std::vector<Pattern>  m_sub_patterns;
    
    TAGGED_ENUM(Data, Any,
        (Any,       () ),
        (MaybeBind, () ),
        (Ref,       (bool mut; unique_ptr<ExprNode> sub;) ),
        (Value,     (unique_ptr<ExprNode> start; unique_ptr<ExprNode> end;) ),
        (Tuple,     (::std::vector<Pattern> sub_patterns;) ),
        (StructTuple, (Path path; ::std::vector<Pattern> sub_patterns;) ),
        (Struct,    (Path path; ::std::vector< ::std::pair< ::std::string,Pattern> > sub_patterns;) )
        ) m_data;
    
public:
    Pattern():
        m_class(ANY)
    {}

    // Wildcard = '..', distinct from '_'
    // TODO: Store wildcard as a different pattern type
    struct TagWildcard {};
    Pattern(TagWildcard):
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
    Pattern(TagValue, unique_ptr<ExprNode> node, unique_ptr<ExprNode> node2 = 0):
        m_class(VALUE),
        m_data( Data::make_Value({ ::std::move(node), ::std::move(node2) }) )
    {}
    
    
    struct TagReference {};
    Pattern(TagReference, Pattern sub_pattern):
        m_class(REF),
        m_sub_patterns()
    {
        m_sub_patterns.push_back( ::std::move(sub_pattern) );
    }

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
    void set_bind(::std::string name, bool is_ref, bool is_mut) {
        m_binding = name;
    }
    
    ::std::unique_ptr<ExprNode> take_node() {
        assert(m_class == VALUE);
        m_class = ANY;
        assert(m_data.is_Value());
        return ::std::move(m_data.unwrap_Value().start);
    }
    
    // Accessors
    const ::std::string& binding() const { return m_binding; }
    BindType type() const { return m_class; }
    ExprNode& node() {
        return *m_data.as_Value().start;
    }
    const ExprNode& node() const {
        return *m_data.as_Value().start;
    }
    Path& path() { return m_path; }
    const Path& path() const { return m_path; }
    ::std::vector<Pattern>& sub_patterns() { return m_sub_patterns; }
    const ::std::vector<Pattern>& sub_patterns() const { return m_sub_patterns; }

    friend ::std::ostream& operator<<(::std::ostream& os, const Pattern& pat);

    SERIALISABLE_PROTOTYPES();
};

};

#endif
