/*
 * MRustC - Mutabah's Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * ast/lifetime_ref.hpp
 * - AST Lifetime reference
 */
#pragma once
#include "../common.hpp"
#include "ident.hpp"

namespace AST {
    class LifetimeRef
    {
    public:
        static const uint16_t BINDING_STATIC = 0xFFFF;
        static const uint16_t BINDING_UNBOUND = 0xFFFE;
        static const uint16_t BINDING_INFER = 0xFFFD;

    private:
        Ident   m_name;
        uint16_t  m_binding;

        LifetimeRef(Ident name, uint32_t binding):
            m_name( ::std::move(name) ),
            m_binding( binding )
        {
        }
    public:
        LifetimeRef():
            LifetimeRef("", BINDING_INFER)
        {
        }
        LifetimeRef(Ident name):
            LifetimeRef(::std::move(name), BINDING_UNBOUND)
        {
        }
        static LifetimeRef new_static() {
            return LifetimeRef("static", BINDING_STATIC);
        }

        void set_binding(uint16_t b) { assert(m_binding == BINDING_UNBOUND); m_binding = b; }
        bool is_unbound() const { return m_binding == BINDING_UNBOUND; }
        bool is_infer() const { return m_binding == BINDING_INFER; }

        const Ident& name() const { return m_name; }
        uint16_t binding() const { return m_binding; }
        Ordering ord(const LifetimeRef& x) const { return ::ord(m_name.name, x.m_name.name); }
        bool operator==(const LifetimeRef& x) const { return ord(x) == OrdEqual; }
        bool operator!=(const LifetimeRef& x) const { return ord(x) != OrdEqual; }
        bool operator<(const LifetimeRef& x) const { return ord(x) == OrdLess; };

        friend ::std::ostream& operator<<(::std::ostream& os, const LifetimeRef& x);
    };
}
