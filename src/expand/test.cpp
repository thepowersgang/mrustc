/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/test.cpp
 * - #[test] handling
 */
#include <synext_decorator.hpp>
#include <ast/ast.hpp>
#include <ast/crate.hpp>

class CTestHandler:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Pre; }

    void handle(const Span& sp, const AST::MetaItem& mi, ::AST::Crate& crate, const AST::Path& path, AST::Module& mod, AST::Item&i) const override {
        if( ! i.is_Function() ) {
            ERROR(sp, E0000, "#[test] can only be put on functions - found on " << i.tag_str());
        }

        if( crate.m_test_harness )
        {
            ::AST::TestDesc td;
            td.name = path.nodes().back().name();
            td.path = ::AST::Path(path);
            crate.m_tests.push_back( mv$(td) );
        }
        else
        {
            i = AST::Item::make_None({});
        }
    }
};

STATIC_DECORATOR("test", CTestHandler);

