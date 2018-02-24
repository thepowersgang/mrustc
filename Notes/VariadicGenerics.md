
Syntax
======

Definition
----------

Variadic generics are defined by specifying `...` followed by a parameter name in the generic list, this can
optionally be followed by a list of trait bounds in the same way as type parameters (see "Bounds" below)

Use in types
------------
The only valid way to use a variadic generic is by first prefixing its use with `...`, which specifies that it is
being unpacked in some way.

Use in bindings
---------------
To bind to a variadic, the binding name must be preceded by `...` (after any binding modifiers e.g. `mut` or `ref`).

Expansions in Expressions
-------------------------
`...` is valid at the start of a statement, and before an expression that can be repeated. If applies to the following
statement/expression fully (e.g. `... foo += some_variadic_bar;` is valid and expands to `n` copies of the addition).
_TODO: May want to restrict ... at the statement level to block-like statements?_

Trait Bounds
------------
Trait bounds apply to all types within the pack.

Example
-------
```rust
struct Tuple<...T>(...T);

impl<...T> fmt::Debug for Tuple<...T>
where
    ...T: fmt:Debug,
{
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result
    {
        let &Tuple(ref ...val) = self;
        ... fmt::Debug::fmt(val, f)?;
        Ok( () )
    }
}

fn sum_ts<U, ...T>(base: U, ...args: ...T) -> U
where
    ...U: AddAssign<T>
{
    let mut rv = base;
    ... { rv += args; }
    rv
}
```
_TODO: The bound `...U: AddAssign<T>` above feels odd. It expands for each `T` argument, bounding on U with different
arguments._


Possible extensions:
--------------------
- It could be desired to explicitly specifiy what parameters are being iterated/expanded in a `...` (e.g. for counting
  params without using them)?


AST and HIR Encoding
====================

Expression
----------
New node type `UnpackVariadic` that is only ever valid in a position that allows it to eventually expand into multiple
values. This node contains: an expression (including blocks), and a type parameter name/index (determined after/during
typecheck).

Valid locations are:
- Function arguments, tuple \[struct\] constructors, array constructors, block contents

Patterns
--------
_TODO: Codify specifics and encoding of variadics in patterns_

Type/Generics
-------------
- A TypeParams definition can optionally end with a named variadic specifier, this applies a restriction to only allow
  one variadic generic per parameter set (which makes the maximum number of generics in scope 2, in the current
  structure)
- (IMPL NOTE) The binding index of this "type" will be the maximum value for binding indexes at that level (0xFF
  currently)
- Use of a variadic type is valid anywhere in a type parameter list.
- In expressions, it's only valid within an unpack context.


MIR Encoding
============

Locals
------
Variadic-typed locals can just be locals as usual, the rules apply as normal
- When being monomorphised, subsequent repeitions get allocated new locals.
- Temporaries and non-variadic scoped locals use the same value (is this valid?)

Within an expansion grouping (see "Blocks" below) a variadic-typed local is valid anywhere (and will expand to the
single value for that iteration).

Outside of a grouping, a variadic-typed local is ONLY valid somewhere it can expand to multiple values: I.e. call
argument lists, tuple constructors, struct constructors, and (_TODO: if possible with integer generics_) array
constructors.

Blocks
------
Add new terminator type `Variadic(Enter|Exit, Name, bb)` which marks the start/end of a repetiion of the
named/specified variadic type param.
- These cannot be nested with the same name
- This forms a type-level loop

