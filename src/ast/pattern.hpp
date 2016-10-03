
#ifndef _AST__PATTERN_HPP_INCLUDED_
#define _AST__PATTERN_HPP_INCLUDED_

#include <vector>
#include <memory>
#include <string>
#include <tagged_union.hpp>

namespace AST {

using ::std::unique_ptr;
using ::std::move;
class MacroInvocation;

class PatternBinding
{
public:
    enum class Type {
        MOVE,
        REF,
        MUTREF,
    };
    ::std::string   m_name;
    Type    m_type;
    bool    m_mutable;
    unsigned int    m_slot;

    PatternBinding():
        m_name(""),
        m_type(Type::MOVE),
        m_mutable(false),
        m_slot( ~0u )
    {}
    PatternBinding(::std::string name, Type ty, bool ismut):
        m_name(name),
        m_type(ty),
        m_mutable(ismut),
        m_slot( ~0u )
    {}
    
    bool is_valid() const { return m_name != ""; }
};

class Pattern:
    public Serialisable
{
public:
    TAGGED_UNION(Value, Invalid,
        (Invalid, struct {}),
        (Integer, struct {
            enum eCoreType type;
            uint64_t value; // Signed numbers are encoded as 2's complement
            }),
        (Float, struct {
            enum eCoreType type;
            double value;
            }),
        (String, ::std::string),
        (ByteString, struct { ::std::string v; }),
        (Named, Path)
        );

    struct TuplePat {
        ::std::vector<Pattern>  start;
        bool has_wildcard;
        ::std::vector<Pattern>  end;
    };
    
    TAGGED_UNION(Data, Any,
        (MaybeBind, struct { ::std::string name; } ),
        (Macro,     struct { unique_ptr<::AST::MacroInvocation> inv; } ),
        (Any,       struct { } ),
        (Box,       struct { unique_ptr<Pattern> sub; } ),
        (Ref,       struct { bool mut; unique_ptr<Pattern> sub; } ),
        (Value,     struct { Value start; Value end; } ),
        (Tuple,     TuplePat ),
        (StructTuple, struct { Path path; TuplePat tup_pat; } ),
        (Struct,    struct { Path path; ::std::vector< ::std::pair< ::std::string, Pattern> > sub_patterns; bool is_exhaustive; } ),
        (Slice,     struct { ::std::vector<Pattern> leading; ::std::string extra_bind; ::std::vector<Pattern> trailing; } )
        );
private:
    Span    m_span;
    PatternBinding  m_binding;
    Data m_data;
    
public:
    virtual ~Pattern();
    
    Pattern()
    {}
    Pattern(Pattern&&) = default;
    Pattern& operator=(Pattern&&) = default;
    
    Pattern(Data dat):
        m_binding(),
        m_data( mv$(dat) )
    {};

    struct TagMaybeBind {};
    Pattern(TagMaybeBind, ::std::string name):
        m_binding(),
        m_data( Data::make_MaybeBind({name}) )
    {}

    struct TagMacro {};
    Pattern(TagMacro, unique_ptr<::AST::MacroInvocation> inv):
        m_data( Data::make_Macro({mv$(inv)}) )
    {}

    struct TagBind {};
    Pattern(TagBind, ::std::string name, PatternBinding::Type ty = PatternBinding::Type::MOVE, bool is_mut=false):
        m_binding( PatternBinding(name, ty, is_mut) )
    {}

    struct TagBox {};
    Pattern(TagBox, Pattern sub):
        m_data( Data::make_Box({ unique_ptr<Pattern>(new Pattern(mv$(sub))) }) )
    {}

    struct TagValue {};
    Pattern(TagValue, Value val, Value end = Value()):
        m_data( Data::make_Value({ ::std::move(val), ::std::move(end) }) )
    {}
    
    
    struct TagReference {};
    Pattern(TagReference, bool is_mutable, Pattern sub_pattern):
        m_data( Data::make_Ref( /*Data::Data_Ref */ {
            is_mutable, unique_ptr<Pattern>(new Pattern(::std::move(sub_pattern)))
            }) )
    {
    }

    struct TagTuple {};
    Pattern(TagTuple, ::std::vector<Pattern> pats):
        m_data( Data::make_Tuple( TuplePat { mv$(pats), false, {} } ) )
    {}
    Pattern(TagTuple, TuplePat pat):
        m_data( Data::make_Tuple( mv$(pat) ) )
    {}

    struct TagNamedTuple {};
    Pattern(TagNamedTuple, Path path, ::std::vector<Pattern> pats):
        m_data( Data::make_StructTuple( { mv$(path), TuplePat { mv$(pats), false, {} } } ) )
    {}
    Pattern(TagNamedTuple, Path path, TuplePat pat = TuplePat { {}, false, {} }):
        m_data( Data::make_StructTuple( { ::std::move(path), ::std::move(pat) } ) )
    {}

    struct TagStruct {};
    Pattern(TagStruct, Path path, ::std::vector< ::std::pair< ::std::string,Pattern> > sub_patterns, bool is_exhaustive):
        m_data( Data::make_Struct( { ::std::move(path), ::std::move(sub_patterns), is_exhaustive } ) ) 
    {}

    struct TagSlice {};
    Pattern(TagSlice):
        m_data( Data::make_Slice( {} ) )
    {}
    
    // Mutators
    void set_bind(::std::string name, PatternBinding::Type type, bool is_mut) {
        m_binding = PatternBinding(name, type, is_mut);
    }
    
    
    const Span& span() const { return m_span; }
    void set_span(Span sp) { m_span = mv$(sp); }
    
    Pattern clone() const;
    
    // Accessors
          PatternBinding& binding()       { return m_binding; }
    const PatternBinding& binding() const { return m_binding; }
          Data& data()       { return m_data; }
    const Data& data() const { return m_data; }
          Path& path()       { return m_data.as_StructTuple().path; }
    const Path& path() const { return m_data.as_StructTuple().path; }

    friend ::std::ostream& operator<<(::std::ostream& os, const Pattern& pat);

    SERIALISABLE_PROTOTYPES();
    static ::std::unique_ptr<Pattern> from_deserialiser(Deserialiser& s) {
        ::std::unique_ptr<Pattern> ret(new Pattern);
        s.item(*ret);
        return ret;
    }
};

extern ::std::ostream& operator<<(::std::ostream& os, const Pattern::Value& val);
extern ::std::ostream& operator<<(::std::ostream& os, const Pattern::TuplePat& val);

};

#endif
