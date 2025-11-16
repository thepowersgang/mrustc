# CLAUDE.md - mrustc Developer Guide for AI Assistants

This document provides a comprehensive guide to the mrustc codebase structure, development workflows, and conventions for AI assistants contributing to the project.

## Project Overview

**mrustc** (Mutabah's Rust Compiler) is an alternative Rust compiler written in C++14 that can bootstrap rustc. Unlike the official Rust compiler, mrustc compiles Rust to C code (not LLVM IR), which is then compiled by gcc/clang.

### Key Characteristics
- **Primary Goal**: Bootstrap rustc from source
- **Language**: C++14
- **Supported Rust Versions**: 1.19.0, 1.29.0, 1.39.0, 1.54.0, 1.74.0
- **Output Format**: C code (can emit MMIR for interpreter)
- **Philosophy**: Assumes valid input; focuses on correctness over friendly error messages
- **Codebase Size**: ~132,000 lines of C++ across 200+ source files

---

## Codebase Structure

### Directory Layout

```
mrustc/
├── src/                    # Main compiler source code (~132k lines C++)
│   ├── include/           # Common headers and utilities
│   ├── ast/               # Abstract Syntax Tree (1:1 with source)
│   ├── parse/             # Lexer and parser
│   ├── expand/            # Macro and attribute expansion
│   ├── macro_rules/       # macro_rules! implementation
│   ├── resolve/           # Name resolution
│   ├── hir/               # High-level IR (simplified AST)
│   ├── hir_conv/          # HIR transformations
│   ├── hir_typeck/        # Type checking and inference
│   ├── hir_expand/        # Post-typecheck HIR transformations
│   ├── mir/               # Mid-level IR (control flow graph)
│   └── trans/             # Code generation and translation
├── tools/                 # Companion tools
│   ├── minicargo/         # Cargo-like build tool
│   ├── standalone_miri/   # MIR interpreter
│   ├── testrunner/        # Test harness
│   └── dump_hirfile/      # HIR inspection tool
├── docs/                  # User documentation
├── Notes/                 # Extensive developer documentation
├── samples/               # Test programs
├── script-overrides/      # Pre-computed build script outputs
├── lib/                   # Rust support libraries
├── Makefile              # Main compiler build
├── minicargo.mk          # rustc/cargo bootstrap orchestration
└── README.md             # Project readme
```

---

## Compilation Pipeline

mrustc implements a multi-stage transformation pipeline. Understanding this is crucial for working on the compiler.

### Complete Pipeline (44 Stages)

#### AST Phase (Source → Abstract Syntax Tree)
1. **Target Load** - Parse target specification
2. **Parse** - Source code → AST (1:1 mapping, preserves ordering)
3. **LoadCrates** - Load explicitly mentioned extern crates
4. **Expand** - Macro/attribute expansion, load remaining crates
5. **Implicit Crates** - Add test harness, allocator, panic crates
6. **Resolve Use** - Resolve `use` statements
7. **Resolve Index** - Build module item indexes
8. **Resolve Absolute** - Convert all paths to absolute

#### HIR Phase (Simplified High-level IR)
9. **HIR Lower** - AST → HIR conversion
10. **Lifetime Elision** - Infer omitted lifetimes
11. **Resolve Type Aliases** - Expand type aliases
12. **Resolve Bind** - Set binding annotations
13. **Resolve UFCS Outer** - Resolve outer UFCS paths
14. **Resolve HIR Self Type** - Expand `Self`
15. **Resolve HIR Markings** - Generate trait markers (Copy, Send, Sync, etc.)
16. **Sort Impls** - Organize implementations
17. **Resolve UFCS paths** - Complete UFCS resolution
18. **Constant Evaluate** - Basic constant evaluation

#### Typecheck Phase
19. **Typecheck Outer** - Check impl signatures
20. **Typecheck Expressions** - Full type inference

#### HIR Expansion Phase
21. **Expand HIR Annotate** - Annotate expression usage
22. **Expand HIR Static Borrow Mark** - Mark static borrows
23. **Expand HIR Lifetimes** - Lifetime inference
24. **Expand HIR Closures** - Desugar closures into structs
25. **Expand HIR Static Borrow** - Handle static borrows
26. **Expand HIR VTables** - Generate vtables
27. **Expand HIR Calls** - Convert all calls to UFCS
28. **Expand HIR Reborrows** - Insert reborrow operations
29. **Expand HIR ErasedType** - Handle `impl Trait`
30. **Typecheck Expressions (validate)** - Re-validate types

#### MIR Phase (Control Flow Graph)
31. **Lower MIR** - HIR expressions → MIR CFG
32. **MIR Validate** - Basic validation
33. **MIR Cleanup** - Various transformations
34. **MIR Validate Full Early** - Optional exhaustive validation
35. **MIR Borrowcheck** - Borrow checking (optional)
36. **MIR Optimise** - Optimizations (const prop, DCE, inlining)
37. **MIR Validate PO** - Post-optimization validation
38. **MIR Validate Full** - Optional exhaustive validation

#### Translation Phase
39. **HIR Serialise** - Write .hir files
40. **Trans Enumerate** - Find all needed items
41. **Trans Auto Impls** - Generate auto trait impls
42. **Trans Monomorph** - Instantiate generics
43. **MIR Optimise Inline** - Inline with full type info
44. **Trans Codegen** - Generate C code

### Key Stage Transitions
- **Parse → AST**: 1:1 with source, maintains ordering, unexpanded macros
- **AST → HIR**: Simplified, only absolute paths, no high-level constructs
- **HIR → MIR**: Control flow graph, basic blocks, single-assignment variables
- **MIR → C**: C code generation for gcc/clang

---

## Key Source Files

### Critical Files by Size/Complexity

| File | Lines | Purpose |
|------|-------|---------|
| `trans/codegen_c.cpp` | 396,835 | C code generation backend |
| `mir/optimise.cpp` | 283,932 | MIR optimizations |
| `mir/from_hir_match.cpp` | 166,398 | Pattern matching lowering |
| `mir/from_hir.cpp` | 143,963 | Expression → MIR lowering |
| `hir_expand/lifetime_infer.cpp` | 132,584 | Lifetime inference |
| `expand/mod.cpp` | 109,652 | Macro expansion orchestration |
| `trans/target.cpp` | 100,815 | Target specifications |
| `hir_expand/closures.cpp` | 99,731 | Closure desugaring |
| `expand/derive.cpp` | 93,387 | Derive macro implementation |

### Core Infrastructure Files
- **src/main.cpp** (53,878 lines) - Compiler entry point, orchestrates all passes
- **src/common.hpp** - Global utilities (mv$, box$, FMT macros, Ordering)
- **src/include/tagged_union.hpp** - Discriminated union macro system
- **src/debug.cpp** - Phase-based debug logging
- **src/span.cpp** - Source location tracking

### Key Tool Files
- **tools/minicargo/main.cpp** (20,245 lines) - Build tool entry point
- **tools/minicargo/build.cpp** (43,434 lines) - Build orchestration
- **tools/standalone_miri/miri.cpp** (97,106 lines) - MIR interpreter

---

## Coding Conventions and Patterns

### C++ Conventions

#### Custom Macros (defined in common.hpp)
```cpp
mv$(x)           // Shorthand for std::move(x)
box$(x)          // Create unique_ptr: make_unique_ptr(std::move(x))
rc_new$(x)       // Create shared_ptr: make_shared_ptr(std::move(x))
FMT(ss)          // String formatting: ostringstream() << ss
```

#### Tagged Unions (Discriminated Unions)
mrustc heavily uses tagged unions via macros in `include/tagged_union.hpp`:

```cpp
// Matching on tagged unions
TU_MATCH(variant, (value), (binding),
    (CaseA, binding_a,
        // Handle CaseA
    ),
    (CaseB, binding_b,
        // Handle CaseB
    )
)

TU_MATCH_DEF(variant, (value), (binding),  // With default case
    (CaseA, binding_a,
        // Handle CaseA
    )
)
(
    // Default case
)

TU_MATCH_HDRA(args...) // Header-only variant matching
```

#### Ordering and Comparison
```cpp
enum Ordering {
    OrdLess = -1,
    OrdEqual,
    OrdGreater
};

// Three-way comparison using ord() functions
Ordering result = ord(left, right);
```

#### Reference Counting Patterns
- `RcString` - Reference-counted strings for deduplication
- `HIR::TypeRef` - Reference-counted type references
- Extensive use of `unique_ptr` and `shared_ptr`

### Code Organization Patterns

#### Visitor Pattern
Used extensively for AST/HIR traversal:
```cpp
class Visitor {
    virtual void visit_expr(AST::Expr& expr) = 0;
    virtual void visit_type(AST::Type& ty) = 0;
    // ...
};
```

#### Debug Infrastructure
```cpp
// Timed phase tracking
DebugTimedPhase phase("Phase Name");

// Phase-based logging (controlled by MRUSTC_DEBUG env var)
DEBUG("PhaseNamePass1:File" << level, "Message: " << value);

// Spans for error reporting
BUG(span, "Internal compiler error message");
ERROR(span, E0000, "Error message");
```

#### LValue Representation (MIR)
MIR LValues use a nested wrapper pattern:
```cpp
// Root value with zero or more wrappers
LValue {
    root: Variable(index),
    wrappers: [Field(0), Deref, Index(var)]
}
```

---

## Build System

### Primary Build Targets

#### Building mrustc Compiler
```bash
make                          # Build mrustc compiler
make PARLEVEL=$(nproc)       # Parallel build (can be unstable)
make GPROF=1                 # Build with gprof profiling
```

**Output**: `bin/mrustc` (or `bin/mrustc-gprof`)
**Object Files**: `.obj/` (or `.obj-gprof/`)

#### Building rustc/cargo with mrustc
```bash
make RUSTCSRC                                    # Download rustc source
make -f minicargo.mk                            # Build libstd, rustc, cargo
make -f minicargo.mk LIBS                       # Build just standard library
make -f minicargo.mk RUSTC_VERSION=1.54.0       # Specify rustc version
make -C run_rustc                               # Test built rustc
```

#### Building Custom Code
```bash
# Build your project with minicargo
minicargo -L output/libstd <crate_path>

# Direct mrustc invocation
mrustc -L output/libstd --out-dir output <path_to_main.rs>
```

### Build Configuration

#### Environment Variables
- **PARLEVEL** - Number of parallel jobs (default: auto, use 1 if unstable)
- **MRUSTC_DEBUG** - Colon-separated list of debug passes (e.g., `Expand:Parse`)
- **MRUSTC_FULL_VALIDATE** - Enable exhaustive MIR validation
- **MRUSTC_TARGET** - Cross-compilation target
- **RUSTC_VERSION** - rustc version to bootstrap (1.19.0-1.74.0)
- **MMIR** - Enable MMIR backend

#### Makefile Variables
- **CXX** - C++ compiler (default: g++)
- **V** or **VERBOSE** - Enable verbose output
- **GPROF** - Enable gprof profiling
- **CXXFLAGS_EXTRA** - Additional C++ flags
- **LINKFLAGS_EXTRA** - Additional linker flags

### Build Output Structure
```
bin/                    # Executables (mrustc, minicargo, etc.)
.obj/                   # Object files
output/                 # Build artifacts (libstd, .hir files, etc.)
  libcore.hir          # HIR serialization files
  libcore.hir_dbg.txt  # Debug output from compilation
```

---

## Development Workflows

### Working on the Compiler

#### 1. Understanding a Pass
```bash
# Read the phase overview
cat Notes/PhaseOverview.md

# Find the relevant source file
# AST passes: src/ast/, src/parse/, src/expand/, src/resolve/
# HIR passes: src/hir/, src/hir_conv/, src/hir_typeck/, src/hir_expand/
# MIR passes: src/mir/
# Codegen: src/trans/

# Enable debug output for specific passes
export MRUSTC_DEBUG="Expand:Parse"
make -f minicargo.mk LIBS
```

#### 2. Modifying a Pass
```bash
# Edit the relevant file in src/
vim src/hir_typeck/expr_cs.cpp

# Rebuild mrustc
make

# Test with a simple program
echo 'fn main() { println!("test"); }' > test.rs
./bin/mrustc test.rs --out-dir output -L output/libstd
```

#### 3. Testing Changes
```bash
# Run rustc bootstrap test
./TestRustcBootstrap.sh

# Run with debug output
MRUSTC_DEBUG="Pass1:Pass2" make -f minicargo.mk LIBS

# Check debug logs
less output/libcore.hir_dbg.txt
```

### Adding a New Feature

#### 1. Determine Affected Stages
Most features require changes across multiple stages:
- **Syntax changes**: parse/, ast/
- **Macro-related**: expand/
- **Name resolution**: resolve/, hir_conv/
- **Type system**: hir_typeck/
- **Code generation**: hir_expand/, mir/, trans/

#### 2. Implementation Order
Follow the compilation pipeline:
1. Update parser if syntax changes
2. Update AST structures
3. Update HIR lowering
4. Update type checking
5. Update MIR generation
6. Update code generation

#### 3. Add Tests
```bash
# Add test cases to samples/
echo 'fn main() { /* test code */ }' > samples/test/new_feature.rs

# Test compilation
./bin/mrustc samples/test/new_feature.rs --out-dir output -L output/libstd
```

### Debugging

#### Enable Debug Output
```bash
# Show all available debug categories
MRUSTC_DEBUG=help ./bin/mrustc

# Enable specific passes
MRUSTC_DEBUG="Expand:Typecheck" ./bin/mrustc ...

# Enable all debug output (very verbose!)
MRUSTC_DEBUG="*" ./bin/mrustc ...
```

#### Dump Intermediate Representations
```bash
# Dump AST
./bin/mrustc --dump-ast test.rs

# Dump HIR
./bin/mrustc --dump-hir test.rs

# Dump MIR
./bin/mrustc --dump-mir test.rs

# Inspect HIR file
./bin/dump_hirfile output/libcore.hir
```

#### Use MIR Validation
```bash
# Enable exhaustive MIR validation
export MRUSTC_FULL_VALIDATE=1
make -f minicargo.mk LIBS
```

#### Common Debug Patterns
1. **Segfault/crash**: Run under gdb/lldb
   ```bash
   gdb --args ./bin/mrustc test.rs
   ```

2. **Type inference issues**: Enable `Typecheck` debug
   ```bash
   MRUSTC_DEBUG="Typecheck" ./bin/mrustc test.rs
   ```

3. **MIR generation issues**: Enable `MIR` and dump MIR
   ```bash
   MRUSTC_DEBUG="MIR" ./bin/mrustc --dump-mir test.rs
   ```

4. **Codegen issues**: Check generated C code
   ```bash
   # C code is in output/ directory
   less output/test.c
   ```

---

## Testing

### Test Infrastructure

#### Bootstrap Testing
```bash
# Full rustc bootstrap validation
./TestRustcBootstrap.sh

# Test specific version
./build-1.54.0.sh
```

#### Standalone MIRI Testing
```bash
# Run MIRI tests
./test_smiri.sh
```

#### Sample Programs
```bash
# Test samples
cd samples/test
for f in *.rs; do
    ../../bin/mrustc "$f" --out-dir output -L ../../output/libstd || echo "FAILED: $f"
done
```

### Known Test Failures
Check `disabled_tests_run-pass.txt` for tests that are known to fail.

### Adding Tests
1. Add test program to `samples/test/`
2. Ensure it compiles with official rustc
3. Test with mrustc
4. Add to test suite if needed

---

## Important Files and Documentation

### Essential Documentation

#### User Documentation
- **README.md** - Project overview, getting started
- **docs/running.md** - How to use mrustc and minicargo
- **docs/target.md** - Custom target specification format

#### Developer Documentation (Notes/)
- **PhaseOverview.md** - Complete compilation pipeline
- **ImplementationNotes/00-Overall.md** - Architecture overview
- **MIR.md** - MIR design and structure
- **Bootstrapping.md** - Bootstrap process details
- **BorrowChecker.md** - Borrow checker implementation
- **MacroRules.md** - macro_rules! implementation
- **TypecheckIssues.md** - Known type checking issues (56KB!)

#### Issue Documentation
- **ISSUE-*.txt** files document specific known problems
- **BugStories.txt** - Interesting bugs and their fixes
- **UpgradeQuirks.txt** - Version upgrade considerations

### Configuration Files
- **rust-version** - Default rustc version to build
- **rustc-*.patch** - Version-specific patches to rustc source
- **rustc-*.toml** - Override files for minicargo

---

## Architecture Deep Dive

### AST Design
- **Purpose**: 1:1 representation of source code
- **Features**:
  - Maintains ordering
  - Preserves unexpanded macros
  - Supports relative paths
  - Contains all source information
- **Key types**: `AST::Module`, `AST::Item`, `AST::Expr`, `AST::Type`, `AST::Pattern`

### HIR Design
- **Purpose**: Simplified, lowered representation
- **Features**:
  - Only absolute paths
  - No macros (fully expanded)
  - Simpler type system
  - No high-level constructs (for loops, while, etc.)
  - Can be serialized to .hir files
- **Key types**: `HIR::Module`, `HIR::Item`, `HIR::ExprNode`, `HIR::TypeRef`

### MIR Design
- **Based on**: rustc MIR RFC #1211 (not rustc implementation!)
- **Structure**: Control flow graph with basic blocks
- **Variables**: Single-assignment (mutable via &mut)
- **LValues**: Root + wrapper chain (deref, field, index)
- **Statements**: Assign, Drop, Asm, SetDropFlag, ScopeEnd
- **Terminators**: Goto, Return, If, Switch, Call, Panic
- **Key types**: `MIR::Function`, `MIR::BasicBlock`, `MIR::Statement`, `MIR::Terminator`

### Type System
- **Type Inference**: Constraint-based in `hir_typeck/expr_cs.cpp`
- **Trait Resolution**: Recursive search with caching
- **Lifetime Inference**: Full inference in `hir_expand/lifetime_infer.cpp`
- **Auto Traits**: Generated in `trans/auto_impls.cpp`

### Code Generation
- **Primary Backend**: C code generation (`trans/codegen_c.cpp`)
- **Process**: MIR → C → gcc/clang → executable/library
- **Features**:
  - Basic optimizations rely on C compiler
  - Custom mangling scheme
  - Support for multiple targets via target specs
  - Can emit standalone or library code

---

## AI Assistant Guidelines

### When Modifying Code

#### 1. Always Read Before Writing
- Use Read tool to examine existing code
- Understand the context and surrounding code
- Follow existing patterns and conventions

#### 2. Respect the Pipeline
- Changes often need to propagate through multiple stages
- If you modify AST, check if HIR lowering needs updates
- If you modify HIR, check if MIR generation needs updates
- If you modify MIR, check if codegen needs updates

#### 3. Use Proper Macros
- Use `mv$()` instead of `std::move()`
- Use `box$()` for unique_ptr creation
- Use `FMT()` for string formatting
- Use `TU_MATCH` family for variant matching
- Use `DEBUG()` for logging, not cout/cerr

#### 4. Handle Tagged Unions Carefully
- Always handle all variants (use TU_MATCH_DEF for default)
- Use proper binding syntax
- Don't forget to move values out with mv$()

#### 5. Maintain Span Information
- Always propagate `Span` objects for error reporting
- Use `BUG()` for internal errors, `ERROR()` for user errors
- Include helpful context in error messages

### When Debugging

#### 1. Start with Debug Logs
- Set `MRUSTC_DEBUG` to relevant passes
- Check `output/*_dbg.txt` files
- Look for "BUG hit" or assertion failures

#### 2. Use Dumps
- `--dump-ast`, `--dump-hir`, `--dump-mir` to see intermediate forms
- Compare with working code
- Check if transformations are correct

#### 3. Narrow Down the Issue
- Test with minimal reproduction
- Binary search through passes (comment out passes in main.cpp)
- Check if issue is in parsing, typechecking, or codegen

#### 4. Validate Assumptions
- Enable `MRUSTC_FULL_VALIDATE` for MIR issues
- Check HIR serialization/deserialization
- Verify type inference with debug output

### Common Pitfalls

#### 1. Tagged Union Matching
```cpp
// WRONG - doesn't handle all cases
TU_MATCH(MyType::Data, (value), (e),
(Integer, i,
    // ...
)
)

// RIGHT - handles all cases
TU_MATCH_DEF(MyType::Data, (value), (e),
(Integer, i,
    // ...
)
)
(
    // Default case
)
```

#### 2. Move Semantics
```cpp
// WRONG - value still used after move
auto x = mv$(value);
use_value(value);  // BUG!

// RIGHT - use moved value
auto x = mv$(value);
use_value(x);
```

#### 3. Span Propagation
```cpp
// WRONG - no span for errors
auto new_expr = create_expr();

// RIGHT - always include span
auto new_expr = create_expr(original.span());
```

#### 4. Debug Output
```cpp
// WRONG - direct output
std::cout << "Debug: " << value << std::endl;

// RIGHT - use DEBUG macro
DEBUG("PassName", "Debug: " << value);
```

### Performance Considerations

#### 1. RcString Usage
- Use RcString for frequently duplicated strings
- Saves memory in AST/HIR
- Efficient comparison (pointer equality)

#### 2. HIR Serialization
- .hir files cache parsed/typechecked crates
- Significantly speeds up builds
- Ensure changes don't break serialization

#### 3. MIR Optimizations
- Most optimization happens in MIR phase
- C compiler also optimizes, but mrustc does basic work
- Don't over-optimize in early passes

### Building and Testing Workflow

#### 1. Incremental Development
```bash
# Make small changes
vim src/path/to/file.cpp

# Rebuild just mrustc
make

# Test with small program
./bin/mrustc test.rs -L output/libstd --out-dir output

# If successful, test with larger program
make -f minicargo.mk LIBS
```

#### 2. Full Validation
```bash
# After significant changes
make clean
make -f minicargo.mk

# Full bootstrap test
./TestRustcBootstrap.sh
```

#### 3. Debug Build Failures
```bash
# Check debug logs
less output/failing_crate.hir_dbg.txt

# Enable specific debug output
MRUSTC_DEBUG="Relevant:Passes" make -f minicargo.mk LIBS

# Use minimal reproduction
./bin/mrustc minimal_test.rs --dump-mir
```

---

## Git Workflow

### Branch Management
- Development happens on feature branches
- Branch naming: `claude/claude-md-*` for AI assistant work
- Always push to the correct branch with `-u origin <branch-name>`

### Commit Messages
- Clear, descriptive messages
- Reference issue numbers if applicable
- Separate logical changes into separate commits

### Making Changes
1. Ensure you're on the correct branch
2. Make focused, logical changes
3. Build and test locally
4. Commit with descriptive message
5. Push to remote with retry logic for network issues

---

## Quick Reference

### Most Important Files to Understand
1. `src/main.cpp` - Compiler orchestration
2. `src/common.hpp` - Core macros and utilities
3. `Notes/PhaseOverview.md` - Complete pipeline
4. `src/hir_typeck/expr_cs.cpp` - Type inference
5. `src/mir/from_hir.cpp` - MIR generation
6. `src/trans/codegen_c.cpp` - Code generation

### Debug Environment Variables
```bash
export MRUSTC_DEBUG="Parse:Expand:Typecheck"  # Enable debug passes
export MRUSTC_FULL_VALIDATE=1                  # Exhaustive MIR validation
export PARLEVEL=1                              # Disable parallel build
```

### Quick Build Commands
```bash
make                                    # Build mrustc compiler
make -f minicargo.mk LIBS              # Build standard library
make -f minicargo.mk                   # Build rustc and cargo
./bin/mrustc file.rs -L output/libstd  # Compile single file
```

### Common Debug Tasks
```bash
# See all debug categories
MRUSTC_DEBUG=help ./bin/mrustc

# Dump intermediate forms
./bin/mrustc --dump-ast file.rs
./bin/mrustc --dump-hir file.rs
./bin/mrustc --dump-mir file.rs

# Inspect HIR file
./bin/dump_hirfile output/libcore.hir

# Check generated C code
less output/filename.c
```

---

## Additional Resources

### External Documentation
- Rust MIR RFC #1211: https://rust-lang.github.io/rfcs/1211-mir.html
- Rust Reference: https://doc.rust-lang.org/reference/
- Rust Book: https://doc.rust-lang.org/book/

### Project Resources
- GitHub: https://github.com/thepowersgang/mrustc
- IRC: irc.libera.chat#mrustc

### Notes Directory Deep Dive
Key files in `Notes/`:
- **PhaseOverview.md** - Your map of the compilation process
- **MIR.md** - Essential for understanding MIR
- **TypecheckIssues.md** - Known issues and quirks
- **BugStories.txt** - Learn from past bugs
- **ImplementationNotes/** - Detailed design docs

---

## Version Information

This CLAUDE.md is current as of:
- **mrustc commit**: aeb58f3 (Merge PR #372)
- **Supported rustc versions**: 1.19.0, 1.29.0, 1.39.0, 1.54.0, 1.74.0
- **C++ standard**: C++14
- **Primary platform**: x86-64 Linux GNU

---

## Final Notes for AI Assistants

### Philosophy
- mrustc prioritizes **correctness for bootstrapping** over user-friendly errors
- Assumes input is valid Rust code
- Most "errors" are internal compiler bugs, not user errors
- Complex but well-organized; follow the pipeline

### Success Patterns
- Small, focused changes
- Test incrementally
- Use debug output extensively
- Follow existing patterns
- Understand the full pipeline

### When in Doubt
1. Read the existing code
2. Check `Notes/` documentation
3. Enable debug output
4. Test with minimal reproduction
5. Compare with working code
6. Ask specific questions

**Remember**: This is a sophisticated compiler with 40+ compilation stages. Take time to understand the architecture before making changes. The debug infrastructure is your friend!
