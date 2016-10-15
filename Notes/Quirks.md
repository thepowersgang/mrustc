
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


The `ident` fragment matches reserved words
===

Typically used for replacing `self` in macros.



Any integer can cast to a pointer
===================
This includes `u8`


Modules with the same name as primtiive types have interesting lookup quirks
===================
- If a path like `u32::some_name` is seen, if the module `u32` exists in the current scope and `some_name` is an item in that module
 - Treat the path as `self::u32::some_name`
 - Otherwise, treat it as `<u32 as ?>::some_name`



