/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/pattern.hpp
 * - HIR Representation of patterns
 */
#pragma once

#include <memory>
#include <vector>
#include <int128.h>
#include <tagged_union.hpp>
#include <hir/path.hpp>
#include <hir/type.hpp>

namespace HIR {

class Struct;
class Enum;
class Constant;

struct PatternBinding
{
    enum class Type {
        Move,
        Ref,
        MutRef,
    };

    bool    m_mutable;
    Type    m_type;
    RcString    m_name;
    unsigned int    m_slot;

    unsigned m_implicit_deref_count = 0;

    bool is_valid() const { return m_name != ""; }

    PatternBinding():
        m_mutable(false),
        m_type(Type::Move),
        m_name(""),
        m_slot(0),
        m_implicit_deref_count(0)
    {}
    PatternBinding(bool mut, Type type, RcString name, unsigned int slot):
        m_mutable(mut),
        m_type(type),
        m_name( mv$(name) ),
        m_slot( slot ),
        m_implicit_deref_count(0)
    {}

    friend ::std::ostream& operator<<(::std::ostream& os, const PatternBinding& x);
};

struct Pattern
{
    TAGGED_UNION(Value, String,
        (Integer, struct {
            ::HIR::CoreType type;  // Str == _
            U128    value; // Signed numbers are encoded as 2's complement
            }),
        (Float, struct {
            ::HIR::CoreType type;  // Str == _
            double value;
            }),
        (String, ::std::string),
        (ByteString, struct { ::std::string v; }),
        (Named, struct {
            Path    path;
            const ::HIR::Constant*  binding;
            })
        );
    friend ::std::ostream& operator<<(::std::ostream& os, const Pattern::Value& x);

    enum class GlobPos {
        None,
        Start,
        End,
    };

    TAGGED_UNION_EX(PathBinding, (), Unbound, (
        (Unbound, struct {}),
        (Struct, const Struct*),
        (Enum, struct {
            const Enum* ptr;
            unsigned var_idx;
            })
        ), (), (), (
            PathBinding clone() const {
                TU_MATCH_HDRA( (*this), {)
                TU_ARMA(Unbound, e) return e;
                TU_ARMA(Struct, e) return e;
                TU_ARMA(Enum, e) return e;
                }
                abort();
            };
        ));

    TAGGED_UNION(Data, Any,
        // Irrefutable / destructuring
        (Any,       struct { } ),
        (Box,       struct { ::std::unique_ptr<Pattern> sub; }),
        (Ref,       struct { ::HIR::BorrowType type; ::std::unique_ptr<Pattern> sub; } ),
        (Tuple,     struct {
            ::std::vector<Pattern> sub_patterns;
            }),
        (SplitTuple, struct {
            ::std::vector<Pattern> leading;
            ::std::vector<Pattern> trailing;
            unsigned int total_size;
            }),
        // Maybe refutable
        // - Can be converted into `Value`, or resolved to be an enum/struct value
        (PathValue, struct {
            ::HIR::Path path;
            PathBinding binding;
            }),
        // - Tuple-like enum/struct value
        (PathTuple, struct {
            ::HIR::Path path;
            PathBinding binding;
            ::std::vector<Pattern> leading;
            bool    is_split;
            ::std::vector<Pattern> trailing;
            // Cache making MIR gen easier for split patterns
            unsigned int total_size;
            } ),
        // - Struct-like enum/struct value
        (PathNamed, struct {
            ::HIR::Path path;
            PathBinding binding;

            ::std::vector< ::std::pair<RcString, Pattern> > sub_patterns;
            bool is_exhaustive;

            bool is_wildcard() const {
                return sub_patterns.empty() && !is_exhaustive;
            }
            } ),
        // Split/or patterns
        (Or, std::vector<Pattern>),
        // Always refutable
        (Value,     struct { Value val; } ),
        (Range,     struct { std::unique_ptr<Value> start; std::unique_ptr<Value> end; bool is_inclusive; } ),
        (Slice,     struct {
            ::std::vector<Pattern> sub_patterns;
            } ),
        (SplitSlice, struct {
            ::std::vector<Pattern> leading;
            PatternBinding extra_bind;
            ::std::vector<Pattern> trailing;
            } )
        );

    std::vector<PatternBinding> m_bindings;
    Data    m_data;
    unsigned m_implicit_deref_count = 0;

    Pattern() {}
    Pattern(std::vector<PatternBinding> pbs, Data d):
        m_bindings(mv$(pbs)),
        m_data( mv$(d) )
    {
    }
    Pattern(PatternBinding pb, Data d):
        m_data( mv$(d) )
    {
        if(pb.is_valid()) {
            m_bindings.push_back(std::move(pb));
        }
    }
    Pattern(const Pattern&) = delete;
    Pattern(Pattern&&) = default;
    Pattern& operator=(const Pattern&) = delete;
    Pattern& operator=(Pattern&&) = default;

    Pattern clone() const;

    friend ::std::ostream& operator<<(::std::ostream& os, const Pattern& x);
};

}

