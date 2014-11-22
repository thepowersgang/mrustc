#include <iostream>
#include "parse/lex.hpp"

using namespace std;

extern void Parse_Crate(::std::string mainfile);

int main(int argc, char *argv[])
{
    Parse_Crate("samples/1.rs");
    return 0;
}
