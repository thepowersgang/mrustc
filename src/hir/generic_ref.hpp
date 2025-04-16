/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir/generic_ref.hpp
 * - Reference to a generic
 */
#pragma once
#include <cstdint>
#include <rc_string.hpp>

/// Binding index for a Generic that indicates "Self"
#define GENERIC_Self    0xFFFF
/// `Self` in the context of an erased type
#define GENERIC_ErasedSelf  0xFFFE

namespace HIR {

enum GenericGroup {
    GENERIC_Impl,
    GENERIC_Item,
    GENERIC_Placeholder,
    GENERIC_Hrtb,
};

struct GenericRef
{
    RcString    name;
    // 0xFFFF = Self, 0-255 = Type/Trait, 256-511 = Method, 512-767 = Placeholder
    uint32_t binding;

    GenericRef(RcString name, uint32_t binding)
        : name(::std::move(name))
        , binding(binding)
    {
    }
    GenericRef(RcString name, GenericGroup group, uint16_t idx)
        : name(::std::move(name))
        , binding(group * 256 + idx)
    {
        assert(idx < 256);
    }
    static GenericRef new_self() {
        return GenericRef(RcString::new_interned("Self"), GENERIC_Self);
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
        return (binding >> 8) == GENERIC_Placeholder;
    }


    Ordering ord(const GenericRef& x) const {
        auto rv = ::ord(binding, x.binding);
        if(rv)  return rv;
        if(group() == GENERIC_Placeholder ) {
            return ::ord(name, x.name); // names matter for placeholders
        }
        return rv;
    }
    bool operator==(const GenericRef& x) const { return this->ord(x) == OrdEqual; }
    bool operator!=(const GenericRef& x) const { return this->ord(x) != OrdEqual; }
    bool operator<(const GenericRef& x) const { return this->ord(x) == OrdLess; }

    void fmt(::std::ostream& os) const;

    friend ::std::ostream& operator<<(::std::ostream& os, const GenericRef& x) {
        x.fmt(os);
        return os;
    }
};

struct LifetimeRef
{
    static const uint32_t STATIC = 0xFFFF;  // `'static`
    static const uint32_t UNKNOWN = 0xFFFE; // omitted
    static const uint32_t INFER = 0xFFFD;   // `'_`
    static const uint32_t MAX_LOCAL = 0x8'0000;

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

    bool is_param() const {
        return binding < 0xFF00;
    }
    GenericRef as_param() const {
        assert(is_param());
        return GenericRef(RcString(), binding);
    }
    bool is_hrl() const {
        return is_param() && as_param().group() == 3;
    }

    Ordering ord(const LifetimeRef& x) const {
        return ::ord(binding, x.binding);
    }
    bool operator<(const LifetimeRef& x) const { return this->ord(x) == OrdLess; }
    bool operator==(const LifetimeRef& x) const {
        return binding == x.binding;
    }
    bool operator!=(const LifetimeRef& x) const {
        return !(*this == x);
    }
    friend ::std::ostream& operator<<(::std::ostream& os, const LifetimeRef& x) {
        if( x.binding == INFER )
        {
            os << "'_";
        }
        else if( x.binding == UNKNOWN )
        {
            os << "'#omitted";
        }
        else if( x.binding == STATIC )
        {
            os << "'static";
        }
        else if( x.binding < 0xFFFF )
        {
            switch( (x.binding & 0xFF00) >> 8 )
            {
            case 0: os << "'I" << (x.binding & 0xFF);   break;  // Impl/type
            case 1: os << "'M" << (x.binding & 0xFF);   break;  // Method/value
            case 2: os << "'P" << (x.binding & 0xFF);   break;  // HRLS
            case 3: os << "'H" << (x.binding & 0xFF);   break;  // HRLS
            default: os << "'unk" << std::hex << x.binding << std::dec;   break;
            }
        }
        else if( x.binding < MAX_LOCAL )
        {
            os << "'#local" << (x.binding - 0x1'0000);
        }
        else
        {
            os << "'#ivar" << (x.binding - MAX_LOCAL);
        }
        return os;
    }
};

}
