#pragma once

#include "ast_types.hpp"

struct ParserContext
{
	::std::string	filename;
	::std::string	base_path;

	::std::unique_ptr<Module>	output_module;

	// semi-evil hack used to break '>>' apart into '>' '>'
	::std::vector<int>	next_token;


	ParserContext(::std::string filename):
		filename(filename),
		output_module(),
		next_token(0)
	{
		//next_token.reserve(2);
	}

	int popback() {
		if( next_token.size() > 0 ) {
			int rv = next_token.back();
			next_token.pop_back();
			return rv;
		}
		else {
			return 0;
		}
	}
	void pushback(int tok) {
		assert(next_token.size() < 2);
		next_token.push_back( tok );
	}
};
#include ".gen/rust.tab.hpp"

extern int yylineno;
extern int yylex(YYSTYPE* lvalp, ParserContext& context);

extern void yyerror(ParserContext& context, const char *s);
extern int yydebug;

