% Macro Hygine rules and notes

Usecases
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

Implementaton Notes
===================

Idea 1: Index list
-------------------

Lexer maintains a stack of counts - incremented after every TT end (closing bracket) and for every IDENT lexed. The stack grows into each TT.

This stack is the hygine information for an identifier. During lookup a symbol is only usable if its hygine information is a superset or before yours.
```
if( item_hygine[..-1].is_subset_of(my_hygine) && item_hygine[-1] < my_hygine[item_hygine.len] ) {
    /* allowed */
}
```

Hygine storage points:
- Relative paths (of the leading node)
- Pattern bindings (variable definitions)
- _TODO: Where else?_
- ?Generic definitions
- Items? (when would that be checked? They're "globally" addressable)


Problem: Destructuring patterns end up with an incorrect hygine using this system.

Idea 2: Hygine is directly related to the parser state
--------------------------

- Each lexer has a hygine tag
- Token trees carry with them the hygine tag for the contained tokens
- :ident captures Ident as TOK_INTERPOLATED_IDENT


<!-- vim: ft=markdown
-->

