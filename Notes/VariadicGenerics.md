
Syntax
======

Definition
----------

Variadic generics are defined by specifying '...' followed by a parameter name in the generic list, this can
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
```
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


Possible extensions:
--------------------
- It could be desired to explicitly specifiy what parameters are being iterated/expanded in a `...` (e.g. for counting
  params without using them)?


AST and HIR Encoding
====================

Expression
----------
New node type `Variadic` that is only ever valid in a repetition context

Valid locations are:
- Function arguments, tuple \[struct\] constructors, array constructors, block contents

Type/Generics
-------------
- A TypeParams definition can optionally end with a named variadic specifier.
- In binding indexes, the maximum of a level (255? iirc) directly refers to the un-expanded variadic generic
- Only one can exist per generic item.


MIR Encoding
============

Locals
------
Variadic-typed locals can just be locals as usual, the rules apply as normal
- When being monomorphised, subsequent repeitions get allocated new locals.
- Temporaries and non-variadic scoped locals use the same value (is this valid?)

A variadic-typed local is valid anywhere within an expansion context (see below), but outside of that it is only valid
in: call argument lists (anywhere?), array constructors (TODO: requires integer generics?), tuple constructors, and
struct constructors.

Blocks
------
Add new terminator type `Variadic(Enter|Exit, Name, bb)` which marks the start/end of a repetiion of the
named/specified variadic type param.
- These cannot be nested with the same name
- This forms a type-level loop

