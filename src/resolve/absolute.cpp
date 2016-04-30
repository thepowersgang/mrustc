/*
 * Convert all paths in AST into absolute form (or to the relevant local item)
 *
 * After complete there should be no:
 * - Relative/super/self paths
 * - MaybeBind patterns
 */
#include <ast/crate.hpp>
#include <main_bindings.hpp>

void Resolve_Absolutise(AST::Crate& crate)
{
    TODO(Span(), "Run 'absolutise' resolve pass");
}


