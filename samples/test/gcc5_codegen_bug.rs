// compile-flags: -O
extern crate test;

// A reduction of issue #63, where gcc5 (but not 6) will generate invalid code for the following iterator adapter's
// `next` function (seen in librustc)
// ::"core"::iter::Map<::"core"::iter::Chain<::"core"::iter::sources::Once<::"syntax"::ast::NodeId/*S*/,>/*S*/,::"core"::iter::Map<::"core"::slice::Iter<::"syntax"::codemap::Spanned<::"syntax"::ast::PathListItem_/*S*/,>/*S*/,>/*S*/,::"rustc"::closure_I_82<::"rustc"::hir::lowering::LoweringContext/*S*/,>/*S*/,>/*S*/,>/*S*/,::"rustc"::closure_I_83<::"rustc"::hir::lowering::LoweringContext/*S*/,>/*S*/,>
//
// Symptom is an abort due to bad enum tag on Option<u32> when handling the second call to `it.next()`

fn main() {
    let n = 0;
    let list: &[u32] = &[1]; // List can be zero length, it's appears to be the _last_ result that leads to a bad tag.
    // Has to have the second map - even if it's just identity, first map probably needs to exist
    let mut it = Iterator::chain(
            ::std::iter::once(0u32),
            list.iter().map(|v| *v)
        ).map(|id| id);

    // First works, and Foo is printed
    it.next();
    println!("Foo");
    // Second one will crash with an abort (used in condition to avoid it being optimised out)
    if it.next().is_none() {
        println!("Bar");
    }
    else {
        println!("Not-Bar");
    }
    if it.next().is_none() {
        println!("Bar");
    }
    else {
        println!("Not-Bar");
    }
}

