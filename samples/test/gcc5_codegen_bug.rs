// compile-flags: -O

// A reduction of issue #63, where gcc5 (but not 6) will generate invalid code for the following iterator adapter's
// `next` function (seen in librustc)
// ::"core"::iter::Map<::"core"::iter::Chain<::"core"::iter::sources::Once<::"syntax"::ast::NodeId/*S*/,>/*S*/,::"core"::iter::Map<::"core"::slice::Iter<::"syntax"::codemap::Spanned<::"syntax"::ast::PathListItem_/*S*/,>/*S*/,>/*S*/,::"rustc"::closure_I_82<::"rustc"::hir::lowering::LoweringContext/*S*/,>/*S*/,>/*S*/,>/*S*/,::"rustc"::closure_I_83<::"rustc"::hir::lowering::LoweringContext/*S*/,>/*S*/,>
//
//

fn main()
{
    // The function call is kinda needed to trigger the bug
    // - Probably because of how it sets up `rbx` to be non-zero
    println!("{:?}", foo(0, &[]));
}

// Has to have the second map - even if it's just identity
fn foo(n: u32, list: &[u32]) -> Vec<u32> {
    Iterator::chain(
            ::std::iter::once(0u32),
            list.iter().map(|v| *v)
        ).map(|id| id)
        .collect()
}

