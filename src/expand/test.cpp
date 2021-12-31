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
#include <parse/ttstream.hpp>
#include <synext.hpp>   // Expand_ParseAndExpand_ExprVal
#include <parse/parseerror.hpp> // ParseError
#include <parse/interpolated_fragment.hpp>

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

                if( mi.data().size() != 0 )
                {
                    td.panic_type = ::AST::TestDesc::ShouldPanic::YesWithMessage;

                    TTStream    lex(sp, ParseState(), mi.data());
                    lex.getTokenCheck(TOK_PAREN_OPEN);
                    while(lex.lookahead(0) != TOK_PAREN_CLOSE)
                    {
                        auto n = lex.getTokenCheck(TOK_IDENT).ident().name;
                        if( n == "expected" ) {
                            lex.getTokenCheck(TOK_EQUAL);
                            auto n = Expand_ParseAndExpand_ExprVal(crate, mod, lex);
                            if( auto* v = dynamic_cast<::AST::ExprNode_String*>(&*n) ) {
                                td.expected_panic_message = v->m_value;
                            }
                            else
                            {
                                throw ParseError::Unexpected(lex, Token(InterpolatedFragment(InterpolatedFragment::EXPR, n.release())), TOK_STRING);
                            }
                        }
                        else {
                            TODO(sp, "Handle #[should_panic(" << n << ")");
                        }
                        if(!lex.getTokenIf(TOK_COMMA))
                            break;
                    }
                    lex.getTokenCheck(TOK_PAREN_CLOSE);
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

