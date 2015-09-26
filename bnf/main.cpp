#include <stdio.h>
#include <string.h>
#include "lex.hpp"

extern FILE*	yyin;

::std::string get_dir_path(const ::std::string& input) {
	auto pos = input.rfind("/");
	if( pos == ::std::string::npos) {
		return "";
	}
	else {
		return ::std::string( input, 0, input.rfind("/") + 1 );
	}
}

int main(int argc, char *argv[]) {
	const char *base_filename;
	if(argc < 2 || strcmp(argv[1], "-") == 0) {
		yyin = stdin;
		base_filename = "-";
	}
	else {
		base_filename = argv[1];
		yyin = fopen(argv[1], "r");
		if( !yyin ) {
			fprintf(stderr, "ERROR: Unable to open '%s': '%s'\n", argv[1], strerror(errno));
			return 1;
		}
	}
	yylineno = 1;
	yydebug = (getenv("BNFDEBUG") != NULL);

	ParserContext	context(base_filename);
	if( context.filename != "-" ) {
		context.base_path = get_dir_path(context.filename);
	}

	int rv = yyparse(context);
	fclose(yyin);
	if( rv != 0 ) {
		return rv;
	}

	assert( context.output_module.get() );
	auto& mod = *context.output_module;
	printf("output module = %p\n", &mod);
	
	for(auto& item : mod.items()) {
		Module* opt_submod = dynamic_cast<Module*>(item.get());
		if( !opt_submod )	continue;
		Module& submod = *opt_submod;
		printf("- Module %s\n", submod.name().c_str());
		
		if( ! submod.is_external() ) {
			// - inline modules can be ignored.. for now
			continue ;
		}
		::std::string	filename;
		::std::string	base_path;
		if( context.filename == "-" ) {
			return 1;
		}
		else if( submod.attrs().has("path") ) {
			filename = get_dir_path(context.filename) + submod.attrs().get_first("path").string();
			base_path = get_dir_path(filename);
		}
		else if( context.base_path == "" ) {
			return 1;
		}
		else {
			filename = context.base_path + submod.name() + ".rs";
			base_path = "";
			yyin = fopen( filename.c_str(), "r" );
			if( !yyin ) {
				printf("> '%s' not found\n", filename.c_str());
				base_path = context.base_path + submod.name() + "/";
				filename = base_path + "mod.rs";
			}
			else {
				fclose(yyin);
			}
		}
		assert(filename.size() > 0);
		yyin = fopen( filename.c_str(), "r" );
		if( !yyin ) {
			printf("> '%s' not found\n", filename.c_str());
			return 1;
		}
		
		ParserContext	new_context(filename);
		int rv = yyparse(new_context);
		fclose(yyin);
		if( rv != 0 ) {
			return rv;
		}
		submod = ::std::move( *new_context.output_module );
	}
	
	return 0;
}
void yyerror(ParserContext& context, const char *s) {
	fprintf(stderr, "\x1b[31mERROR: %s:%d: yyerror(%s)\x1b[0m\n", context.filename.c_str(), yylineno, s);
	exit(1);
}
