% Generators

# Lowering point
- Doing gen during MIR gen is best... but that gets hacky.

# MIR Lowering
- `return` needs to be wrapped
- `yeild` becomes a return, and add all currently active variables to a save set
- After generation, rewrite all saved variables into state members
  - Filter out Copy variables that appear once, but are only used between yields and aren't borrowed
  - Filter out the if condition variable
  - Include drop flags!

Problem:
- Moving out of captures
  - Generators always have `Pin<&mut Self>`, so can't move out of them in usual ways...
  - Brings back the interesting question of Drop handling...
  - Maybe move owned values into locals unconditionally, and let normal value tracking run
  - Store owned/moved values in ManuallyDrop... same with saved values

- Drop impl
  - Needs to be generated during lower of the generator method
  - Or somehow the information needed needs to be propagated...
  - *Solution*: Generate drop glue during MIR gen and save for injection at the drop glue stage
    - Also solves the need for ManuallyDrop