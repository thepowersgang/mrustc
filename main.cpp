#include <iostream>
#include "parse/lex.hpp"
#include "parse/parseerror.hpp"

using namespace std;

extern void Parse_Crate(::std::string mainfile);

int main(int argc, char *argv[])
{
    try
    {
        Parse_Crate("samples/1.rs");
    }
    catch(const ParseError::Base& e)
    {
        ::std::cerr << "Parser Error: " << e.what() << ::std::endl;
    }
    return 0;
}
