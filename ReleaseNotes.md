# 2021-04-04: rustc 1.39.0 bootstrap

New/interesting supported features since 1.29
* Basic/minimal constant generics
  * This forced a much-needed internal cleanup of generic/monomorphisation handling
* Generators (needed for one small bit in `librustc_interface`)
* Much better handling of editions and the mess involved around handling different editions across macros
* By-value trait object methods (lots of hackery here)

Prominent internal changes
* Type resolution (`hir_typeck/expr_cs.cpp`) completely reviewed and mostly justified (see `Notes/TypecheckIssues.md`)
  to hopefully finally stop typecheck errors being the bane of my existence
* `HIR::TypeRef` reworked to actually be a "Ref" (reference counted pointer). This was profiled to save quite a bit of
  memory via de-duplication and reduction of inline sizes
* Enum niche optimisation implemented (including a correct implementation of `repr(Rust,uNN)` layouts
* Full MIRI constant evaluation (near-complete removal of the `HIR::Literal` type).
  * Resolved statics and constants are now stored as bytes with relocations
* `proc_macro` library expanded to support new features (and allow better testing)
* `minicargo` supports overriding parts of crate manifests (as an alternative to doing source patches)
* `minicargo` can now handle `rustc` as well as `mrustc`
* Cleaned up handling of hygine (attaching directly to idents)
* `macro_rules` handling reworked to pre-calculate looping logic
* Classification of named patterns into structs and enums deferred until after associated type expansion
* De-duplication of impl resolution logic for looking up values (for better and more consistent support of impl specialisation)
* Specialised `RcString::ord` for interned strings (using a lazily cached ordering) for faster lookups)
* Explicit MIR-level generation of vtables and drop glue (`trans/auto_impls.cpp`)

Minor/notable changes
* Improved macro resolution logic
* Cleanup to AST path handling
* Support for one AST path referring to two different items (e.g. a type and a function, via colliding `use` statements)
* Unified trait impl lists after HIR generation (to avoid searching `n_crates` different impl lists)
* LOTS of usage of `TU_MATCH`/`TU_MATCH_DEF` replaced with `TU_MATCH_HDRA`
* HIR serialisation reworked to support some encoded structure (for better error diagnosis)
* Speed up Trans Enumerate by caching paths/types needed by a function

