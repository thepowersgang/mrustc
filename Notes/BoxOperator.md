% `box` and `in` operators

See: https://github.com/rust-lang/rfcs/pull/1426#r48383364

Structures
=========

AST
---

UniOp for `box` and BinOp for `in` and `<-`


HIR
---

```c++
struct ExprNode_Emplace
{
    /// This influences the ops trait used
    enum class Type {
        Placer,
        Boxer,
    };

    Type    m_type;
    ExprNodeP   m_place;
    ExprNodeP   m_value;
}
```

Type Inferrence
===============

The `_Emplace` node type has a revisit to obtain the expected result type.
1. If the place has an unknown type, the pre-coercion output type is located by taking the result type and replacing all params with fresh ivars
1. A bound is added that this new type must implement `ops::Placer<T>` (or `ops::Boxer<T>`) where `T` is the result type of `m_value`
1. Add a coercion at this node? (Or leave it up to the parent node to have inserted one)


Expansion / Lowering
====================

IDEAS:
- Convert as any other operator is - in the post-typeck expansion pass
 - However, since this doesn't expand to a single function call (the current RFC is four calls) that would be interesting
 - Also, the current RFC introduces bindings, which can't (easily) be done in that pass
- Keep until MIR, and lower into call sequence
 - Can use temporaries
 - Downside: Avoids the extra validation other operators get.
 - Still the best solution.




```rust
let p = PLACE;
let mut place = Placer::make_place(p);
let raw_place = Place::pointer(&mut place);
let value = EXPR;
unsafe {
    std::ptr::write(raw_place, value);
    InPlace::finalize(place)
}
```

```rust
let mut place = BoxPlace::make_place();
let raw_place = Place::pointer(&mut place);
let value = EXPR;
unsafe {
    ::std::ptr::write(raw_place, value);
    Boxed::finalize(place)
}
```


