/*
 */
#ifndef _MAIN_BINDINGS_HPP_
#define _MAIN_BINDINGS_HPP_

#include <string>

namespace AST {
    class Crate;
    class Flat;
}

/// Parse a crate from the given file
extern AST::Crate Parse_Crate(::std::string mainfile);


extern void Expand(::AST::Crate& crate);

/// Process #[] decorators
extern void Process_Decorators(AST::Crate& crate);

extern void Resolve_Use(::AST::Crate& crate);

/// Resolve all in-text paths to absolute variants
extern void ResolvePaths(AST::Crate& crate);
/// Check that generic bounds are valid
extern void Typecheck_GenericBounds(AST::Crate& crate);
/// Check that parameters for generics are valid
extern void Typecheck_GenericParams(AST::Crate& crate);
/// Type resolution (and hence checking) for expressions
extern void Typecheck_Expr(AST::Crate& crate);

/// Convert the AST to a flat tree
extern AST::Flat Convert_Flatten(const AST::Crate& crate);


/// Dump the crate as annotated rust
extern void Dump_Rust(const char *Filename, const AST::Crate& crate);

#endif

