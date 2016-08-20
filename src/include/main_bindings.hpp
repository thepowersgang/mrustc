/*
 */
#ifndef _MAIN_BINDINGS_HPP_
#define _MAIN_BINDINGS_HPP_

#include <string>
#include <memory>

namespace AST {
    class Crate;
    class Flat;
}

/// Parse a crate from the given file
extern AST::Crate Parse_Crate(::std::string mainfile);


extern void Expand(::AST::Crate& crate);

/// Process #[] decorators
extern void Process_Decorators(AST::Crate& crate);

/// Convert the AST to a flat tree
extern AST::Flat Convert_Flatten(const AST::Crate& crate);


/// Dump the crate as annotated rust
extern void Dump_Rust(const char *Filename, const AST::Crate& crate);

#endif

