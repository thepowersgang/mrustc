/*
 */
#pragma once

class MacroRules;

class MacroRulesPtr:
    public Serialisable
{
    MacroRules* m_ptr;
public:
    MacroRulesPtr() {}
    MacroRulesPtr(MacroRules* p): m_ptr(p) {}
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
    
          MacroRules& operator*()       { assert(m_ptr); return *m_ptr; }
    const MacroRules& operator*() const { assert(m_ptr); return *m_ptr; }
    
    SERIALISABLE_PROTOTYPES();
};
