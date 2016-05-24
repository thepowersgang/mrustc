/*
 */
#include <synext.hpp>
#include <ast/crate.hpp>

class Decorator_NoStd:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::EarlyPre; }
    
    void handle(const Span& sp, const AST::MetaItem& mi, AST::Crate& crate) const override {
        if( crate.m_load_std != AST::Crate::LOAD_STD ) {
            ERROR(sp, E0000, "Invalid use of #![no_std] with itself or #![no_core]");
        }
        crate.m_load_std = AST::Crate::LOAD_CORE;
    }
};
class Decorator_NoCore:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::EarlyPre; }
    
    void handle(const Span& sp, const AST::MetaItem& mi, AST::Crate& crate) const override {
        if( crate.m_load_std != AST::Crate::LOAD_STD ) {
            ERROR(sp, E0000, "Invalid use of #![no_core] with itself or #![no_std]");
        }
        crate.m_load_std = AST::Crate::LOAD_NONE;
    }
};
//class Decorator_Prelude:
//    public ExpandDecorator
//{
//public:
//    AttrStage stage() const override { return AttrStage::EarlyPre; }
//};

class Decorator_NoPrelude:
    public ExpandDecorator
{
public:
    AttrStage stage() const override { return AttrStage::EarlyPre; }
};



STATIC_DECORATOR("no_std", Decorator_NoStd)
STATIC_DECORATOR("no_core", Decorator_NoCore)
//STATIC_DECORATOR("prelude", Decorator_Prelude)

STATIC_DECORATOR("no_prelude", Decorator_NoPrelude)

