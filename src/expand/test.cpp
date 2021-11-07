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
    AttrStage   stage() const override { return AttrStage::Pre; }   // Expand early so tests are removed before inner expansion

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        if( ! i.is_Function() ) {
            ERROR(sp, E0000, "#[test] can only be put on functions - found on " << i.tag_str());
        }

        if( crate.m_test_harness )
        {
            ::AST::TestDesc td;
            for(const auto& node : path.nodes)
            {
                td.name += "::";
                td.name += node.c_str();
            }
            td.path = path;

            crate.m_tests.push_back( mv$(td) );
        }
        else
        {
            i = AST::Item::make_None({});
        }
    }
};
class CTestHandler_SP:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Post; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        if( ! i.is_Function() ) {
            ERROR(sp, E0000, "#[should_panic] can only be put on functions - found on " << i.tag_str());
        }

        if( crate.m_test_harness )
        {
            // TODO: If this test doesn't yet exist, create it (but as disabled)?
            for(auto& td : crate.m_tests)
            {
                if( td.path != path )
                    continue ;

                if( mi.has_sub_items() )
                {
                    td.panic_type = ::AST::TestDesc::ShouldPanic::YesWithMessage;
                    // TODO: Check that name is correct and that it is a string
                    td.expected_panic_message = mi.items().at(0).string();
                }
                else
                {
                    td.panic_type = ::AST::TestDesc::ShouldPanic::Yes;
                }
                return ;
            }
            //ERROR()
        }
    }
};
class CTestHandler_Ignore:
    public ExpandDecorator
{
    AttrStage   stage() const override { return AttrStage::Post; }

    void handle(const Span& sp, const AST::Attribute& mi, ::AST::Crate& crate, const AST::AbsolutePath& path, AST::Module& mod, slice<const AST::Attribute> attrs, AST::Item&i) const override {
        if( ! i.is_Function() ) {
            ERROR(sp, E0000, "#[ignore] can only be put on functions - found on " << i.tag_str());
        }

        if( crate.m_test_harness )
        {
            for(auto& td : crate.m_tests)
            {
                if( td.path != path )
                    continue ;

                td.ignore = true;
                return ;
            }
            //ERROR()
        }
    }
};

STATIC_DECORATOR("test", CTestHandler);
STATIC_DECORATOR("should_panic", CTestHandler_SP);
STATIC_DECORATOR("ignore", CTestHandler_Ignore);

