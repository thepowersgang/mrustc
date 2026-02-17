```rust
trait Foo {
    fn no_default(&self) -> impl ::std::fmt::Debug;
    fn with_default(&self) -> impl ::std::fmt::Debug {
        0
    }
}
```

# Solution: Replace the erased types with associated types
- After typecheck (in VTable generation, just because): Visit trait/trait-impl function returns and rewrite ErasedType to ATYs (creating the aty definition)
- In ErasedType expand: Replace with the ATY if the associated path refers to the trait (not a type)

# Issue:
This works well until defaulted methods are involved, as they have a conflict at final validation (the method returns the ATY, but the body expects the real type).
That's a critical problem with this approach - the trait (in HIR metadata) needs to reference the ATY (or the bare ErasedType), but the body of the method needs the actual type for type checking and codegen to be happy.

# Possible solutions:
- Special case monomorph of return type for method validation.
- Separate the public signature and internal return type in functions
  - This could work? Would also allow the type to stay erased in crate metadata
  - Slightly sticky with validating function calls
- Have a new variant of ErasedType that gets used for trait methods
  - Doesn't really gel with how "Expand HIR ErasedType" works
  - Although, maybe it should - current model seems to wipe the ErasedType in crate metadata


# Solution 1: Separate the public and internal return types
- Add a return type to `ExprPtr`, this is the internal return and gets fully expanded from ErasedType
  - Good in theory, but annoying everywhere else - so fiddly
- Could modify monomorph of return types a bit?