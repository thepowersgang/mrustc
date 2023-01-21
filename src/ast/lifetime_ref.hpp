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
        // NOTE: These (the first three) must match HIR::LifetimeRef's versions
        static const uint16_t BINDING_STATIC      = 0xFFFF; // 'static
        static const uint16_t BINDING_UNSPECIFIED = 0xFFFE; // <unspec>
        static const uint16_t BINDING_INFER       = 0xFFFD; // '_
        static const uint16_t BINDING_UNBOUND     = 0xFFFC;

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
            LifetimeRef("", BINDING_UNSPECIFIED)
        {
        }
        LifetimeRef(Ident name):
            LifetimeRef(::std::move(name), BINDING_UNBOUND)
        {
        }
        static LifetimeRef new_static() {
            return LifetimeRef("static", BINDING_STATIC);
        }
        static LifetimeRef new_infer() {
            return LifetimeRef("_", BINDING_INFER);
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
