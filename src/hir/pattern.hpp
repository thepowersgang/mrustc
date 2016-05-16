/*
 */
#pragma once

#include <memory>
#include <vector>
#include <tagged_union.hpp>
#include <hir/path.hpp>
#include <hir/type.hpp>

namespace HIR {

struct PatternBinding
{
    ::std::string   m_name;
    unsigned int    m_slot;
    
    bool is_valid() const { return m_name == ""; }
};

struct Pattern
{
    TAGGED_UNION(Value, String,
        (Integer, struct {
            enum ::HIR::CoreType type;
            uint64_t value; // Signed numbers are encoded as 2's complement
            }),
        (String, ::std::string),
        (Named, Path)
        );

    TAGGED_UNION(Data, Any,
        (Any,       struct { } ),
        (Box,       struct { ::std::unique_ptr<Pattern> sub; }),
        (Ref,       struct { bool mut; ::std::unique_ptr<Pattern> sub; } ),
        (Value,     struct { Value val; } ),
        (Range,     struct { Value start; Value end; } ),
        (Tuple,     struct { ::std::vector<Pattern> sub_patterns; } ),
        (StructTuple, struct { GenericPath path; ::std::vector<Pattern> sub_patterns; } ),
        (Struct,    struct { GenericPath path; ::std::vector< ::std::pair< ::std::string, Pattern> > sub_patterns; } ),
        (Slice,     struct {
            ::std::vector<Pattern> leading;
            } ),
        (SplitSlice, struct {
            ::std::vector<Pattern> leading;
            PatternBinding extra_bind;
            ::std::vector<Pattern> trailing;
            } )
        );

    PatternBinding  m_binding;
    Data    m_data;
};

}

