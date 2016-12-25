#include <stdio.h>
#include <string.h>
#include <iostream>
#include "lex.hpp"
#include "common.hpp"

extern FILE*	yyin;

::std::string get_dir_path(const ::std::string& input) {
	auto pos = input.rfind("/");
	if( pos == ::std::string::npos) {
		return "./";
	}
	else {
		return ::std::string( input, 0, input.rfind("/") + 1 );
	}
}

::std::unique_ptr<Module> parse_module_file(::std::string filename, ::std::string base_path)
{
	::std::cout << "filename = " << filename << ", base_path = " << base_path << ";" << ::std::endl;

	if( filename != "-" ) {
		yyin = fopen(filename.c_str(), "r");
		if( !yyin ) {
			fprintf(stderr, "ERROR: Unable to open '%s': '%s'\n", filename.c_str(), strerror(errno));
			exit(1);
		}
	}

	ParserContext	context( ::std::move(filename) );
	context.base_path = ::std::move(base_path);

	yylineno = 1;
	int rv = yyparse(context);
	if( yyin != stdin ) {
		fclose(yyin);
	}
	if( rv != 0 ) {
		exit(1);
	}

	assert( context.output_module.get() );
	//context.output_module->set_paths( ::std::move(context.filename), ::std::move(context.base_path) );
	return ::std::move(context.output_module);
}

void post_process_module(Module& mod, const ::std::string& mod_filename, const ::std::string& mod_dir_path)
{
	::std::cout << "mod = ["<<mod.mod_path()<<"], mod_filename = " << mod_filename << ", mod_dir_path = " << mod_dir_path << ";" << ::std::endl;

	for(auto& item : mod.items())
	{
		Module* opt_submod = dynamic_cast<Module*>(item.get());
		if( !opt_submod )	continue;
		Module& submod = *opt_submod;
		printf("- Module %s\n", submod.name().c_str());

		if( ! submod.is_external() ) {
			// - inline modules can be ignored.. for now
			if( mod_dir_path != "" ) {
				submod.set_paths( mod_filename, mod_dir_path + submod.name() + "/" );
			}
		}
		else
		{
			::std::string	filename;
			::std::string	base_path;
			if( mod_filename == "-" ) {
				fprintf(stderr, "ERROR: Referencing 'mod %s;' from stdin\n", submod.name().c_str());
				exit(1);
			}
			else if( submod.attrs().has("path") ) {
				filename = get_dir_path(mod_filename) + submod.attrs().get_first("path").string();
				base_path = get_dir_path(filename);
			}
			else if( mod_dir_path == "" ) {
				fprintf(stderr, "ERROR: Referencing 'mod %s;' from non-controlling file\n", submod.name().c_str());
				exit(1);
			}
			else {
				filename = mod_dir_path + submod.name() + ".rs";
				base_path = "";
				yyin = fopen( filename.c_str(), "r" );
				if( !yyin ) {
					printf("> '%s' not found\n", filename.c_str());
					base_path = mod_dir_path + submod.name() + "/";
					filename = base_path + "mod.rs";
				}
				else {
					fclose(yyin);
				}
			}

			assert(filename.size() > 0);
			submod = ::std::move( *parse_module_file(filename, base_path) );
			submod.set_paths( filename, base_path );
		}

		auto mp = mod.mod_path();
		mp.push_back(submod.name());
		submod.set_mod_path(::std::move(mp));
	}

	for(auto& item : mod.items())
	{
		Module* opt_submod = dynamic_cast<Module*>(item.get());
		if( !opt_submod )	continue;
		Module& submod = *opt_submod;

		const ::std::string& submod_fname = submod.filename();
		const ::std::string& submod_dir = submod.base_dir();
		post_process_module(submod, submod_fname, submod_dir);
	}
}

int main(int argc, char *argv[]) {
	::std::string	base_filename;
	if(argc < 2 || strcmp(argv[1], "-") == 0) {
		yyin = stdin;
		base_filename = "-";
	}
	else {
		base_filename = argv[1];
	}
	yydebug = (getenv("BNFDEBUG") != NULL);

	::std::string	base_path = (base_filename ==  "-" ? ::std::string("") : get_dir_path(base_filename));
	auto mod = parse_module_file(base_filename, base_path);
	printf("output module = %p\n", &*mod);

	post_process_module(*mod, base_filename, base_path);

	printf("Crate parsed\n");

	return 0;
}
void yyerror(ParserContext& context, const char *s) {
	fprintf(stderr, "\x1b[31mERROR: %s:%d: yyerror(%s)\x1b[0m\n", context.filename.c_str(), yylineno, s);
	exit(1);
}
