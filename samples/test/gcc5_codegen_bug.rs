// compile-flags: -O

// ::"core"::iter::Map<::"core"::iter::Chain<::"core"::iter::sources::Once<::"syntax"::ast::NodeId/*S*/,>/*S*/,::"core"::iter::Map<::"core"::slice::Iter<::"syntax"::codemap::Spanned<::"syntax"::ast::PathListItem_/*S*/,>/*S*/,>/*S*/,::"rustc"::closure_I_82<::"rustc"::hir::lowering::LoweringContext/*S*/,>/*S*/,>/*S*/,>/*S*/,::"rustc"::closure_I_83<::"rustc"::hir::lowering::LoweringContext/*S*/,>/*S*/,>

#[derive(Copy,Debug)]
struct NodeId(u32);
struct Ident(u32);
struct PathListItem_
{
    i: Ident,
    i2: Option<Ident>,
    n: NodeId,
}
struct Span(u32, u32, u32);
struct Spanned<T>
{
    v: T,
    sp: Span,
}
#[derive(Copy,Debug)]
struct ItemId {
    id: NodeId,
}

fn main()
{
    // return iter::once(i.id).chain(imports.iter().map(|import| import.node.id)).map(|id| hir::ItemId { id: id }).collect();
    //let list = [ Spanned { v: PathListItem_ { i: Ident(0), i2: None, n: NodeId(0) }, sp: Span(1,1,1) } ];
    let list: [Spanned<PathListItem_>; 0] = [ ];

    println!("{:?}", foo(0, &[]));
    println!("{:?}", foo(0, &[ Spanned { v: PathListItem_ { i: Ident(0), i2: None, n: NodeId(0) }, sp: Span(1,1,1) } ]));
    println!("{:?}", foo(0, &vec![]));
}

fn foo(n: u32, list: &[Spanned<PathListItem_>]) -> Vec<ItemId> {
    Iterator::chain(
            ::std::iter::once(NodeId(0)),
            list.iter().map(|v| v.v.n)
        ).map(|id| ItemId { id: id })
        .collect()
}

