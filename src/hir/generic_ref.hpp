/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/generic_ref.hpp
 * - Reference to a generic
 */
#pragma once
#include <rc_string.hpp>

/// Binding index for a Generic that indicates "Self"
#define GENERIC_Self    0xFFFF

namespace HIR {


struct GenericRef
{
    RcString    name;
    // 0xFFFF = Self, 0-255 = Type/Trait, 256-511 = Method, 512-767 = Placeholder
    uint32_t binding;

    GenericRef(RcString name, uint32_t binding)
        : binding(binding)
        , name(::std::move(name))
    {
    }
    static GenericRef new_self() {
        return GenericRef("Self", GENERIC_Self);
    }

    bool is_self() const {
        return binding == 0xFFFF;
    }
    unsigned idx() const {
        return binding & 0xFF;
    }
    unsigned group() const {
        return binding >> 8;
    }

    bool is_placeholder() const {
        return (binding >> 8) == 2;
    }


    Ordering ord(const GenericRef& x) const {
        return ::ord(binding, x.binding);
    }
    bool operator==(const GenericRef& x) const { return this->ord(x) == OrdEqual; }
    bool operator!=(const GenericRef& x) const { return this->ord(x) != OrdEqual; }

    void fmt(::std::ostream& os) const;

    friend ::std::ostream& operator<<(::std::ostream& os, const GenericRef& x) {
        x.fmt(os);
        return os;
    }
};

struct LifetimeRef
{
    static const uint32_t UNKNOWN = 0;
    static const uint32_t STATIC = 0xFFFF;

    //RcString  name;
    // Values below 2^16 are parameters/static, values above are per-function region IDs allocated during region inferrence.
    uint32_t  binding = UNKNOWN;

    LifetimeRef()
        :binding(UNKNOWN)
    {
    }
    LifetimeRef(uint32_t binding)
        :binding(binding)
    {
    }

    static LifetimeRef new_static() {
        LifetimeRef rv;
        rv.binding = STATIC;
        return rv;
    }

    Ordering ord(const LifetimeRef& x) const {
        return ::ord(binding, x.binding);
    }
    bool operator==(const LifetimeRef& x) const {
        return binding == x.binding;
    }
    bool operator!=(const LifetimeRef& x) const {
        return !(*this == x);
    }
    friend ::std::ostream& operator<<(::std::ostream& os, const LifetimeRef& x) {
        if( x.binding == UNKNOWN )
        {
            os << "'_";
        }
        else if( x.binding == STATIC )
        {
            os << "'static";
        }
        else if( x.binding < 0xFFFF )
        {
            switch( x.binding & 0xFF00 )
            {
            case 0: os << "'I" << (x.binding & 0xFF);   break;
            case 1: os << "'M" << (x.binding & 0xFF);   break;
            default: os << "'unk" << x.binding;   break;
            }
        }
        else
        {
            os << "'_" << (x.binding - 0x1000);
        }
        return os;
    }
};

}
