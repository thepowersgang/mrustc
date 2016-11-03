% Macro Hygiene rules and notes

Use-cases
=========

A variable defined within the macro won't collide with one in a capture
```rust
macro_rules! foo {
    ($v:expr) => {{ let a = 123; a + $v }};
}

fn bar(a: usize) -> usize {
    // `a` here must resolve to the argument, not the macro parameter
    foo!(a)
}
```

Macros within functions can access values defined before the invocation
```rust
fn bar(a: usize) -> usize {
    let b = 1;
    macro_rules! foo {
        ($v:expr) => {{ a + b + $v }};
    }
    let a = 123;
    // `a` here must resolve to the local above, while the one in the macro resolves to the argument
    foo!(a)
}
```


Macros in outer scope can access any outer-scope name

Identifiers introduced within macros are inaccessible outside
```rust
fn bar() {
    macro_rules! foo {
        ($v:expr) => {let a = $v;};
    }

    foo!(123);
    //let b = a;    // ERROR
}
```

Implementation Notes
===================

Idea 1: Index list
-------------------

Lexer maintains a stack of counts - incremented after every TT end (closing bracket) and for every IDENT lexed. The stack grows into each TT.

This stack is the hygiene information for an identifier. During lookup a symbol is only usable if its hygiene information is a superset or before yours.
```
if( item_hygine[..-1].is_subset_of(my_hygine) && item_hygiene[-1] < my_hygiene[item_hygiene.len] ) {
    /* allowed */
}
```

Hygiene storage points:
- Relative paths (of the leading node)
- Pattern bindings (variable definitions)
- _TODO: Where else?_
- ?Generic definitions
- Items? (when would that be checked? They're "globally" addressable)


Problem: Destructuring patterns end up with an incorrect hygiene using this system.

Idea 2: Hygiene is directly related to the parser state
--------------------------

- Each lexer has a hygiene tag
- Token trees carry with them the hygiene tag for the contained tokens
- :ident captures a TT with associated hygiene context


<!-- vim: ft=markdown
-->

