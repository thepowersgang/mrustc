
Intro
===
This project is an attempt at creating a simple rust compiler in C++, with the ultimate goal of being a seperate reimplementation.

The short-term goal is to compile pre-borrowchecked rust code into C, for passing to an existing C compiler. Thankfully, (from what I have seen), the borrow checker is not needed to compile rust code (just to ensure that it's valid)

Current Features
===
- Successfully parses libcore and rustc's run-pass tests
- Attribute and macro expansion
- Resolves all paths to absolute forms
- Outputs the processed AST as (almost) rust code
 - Almost because it uses special path types to handle external crates and anonymous modules.
- Converts name-resolved AST into a more compact "HIR"
- Hackily evaluates constants
 - Constant evaluation is done by using duck-typing, which is then validated by the Type Check pass
- Partial type checking and inferrence

Short-Term Plans
===
- Completed type checking, including a validation pass
- Convert HIR expressions into a MIR similar to rustc's
- Storing of HIR/MIR for `extern crate` handling

Medium-Term Goals
===
- Convert MIR or HIR into C
 - Bonus points for making it readable C
 - NOTE: Due to `#[cfg]` attributes being resolved already, the C code won't be portable.
- Propagate lifetime annotations so that MIR can include a borrow checker

