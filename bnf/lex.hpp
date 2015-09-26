#pragma once

#include "ast_types.hpp"

struct ParserContext
{
	::std::string	filename;
	::std::string	base_path;
	
	::std::unique_ptr<Module>	output_module;
	
	// semi-evil hack used to break '>>' apart into '>' '>'
	 int	next_token;
	
	
	ParserContext(::std::string filename):
		filename(filename),
		output_module(),
		next_token(0)
	{}
	
	void pushback(int tok) {
		assert(next_token == 0);
		next_token = tok;
	}
};
#include ".gen/rust.tab.hpp"

extern int yylineno;
extern int yylex(YYSTYPE* lvalp, ParserContext& context);

extern void yyerror(ParserContext& context, const char *s);
extern int yydebug;

