#include <iostream>
#include <string>
#include "parse/lex.hpp"
#include "parse/parseerror.hpp"
#include "ast/ast.hpp"

extern AST::Crate Parse_Crate(::std::string mainfile);
extern void ResolvePaths(AST::Crate& crate);
extern AST::Flat Convert_Flatten(const AST::Crate& crate);

/// main!
int main(int argc, char *argv[])
{
    try
    {
        AST::Crate crate = Parse_Crate("samples/1.rs");

        // Resolve names to be absolute names (include references to the relevant struct/global/function)
        ResolvePaths(crate);

        // Typecheck / type propagate module (type annotations of all values)

        // Flatten modules into "mangled" set
        AST::Flat flat_crate = Convert_Flatten(crate);

        // Convert structures to C structures / tagged enums
        //Convert_Render(flat_crate, stdout);
    }
    catch(const ParseError::Base& e)
    {
        ::std::cerr << "Parser Error: " << e.what() << ::std::endl;
    }
    return 0;
}
