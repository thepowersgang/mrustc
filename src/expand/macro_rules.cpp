
#include <synext.hpp>
#include "../ast/expr.hpp"
#include "../ast/ast.hpp"
#include "../parse/common.hpp"

class CMacroRulesExpander:
    public ExpandProcMacro
{
    bool    expand_early() const override { return true; }
    
    AST::Expr expand(const ::std::string& ident, const TokenTree& tt, AST::Module& mod, MacroPosition position)
    {
        if( ident == "" ) {
            throw ::std::runtime_error( "ERROR: macro_rules! requires an identifier" );
        }
        
        TTStream    lex(tt);
        auto mac = Parse_MacroRules(lex);
        // TODO: Place into current module using `ident` as the name
        
        return AST::Expr();
    }
};


STATIC_MACRO("macro_rules", CMacroRulesExpander);

