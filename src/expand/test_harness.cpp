/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/mod.cpp
 * - Expand pass core code
 */
#include <ast/ast.hpp>
#include <ast/expr.hpp>
#include <ast/crate.hpp>
#include <main_bindings.hpp>
#include <hir/hir.hpp>  // ABI_RUST

#define NEWNODE(_ty, ...)   ::AST::ExprNodeP(new ::AST::ExprNode##_ty(__VA_ARGS__))

void Expand_TestHarness(::AST::Crate& crate)
{
    ASSERT_BUG(Span(), crate.m_ext_cratename_test != "", "Crate `test` not loaded");
    ASSERT_BUG(Span(), crate.m_ext_cratename_std != "", "Crate `std` not loaded");
    auto c_test = crate.m_ext_cratename_test;
    // Create the following module:
    // ```
    // mod `#test` {
    //   extern crate std;
    //   extern crate test;
    //   fn main() {
    //     self::test::test_main_static(&::`#test`::TESTS);
    //   }
    //   static TESTS: [test::TestDescAndFn; _] = [
    //     test::TestDescAndFn { desc: test::TestDesc { name: "foo", ignore: false, should_panic: test::ShouldPanic::No }, testfn: ::path::to::foo },
    //     ];
    // }
    // ```

    // ---- main function ----
    auto main_fn = ::AST::Function { Span(), {}, ABI_RUST, false, false, false, TypeRef(TypeRef::TagUnit(), Span()), {} };
    {
        auto call_node = NEWNODE(_CallPath,
                ::AST::Path(c_test, { ::AST::PathNode("test_main_static") }),
                ::make_vec1(
                    NEWNODE(_UniOp, ::AST::ExprNode_UniOp::REF,
                        NEWNODE(_NamedValue, ::AST::Path("", { ::AST::PathNode("test#"), ::AST::PathNode("TESTS") }))
                        )
                    )
                );
        main_fn.set_code( mv$(call_node) );
    }


    // ---- test list ----
    ::std::vector< ::AST::ExprNodeP>    test_nodes;

    for(const auto& test : crate.m_tests)
    {
        // HACK: Don't emit should_panic tests
        if( test.panic_type != ::AST::TestDesc::ShouldPanic::No )
            continue ;

        ::AST::ExprNode_StructLiteral::t_values   desc_vals;
        // `name: "foo",`
        desc_vals.push_back({ {}, "name", NEWNODE(_CallPath,
                        ::AST::Path(c_test, { ::AST::PathNode("StaticTestName") }),
                        ::make_vec1( NEWNODE(_String,  test.name) )
                        ) });
        // `ignore: false,`
        desc_vals.push_back({ {}, "ignore", NEWNODE(_Bool,  test.ignore) });
        // `should_panic: ShouldPanic::No,`
        {
            ::AST::ExprNodeP    should_panic_val;
            switch(test.panic_type)
            {
            case ::AST::TestDesc::ShouldPanic::No:
                should_panic_val = NEWNODE(_NamedValue,  ::AST::Path(c_test, { ::AST::PathNode("ShouldPanic"), ::AST::PathNode("No") }));
                break;
            case ::AST::TestDesc::ShouldPanic::Yes:
                should_panic_val = NEWNODE(_NamedValue,  ::AST::Path(c_test, { ::AST::PathNode("ShouldPanic"), ::AST::PathNode("Yes") }));
                break;
            case ::AST::TestDesc::ShouldPanic::YesWithMessage:
                should_panic_val = NEWNODE(_CallPath,
                        ::AST::Path(c_test, { ::AST::PathNode("ShouldPanic"), ::AST::PathNode("YesWithMessage") }),
                        make_vec1( NEWNODE(_String, test.expected_panic_message) )
                        );
                break;
            }
            desc_vals.push_back({ {}, "should_panic", mv$(should_panic_val) });
        }
        if( TARGETVER_LEAST_1_29 )
        {
            // TODO: Get this from attributes
            desc_vals.push_back({ {}, "allow_fail", NEWNODE(_Bool, false) });
        }
        if( TARGETVER_LEAST_1_54 )
        {
            // TODO: Get this from attributes
            desc_vals.push_back({ {}, "compile_fail", NEWNODE(_Bool, false) });
            desc_vals.push_back({ {}, "no_run", NEWNODE(_Bool, false) });
            desc_vals.push_back({ {}, "test_type", NEWNODE(_NamedValue, ::AST::Path(c_test, { AST::PathNode("TestType"), AST::PathNode("UnitTest") })) });
        }
        auto desc_expr = NEWNODE(_StructLiteral,  ::AST::Path(c_test, { ::AST::PathNode("TestDesc")}), nullptr, mv$(desc_vals));

        ::AST::ExprNode_StructLiteral::t_values   descandfn_vals;
        descandfn_vals.push_back({ {}, RcString::new_interned("desc"), mv$(desc_expr) });

        auto test_type_var_name  = test.is_benchmark ? "StaticBenchFn" : "StaticTestFn";
        descandfn_vals.push_back({ {}, RcString::new_interned("testfn"), NEWNODE(_CallPath,
                        ::AST::Path(c_test, { ::AST::PathNode(test_type_var_name) }),
                        ::make_vec1( NEWNODE(_NamedValue, AST::Path(test.path)) )
                        ) });

        test_nodes.push_back( NEWNODE(_StructLiteral,  ::AST::Path(c_test, { ::AST::PathNode("TestDescAndFn")}), nullptr, mv$(descandfn_vals) ) );
        // NOTE: 1.39+ needs &TestDescAndFn here
        if(TARGETVER_LEAST_1_39)
        {
            test_nodes.back() = NEWNODE(_UniOp, ::AST::ExprNode_UniOp::REF, mv$(test_nodes.back()));
        }
    }
    auto* tests_array = new ::AST::ExprNode_Array(mv$(test_nodes));

    size_t test_count = tests_array->m_values.size();
    auto list_item_ty = TypeRef(Span(), ::AST::Path(c_test, { ::AST::PathNode("TestDescAndFn") }));
    // NOTE: 1.39+ needs &TestDescAndFn here
    if(TARGETVER_LEAST_1_39)
    {
        list_item_ty = TypeRef(TypeRef::TagReference(), Span(), AST::LifetimeRef::new_static(), false, mv$(list_item_ty));
    }
    auto tests_list = ::AST::Static { ::AST::Static::Class::STATIC,
        TypeRef(TypeRef::TagSizedArray(), Span(),
                mv$(list_item_ty),
                ::std::shared_ptr<::AST::ExprNode>( new ::AST::ExprNode_Integer(test_count, CORETYPE_UINT) )
               ),
        ::AST::Expr( mv$(tests_array) )
        };

    // ---- module ----
    auto newmod = ::AST::Module { ::AST::AbsolutePath("", { "test#" }) };
    // - TODO: These need to be loaded too.
    //  > They don't actually need to exist here, just be loaded (and use absolute paths)
    //newmod.add_ext_crate(Span(), false, "std", "std", {});
    //newmod.add_ext_crate(Span(), false, "test", "test", {});

    newmod.add_item(Span(), false, "main", mv$(main_fn), {});
    newmod.add_item(Span(), false, "TESTS", mv$(tests_list), {});

    crate.m_root_module.add_item(Span(), false, "test#", mv$(newmod), {});
    crate.m_lang_items["mrustc-main"] = ::AST::AbsolutePath("", { "test#", "main" });
}
