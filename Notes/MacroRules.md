% `macro_rules!` rework #2

Problem Cases
=============

libcollections `vec!`
---------------------

```
( $e:expr ; $n:expr )
( $($e:expr),* )
( $($e:expr , )* )
```

Problems:
- All arms begin with $e:expr (or none)
- Two arms follow the same pattern until the end. (where one terminates with no comma)

Parsing Ruleset?
- if empty, ARM 2
- expect `:expr`
- if `;`
 - expect `:expr`
 - ARM 1
- LOOP
 - if empty, ARM 2
 - expect `,`
 - if empty, ARM 3
 - expect `:expr`



Rule Generation Algorithm
=========================
- For each arm, convert into a set of decision rules
 - `LOOP` - Contains a continuable loop
 - `IF [NOT] pat, BREAK` - Compare the next token and break out of the current loop if it matches (non-consuming)
 - `EXPECT pat` - Error if the current token isn't as expected (consumes token)
 - `COMPLETE` - Complete the current arm early
- Combine rules from arms into a more complex ruleset as above
 - IF pat, COMPLETE
 - IF pat, SUBTREE
 - EXPECT pat
 - COMPLETE
 - LOOP

Example Application: `vec!`
---------------------------

Arm rulesets
- arm 1
 - EXPECT `:expr` , EXPECT `;` , EXPECT `:expr` , COMPLETE
- arm 2
 - IF `:expr` { LOOP { EXPECT `:expr` , IF NOT `,` BREAK , EXPECT `,` } }
- arm 2
 - IF `:expr` { LOOP { EXPECT `:expr` , EXPECT `,` IF empty BREAK } }

Merging
- "EXPECT `:expr` , EXPECT `;` , EXPECT `:expr` , COMPLETE 1"
- "EXPECT `:expr`" + (2) "IF `:expr` { ... } COMPLETE"
 - insert "IF NOT `:expr` COMPLETE 2" at start
- "EXPECT `:expr`" + (2) "LOOP { ... }"
 - Recurse into loop
 - "EXPECT `:expr`" + (2) "EXPECT `:expr`"
  - "EXPECT `:expr`"
 - "EXPECT `;`" + "IF NOT `,` BREAK"
  - "IF `;` { EXPECT `;` ... } LOOP { IF
  - TODO: This needs to break out of the loop.

Problem: Generating LOOP
------------------------
Looping is ideally handled by a loop sub-tree as in the example ruleset, but generating that sub-tree from an overlapping set of rules may be non-trivial.




Solution 2: Lockstep execution
==============================

- Generate a sequence of simple pattern rules from the parsed pattern
 - `EXPECT`, `IF-BREAK`, `END`
 - These are genrated by an "iterator" structure that has a way to be poked if an `IF` succeeded
- Run all arms in lockstep, pruning arms as they fail to match
- Error if two arms attempt to do an incompatible expect (basically any pattern with another pattern)
- TODO: How to handle bindings from `EXPECT` at different levels
 - E.g. In the `vec!` example, the first arm binds at the root, but the other two bind inside a loop
 - Possibly solvable with brute-force allowing non-repeating contexts to read from a single-repetition?
 - Or multi-tagging the binding (so once the arm is known, the level is fixed)
