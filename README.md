
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
 - Almost because it uses special path types to handle: external crates, 'str', and anonymous modules.
- Converts name-resolved AST into a more compact "HIR"

Short-Term Plans
===
- Constant evaluation and insertion
 - Will be done by "executing" the HIR expressions
- Type resolution and checking (quite interlinked)
- Convert HIR expressions into a MIR similar to rustc's

Medium-Term Goals
===
- Convert MIR or HIR into C
 - Bonus points for making it readable C
 - NOTE: Due to `#[cfg]` attributes being resolved already, the C code won't be portable.

