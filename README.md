
Intro
===
This project is an attempt at creating a simple rust compiler in C++, with the ultimate goal of being a seperate reimplementation.

The short-term goal is to compile pre-borrowchecked rust code into C, for passing to an existing C compiler. Thankfully, (from what I have seen), the borrow checker is not needed to compile rust code (just to ensure that it's valid)

Current Features
===
- Successfully parses libcore
- Resolves all paths to absolute forms
- Outputs the processed AST as (almost) rust code
 - Almost because it uses special path types to handle: external crates, 'str', and anonymous modules.

Short-Term Plans
===
- Type resolution and checking (quite interlinked)
- Converting operator invocations to explicit calls

Medium-Term Goals
===
- Flattening AST into an intermediate form with no module higherarchy or generics
- Converting flat AST into C
 - Bonus points for making it readable C

