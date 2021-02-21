/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * macro_rules/macro_rules_ptr.hpp
 * - Pointer to a MacroRules instance
 */
#pragma once

class MacroRules;

class MacroRulesPtr
{
    MacroRules* m_ptr;
public:
    MacroRulesPtr(): m_ptr(nullptr) {}
    MacroRulesPtr(MacroRules* p);
    MacroRulesPtr(MacroRulesPtr&& x):
        m_ptr(x.m_ptr)
    {
        x.m_ptr = nullptr;
    }
    MacroRulesPtr& operator=(MacroRulesPtr&& x)
    {
        m_ptr = x.m_ptr; x.m_ptr = nullptr;
        return *this;
    }

    ~MacroRulesPtr();

    operator bool() const { return m_ptr != nullptr; }
    const MacroRules& operator*() const { assert(m_ptr); return *m_ptr; }
          MacroRules& operator*()       { assert(m_ptr); return *m_ptr; }
    const MacroRules* operator->() const { assert(m_ptr); return m_ptr; }
          MacroRules* operator->()       { assert(m_ptr); return m_ptr; }
};
