
Methods can be called on some "statement-like" expressions
====

https://github.com/rust-lang/rust/blob/master/src/libstd/sys/common/poison.rs#L172
```
fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
    match *self {
        TryLockError::Poisoned(..) => "poisoned lock: another task failed inside",
        TryLockError::WouldBlock => "try_lock failed because the operation would block"
    }.fmt(f)
}
```

Method call on a statement-like match


The `ident` fragment matches ?some reserved words
===

Typically used for replacing `self` in macros.



Any integer can cast to a pointer
===================
This includes `u8`

Array &-ptrs appear to be able to be cast to raw pointers of the element type
=============================================================================



Modules with the same name as primtiive types have interesting lookup quirks
===================
- If a path like `u32::some_name` is seen, if the module `u32` exists in the current scope and `some_name` is an item in that module
 - Treat the path as `self::u32::some_name`
 - Otherwise, treat it as `<u32 as ?>::some_name`


UFCS "inherent" paths can resolve to trait items
================================================
_Unconfirmed_
`<Foo>::SOMECONST` can either refer to `impl Foo { const SOMECONST... }  or `impl Trait for Foo { const SOMECONST ... }`


Blocks that don't yield a value can mark as diverged if ?any statement diverges
=============
- This includes any function call (or method call)
- TODO: Is this just the last statement? or all statements

The "base" value of a struct literal isn't always moved
======================================================
- Only the values used are moved, which can lead to the source not being moved (if all used values are Copy)


Binops are coercion points
==========================
- This only really shows up with some edge cases where the RHS is inferred

Casts can act as coercions
==========================
- E.g. `None as Option<Span>` is perfectly valid, and is the same as `None::<Span>`

