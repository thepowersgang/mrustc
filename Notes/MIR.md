% Mid-level intermediate representation


See https://github.com/rust-lang/rfcs/blob/master/text/1211-mir.md


Overview
========

Graph of "Basic Blocks"
- Stored as a vector, integer indexes between each other
- All function calls terminate their block
- Varaibles are single-assigment, but mutable via &mut or field accesses


Types
=====

LValues (assignable locations)
------------------------------

- `B` - User-declared binding (with a name and slot index)
- `TEMP` - Compiler-inserted temporary (with just an index)
- `ARG` - Function argument (with index only)
- `STATIC` - Reference to a `static` or `static mut`
- `RETURN` - The function's return value
- `LVALUE.f` - Refers to the field `f` of the inner `LVALUE`
- `*LVALUE` - Dereference a pointer (&-ptr or rawptr)
- `LVALUE[LVALUE]` - Index into an array (slice accesses use operator overloads)
- `(LVALUE as VARIANT)` - Downcast `LVALUE` into the specified enum variant (to access it as that variant)

RValues (usable values)
-----------------------

- `use(LVALUE)` - Read out of an LVALUE
- `&'rgn LVALUE` - Immutably borrow an LVALUE
- `&'rgn mut LVALUE` - Mutably borrow an LVALUE
- `LVALUE as TYPE` - Primitive cast operation
- `LVALUE <op> LVALUE` - Binary operation (on numeric primitives only)
- `<op> LVALUE` - Unary operation (numeric primitives and booleans only)
- `[LVALUE; LVALUE]` - Construct a sized array
- `[LVALUE, ...]` - Construct a literal array
- `(LVALUE, ...)` - Construct a tuple
- `PATH { f: LVALUE, ... }` - Construct a named struct
- `meta(LVALUE)` - Extract the fat pointer metadata for an lvalue
- `fatptr(LVALUE, LVALUE)` - Construct a fat pointer from a pair of lvalues
- `CONSTANT` - Subset of RValues that are known at compile-time


