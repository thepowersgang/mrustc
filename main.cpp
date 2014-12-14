#include <iostream>
#include "parse/lex.hpp"
#include "parse/parseerror.hpp"

using namespace std;

extern AST::Crate Parse_Crate(::std::string mainfile);
extern void ResolvePaths(AST::Crate& crate);

/// main!
int main(int argc, char *argv[])
{
    try
    {
        AST::Crate crate = Parse_Crate("samples/1.rs");

        // Resolve names into absolute?
        ResolvePaths(crate);

        // Flatten modules into "mangled" set

        // Typecheck / type propagate module (type annotations of all values)

        // Convert structures to C structures / tagged enums
    }
    catch(const ParseError::Base& e)
    {
        ::std::cerr << "Parser Error: " << e.what() << ::std::endl;
    }
    return 0;
}
