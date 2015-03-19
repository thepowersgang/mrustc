
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
    TAGGED_ENUM(Data, Any,
        (Any,       () ),
        (MaybeBind, () ),
        (Ref,       (bool mut; unique_ptr<Pattern> sub;) ),
        (Value,     (unique_ptr<ExprNode> start; unique_ptr<ExprNode> end;) ),
        (Tuple,     (::std::vector<Pattern> sub_patterns;) ),
        (StructTuple, (Path path; ::std::vector<Pattern> sub_patterns;) ),
        (Struct,    (Path path; ::std::vector< ::std::pair< ::std::string,Pattern> > sub_patterns;) )
        );
private:
    ::std::string   m_binding;
    Data m_data;
    
public:
    Pattern()
    {}

    // Wildcard = '..', distinct from '_'
    // TODO: Store wildcard as a different pattern type
    struct TagWildcard {};
    Pattern(TagWildcard)
    {}

    struct TagBind {};
    Pattern(TagBind, ::std::string name):
        m_binding(name)
    {}

    struct TagMaybeBind {};
    Pattern(TagMaybeBind, ::std::string name):
        m_binding(name),
        m_data( Data::make_MaybeBind({}) )
    {}

    struct TagValue {};
    Pattern(TagValue, unique_ptr<ExprNode> node, unique_ptr<ExprNode> node2 = 0):
        m_data( Data::make_Value({ ::std::move(node), ::std::move(node2) }) )
    {}
    
    
    struct TagReference {};
    Pattern(TagReference, Pattern sub_pattern):
        m_data( Data::make_Ref( /*Data::Data_Ref */ {
            false, unique_ptr<Pattern>(new Pattern(::std::move(sub_pattern)))
            }) )
    {
    }

    struct TagTuple {};
    Pattern(TagTuple, ::std::vector<Pattern> sub_patterns):
        m_data( Data::make_Tuple( { ::std::move(sub_patterns) } ) )
    {}

    struct TagEnumVariant {};
    Pattern(TagEnumVariant, Path path, ::std::vector<Pattern> sub_patterns):
        m_data( Data::make_StructTuple( { ::std::move(path), ::std::move(sub_patterns) } ) ) 
    {}

    struct TagStruct {};
    Pattern(TagStruct, Path path, ::std::vector< ::std::pair< ::std::string,Pattern> > sub_patterns):
        m_data( Data::make_Struct( { ::std::move(path), ::std::move(sub_patterns) } ) ) 
    {}
    
    // Mutators
    void set_bind(::std::string name, bool is_ref, bool is_mut) {
        m_binding = name;
    }
    
    ::std::unique_ptr<ExprNode> take_node() {
        assert(m_data.is_Value());
        return ::std::move(m_data.unwrap_Value().start);
    }
    
    // Accessors
    const ::std::string& binding() const { return m_binding; }
    Data& data() { return m_data; }
    const Data& data() const { return m_data; }
    ExprNode& node() {
        return *m_data.as_Value().start;
    }
    const ExprNode& node() const {
        return *m_data.as_Value().start;
    }
    Path& path() { return m_data.as_StructTuple().path; }
    const Path& path() const { return m_data.as_StructTuple().path; }

    friend ::std::ostream& operator<<(::std::ostream& os, const Pattern& pat);

    SERIALISABLE_PROTOTYPES();
    static ::std::unique_ptr<Pattern> from_deserialiser(Deserialiser& s) {
        ::std::unique_ptr<Pattern> ret(new Pattern);
        s.item(*ret);
        return ::std::move(ret);
    }
};

};

#endif
