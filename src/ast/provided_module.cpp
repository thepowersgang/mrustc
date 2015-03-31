/*
 */
#include "ast.hpp"

AST::Module g_compiler_module;

void AST_InitProvidedModule()
{
    // "struct str([u8])"
    g_compiler_module.add_struct(true, "str", AST::TypeParams(), ::std::vector<AST::StructItem> {
        AST::StructItem("", TypeRef(TypeRef::TagUnsizedArray(), TypeRef(CORETYPE_U8)), false),
        });
    
    AST::Path   copy_marker_path({AST::PathNode("marker"),AST::PathNode("Copy")});
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(CORETYPE_F32), copy_marker_path));
    g_compiler_module.add_impl(AST::Impl(AST::TypeParams(), TypeRef(CORETYPE_F64), copy_marker_path));
}

